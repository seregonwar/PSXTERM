use crate::i18n::{tr, trf};
use crate::input::Input;
use crate::{
    clipboard,
    config::{Config, Console},
    flash::{ACTIONS, filtered},
    protocol::{Command, Connection},
    terminal::Pane,
};
use anyhow::Result;
use crossterm::event::{Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseEventKind};
use ratatui::layout::Rect;
#[cfg(test)]
use ratatui::{Terminal, backend::TestBackend};
use std::{collections::HashMap, path::PathBuf};

/// A text selection anchored to the content of one terminal pane.
///
/// The coordinates are columns and content lines, where a line counts from the
/// bottom of the live screen, not screen cells: that is what keeps a selection
/// on the text it was made on when the view scrolls underneath it.
#[derive(Clone, Copy)]
pub struct Selection {
    pub pane: u64,
    pub start: (u16, u16),
    pub end: (u16, u16),
}

impl Selection {
    /// Inclusive bounds, normalised so the first pair is the top-left one.
    pub fn bounds(&self) -> ((u16, u16), (u16, u16)) {
        let (mut c0, mut l0) = self.start;
        let (mut c1, mut l1) = self.end;

        if l0 < l1 || (l0 == l1 && c0 > c1) {
            std::mem::swap(&mut c0, &mut c1);
            std::mem::swap(&mut l0, &mut l1);
        }

        ((c0, l0), (c1, l1))
    }

    pub fn is_empty(&self) -> bool {
        self.start == self.end
    }
}

pub enum Overlay {
    None,
    Consoles {
        selected: usize,
    },
    Palette {
        query: Input,
        selected: usize,
    },
    Form {
        editing: Option<u64>,
        fields: [Input; 4],
        field: usize,
        error: String,
    },
    Rename(Input),
    Search(Box<crate::search::Search>),
    Export(crate::export::Dialog),
    Text {
        title: String,
        text: String,
        scroll: u16,
    },
    ConfirmClose(u64),
    ConfirmRemove(u64),
    ConfirmQuit,
    ConfirmPaste(String),
}
#[derive(Clone)]
pub enum Hit {
    Console,
    New,
    Palette,
    Pane(u64),
    Close(u64),
    Action(String),
    Choice(usize),
    Input(usize),
    Confirm(bool),
    SearchStep(isize),
    SearchRefresh,
    SearchClose,
    ExportField(usize),
    ExportScope(crate::export::Scope),
}

pub struct App {
    pub config: Config,
    pub config_path: PathBuf,
    pub panes: Vec<Pane>,
    pub console: Option<u64>,
    pub active: Option<u64>,
    pub remembered: HashMap<u64, u64>,
    pub split: bool,
    pub sidebar_focus: bool,
    pub sidebar_collapsed: bool,
    pub overlay: Overlay,
    pub notice: String,
    pub quit: bool,
    pub demo: bool,
    pub hits: Vec<(Rect, Hit)>,
    /// Content painted by the last layout, independent of modal mouse targets.
    pub(crate) rendered_panes: Vec<u64>,
    pub text_scroll_limit: u16,
    /// Text selection, anchored to the content of the pane it started in.
    pub sel: Option<Selection>,
    pub screen: (u16, u16),
    retiring: Vec<Connection>,
    next_pane: u64,
    poll_cursor: usize,
    pub(crate) poll_pending: bool,
    export_job: Option<crate::export::Job>,
    export_retry: Option<crate::export::Dialog>,
}
impl App {
    pub fn new(config: Config, config_path: PathBuf, demo: bool) -> Self {
        let console = config.consoles.first().map(|c| c.id);
        Self {
            config,
            config_path,
            panes: vec![],
            console,
            active: None,
            remembered: HashMap::new(),
            split: false,
            sidebar_focus: false,
            sidebar_collapsed: false,
            overlay: Overlay::None,
            notice: tr("hint.start"),
            quit: false,
            demo,
            hits: vec![],
            rendered_panes: Vec::with_capacity(4),
            text_scroll_limit: 0,
            sel: None,
            screen: (0, 0),
            retiring: vec![],
            next_pane: 1,
            poll_cursor: 0,
            poll_pending: false,
            export_job: None,
            export_retry: None,
        }
    }

    /// Rectangle of a terminal pane, from the hit boxes the last draw left.
    pub(crate) fn pane_rect(&self, pane: u64) -> Option<Rect> {
        self.hits.iter().rev().find_map(|(rect, hit)| match hit {
            Hit::Pane(id) if *id == pane => Some(Rect::new(
                rect.x.saturating_add(1),
                rect.y.saturating_add(1),
                rect.width.saturating_sub(2),
                rect.height.saturating_sub(2),
            )),
            _ => None,
        })
    }

    fn pane_scrollback(&self, pane: u64) -> u16 {
        self.panes
            .iter()
            .find(|p| p.id == pane)
            .map(|p| p.parser.screen().scrollback() as u16)
            .unwrap_or(0)
    }

    fn set_pane_scrollback(&mut self, pane: u64, offset: u16) {
        if let Some(p) = self.panes.iter_mut().find(|p| p.id == pane) {
            p.parser.screen_mut().set_scrollback(offset as usize);
        }
    }

    /// Content line shown at a screen row of that pane.
    ///
    /// The number counts from the live bottom of the view: scrollback plus the
    /// rows left below the given row. Anchored that way it does not change when
    /// the view scrolls, which is what keeps a selection on its text.
    fn content_line(&self, pane: u64, rect: Rect, row: u16) -> u16 {
        let below = rect
            .height
            .saturating_sub(1)
            .saturating_sub(row.saturating_sub(rect.y));

        self.pane_scrollback(pane).saturating_add(below)
    }

    /// Screen row where a content line is currently visible, if it is.
    #[cfg(test)]
    pub(crate) fn screen_row_for_line(&self, pane: u64, rect: Rect, line: u16) -> Option<u16> {
        let above = line.checked_sub(self.pane_scrollback(pane))?;

        (above < rect.height).then_some(rect.y + (rect.height - 1 - above))
    }

    /// Copy the current selection, or the visible content of the active pane
    /// when there is none, into the clipboard of the terminal displaying this
    /// client.
    pub fn copy_selection(&mut self) {
        let sel = match self.sel {
            Some(sel) if !sel.is_empty() => sel,
            _ => {
                /* Nothing selected: the visible content of the active pane is
                 * the natural thing to copy. */
                let Some(pane) = self.active else {
                    return;
                };
                let Some(rect) = self.pane_rect(pane) else {
                    return;
                };
                if rect.width == 0 || rect.height == 0 {
                    return;
                }

                Selection {
                    pane,
                    start: (rect.x, self.content_line(pane, rect, rect.y)),
                    end: (
                        rect.x + rect.width - 1,
                        self.content_line(pane, rect, rect.y + rect.height - 1),
                    ),
                }
            }
        };

        match self.selection_text(sel) {
            Some(text) if !text.trim().is_empty() => {
                /* Show what was copied, not only how much: the feedback has to
                 * say whether the intended text was grabbed. */
                let preview: String = text
                    .lines()
                    .find(|line| !line.trim().is_empty())
                    .unwrap_or("")
                    .trim()
                    .chars()
                    .take(40)
                    .collect();

                match clipboard::copy(&text) {
                    Ok(count) => {
                        self.notice = trf("notice.copied_text", &[count.to_string(), preview])
                    }
                    Err(e) => self.notice = e.to_string(),
                }
            }
            Some(_) => self.notice = tr("notice.copy_empty"),
            None => self.notice = tr("ui.tiny"),
        }

        self.sel = None;
    }

    /// The text under the selection.
    ///
    /// Read terminal cells directly; never render the workspace or change the
    /// live scroll position, pane geometry, mouse targets, or remote dimensions.
    pub fn selection_text(&self, sel: Selection) -> Option<String> {
        let rect = self.pane_rect(sel.pane)?;
        if rect.width == 0 || rect.height == 0 {
            return None;
        }

        let ((c0, l0), (c1, l1)) = sel.bounds();
        let pane = self.panes.iter().find(|p| p.id == sel.pane)?;
        let mut screen = pane.parser.screen().clone();
        let (rows, cols) = screen.size();
        if rows == 0 || cols == 0 {
            return None;
        }
        let mut out = String::new();
        for line in (l1..=l0).rev() {
            // Put the requested content line in a cloned viewport. Near the
            // oldest retained row the offset clamps, so calculate its row
            // from the actual offset rather than assuming row zero.
            screen.set_scrollback(usize::from(line.saturating_sub(rows - 1)));
            let above = usize::from(line).checked_sub(screen.scrollback())?;
            if above >= usize::from(rows) {
                return None; // selected history was evicted
            }
            let row = rows - 1 - above as u16;
            let start = if line == l0 { c0 } else { rect.x };
            let end = if line == l1 { c1 } else { rect.right() - 1 };
            let mut start = start.saturating_sub(rect.x).min(cols - 1);
            if screen
                .cell(row, start)
                .is_some_and(|cell| cell.is_wide_continuation())
            {
                start = start.saturating_sub(1);
            }
            let end = end.saturating_sub(rect.x).min(cols - 1);
            let mut text = String::new();
            let mut written_end = 0;
            for x in start..=end {
                if let Some(cell) = screen.cell(row, x)
                    && !cell.is_wide_continuation()
                {
                    text.push_str(if cell.has_contents() {
                        cell.contents()
                    } else {
                        " "
                    });
                    if cell.has_contents() {
                        written_end = text.len();
                    }
                }
            }
            let wraps_to_next = line != l1 && screen.row_wrapped(row);
            if wraps_to_next {
                text.truncate(written_end);
            }
            out.push_str(if wraps_to_next {
                &text
            } else {
                text.trim_end()
            });
            if line != l1 && !wraps_to_next {
                out.push('\n');
            }
        }

        Some(out)
    }
    pub fn current_console(&self) -> Option<&Console> {
        self.config
            .consoles
            .iter()
            .find(|c| Some(c.id) == self.console)
    }
    pub fn current(&self) -> Option<&Pane> {
        self.panes.iter().find(|p| Some(p.id) == self.active)
    }
    pub fn current_mut(&mut self) -> Option<&mut Pane> {
        self.panes.iter_mut().find(|p| Some(p.id) == self.active)
    }
    pub fn visible_ids(&self) -> Vec<u64> {
        self.panes
            .iter()
            .filter(|p| Some(p.console_id) == self.console)
            .map(|p| p.id)
            .collect()
    }
    pub fn unread_count(&self, console: Option<u64>) -> usize {
        self.panes
            .iter()
            .filter(|p| p.unread && console.is_none_or(|id| p.console_id == id))
            .count()
    }
    fn next_unread(&self) -> Option<u64> {
        let count = self.panes.len();
        let start = self
            .panes
            .iter()
            .position(|p| Some(p.id) == self.active)
            .map_or(0, |n| n + 1);
        (0..count)
            .map(|n| &self.panes[(start + n) % count])
            .find(|p| p.unread)
            .map(|p| p.id)
    }
    pub fn select(&mut self, id: u64) {
        if self.sel.is_some_and(|selection| selection.pane != id) {
            self.sel = None;
        }
        if let Some(p) = self.panes.iter_mut().find(|p| p.id == id) {
            self.active = Some(id);
            self.console = Some(p.console_id);
            self.remembered.insert(p.console_id, id);
            p.unread = false;
        }
    }
    pub fn switch(&mut self, id: u64) {
        self.console = Some(id);
        self.active = self
            .remembered
            .get(&id)
            .copied()
            .filter(|p| self.panes.iter().any(|t| t.id == *p))
            .or_else(|| self.panes.iter().find(|p| p.console_id == id).map(|p| p.id));
        if let Some(p) = self.current_mut() {
            p.unread = false;
        }
    }
    pub fn new_pane(&mut self) -> Result<u64> {
        let c = self
            .current_console()
            .cloned()
            .ok_or_else(|| anyhow::anyhow!(tr("error.no_console")))?;
        if self.visible_ids().len() >= 8 || self.panes.len() + self.retiring.len() >= 32 {
            anyhow::bail!(tr("error.panes_limit"));
        }
        let id = self.next_pane;
        self.next_pane += 1;
        self.panes.push(Pane::new(id, &c, self.demo));
        self.select(id);
        self.sidebar_focus = false;
        Ok(id)
    }
    pub fn poll(&mut self) -> bool {
        let retiring = self.retiring.len();
        self.retiring.retain(|c| !c.finished());
        let mut changed = self.retiring.len() != retiring;
        if let Some(result) = self.export_job.as_ref().and_then(crate::export::Job::poll) {
            self.export_job = None;
            match result {
                Ok(saved) => {
                    self.export_retry = None;
                    self.notice = trf(
                        "export.saved",
                        &[saved.path.display().to_string(), saved.bytes.to_string()],
                    );
                }
                Err(error) => {
                    let message = error.message();
                    if let Some(dialog) = &mut self.export_retry {
                        dialog.error = message.clone();
                    }
                    if let Overlay::Export(dialog) = &mut self.overlay {
                        dialog.error = message.clone();
                    }
                    self.notice = message;
                }
            }
            changed = true;
        }
        let active = self.active;
        let count = self.panes.len();
        let start = self.poll_cursor % count.max(1);
        let mut budget = crate::work::Budget::new();
        self.poll_pending = false;
        for offset in 0..count {
            if !budget.available() {
                self.poll_pending = true;
                break;
            }
            let index = (start + offset) % count;
            self.poll_cursor = (index + 1) % count;
            let p = &mut self.panes[index];
            let revision = p.output_revision;
            let (pane_changed, pending) = p.poll_with_budget(
                Some(p.id) == active,
                self.rendered_panes.contains(&p.id),
                &mut budget,
            );
            changed |= pane_changed;
            self.poll_pending |= pending;
            if p.output_revision != revision && self.sel.is_some_and(|sel| sel.pane == p.id) {
                // Content coordinates are relative to the live bottom. If new
                // output changes it, cancel rather than copy different text.
                self.sel = None;
            }
        }
        if self.poll_cursor == start && count > 0 {
            self.poll_cursor = (start + 1) % count;
        }
        if matches!(self.overlay, Overlay::None)
            && let Some(text) = self.current_mut().and_then(|p| p.report.take())
        {
            self.overlay = Overlay::Text {
                title: tr("action.sessions"),
                text,
                scroll: 0,
            };
            changed = true;
        }
        changed
    }
    pub fn work_pending(&self) -> bool {
        self.poll_pending
    }
    pub fn connections_finished(&self) -> bool {
        self.export_job
            .as_ref()
            .is_none_or(crate::export::Job::finished)
            && self.retiring.iter().all(Connection::finished)
            && self
                .panes
                .iter()
                .all(|p| p.connection.as_ref().is_none_or(Connection::finished))
    }
    pub fn save(&mut self) -> Result<()> {
        if !self.demo {
            self.config.save(&self.config_path)?;
        }
        Ok(())
    }
    fn form(&mut self, editing: bool) {
        let c = if editing {
            self.current_console().cloned()
        } else {
            None
        };
        self.overlay = Overlay::Form {
            editing: c.as_ref().map(|c| c.id),
            fields: [
                Input::new(c.as_ref().map(|c| c.name.clone()).unwrap_or_default(), 48),
                Input::bytes(c.as_ref().map(|c| c.host.clone()).unwrap_or_default(), 253),
                Input::new(
                    c.as_ref()
                        .map(|c| c.port.to_string())
                        .unwrap_or("2323".into()),
                    5,
                ),
                Input::bytes(c.and_then(|c| c.token).unwrap_or_default(), 255),
            ],
            field: 0,
            error: String::new(),
        };
    }
    pub fn action(&mut self, id: &str) {
        if let Some(reason) = self.disabled_reason(id) {
            self.notice = reason;
            return;
        }
        self.overlay = Overlay::None;
        let result = self.perform(id);
        if let Err(e) = result {
            self.notice = e.to_string();
        }
    }
    pub fn disabled_reason(&self, id: &str) -> Option<String> {
        if id == "unread" && self.next_unread().is_none() {
            return Some(tr("error.no_unread"));
        }
        if id == "export" && self.export_job.is_some() {
            return Some(tr("export.busy"));
        }
        let remote = ACTIONS
            .iter()
            .find(|a| a.id == id)
            .is_some_and(|a| a.command.is_some());
        if (remote || matches!(id, "edit" | "remove" | "reconnect")) && self.console.is_none() {
            return Some(tr("error.no_console"));
        }
        if matches!(id, "ping" | "sessions") && !self.current().is_some_and(|p| p.online) {
            return Some(tr("error.offline"));
        }
        if matches!(
            id,
            "rename" | "close" | "clear" | "banner" | "search" | "export"
        ) && self.active.is_none()
        {
            return Some(tr("error.open_first"));
        }
        if (remote || id == "new")
            && (self.visible_ids().len() >= 8 || self.panes.len() + self.retiring.len() >= 32)
        {
            return Some(tr("error.panes_limit"));
        }
        None
    }
    fn perform(&mut self, id: &str) -> Result<()> {
        if let Some(command) = ACTIONS.iter().find(|a| a.id == id).and_then(|a| a.command) {
            self.new_pane()?;
            let demo = self.demo;
            let p = self.current_mut().unwrap();
            p.title = tr(&format!("action.{id}"));
            p.pending_command = Some(command.into());
            if demo {
                p.notice(&trf("demo.flash", &[id.into(), command.into()]));
            }
            self.notice = trf("hint.flash", &[id.into()]);
            return Ok(());
        }
        match id {
            "new" => {
                if self.console.is_none() {
                    self.form(false);
                } else {
                    self.new_pane()?;
                }
            }
            "console" => {
                self.overlay = Overlay::Consoles {
                    selected: self
                        .config
                        .consoles
                        .iter()
                        .position(|c| Some(c.id) == self.console)
                        .unwrap_or(0),
                }
            }
            "add" => {
                if self.config.consoles.len() >= 32 {
                    anyhow::bail!(tr("error.console_limit"));
                }
                self.form(false);
            }
            "edit" => {
                if self.console.is_none() {
                    anyhow::bail!(tr("error.select_console"));
                }
                self.form(true);
            }
            "remove" => {
                if let Some(id) = self.console {
                    self.overlay = Overlay::ConfirmRemove(id);
                }
            }
            "split" => self.split = !self.split,
            "export" => {
                if let Some(p) = self.current() {
                    let default = crate::export::Dialog {
                        pane: p.id,
                        title: p.title.clone(),
                        path: crate::input::Input::new(
                            crate::export::suggested_path(p.id)?.display().to_string(),
                            crate::export::MAX_PATH_CHARS,
                        ),
                        scope: Default::default(),
                        field: 0,
                        error: String::new(),
                    };
                    self.overlay = Overlay::Export(
                        self.export_retry
                            .take()
                            .filter(|d| d.pane == default.pane)
                            .unwrap_or(default),
                    );
                    self.sel = None;
                }
            }
            "search" => {
                if let Some(p) = self.current() {
                    self.overlay = Overlay::Search(Box::new(crate::search::Search::new(
                        p.id,
                        p.parser.screen(),
                        p.output_revision,
                    )));
                    self.sel = None;
                    self.sidebar_focus = false;
                }
            }
            "previous" => self.rotate(-1),
            "next" => self.rotate(1),
            "unread" => {
                if let Some(id) = self.next_unread() {
                    self.select(id);
                    self.sidebar_focus = false;
                }
            }
            "rename" => {
                if let Some(p) = self.current() {
                    self.overlay = Overlay::Rename(Input::new(p.title.clone(), 32));
                }
            }
            "reconnect" => {
                if self.demo {
                    self.notice = tr("status.demo");
                    return Ok(());
                }
                let c = self.current_console().cloned();
                if let (Some(c), Some(p)) = (c, self.current_mut()) {
                    p.reconnect(&c);
                } else {
                    self.new_pane()?;
                }
            }
            "ping" | "sessions" => {
                if let Some(p) = self.current_mut() {
                    p.send(if id == "ping" {
                        Command::Ping
                    } else {
                        Command::Sessions
                    })?;
                } else {
                    anyhow::bail!(tr("error.open_first"));
                }
            }
            "clear" => {
                self.sel = None;
                if let Some(p) = self.current_mut() {
                    p.parser.process(b"\x1b[2J\x1b[H");
                    p.parser.screen_mut().set_scrollback(0);
                }
            }
            "banner" => {
                self.sel = None;
                if let Some(p) = self.current_mut() {
                    p.parser.process(crate::terminal::banner().as_bytes());
                }
            }
            "help" => {
                self.overlay = Overlay::Text {
                    title: tr("action.help"),
                    text: tr("help.text"),
                    scroll: 0,
                }
            }
            "license" => {
                self.overlay = Overlay::Text {
                    title: "GNU GPLv3 · by seregonwar".into(),
                    text: include_str!("../../../LICENSE").into(),
                    scroll: 0,
                }
            }
            "close" => {
                if let Some(id) = self.active {
                    self.overlay = Overlay::ConfirmClose(id);
                }
            }
            "language" => {
                let old = self.config.language;
                let lang = if old == crate::i18n::Language::En {
                    crate::i18n::Language::It
                } else {
                    crate::i18n::Language::En
                };
                self.config.language = lang;
                if let Err(e) = self.save() {
                    self.config.language = old;
                    return Err(e);
                }
                crate::i18n::set(lang);
                self.notice = tr("hint.start");
                for p in &mut self.panes {
                    let old_title =
                        crate::i18n::translate(old, "pane.title").replace("{0}", &p.id.to_string());
                    if p.title == old_title {
                        p.title = trf("pane.title", &[p.id.to_string()]);
                    }
                    p.status = tr(if self.demo {
                        "status.demo"
                    } else if p.online {
                        "ui.active"
                    } else if p.connection.is_some() {
                        "status.connecting"
                    } else {
                        "status.disconnected"
                    });
                    if p.online {
                        p.status = trf("status.online", &[p.remote_sid.unwrap_or(0).to_string()]);
                    }
                }
            }
            "colors" => {
                let old = self.config.output_colors;
                self.config.output_colors = old.toggle();
                if let Err(error) = self.save() {
                    self.config.output_colors = old;
                    return Err(error);
                }
                self.notice = tr(match self.config.output_colors {
                    crate::colors::OutputColors::Readable => "notice.colors_readable",
                    crate::colors::OutputColors::Original => "notice.colors_original",
                });
            }
            _ => {}
        }
        Ok(())
    }
    fn close_pane(&mut self, id: u64) {
        if let Some(i) = self.panes.iter().position(|p| p.id == id) {
            if let Some(c) = self.panes[i].connection.take() {
                c.close();
                self.retiring.push(c);
            }
            self.panes.remove(i);
        }
        if self.active == Some(id) {
            self.active = self.visible_ids().first().copied();
        }
        self.remembered.retain(|_, v| *v != id);
    }
    fn rotate(&mut self, delta: isize) {
        let ids = self.visible_ids();
        if ids.is_empty() {
            return;
        }
        let n = ids
            .iter()
            .position(|id| Some(*id) == self.active)
            .unwrap_or(0);
        self.select(ids[(n as isize + delta).rem_euclid(ids.len() as isize) as usize]);
    }
    pub fn handle(&mut self, event: Event) {
        match event {
            Event::Resize(..) => self.sel = None,
            Event::Key(k) if k.kind != KeyEventKind::Release => self.key(k),
            Event::Paste(text) => {
                match &mut self.overlay {
                    Overlay::Form {
                        fields,
                        field,
                        error,
                        ..
                    } => {
                        if fields[*field].insert(&text) {
                            error.clear();
                        }
                        return;
                    }
                    Overlay::Rename(title) => {
                        title.insert(&text);
                        return;
                    }
                    Overlay::Palette { query, selected } => {
                        if query.insert(&text) {
                            *selected = 0;
                        }
                        return;
                    }
                    Overlay::Search(_) => {
                        let Overlay::Search(mut search) =
                            std::mem::replace(&mut self.overlay, Overlay::None)
                        else {
                            unreachable!()
                        };
                        if search.query.insert(&text) {
                            if !self.refresh_search_if_changed(&mut search) {
                                search.find();
                            }
                            self.focus_search_result(&search);
                        }
                        self.overlay = Overlay::Search(search);
                        return;
                    }
                    Overlay::Export(dialog) => {
                        if dialog.field == 0 {
                            dialog.append_path(&text);
                        }
                        return;
                    }
                    _ => {}
                }
                if !matches!(self.overlay, Overlay::None) {
                    self.notice = tr("hint.paste_overlay");
                } else if text.len() > crate::protocol::MAX_PAYLOAD {
                    self.notice = tr("error.paste");
                } else if text.contains(['\n', '\r']) {
                    self.overlay = Overlay::ConfirmPaste(text);
                } else if let Some(p) = self.current_mut()
                    && let Err(e) = p.paste(&text)
                {
                    self.notice = e.to_string();
                }
            }
            Event::Mouse(m) => match m.kind {
                MouseEventKind::Down(crossterm::event::MouseButton::Left) => {
                    let target = self
                        .hits
                        .iter()
                        .rev()
                        .find(|(r, _)| r.contains((m.column, m.row).into()))
                        .map(|(r, h)| (*r, h.clone()));
                    let hit_rect = target.as_ref().map(|(r, _)| *r);
                    let hit = target.map(|(_, h)| h);

                    /*
                     * A press starts a text selection only over a pane or empty
                     * space: buttons, tabs and the sidebar keep their click
                     * semantics, and a drag inside a pane stays inside it so the
                     * sidebar and the action list can never end up copied by
                     * accident.
                     */
                    let interactive = matches!(
                        hit,
                        Some(Hit::Console)
                            | Some(Hit::New)
                            | Some(Hit::Palette)
                            | Some(Hit::Close(_))
                            | Some(Hit::Action(_))
                            | Some(Hit::Choice(_))
                            | Some(Hit::Input(_))
                            | Some(Hit::Confirm(_))
                            | Some(Hit::SearchStep(_))
                            | Some(Hit::SearchRefresh)
                            | Some(Hit::SearchClose)
                            | Some(Hit::ExportField(_))
                            | Some(Hit::ExportScope(_))
                    );

                    if matches!(self.overlay, Overlay::None) && !interactive {
                        self.sel = None;

                        if let Some(Hit::Pane(id)) = hit
                            && let Some(rect) = self.pane_rect(id)
                            && rect.width > 0
                            && rect.height > 0
                            && rect.contains((m.column, m.row).into())
                        {
                            /* Anchored to the pane's content, so the wheel does
                             * not drag the selection off the text it was made
                             * on. */
                            let col = m.column.clamp(rect.x, rect.x + rect.width - 1);
                            let line = self.content_line(id, rect, m.row);

                            self.sel = Some(Selection {
                                pane: id,
                                start: (col, line),
                                end: (col, line),
                            });
                        }
                    }

                    match hit {
                        Some(Hit::Input(index)) => {
                            if let Some(rect) = hit_rect {
                                let column = m.column.saturating_sub(rect.x);
                                match &mut self.overlay {
                                    Overlay::Form { fields, field, .. } if index < fields.len() => {
                                        *field = index;
                                        fields[index].click(column, rect.width, index == 3);
                                    }
                                    Overlay::Rename(input)
                                    | Overlay::Palette { query: input, .. } => {
                                        input.click(column, rect.width, false)
                                    }
                                    Overlay::Search(search) => {
                                        search.query.click(column, rect.width, false)
                                    }
                                    _ => {}
                                }
                            }
                        }
                        Some(Hit::ExportField(field)) => {
                            if let Overlay::Export(dialog) = &mut self.overlay {
                                dialog.field = field;
                                if field == 0
                                    && let Some(rect) = hit_rect
                                {
                                    dialog.path.click(
                                        m.column.saturating_sub(rect.x),
                                        rect.width,
                                        false,
                                    );
                                }
                            }
                        }
                        Some(Hit::ExportScope(scope)) => {
                            if let Overlay::Export(dialog) = &mut self.overlay {
                                dialog.scope = scope;
                                dialog.field = 1;
                            }
                        }
                        Some(Hit::SearchStep(delta)) => {
                            self.overlay_key(KeyEvent::new(
                                if delta < 0 {
                                    KeyCode::Up
                                } else {
                                    KeyCode::Down
                                },
                                KeyModifiers::NONE,
                            ));
                        }
                        Some(Hit::SearchRefresh) => {
                            self.overlay_key(KeyEvent::new(KeyCode::F(5), KeyModifiers::NONE))
                        }
                        Some(Hit::SearchClose) => {
                            self.overlay_key(KeyEvent::new(KeyCode::Esc, KeyModifiers::NONE))
                        }
                        Some(Hit::Confirm(confirm)) => {
                            if confirm && let Overlay::Form { field, .. } = &mut self.overlay {
                                *field = 3;
                            }
                            self.overlay_key(KeyEvent::new(
                                if confirm {
                                    KeyCode::Enter
                                } else {
                                    KeyCode::Esc
                                },
                                KeyModifiers::NONE,
                            ));
                        }
                        Some(Hit::Choice(n)) => match &mut self.overlay {
                            Overlay::Consoles { selected } => {
                                *selected = n;
                                self.overlay_key(KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE));
                            }
                            Overlay::Palette { selected, .. } => {
                                *selected = n;
                                self.overlay_key(KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE));
                            }
                            Overlay::Form { field, .. } => *field = n,
                            _ => {}
                        },
                        Some(h) if matches!(self.overlay, Overlay::None) => match h {
                            Hit::Console => self.action("console"),
                            Hit::New => self.action("new"),
                            Hit::Palette => {
                                self.overlay = Overlay::Palette {
                                    query: Input::new(String::new(), 100),
                                    selected: 0,
                                }
                            }
                            Hit::Pane(id) => {
                                self.select(id);
                                self.sidebar_focus = false;
                            }
                            Hit::Close(id) => self.overlay = Overlay::ConfirmClose(id),
                            Hit::Action(id) => self.action(&id),
                            _ => {}
                        },
                        _ => {}
                    }
                }
                MouseEventKind::Drag(crossterm::event::MouseButton::Left) => {
                    if let Some(mut sel) = self.sel {
                        let pane = sel.pane;

                        if let Some(rect) = self.pane_rect(pane) {
                            let last = rect.y + rect.height.saturating_sub(1);

                            /*
                             * Dragging past the pane scrolls it, which is the
                             * only way to select text that is not on screen
                             * yet.
                             */
                            if m.row < rect.y || m.row > last {
                                let scrollback = self.pane_scrollback(pane);
                                let step = if m.row < rect.y {
                                    rect.y - m.row
                                } else {
                                    m.row - last
                                };

                                let target = if m.row < rect.y {
                                    scrollback.saturating_add(step)
                                } else {
                                    scrollback.saturating_sub(step)
                                };

                                self.set_pane_scrollback(pane, target);
                            }

                            let row = m.row.clamp(rect.y, last);
                            let col = m.column.clamp(rect.x, rect.x + rect.width - 1);

                            sel.end = (col, self.content_line(pane, rect, row));
                        }

                        self.sel = Some(sel);
                    }
                }
                MouseEventKind::Up(crossterm::event::MouseButton::Left) => {
                    if self.sel.is_some_and(|sel| !sel.is_empty()) {
                        self.copy_selection();
                    } else {
                        self.sel = None;
                    }
                }
                MouseEventKind::ScrollUp | MouseEventKind::ScrollDown => {
                    let delta = if m.kind == MouseEventKind::ScrollUp {
                        3
                    } else {
                        -3
                    };
                    match &mut self.overlay {
                        Overlay::Text { scroll, .. } => {
                            *scroll = scroll
                                .saturating_add_signed(-delta as i16)
                                .min(self.text_scroll_limit)
                        }
                        Overlay::Palette { query, selected } => {
                            *selected = selected
                                .saturating_add_signed(-delta)
                                .min(filtered(query).len().saturating_sub(1))
                        }
                        Overlay::Consoles { selected } => {
                            *selected = selected
                                .saturating_add_signed(-delta)
                                .min(self.config.consoles.len())
                        }
                        Overlay::None => {
                            let target = self.hits.iter().rev().find_map(|(rect, hit)| {
                                if rect.contains((m.column, m.row).into())
                                    && let Hit::Pane(id) = hit
                                {
                                    Some(*id)
                                } else {
                                    None
                                }
                            });
                            if let Some(p) =
                                target.and_then(|id| self.panes.iter_mut().find(|p| p.id == id))
                            {
                                p.scroll(delta);
                            }
                        }
                        _ => {}
                    }
                }
                MouseEventKind::Moved => {
                    if let Some((_, Hit::Choice(n))) = self
                        .hits
                        .iter()
                        .rev()
                        .find(|(r, _)| r.contains((m.column, m.row).into()))
                    {
                        match &mut self.overlay {
                            Overlay::Palette { selected, .. } | Overlay::Consoles { selected } => {
                                *selected = *n
                            }
                            _ => {}
                        }
                    }
                }
                _ => {}
            },
            _ => {}
        }
    }
    fn key(&mut self, k: KeyEvent) {
        /*
         * Copying is reachable from the keyboard as well as from the mouse:
         * Ctrl+Shift+C hands the selection - or the whole visible workspace -
         * to the terminal's clipboard. Ctrl+C itself stays untouched, because
         * that belongs to the session on the other side.
         */
        if k.modifiers.contains(KeyModifiers::CONTROL)
            && k.modifiers.contains(KeyModifiers::SHIFT)
            && k.code == KeyCode::Char('c')
            && matches!(self.overlay, Overlay::None)
        {
            self.copy_selection();
            return;
        }

        if !matches!(self.overlay, Overlay::None) {
            self.overlay_key(k);
            return;
        }
        if k.modifiers.contains(KeyModifiers::CONTROL) && k.code == KeyCode::Char('q') {
            self.overlay = Overlay::ConfirmQuit;
            return;
        }
        let action = match k.code {
            KeyCode::F(1) => Some("help"),
            KeyCode::F(3) => Some("console"),
            KeyCode::F(4) => Some("new"),
            KeyCode::F(5) => Some("reconnect"),
            KeyCode::F(6) => Some("split"),
            KeyCode::F(7) => Some("rename"),
            KeyCode::F(8) => Some("close"),
            KeyCode::F(11) => Some("search"),
            KeyCode::F(12) => Some("export"),
            _ => None,
        };
        if let Some(a) = action {
            self.action(a);
            return;
        }
        if k.code == KeyCode::F(2) {
            self.overlay = Overlay::Palette {
                query: Input::new(String::new(), 100),
                selected: 0,
            };
            return;
        }
        if k.code == KeyCode::F(9) {
            self.sidebar_focus = !self.sidebar_focus;
            return;
        }
        if k.code == KeyCode::F(10) {
            self.sidebar_collapsed = !self.sidebar_collapsed;
            self.sidebar_focus = false;
            return;
        }
        if k.modifiers.contains(KeyModifiers::ALT)
            && let KeyCode::Char(c @ '1'..='8') = k.code
        {
            if let Some(id) = self.visible_ids().get(c as usize - '1' as usize).copied() {
                self.select(id);
            }
            return;
        }
        if k.modifiers.contains(KeyModifiers::CONTROL)
            && matches!(k.code, KeyCode::PageUp | KeyCode::PageDown)
        {
            self.rotate(if k.code == KeyCode::PageUp { -1 } else { 1 });
            return;
        }
        if k.modifiers.contains(KeyModifiers::SHIFT)
            && matches!(k.code, KeyCode::PageUp | KeyCode::PageDown)
        {
            if let Some(p) = self.current_mut() {
                p.scroll(if k.code == KeyCode::PageUp { 12 } else { -12 });
            }
            return;
        }
        if k.code == KeyCode::End && k.modifiers.contains(KeyModifiers::CONTROL) {
            if let Some(p) = self.current_mut() {
                p.parser.screen_mut().set_scrollback(0);
            }
            return;
        }
        if self.sidebar_focus {
            match k.code {
                KeyCode::Up => self.rotate(-1),
                KeyCode::Down => self.rotate(1),
                KeyCode::Char('n') => self.action("new"),
                KeyCode::Enter => self.sidebar_focus = false,
                _ => {}
            }
            return;
        }
        if let Some(p) = self.current_mut()
            && let Err(e) = p.input(k)
        {
            self.notice = e.to_string();
        }
    }
    fn overlay_key(&mut self, k: KeyEvent) {
        if k.code == KeyCode::Esc {
            self.overlay = Overlay::None;
            return;
        }
        let overlay = std::mem::replace(&mut self.overlay, Overlay::None);
        match overlay {
            Overlay::Export(mut dialog) => {
                if dialog.field == 0
                    && let Some(changed) = dialog.path.key(k)
                {
                    if changed {
                        dialog.error.clear();
                    }
                    self.overlay = Overlay::Export(dialog);
                    return;
                }
                match k.code {
                    KeyCode::Tab | KeyCode::BackTab => dialog.field = 1 - dialog.field,
                    KeyCode::Up | KeyCode::Down | KeyCode::Left | KeyCode::Right
                        if dialog.field == 1 =>
                    {
                        dialog.scope = dialog.scope.toggle()
                    }
                    KeyCode::Char(' ') if dialog.field == 1 => dialog.scope = dialog.scope.toggle(),
                    KeyCode::Enter => {
                        if dialog.path.as_str().trim().is_empty() {
                            dialog.error = crate::export::Error::InvalidPath.message();
                        } else if self.export_job.is_some() {
                            dialog.error = tr("export.busy");
                        } else if let Some(p) = self.panes.iter().find(|p| p.id == dialog.pane) {
                            match crate::export::Job::start(
                                p.parser.screen().clone(),
                                dialog.scope,
                                dialog.path.as_str().into(),
                            ) {
                                Ok(job) => {
                                    self.export_job = Some(job);
                                    self.export_retry = Some(dialog);
                                    self.notice = tr("export.saving");
                                    return;
                                }
                                Err(error) => {
                                    dialog.error = crate::export::Error::Io(error).message()
                                }
                            }
                        } else {
                            dialog.error = tr("error.open_first");
                        }
                    }
                    _ => {}
                }
                self.overlay = Overlay::Export(dialog);
            }
            Overlay::Search(mut search) => {
                if k.code == KeyCode::F(11) {
                    return;
                }
                let list_navigation = matches!(k.code, KeyCode::Home | KeyCode::End)
                    && !k.modifiers.contains(KeyModifiers::CONTROL);
                if !list_navigation && let Some(changed) = search.query.key(k) {
                    if changed {
                        if !self.refresh_search_if_changed(&mut search) {
                            search.find();
                        }
                        self.focus_search_result(&search);
                    }
                    self.overlay = Overlay::Search(search);
                    return;
                }
                let refreshed = self.refresh_search_if_changed(&mut search);
                match k.code {
                    KeyCode::F(5) if !refreshed => {
                        if let Some(p) = self.panes.iter().find(|p| p.id == search.pane) {
                            search.refresh(p.parser.screen(), p.output_revision);
                        }
                    }
                    KeyCode::Enter | KeyCode::Down if !refreshed => {
                        search.step(if k.modifiers.contains(KeyModifiers::SHIFT) {
                            -1
                        } else {
                            1
                        })
                    }
                    KeyCode::Up if !refreshed => search.step(-1),
                    KeyCode::Home => search.selected = 0,
                    KeyCode::End => search.selected = search.matches.len().saturating_sub(1),
                    _ => {}
                }
                self.focus_search_result(&search);
                self.overlay = Overlay::Search(search);
            }
            Overlay::Consoles { mut selected } => {
                let count = self.config.consoles.len() + 1;
                match k.code {
                    KeyCode::Up => selected = (selected + count - 1) % count,
                    KeyCode::Down => selected = (selected + 1) % count,
                    KeyCode::Enter => {
                        if selected == count - 1 {
                            self.action("add");
                        } else {
                            self.switch(self.config.consoles[selected].id);
                        }
                        return;
                    }
                    KeyCode::Char('n') => {
                        self.action("add");
                        return;
                    }
                    _ => {}
                }
                self.overlay = Overlay::Consoles { selected };
            }
            Overlay::Palette {
                mut query,
                mut selected,
            } => {
                let list_navigation = matches!(k.code, KeyCode::Home | KeyCode::End)
                    && !k.modifiers.contains(KeyModifiers::CONTROL);
                if !list_navigation && let Some(changed) = query.key(k) {
                    if changed {
                        selected = 0;
                    }
                    self.overlay = Overlay::Palette { query, selected };
                    return;
                }
                let items = filtered(&query);
                match k.code {
                    KeyCode::Up => selected = selected.saturating_sub(1),
                    KeyCode::Down => selected = (selected + 1).min(items.len().saturating_sub(1)),
                    KeyCode::Home => selected = 0,
                    KeyCode::End => selected = items.len().saturating_sub(1),
                    KeyCode::PageUp => selected = selected.saturating_sub(8),
                    KeyCode::PageDown => {
                        selected = (selected + 8).min(items.len().saturating_sub(1))
                    }
                    KeyCode::Enter => {
                        if let Some(i) = items.get(selected) {
                            self.overlay = Overlay::Palette { query, selected };
                            self.action(ACTIONS[*i].id);
                        }
                        return;
                    }
                    _ => {}
                }
                self.overlay = Overlay::Palette { query, selected };
            }
            Overlay::Form {
                editing,
                mut fields,
                mut field,
                mut error,
            } => {
                if let Some(changed) = fields[field].key(k) {
                    if changed {
                        error.clear();
                    }
                    self.overlay = Overlay::Form {
                        editing,
                        fields,
                        field,
                        error,
                    };
                    return;
                }
                match k.code {
                    KeyCode::Tab | KeyCode::Down => field = (field + 1) % 4,
                    KeyCode::BackTab | KeyCode::Up => field = (field + 3) % 4,
                    KeyCode::Enter if field < 3 => field += 1,
                    KeyCode::Enter => {
                        let c = Console {
                            id: editing.unwrap_or_else(|| self.config.next_id()),
                            name: fields[0].trim().into(),
                            host: fields[1].trim().trim_matches(['[', ']']).into(),
                            port: fields[2].parse().unwrap_or(0),
                            token_env: editing.and_then(|id| {
                                self.config
                                    .consoles
                                    .iter()
                                    .find(|c| c.id == id)
                                    .and_then(|c| c.token_env.clone())
                            }),
                            token: if fields[3].is_empty() {
                                None
                            } else {
                                Some(fields[3].as_str().into())
                            },
                        };
                        match c.validate() {
                            Err(e) => error = e.to_string(),
                            Ok(()) => {
                                let id = c.id;
                                let previous = editing.and_then(|id| {
                                    self.config.consoles.iter().position(|c| c.id == id)
                                });
                                let backup = if let Some(i) = previous {
                                    Some(std::mem::replace(&mut self.config.consoles[i], c))
                                } else {
                                    self.config.consoles.push(c);
                                    None
                                };
                                if let Err(e) = self.save() {
                                    if let (Some(i), Some(old)) = (previous, backup) {
                                        self.config.consoles[i] = old;
                                    } else {
                                        self.config.consoles.pop();
                                    }
                                    error = e.to_string();
                                } else {
                                    self.switch(id);
                                    self.notice = tr("hint.saved");
                                    return;
                                }
                            }
                        }
                    }
                    _ => {}
                }
                self.overlay = Overlay::Form {
                    editing,
                    fields,
                    field,
                    error,
                };
            }
            Overlay::Rename(mut title) => {
                match k.code {
                    KeyCode::Enter => {
                        if !title.trim().is_empty()
                            && let Some(p) = self.current_mut()
                        {
                            p.title = title.trim().into();
                        }
                        return;
                    }
                    _ => {
                        title.key(k);
                    }
                }
                self.overlay = Overlay::Rename(title);
            }
            Overlay::Text {
                title,
                text,
                mut scroll,
            } => {
                match k.code {
                    KeyCode::Down => scroll = scroll.saturating_add(1),
                    KeyCode::Up => scroll = scroll.saturating_sub(1),
                    KeyCode::PageDown => scroll = scroll.saturating_add(12),
                    KeyCode::PageUp => scroll = scroll.saturating_sub(12),
                    KeyCode::Home => scroll = 0,
                    _ => {}
                }
                scroll = scroll.min(self.text_scroll_limit);
                self.overlay = Overlay::Text {
                    title,
                    text,
                    scroll,
                };
            }
            Overlay::ConfirmClose(id) => {
                if k.code == KeyCode::Enter {
                    self.close_pane(id);
                } else {
                    self.overlay = Overlay::ConfirmClose(id);
                }
            }
            Overlay::ConfirmRemove(id) => {
                if k.code == KeyCode::Enter {
                    let i = self
                        .config
                        .consoles
                        .iter()
                        .position(|c| c.id == id)
                        .unwrap();
                    let c = self.config.consoles.remove(i);
                    if let Err(e) = self.save() {
                        self.config.consoles.insert(i, c);
                        self.notice = e.to_string();
                        return;
                    }
                    let ids: Vec<_> = self
                        .panes
                        .iter()
                        .filter(|p| p.console_id == id)
                        .map(|p| p.id)
                        .collect();
                    for id in ids {
                        self.close_pane(id);
                    }
                    self.console = self.config.consoles.first().map(|c| c.id);
                    self.active = None;
                    if let Some(id) = self.console {
                        self.switch(id);
                    }
                } else {
                    self.overlay = Overlay::ConfirmRemove(id);
                }
            }
            Overlay::ConfirmQuit => {
                if k.code == KeyCode::Enter {
                    if let Some(job) = &self.export_job {
                        job.cancel();
                    }
                    for p in &self.panes {
                        if let Some(c) = &p.connection {
                            c.close();
                        }
                    }
                    self.quit = true;
                } else {
                    self.overlay = Overlay::ConfirmQuit;
                }
            }
            Overlay::ConfirmPaste(text) => {
                if k.code == KeyCode::Enter {
                    if let Some(p) = self.current_mut()
                        && let Err(e) = p.paste(&text)
                    {
                        self.notice = e.to_string();
                    }
                } else {
                    self.overlay = Overlay::ConfirmPaste(text);
                }
            }
            Overlay::None => {}
        }
    }

    pub fn export_pending(&self) -> bool {
        self.export_job.is_some()
    }

    fn refresh_search_if_changed(&self, search: &mut crate::search::Search) -> bool {
        if let Some(p) = self.panes.iter().find(|p| p.id == search.pane)
            && !search.is_current(p.parser.screen(), p.output_revision)
        {
            search.refresh(p.parser.screen(), p.output_revision);
            return true;
        }
        false
    }
    fn focus_search_result(&mut self, search: &crate::search::Search) {
        if let Some(found) = search.current()
            && let Some(p) = self.panes.iter_mut().find(|p| p.id == search.pane)
        {
            let rows = usize::from(p.parser.screen().size().0);
            p.parser
                .screen_mut()
                .set_scrollback(found.start.line.saturating_sub(rows.saturating_sub(1)));
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn hidden_output_marks_activity_once_then_drains_without_repainting() {
        let mut a = app();
        let hidden = a.new_pane().unwrap();
        a.new_pane().unwrap();
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        let pane = a.panes.iter_mut().find(|p| p.id == hidden).unwrap();
        pane.connection = Some(Connection::queued(
            (0..24)
                .map(|_| crate::protocol::Event::Output(b"background\r\n".to_vec()))
                .collect(),
        ));
        assert!(
            a.poll(),
            "first unread output must update the activity marker"
        );
        assert!(a.work_pending());
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        assert!(
            !a.poll(),
            "an already-unread hidden terminal must not redraw the workspace for each output batch"
        );
        assert!(!a.poll());
        assert!(a.work_pending());
        assert!(!a.poll());
        assert!(!a.work_pending(), "empty queues must restore idle polling");
        let pane = a.panes.iter().find(|p| p.id == hidden).unwrap();
        assert_eq!(pane.output_revision, 24, "suppress redraws, not parsing");
        assert!(pane.unread);
    }
    #[test]
    fn split_and_modal_layouts_keep_visible_output_live_and_tiny_layouts_do_not_redraw() {
        let mut a = app();
        let first = a.new_pane().unwrap();
        a.new_pane().unwrap();
        a.split = true;
        let mut terminal = Terminal::new(TestBackend::new(140, 44)).unwrap();
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        let pane = a.panes.iter_mut().find(|p| p.id == first).unwrap();
        pane.unread = true;
        pane.connection = Some(Connection::queued(
            (0..16)
                .map(|_| crate::protocol::Event::Output(b"live split output\r\n".to_vec()))
                .collect(),
        ));
        assert!(
            a.poll(),
            "an unfocused but visible pane still needs repainting"
        );
        a.action("search");
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        assert!(a.hits.iter().all(|(_, h)| !matches!(h, Hit::Pane(_))));
        assert!(
            a.poll(),
            "modal mouse targets must not hide underlying live content"
        );
        assert_eq!(
            a.panes
                .iter()
                .find(|p| p.id == first)
                .unwrap()
                .output_revision,
            16
        );
        a.overlay = Overlay::None;
        let active = a.current_mut().unwrap();
        active.connection = Some(Connection::queued(
            (0..16)
                .map(|_| crate::protocol::Event::Output(b"tiny output\r\n".to_vec()))
                .collect(),
        ));
        let mut tiny = Terminal::new(TestBackend::new(20, 8)).unwrap();
        tiny.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        assert!(!a.poll());
        assert!(!a.poll());
        assert_eq!(a.current().unwrap().output_revision, 16);
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        assert!(a.rendered_panes.contains(&a.active.unwrap()));
    }
    #[test]
    fn empty_output_does_not_notify_and_background_errors_are_discoverable() {
        let mut a = app();
        let hidden = a.new_pane().unwrap();
        a.new_pane().unwrap();
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        a.panes
            .iter_mut()
            .find(|p| p.id == hidden)
            .unwrap()
            .connection = Some(Connection::queued(vec![crate::protocol::Event::Output(
            vec![],
        )]));
        assert!(!a.poll());
        assert_eq!(a.unread_count(None), 0);
        a.panes
            .iter_mut()
            .find(|p| p.id == hidden)
            .unwrap()
            .connection = Some(Connection::queued(vec![crate::protocol::Event::Error(
            "connection failed".into(),
        )]));
        assert!(a.poll());
        assert_eq!(a.unread_count(None), 1);
        a.action("unread");
        assert_eq!(a.active, Some(hidden));
        assert_eq!(a.unread_count(None), 0);
        assert!(
            a.current()
                .unwrap()
                .parser
                .screen()
                .contents()
                .contains("connection failed")
        );
    }
    #[test]
    fn busy_poll_yields_to_input_and_drains_all_panes_in_order() {
        let mut a = app();
        for _ in 0..8 {
            a.new_pane().unwrap();
        }
        for pane in &mut a.panes {
            let mut events = Vec::new();
            for index in 0..64 {
                let mut bytes = format!("\r\nROW {index:02}\r\n").into_bytes();
                // Large ordinary output frames, interleaved with tiny frames.
                if index % 2 == 0 {
                    bytes.resize(crate::protocol::MAX_PAYLOAD, b'x');
                }
                events.push(crate::protocol::Event::Output(bytes));
            }
            events.push(crate::protocol::Event::Disconnected);
            pane.connection = Some(Connection::queued(events));
        }
        let start = std::time::Instant::now();
        assert!(a.poll());
        let elapsed = start.elapsed();
        let first_revisions: Vec<_> = a.panes.iter().map(|p| p.output_revision).collect();
        eprintln!("first poll: {elapsed:?}; per-pane frames: {first_revisions:?}");
        assert!(
            first_revisions.iter().sum::<u64>() < 8 * 64,
            "one poll consumed the complete burst before returning to input"
        );
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(8);
        let mut updates = 1;
        while a.panes.iter().any(|p| p.connection.is_some()) {
            a.handle(Event::Key(KeyEvent::new(KeyCode::F(7), KeyModifiers::NONE)));
            assert!(matches!(a.overlay, Overlay::Rename(_)));
            a.handle(Event::Key(KeyEvent::new(KeyCode::Esc, KeyModifiers::NONE)));
            a.poll();
            updates += 1;
            assert!(
                std::time::Instant::now() < deadline,
                "output failed to drain"
            );
        }
        eprintln!("complete burst: {:?}, {updates} updates", start.elapsed());
        for pane in &a.panes {
            assert_eq!(pane.output_revision, 64);
            assert!(pane.parser.screen().contents().ends_with("ROW 63"));
        }
    }
    use super::*;
    fn app() -> App {
        App::new(
            Config {
                version: 1,
                language: crate::i18n::Language::En,
                output_colors: Default::default(),
                consoles: (1..=2)
                    .map(|id| Console {
                        id,
                        name: format!("PS{id}"),
                        host: "localhost".into(),
                        port: 2323,
                        token_env: None,
                        token: None,
                    })
                    .collect(),
            },
            PathBuf::new(),
            true,
        )
    }
    #[test]
    fn a_selection_keeps_its_text_when_the_view_scrolls() {
        let mut a = app();
        let pane = a.new_pane().unwrap();

        /* Enough output that the pane has a scrollback to scroll into. */
        if let Some(p) = a.panes.iter_mut().find(|p| p.id == pane) {
            for i in 0..200 {
                p.parser.process(format!("line {i}\r\n").as_bytes());
            }
        }

        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();

        let rect = a.pane_rect(pane).unwrap();
        let row = rect.y + 2;
        let line = a.content_line(pane, rect, row);

        assert_eq!(a.screen_row_for_line(pane, rect, line), Some(row));

        /*
         * The wheel scrolls the view; the anchor is on the text, so the line is
         * the same one and it is simply drawn lower down. This is what the user
         * saw break: the selection used to stay on the screen rows instead.
         */
        a.set_pane_scrollback(pane, a.pane_scrollback(pane) + 3);

        assert_eq!(a.screen_row_for_line(pane, rect, line), Some(row + 3));
        assert_eq!(a.content_line(pane, rect, row + 3), line);
    }

    #[test]
    fn copying_multiline_unicode_is_ordered_and_leaves_the_workspace_untouched() {
        let mut a = app();
        let pane = a.new_pane().unwrap();
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        let rect = a.pane_rect(pane).unwrap();
        let p = a.current_mut().unwrap();
        let dimensions = p.dimensions;
        p.parser = vt100::Parser::new(dimensions.0, dimensions.1, crate::terminal::SCROLLBACK);
        p.output("alpha\r\nβeta\r\n界é".as_bytes());
        let hits: Vec<_> = a.hits.iter().map(|(rect, _)| *rect).collect();
        let top = a.content_line(pane, rect, rect.y);
        let bottom = a.content_line(pane, rect, rect.y + 2);
        let forward = Selection {
            pane,
            start: (rect.x + 1, top),
            end: (rect.x + 2, bottom),
        };
        let backward = Selection {
            pane,
            start: forward.end,
            end: forward.start,
        };
        for selection in [forward, backward] {
            assert_eq!(
                a.selection_text(selection).as_deref(),
                Some("lpha\nβeta\n界é")
            );
            assert_eq!(a.current().unwrap().dimensions, dimensions);
            assert_eq!(a.current().unwrap().parser.screen().scrollback(), 0);
            assert_eq!(
                a.hits.iter().map(|(rect, _)| *rect).collect::<Vec<_>>(),
                hits
            );
        }
        let wide_half = Selection {
            pane,
            start: (rect.x + 1, bottom),
            end: (rect.x + 1, bottom),
        };
        assert_eq!(a.selection_text(wide_half).as_deref(), Some("界"));
    }

    #[test]
    fn copying_history_uses_retained_rows_without_scrolling_the_live_pane() {
        let mut a = app();
        let pane = a.new_pane().unwrap();
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        let rect = a.pane_rect(pane).unwrap();
        let p = a.current_mut().unwrap();
        p.parser = vt100::Parser::new(p.dimensions.0, p.dimensions.1, crate::terminal::SCROLLBACK);
        for n in 0..40 {
            p.output(format!("line {n}\r\n").as_bytes());
        }
        let line = rect.height + 5;
        let selection = Selection {
            pane,
            start: (rect.x, line),
            end: (rect.right() - 1, line),
        };
        assert_eq!(
            a.selection_text(selection),
            Some(format!("line {}", 40 - line))
        );
        assert_eq!(a.current().unwrap().parser.screen().scrollback(), 0);
        let missing = Selection {
            start: (rect.x, 6000),
            end: (rect.right() - 1, 6000),
            ..selection
        };
        assert!(a.selection_text(missing).is_none());
    }

    #[test]
    fn copying_soft_wrapped_lines_keeps_spaces_without_inserting_a_newline() {
        let mut a = app();
        let pane = a.new_pane().unwrap();
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        let rect = a.pane_rect(pane).unwrap();
        let p = a.current_mut().unwrap();
        p.parser = vt100::Parser::new(p.dimensions.0, p.dimensions.1, crate::terminal::SCROLLBACK);
        let text = format!("{} tail", "x".repeat(usize::from(rect.width) - 1));
        p.output(text.as_bytes());
        let selection = Selection {
            pane,
            start: (rect.x, a.content_line(pane, rect, rect.y)),
            end: (rect.x + 3, a.content_line(pane, rect, rect.y + 1)),
        };
        assert_eq!(a.selection_text(selection), Some(text));
        let p = a.current_mut().unwrap();
        p.parser = vt100::Parser::new(p.dimensions.0, p.dimensions.1, crate::terminal::SCROLLBACK);
        let wide_text = format!("{}界", "x".repeat(usize::from(rect.width) - 1));
        p.output(wide_text.as_bytes());
        assert_eq!(
            a.selection_text(selection),
            Some(wide_text.replace('界', "\n界"))
        );
    }

    #[test]
    fn idle_poll_does_not_request_repainting_and_color_toggle_is_local() {
        let mut a = app();
        a.new_pane().unwrap();
        assert!(!a.poll());
        let before = a.current().unwrap().parser.screen().contents();
        let original = a.config.output_colors;
        a.action("colors");
        assert_eq!(a.config.output_colors, original.toggle());
        assert_eq!(a.current().unwrap().parser.screen().contents(), before);
        assert_eq!(a.panes.len(), 1);
    }
    #[test]
    fn demo_reconnect_stays_offline_and_failed_preferences_are_rolled_back() {
        let mut a = app();
        a.new_pane().unwrap();
        a.action("reconnect");
        assert!(a.current().unwrap().connection.is_none());
        let dir = tempfile::tempdir().unwrap();
        let blocker = dir.path().join("file");
        std::fs::write(&blocker, "not a directory").unwrap();
        a.config_path = blocker.join("profiles.json");
        a.demo = false; // saving fails before any network action
        let previous = a.config.output_colors;
        a.action("colors");
        assert_eq!(a.config.output_colors, previous);
        assert!(!a.notice.is_empty());
    }

    fn finish_export(a: &mut App) {
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(3);
        while a.export_pending() && std::time::Instant::now() < deadline {
            a.poll();
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        assert!(!a.export_pending(), "export worker did not finish");
    }

    fn export_to(a: &mut App, path: &std::path::Path) {
        a.action("export");
        a.key(KeyEvent::new(KeyCode::Char('u'), KeyModifiers::CONTROL));
        a.handle(Event::Paste(path.display().to_string()));
        a.key(KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE));
    }

    #[test]
    fn exporting_captures_one_pane_while_the_workspace_keeps_working() {
        let mut a = app();
        a.config.consoles[0].token = Some("profile-secret-not-in-export".into());
        let pane = a.new_pane().unwrap();
        let dimensions = a.current().unwrap().dimensions;
        let expected = crate::export::text(
            a.current().unwrap().parser.screen().clone(),
            crate::export::Scope::History,
        );
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("export with spaces/出力.txt");
        export_to(&mut a, &path);
        assert!(a.export_pending());
        assert!(matches!(a.overlay, Overlay::None));
        assert!(a.disabled_reason("export").is_some());
        a.current_mut()
            .unwrap()
            .output(b"\r\noutput after capture\r\n");
        a.new_pane().unwrap();
        finish_export(&mut a);
        let saved = std::fs::read_to_string(path).unwrap();
        assert_eq!(saved, expected);
        assert!(!saved.contains("profile-secret-not-in-export"));
        assert!(!saved.contains("output after capture"));
        assert_eq!(
            a.panes.iter().find(|p| p.id == pane).unwrap().dimensions,
            dimensions
        );
        assert!(a.panes.iter().all(|p| p.connection.is_none()));
        assert!(a.export_retry.is_none());
    }

    #[test]
    fn failed_exports_keep_the_path_for_retry_without_interrupting_input() {
        let mut a = app();
        a.new_pane().unwrap();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("existing.txt");
        std::fs::write(&path, "existing file").unwrap();
        export_to(&mut a, &path);
        finish_export(&mut a);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "existing file");
        assert!(matches!(a.overlay, Overlay::None));
        assert_eq!(a.notice, tr("export.exists"));
        a.action("export");
        let Overlay::Export(dialog) = &a.overlay else {
            panic!();
        };
        assert_eq!(dialog.path.as_str(), path.display().to_string());
        assert_eq!(dialog.error, tr("export.exists"));
        a.key(KeyEvent::new(KeyCode::Tab, KeyModifiers::NONE));
        a.key(KeyEvent::new(KeyCode::Down, KeyModifiers::NONE));
        let Overlay::Export(dialog) = &a.overlay else {
            panic!();
        };
        assert_eq!(dialog.scope, crate::export::Scope::Visible);
        a.key(KeyEvent::new(KeyCode::BackTab, KeyModifiers::SHIFT));
        a.key(KeyEvent::new(KeyCode::Char('u'), KeyModifiers::CONTROL));
        let corrected = dir.path().join("new.txt");
        a.handle(Event::Paste(corrected.display().to_string()));
        a.key(KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE));
        finish_export(&mut a);
        assert!(corrected.is_file());
    }

    #[test]
    fn quitting_cancels_pending_exports_and_tracks_worker_shutdown() {
        let mut a = app();
        a.new_pane().unwrap();
        let (release, wait) = std::sync::mpsc::channel();
        a.export_job = Some(
            crate::export::Job::spawn(move |cancelled| {
                wait.recv().unwrap();
                assert!(cancelled.load(std::sync::atomic::Ordering::Acquire));
                Err(crate::export::Error::Cancelled)
            })
            .unwrap(),
        );
        assert!(!a.poll());
        assert!(!a.connections_finished());
        a.key(KeyEvent::new(KeyCode::Char('q'), KeyModifiers::CONTROL));
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| crate::ui::draw(f, &mut a)).unwrap();
        let screen: String = terminal
            .backend()
            .buffer()
            .content
            .iter()
            .map(|cell| cell.symbol())
            .collect();
        assert!(screen.contains("progress"));
        a.key(KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE));
        assert!(a.quit);
        assert!(!a.connections_finished());
        release.send(()).unwrap();
        finish_export(&mut a);
        assert!(a.connections_finished());
        assert_eq!(a.notice, tr("export.cancelled"));
    }

    #[test]
    fn switching_preserves_sessions_and_selection() {
        let mut a = app();
        let first = a.new_pane().unwrap();
        let second = a.new_pane().unwrap();
        a.switch(2);
        let third = a.new_pane().unwrap();
        a.switch(1);
        assert_eq!(a.active, Some(second));
        a.select(first);
        a.switch(2);
        assert_eq!(a.active, Some(third));
        assert_eq!(a.panes.len(), 3);
    }
    #[test]
    fn flashes_do_not_type_into_existing_terminal() {
        let mut a = app();
        let first = a.new_pane().unwrap();
        a.action("pwd");
        assert_eq!(a.panes.len(), 2);
        assert_ne!(a.active, Some(first));
        assert_eq!(a.current().unwrap().pending_command.as_deref(), Some("pwd"));
        a.handle(Event::Key(KeyEvent::new(KeyCode::F(8), KeyModifiers::NONE)));
        assert_eq!(a.panes.len(), 2);
        a.handle(Event::Key(KeyEvent::new(KeyCode::Esc, KeyModifiers::NONE)));
        assert_eq!(a.panes.len(), 2);
    }
    #[test]
    fn multiline_paste_and_quit_require_explicit_confirmation() {
        let mut a = app();
        a.new_pane().unwrap();
        a.handle(Event::Paste("pwd\nls".into()));
        assert!(matches!(a.overlay, Overlay::ConfirmPaste(_)));
        a.key(KeyEvent::new(KeyCode::Esc, KeyModifiers::NONE));
        a.key(KeyEvent::new(KeyCode::Char('q'), KeyModifiers::CONTROL));
        assert!(!a.quit);
        a.key(KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE));
        assert!(a.quit);
    }
}
