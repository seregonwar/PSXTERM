//! Bounded single-line editing with UTF-8-safe cursor movement.
use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

#[derive(Clone, Copy)]
enum Limit {
    Characters(usize),
    Bytes(usize),
}

#[derive(Clone)]
pub struct Input {
    text: String,
    cursor: usize,
    limit: Limit,
}
impl Input {
    pub fn new(text: String, limit: usize) -> Self {
        Self {
            cursor: text.len(),
            text,
            limit: Limit::Characters(limit),
        }
    }
    pub fn bytes(text: String, limit: usize) -> Self {
        Self {
            cursor: text.len(),
            text,
            limit: Limit::Bytes(limit),
        }
    }
    pub fn insert(&mut self, text: &str) -> bool {
        let mut remaining = match self.limit {
            Limit::Characters(limit) => limit.saturating_sub(self.text.chars().count()),
            Limit::Bytes(limit) => limit.saturating_sub(self.text.len()),
        };
        let mut inserted = String::new();
        for c in text.chars().filter(|c| !c.is_control()) {
            let size = match self.limit {
                Limit::Characters(_) => 1,
                Limit::Bytes(_) => c.len_utf8(),
            };
            if size > remaining {
                break;
            }
            inserted.push(c);
            remaining -= size;
        }
        let changed = !inserted.is_empty();
        self.text.insert_str(self.cursor, &inserted);
        self.cursor += inserted.len();
        changed
    }
    pub fn as_str(&self) -> &str {
        &self.text
    }
    pub fn clear(&mut self) -> bool {
        let changed = !self.text.is_empty();
        self.text.clear();
        self.cursor = 0;
        changed
    }
    pub fn home(&mut self) {
        self.cursor = 0;
    }
    pub fn end(&mut self) {
        self.cursor = self.text.len();
    }
    pub fn left(&mut self) {
        if let Some(c) = self.text[..self.cursor].chars().next_back() {
            self.cursor -= c.len_utf8();
        }
    }
    pub fn right(&mut self) {
        if let Some(c) = self.text[self.cursor..].chars().next() {
            self.cursor += c.len_utf8();
        }
    }
    pub fn backspace(&mut self) -> bool {
        let end = self.cursor;
        self.left();
        self.text.replace_range(self.cursor..end, "");
        self.cursor != end
    }
    pub fn delete(&mut self) -> bool {
        if let Some(c) = self.text[self.cursor..].chars().next() {
            self.text
                .replace_range(self.cursor..self.cursor + c.len_utf8(), "");
            return true;
        }
        false
    }
    /// Some(true): text changed; Some(false): consumed cursor/no-op editing;
    /// None: this key belongs to the surrounding dialog's navigation.
    pub fn key(&mut self, key: KeyEvent) -> Option<bool> {
        let changed = match key.code {
            KeyCode::Left
                if !key
                    .modifiers
                    .intersects(KeyModifiers::CONTROL | KeyModifiers::ALT) =>
            {
                self.left();
                false
            }
            KeyCode::Right
                if !key
                    .modifiers
                    .intersects(KeyModifiers::CONTROL | KeyModifiers::ALT) =>
            {
                self.right();
                false
            }
            KeyCode::Home => {
                self.home();
                false
            }
            KeyCode::End => {
                self.end();
                false
            }
            KeyCode::Char('a') if key.modifiers.contains(KeyModifiers::CONTROL) => {
                self.home();
                false
            }
            KeyCode::Char('e') if key.modifiers.contains(KeyModifiers::CONTROL) => {
                self.end();
                false
            }
            KeyCode::Backspace => self.backspace(),
            KeyCode::Delete => self.delete(),
            KeyCode::Char('u') if key.modifiers.contains(KeyModifiers::CONTROL) => self.clear(),
            KeyCode::Char(c)
                if !key
                    .modifiers
                    .intersects(KeyModifiers::CONTROL | KeyModifiers::ALT)
                    && !c.is_control() =>
            {
                self.insert(&c.to_string())
            }
            _ => return None,
        };
        Some(changed)
    }
    fn view_start(&self, width: usize) -> usize {
        let mut start = self.cursor;
        let mut used = 0;
        if self.text[..self.cursor].width() < width {
            start = 0;
        } else {
            for (index, c) in self.text[..self.cursor].char_indices().rev() {
                let w = c.width().unwrap_or(0);
                if used + w > width.saturating_sub(2) {
                    break;
                }
                start = index;
                used += w;
            }
        }
        start
    }
    /// A clipped view that keeps the insertion cursor on screen. Work is
    /// bounded by input length, rather than remeasuring every string suffix.
    pub fn view(&self, width: u16) -> (String, u16) {
        let width = usize::from(width);
        if width == 0 {
            return (String::new(), 0);
        }
        let start = self.view_start(width);
        let mut out = if start > 0 {
            String::from("…")
        } else {
            String::new()
        };
        let mut used = out.width();
        let cursor = (used + self.text[start..self.cursor].width()).min(width - 1) as u16;
        for c in self.text[start..].chars() {
            let w = c.width().unwrap_or(0);
            if used + w > width {
                break;
            }
            out.push(c);
            used += w;
        }
        (out, cursor)
    }
    fn masked_start(&self, width: usize) -> usize {
        let caret = self.text[..self.cursor].chars().count();
        if caret < width {
            0
        } else {
            caret.saturating_sub(width.saturating_sub(2))
        }
    }
    /// Render only mask characters, including when scrolling to the caret.
    pub fn masked_view(&self, width: u16) -> (String, u16) {
        let width = usize::from(width);
        if width == 0 {
            return (String::new(), 0);
        }
        let count = self.text.chars().count();
        let caret = self.text[..self.cursor].chars().count();
        let start = self.masked_start(width);
        let prefix = usize::from(start > 0);
        let mut view = if start > 0 {
            String::from("…")
        } else {
            String::new()
        };
        view.push_str(
            &"•".repeat(
                count
                    .saturating_sub(start)
                    .min(width.saturating_sub(prefix)),
            ),
        );
        (view, (prefix + caret - start).min(width - 1) as u16)
    }
    /// Position the caret in the current viewport, without exposing masked text.
    pub fn click(&mut self, column: u16, width: u16, masked: bool) {
        if width == 0 {
            return;
        }
        let target = usize::from(column.min(width - 1));
        if masked {
            let start = self.masked_start(usize::from(width));
            let index = start + target.saturating_sub(usize::from(start > 0));
            self.cursor = self
                .text
                .char_indices()
                .nth(index)
                .map_or(self.text.len(), |(n, _)| n);
        } else {
            let start = self.view_start(usize::from(width));
            let mut used = usize::from(start > 0);
            self.cursor = start;
            for (index, c) in self.text[start..].char_indices() {
                let w = c.width().unwrap_or(0);
                if used + w > target {
                    break;
                }
                used += w;
                self.cursor = start + index + c.len_utf8();
            }
        }
    }
}

impl std::ops::Deref for Input {
    type Target = str;
    fn deref(&self) -> &str {
        self.as_str()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn byte_limits_apply_to_typing_and_paste_at_utf8_boundaries() {
        let mut input = Input::bytes(String::new(), 7);
        assert!(input.insert("界éabc"));
        assert_eq!(input.as_str(), "界éab");
        assert!(!input.insert("c"));
        input.home();
        assert!(input.delete());
        assert!(
            input
                .key(KeyEvent::new(KeyCode::Char('界'), KeyModifiers::NONE))
                .unwrap()
        );
        assert_eq!(input.as_str(), "界éab");
        assert!(
            !input
                .key(KeyEvent::new(KeyCode::Char('x'), KeyModifiers::NONE))
                .unwrap()
        );
    }
    #[test]
    fn masked_mouse_editing_and_clipping_never_return_secret_text() {
        let mut input = Input::bytes("sëcret界".repeat(15), 255);
        for width in 0..80 {
            let (view, caret) = input.masked_view(width);
            assert!(view.chars().all(|c| c == '•' || c == '…'));
            assert!(view.width() <= usize::from(width));
            assert!(width == 0 || caret < width);
        }
        input.home();
        input.click(2, 10, true);
        input.insert("X");
        assert!(input.as_str().starts_with("sëXcret界"));
        input.end();
        input.click(1, 10, true);
        let old = input.as_str().to_string();
        input.delete();
        assert_eq!(input.as_str().chars().count() + 1, old.chars().count());
        assert!(input.masked_view(10).0.chars().all(|c| "•…".contains(c)));
    }
    #[test]
    fn mouse_and_keyboard_edit_the_same_visible_unicode_position() {
        let mut input = Input::new("dir/界é.txt".into(), 100);
        input.home();
        input.click(5, 20, false); // The second cell of 界 belongs to its start.
        input.insert("X");
        assert_eq!(input.as_str(), "dir/X界é.txt");
        assert_eq!(
            input.key(KeyEvent::new(KeyCode::Char('e'), KeyModifiers::CONTROL)),
            Some(false)
        );
        assert_eq!(
            input.key(KeyEvent::new(KeyCode::Left, KeyModifiers::NONE)),
            Some(false)
        );
        assert_eq!(
            input.key(KeyEvent::new(KeyCode::Char('Y'), KeyModifiers::NONE)),
            Some(true)
        );
        assert!(input.as_str().ends_with(".txYt"));
        assert_eq!(
            input.key(KeyEvent::new(KeyCode::Tab, KeyModifiers::NONE)),
            None
        );
        assert_eq!(
            input.key(KeyEvent::new(KeyCode::Char('a'), KeyModifiers::CONTROL)),
            Some(false)
        );
        input.insert("root/");
        assert!(input.as_str().starts_with("root/dir/"));
    }
    #[test]
    fn edits_middle_of_unicode_paths_without_splitting_utf8() {
        let mut input = Input::new("dir/界é.txt".into(), 100);
        input.home();
        for _ in 0..4 {
            input.right();
        }
        input.delete();
        input.insert("出力");
        assert_eq!(input.text, "dir/出力é.txt");
        input.backspace();
        assert_eq!(input.text, "dir/出é.txt");
        input.end();
        input.left();
        input.insert("X");
        assert_eq!(input.text, "dir/出é.txXt");
    }
    #[test]
    fn clipping_keeps_the_cursor_visible_and_input_bounded() {
        let mut input = Input::new(String::new(), 256);
        input.insert(&"界é".repeat(1000));
        input.insert("\r\n\x1b");
        assert_eq!(input.text.chars().count(), 256);
        for width in 0..80 {
            let (view, cursor) = input.view(width);
            assert!(view.width() <= usize::from(width));
            assert!(width == 0 || cursor < width);
        }
        input.home();
        let (view, cursor) = input.view(20);
        assert!(view.starts_with("界é"));
        assert_eq!(cursor, 0);
        input.clear();
        input.insert("a\nb\rc\x1b");
        assert_eq!(input.text, "abc");
    }
}
