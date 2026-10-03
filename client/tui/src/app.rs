use crate::i18n::{tr, trf};
use crate::{
    config::{Config, Console},
    flash::{ACTIONS, filtered},
    protocol::Command,
    terminal::Pane,
};
use anyhow::Result;
use crossterm::event::{Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseEventKind};
use ratatui::layout::Rect;
use std::{collections::HashMap, path::PathBuf};

pub enum Overlay {
    None,
    Consoles {
        selected: usize,
    },
    Palette {
        query: String,
        selected: usize,
    },
    Form {
        editing: Option<u64>,
        fields: [String; 4],
        field: usize,
        error: String,
    },
    Rename(String),
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
    Confirm(bool),
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
    pub text_scroll_limit: u16,
    next_pane: u64,
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
            text_scroll_limit: 0,
            next_pane: 1,
        }
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
    pub fn select(&mut self, id: u64) {
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
        if self.visible_ids().len() >= 8 || self.panes.len() >= 32 {
            anyhow::bail!(tr("error.panes_limit"));
        }
        let id = self.next_pane;
        self.next_pane += 1;
        self.panes.push(Pane::new(id, &c, self.demo));
        self.select(id);
        self.sidebar_focus = false;
        Ok(id)
    }
    pub fn poll(&mut self) {
        let active = self.active;
        for p in &mut self.panes {
            p.poll(Some(p.id) == active);
        }
        if matches!(self.overlay, Overlay::None)
            && let Some(text) = self.current_mut().and_then(|p| p.report.take())
        {
            self.overlay = Overlay::Text {
                title: tr("action.sessions"),
                text,
                scroll: 0,
            };
        }
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
                c.as_ref().map(|c| c.name.clone()).unwrap_or_default(),
                c.as_ref().map(|c| c.host.clone()).unwrap_or_default(),
                c.as_ref()
                    .map(|c| c.port.to_string())
                    .unwrap_or("2323".into()),
                c.and_then(|c| c.token).unwrap_or_default(),
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
        if matches!(id, "rename" | "close" | "clear" | "banner") && self.active.is_none() {
            return Some(tr("error.open_first"));
        }
        if (remote || id == "new") && (self.visible_ids().len() >= 8 || self.panes.len() >= 32) {
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
            "previous" => self.rotate(-1),
            "next" => self.rotate(1),
            "rename" => {
                if let Some(p) = self.current() {
                    self.overlay = Overlay::Rename(p.title.clone());
                }
            }
            "reconnect" => {
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
                if let Some(p) = self.current_mut() {
                    p.parser.process(b"\x1b[2J\x1b[H");
                    p.parser.screen_mut().set_scrollback(0);
                }
            }
            "banner" => {
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
            _ => {}
        }
        Ok(())
    }
    fn close_pane(&mut self, id: u64) {
        if let Some(i) = self.panes.iter().position(|p| p.id == id) {
            if let Some(c) = &self.panes[i].connection {
                c.close();
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
            Event::Key(k) if k.kind != KeyEventKind::Release => self.key(k),
            Event::Paste(text) => {
                match &mut self.overlay {
                    Overlay::Form { fields, field, .. } => {
                        let target = &mut fields[*field];
                        for c in text.chars().filter(|c| !c.is_control()) {
                            if target.len() + c.len_utf8() > 255 {
                                break;
                            }
                            target.push(c);
                        }
                        return;
                    }
                    Overlay::Rename(title) => {
                        title.extend(
                            text.chars()
                                .filter(|c| !c.is_control())
                                .take(32usize.saturating_sub(title.chars().count())),
                        );
                        return;
                    }
                    Overlay::Palette { query, selected } => {
                        query.extend(
                            text.chars()
                                .filter(|c| !c.is_control())
                                .take(100usize.saturating_sub(query.chars().count())),
                        );
                        *selected = 0;
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
                    let hit = self
                        .hits
                        .iter()
                        .rev()
                        .find(|(r, _)| r.contains((m.column, m.row).into()))
                        .map(|(_, h)| h.clone());
                    match hit {
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
                                    query: String::new(),
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
                            if let Some(p) = self.current_mut() {
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
            _ => None,
        };
        if let Some(a) = action {
            self.action(a);
            return;
        }
        if k.code == KeyCode::F(2) {
            self.overlay = Overlay::Palette {
                query: String::new(),
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
                    KeyCode::Backspace => {
                        query.pop();
                        selected = 0;
                    }
                    KeyCode::Char(c)
                        if !k.modifiers.contains(KeyModifiers::CONTROL) && query.len() < 100 =>
                    {
                        query.push(c);
                        selected = 0;
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
                match k.code {
                    KeyCode::Tab | KeyCode::Down => field = (field + 1) % 4,
                    KeyCode::BackTab | KeyCode::Up => field = (field + 3) % 4,
                    KeyCode::Backspace => {
                        fields[field].pop();
                    }
                    KeyCode::Char(c)
                        if !k.modifiers.contains(KeyModifiers::CONTROL)
                            && fields[field].len() < 255 =>
                    {
                        fields[field].push(c)
                    }
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
                                Some(fields[3].clone())
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
                    KeyCode::Backspace => {
                        title.pop();
                    }
                    KeyCode::Char(c) if !c.is_control() && title.chars().count() < 32 => {
                        title.push(c)
                    }
                    _ => {}
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
}

#[cfg(test)]
mod tests {
    use super::*;
    fn app() -> App {
        App::new(
            Config {
                version: 1,
                language: crate::i18n::Language::En,
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
