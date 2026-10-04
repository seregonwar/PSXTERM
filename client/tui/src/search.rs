//! Local literal search over a bounded terminal snapshot, with cell coordinates.
//! Index only on demand: live output must not rescan 5,000 history rows per frame.

pub const MAX_QUERY_CHARS: usize = 256;
pub const MAX_MATCHES: usize = 10_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Position {
    /// Physical row counted from the bottom of the live terminal.
    pub line: usize,
    pub column: u16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Match {
    pub start: Position,
    pub end: Position,
}

struct CellRun {
    start: usize,
    end: usize,
    first: Position,
    last: Position,
}

struct Index {
    text: String,
    cells: Vec<CellRun>,
    revision: u64,
    dimensions: (u16, u16),
}

impl Index {
    fn new(source: &vt100::Screen, revision: u64) -> Self {
        let mut screen = source.clone();
        let dimensions = screen.size();
        let (rows, cols) = dimensions;
        let mut text = String::new();
        let mut cells = Vec::new();
        if rows > 0 && cols > 0 {
            screen.set_scrollback(usize::MAX);
            let oldest = screen.scrollback() + usize::from(rows) - 1;
            for line in (0..=oldest).rev() {
                screen.set_scrollback(line.saturating_sub(usize::from(rows) - 1));
                let row = usize::from(rows) - 1 - (line - screen.scrollback());
                let mut written_end = text.len();
                for column in 0..cols {
                    let Some(cell) = screen.cell(row as u16, column) else {
                        continue;
                    };
                    if cell.is_wide_continuation() {
                        continue;
                    }
                    let start = text.len();
                    if cell.has_contents() {
                        text.extend(cell.contents().chars().flat_map(char::to_lowercase));
                        written_end = text.len();
                    } else {
                        text.push(' ');
                    }
                    cells.push(CellRun {
                        start,
                        end: text.len(),
                        first: Position { line, column },
                        last: Position {
                            line,
                            column: column
                                .saturating_add(u16::from(cell.is_wide()))
                                .min(cols - 1),
                        },
                    });
                }
                let wraps = screen.row_wrapped(row as u16);
                // Unwritten margin padding is not text. Explicit spaces in
                // wrapped rows remain part of the searchable logical line.
                text.truncate(if wraps {
                    written_end
                } else {
                    text.trim_end_matches(' ').len()
                });
                while cells.last().is_some_and(|cell| cell.start >= text.len()) {
                    cells.pop();
                }
                if !wraps {
                    text.push('\n');
                }
            }
        }
        Self {
            text,
            cells,
            revision,
            dimensions,
        }
    }
}

pub struct Search {
    pub pane: u64,
    pub query: crate::input::Input,
    pub matches: Vec<Match>,
    pub selected: usize,
    pub truncated: bool,
    index: Index,
}

impl Search {
    pub fn new(pane: u64, screen: &vt100::Screen, revision: u64) -> Self {
        Self {
            pane,
            query: crate::input::Input::new(String::new(), MAX_QUERY_CHARS),
            matches: Vec::new(),
            selected: 0,
            truncated: false,
            index: Index::new(screen, revision),
        }
    }
    pub fn is_current(&self, screen: &vt100::Screen, revision: u64) -> bool {
        self.index.revision == revision && self.index.dimensions == screen.size()
    }
    pub fn refresh(&mut self, screen: &vt100::Screen, revision: u64) {
        self.index = Index::new(screen, revision);
        self.find();
    }
    pub fn append(&mut self, text: &str) {
        if self.query.insert(text) {
            self.find();
        }
    }
    pub fn backspace(&mut self) {
        if self.query.backspace() {
            self.find();
        }
    }
    pub fn clear(&mut self) {
        if self.query.clear() {
            self.find();
        }
    }
    pub fn find(&mut self) {
        self.matches.clear();
        self.truncated = false;
        let query = self.query.to_lowercase();
        if !query.is_empty() {
            for (start, _) in self.index.text.rmatch_indices(&query) {
                if self.matches.len() == MAX_MATCHES {
                    self.truncated = true;
                    break;
                }
                let end = start + query.len();
                let first = self.index.cells.partition_point(|cell| cell.end <= start);
                let last = self.index.cells.partition_point(|cell| cell.start < end);
                if first < last {
                    self.matches.push(Match {
                        start: self.index.cells[first].first,
                        end: self.index.cells[last - 1].last,
                    });
                }
            }
            self.matches.reverse();
        }
        // The newest result is generally closest to the current command.
        self.selected = self.matches.len().saturating_sub(1);
    }
    pub fn step(&mut self, delta: isize) {
        if !self.matches.is_empty() {
            self.selected =
                (self.selected as isize + delta).rem_euclid(self.matches.len() as isize) as usize;
        }
    }
    pub fn current(&self) -> Option<Match> {
        self.matches.get(self.selected).copied()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn searches_history_unicode_and_soft_wraps_without_mutating_the_source() {
        let mut parser = vt100::Parser::new(3, 10, 20);
        parser.process("older\r\n\x1b[34m界é İ\x1b[0m\r\n123456789 tail\r\nlatest".as_bytes());
        parser.screen_mut().set_scrollback(1);
        let original = parser.screen().contents_formatted();
        let mut search = Search::new(1, parser.screen(), 0);
        search.append("OLDER");
        assert_eq!(search.matches.len(), 1);
        assert!(search.current().unwrap().start.line >= 3);
        search.clear();
        search.append("界é i̇");
        assert_eq!(search.matches.len(), 1);
        assert_eq!(search.current().unwrap().start.column, 0);
        assert_eq!(search.current().unwrap().end.column, 4);
        search.clear();
        search.append("789 tail");
        let found = search.current().unwrap();
        assert!(
            found.start.line > found.end.line,
            "match must span wrapped rows"
        );
        assert_eq!(found.start.column, 6);
        assert_eq!(found.end.column, 3);
        assert_eq!(parser.screen().contents_formatted(), original);
        assert_eq!(parser.screen().scrollback(), 1);
    }

    #[test]
    fn invalidates_on_output_or_resize_but_not_scrolling_and_bounds_input() {
        let mut parser = vt100::Parser::new(3, 10, 20);
        parser.process(b"a a\r\na");
        let mut search = Search::new(1, parser.screen(), 7);
        search.append("a");
        assert_eq!(search.matches.len(), 3);
        search.step(1);
        assert_eq!(search.selected, 0);
        search.step(-1);
        assert_eq!(search.selected, 2);
        parser.screen_mut().set_scrollback(20);
        assert!(search.is_current(parser.screen(), 7));
        assert!(!search.is_current(parser.screen(), 8));
        parser.screen_mut().set_size(4, 10);
        assert!(!search.is_current(parser.screen(), 7));
        search.refresh(parser.screen(), 8);
        assert!(search.is_current(parser.screen(), 8));
        search.clear();
        search.append(&"界".repeat(1000));
        assert_eq!(search.query.chars().count(), MAX_QUERY_CHARS);
        search.backspace();
        assert_eq!(search.query.chars().count(), MAX_QUERY_CHARS - 1);
        search.clear();
        search.append("\r\n\x1b[31m");
        assert_eq!(search.query.as_str(), "[31m");
    }

    #[test]
    fn bounds_results_and_searches_the_oldest_retained_row() {
        let mut parser = vt100::Parser::new(3, 80, 5000);
        parser.process(b"evicted\r\n");
        for _ in 0..5100 {
            parser.process(b"a a a\r\n");
        }
        let mut search = Search::new(1, parser.screen(), 0);
        search.append("evicted");
        assert!(search.matches.is_empty());
        search.clear();
        search.append("a a a");
        assert!(search.matches[0].start.line >= 5000);
        assert!(!search.truncated);
        search.clear();
        search.append("a");
        assert_eq!(search.matches.len(), MAX_MATCHES);
        assert!(search.truncated);
        assert!(search.matches.last().unwrap().start.line < 3);
    }

    #[test]
    fn wide_glyph_at_the_margin_follows_the_parsers_row_boundary() {
        let mut parser = vt100::Parser::new(3, 4, 20);
        parser.process("abc界".as_bytes());
        let mut search = Search::new(1, parser.screen(), 0);
        search.append("c界");
        // vt100 deliberately treats a wide glyph moved past an empty last
        // column as a separate line. Do not guess whether it was a hard break.
        assert!(search.matches.is_empty());
        search.clear();
        search.append("界");
        let found = search.current().unwrap();
        assert_eq!(found.start.column, 0);
        assert_eq!(found.end.column, 1);
        assert_eq!(found.start.line, found.end.line);
    }
}
