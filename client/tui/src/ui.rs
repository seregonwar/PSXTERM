use crate::{
    app::{App, Hit, Overlay},
    flash::{ACTIONS, filtered},
    i18n::{tr, trf},
    terminal::BANNER,
};
use ratatui::{
    Frame,
    buffer::Buffer,
    layout::{Constraint, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, BorderType, Borders, Clear, Paragraph, Widget, Wrap},
};
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

pub const BG: Color = Color::Rgb(20, 22, 26);
pub const PANEL: Color = Color::Rgb(24, 27, 31);
pub const BORDER: Color = Color::Rgb(57, 62, 70);
pub const FG: Color = Color::Rgb(220, 222, 226);
pub const MUTED: Color = Color::Rgb(145, 151, 160);

fn block(title: impl Into<Line<'static>>, active: bool) -> Block<'static> {
    Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .title(title)
        .title_style(Style::default().fg(if active { FG } else { MUTED }))
        .border_style(Style::default().fg(if active { FG } else { BORDER }))
        .style(Style::default().bg(PANEL).fg(FG))
}
fn paragraph(
    f: &mut Frame,
    area: Rect,
    text: impl Into<ratatui::text::Text<'static>>,
    color: Color,
) {
    f.render_widget(Paragraph::new(text).style(Style::default().fg(color)), area);
}
fn hit(app: &mut App, area: Rect, h: Hit) {
    if area.width > 0 && area.height > 0 {
        app.hits.push((area, h));
    }
}
fn row(area: Rect, y: u16) -> Rect {
    Rect::new(
        area.x,
        area.y.saturating_add(y),
        area.width,
        1.min(area.height.saturating_sub(y)),
    )
}
fn fit(text: &str, width: u16) -> String {
    if text.width() <= width as usize {
        return text.into();
    }
    if width == 0 {
        return String::new();
    }
    let mut result = String::new();
    let mut used = 0;
    for c in text.chars() {
        let w = c.width().unwrap_or(0);
        if used + w > width as usize - 1 {
            break;
        }
        result.push(c);
        used += w;
    }
    result.push('…');
    result
}
fn fit_tail(text: &str, width: u16) -> String {
    if text.width() <= width as usize {
        return text.into();
    }
    if width == 0 {
        return String::new();
    }
    let start = text
        .char_indices()
        .find(|(n, _)| text[*n..].width() < width as usize)
        .map(|(n, _)| n)
        .unwrap_or(text.len());
    format!("…{}", &text[start..])
}

pub fn draw(f: &mut Frame, app: &mut App) {
    let area = f.area();
    app.hits.clear();
    app.screen = (area.width, area.height);
    f.render_widget(Block::default().style(Style::default().bg(BG).fg(FG)), area);
    if area.width < 55 || area.height < 18 {
        f.render_widget(
            Paragraph::new(tr("ui.tiny"))
                .wrap(Wrap { trim: false })
                .style(Style::default().fg(FG))
                .block(block(" PSXTERM ", false)),
            area,
        );
        overlay(f, app);
        return;
    }
    let vertical = Layout::vertical([Constraint::Min(1), Constraint::Length(2)]).split(area);
    let rail = area.width < 72 || app.sidebar_collapsed;
    let side_width = if rail {
        4
    } else if area.width >= 110 {
        29
    } else {
        24
    };
    let columns =
        Layout::horizontal([Constraint::Length(side_width), Constraint::Min(1)]).split(vertical[0]);
    if rail {
        sidebar_rail(f, app, columns[0]);
    } else {
        sidebar(f, app, columns[0]);
    }
    workspace(f, app, columns[1]);
    paragraph(
        f,
        row(vertical[1], 0),
        if area.width >= 100 {
            tr("ui.footer")
        } else {
            tr("ui.compact")
        },
        FG,
    );
    paragraph(
        f,
        row(vertical[1], 1),
        if app.notice == tr("hint.start") && app.current().is_some() {
            if rail {
                tr("ui.hidden_sidebar")
            } else {
                String::new()
            }
        } else {
            app.notice.clone()
        },
        MUTED,
    );
    if rail && app.sidebar_focus && matches!(app.overlay, Overlay::None) {
        let drawer = Rect::new(area.x, area.y, 29.min(area.width), vertical[0].height);
        f.render_widget(Clear, drawer);
        app.hits.clear();
        sidebar(f, app, drawer);
    }
    highlight_selection(f, app);
    overlay(f, app);
}

/// Paint the current text selection in reverse video.
///
/// The selection lives in the pane's content coordinates, so each line is
/// mapped through the pane's current scroll offset: scrolling the view moves
/// the highlight with the text instead of leaving it behind.
fn highlight_selection(f: &mut Frame, app: &App) {
    let Some(sel) = app.sel else {
        return;
    };
    let Some(rect) = app.pane_rect(sel.pane) else {
        return;
    };

    let area = f.area();
    let ((c0, l0), (c1, l1)) = sel.bounds();
    let buffer = f.buffer_mut();

    for line in l0..=l1 {
        let Some(row) = app.screen_row_for_line(sel.pane, rect, line) else {
            continue;
        };

        if row >= area.height {
            continue;
        }

        let start = if line == l0 { c0 } else { rect.x };
        let end = if line == l1 {
            c1
        } else {
            rect.x + rect.width.saturating_sub(1)
        };

        for x in start..=end.min(area.width.saturating_sub(1)) {
            buffer[(x, row)].modifier |= Modifier::REVERSED;
        }
    }
}

fn sidebar(f: &mut Frame, app: &mut App, area: Rect) {
    let b = block(" PSXTERM ", app.sidebar_focus);
    let inner = b.inner(area);
    f.render_widget(b, area);
    let parts = Layout::vertical([
        Constraint::Length(3),
        Constraint::Length(1),
        Constraint::Length(9),
        Constraint::Min(0),
        Constraint::Length(1),
    ])
    .split(inner);
    let name = app
        .current_console()
        .map(|c| c.name.clone())
        .unwrap_or_else(|| tr("action.add"));
    let address = app
        .current_console()
        .map(|c| c.address())
        .unwrap_or_default();
    paragraph(f, row(parts[0], 0), tr("ui.console_label"), MUTED);
    f.render_widget(
        Paragraph::new(format!(
            "{} ▾",
            fit(&name, parts[0].width.saturating_sub(2))
        ))
        .style(Style::default().fg(FG).bg(BORDER).bold()),
        row(parts[0], 1),
    );
    paragraph(f, row(parts[0], 2), fit(&address, parts[0].width), MUTED);
    hit(app, parts[0], Hit::Console);
    paragraph(f, row(parts[2], 0), tr("ui.actions"), MUTED);
    for (y, id) in [
        (1, "palette"),
        (3, "files"),
        (4, "system"),
        (6, "help"),
        (7, "license"),
    ] {
        let r = row(parts[2], y);
        let label = if id == "palette" {
            tr("ui.palette")
        } else {
            tr(&format!("action.{id}"))
        };
        paragraph(f, r, fit(&label, r.width), FG);
        hit(
            app,
            r,
            if id == "palette" {
                Hit::Palette
            } else {
                Hit::Action(id.into())
            },
        );
    }
    // Connection state belongs to the terminal footer; transport details live here.
    if let Some(p) = app.current().filter(|p| p.online) {
        let backend = if p.caps & 1 != 0 {
            "PTY · xterm-256color"
        } else if p.caps & 2 != 0 {
            "PipeTTY · xterm-256color"
        } else {
            "PTTY/1 · xterm-256color"
        };
        paragraph(f, parts[4], fit(backend, parts[4].width), MUTED);
    }
}
fn sidebar_rail(f: &mut Frame, app: &mut App, area: Rect) {
    let b = block("", false);
    let inner = b.inner(area);
    f.render_widget(b, area);
    for (y, label, h) in [
        (0, "C", Hit::Console),
        (2, "+", Hit::New),
        (4, "F", Hit::Palette),
    ] {
        f.render_widget(
            Paragraph::new(label).style(Style::default().fg(FG).bg(BORDER)),
            row(inner, y),
        );
        hit(app, row(inner, y), h);
    }
}

fn workspace(f: &mut Frame, app: &mut App, area: Rect) {
    let compact = area.height < 30;
    let parts = Layout::vertical([
        Constraint::Length(1),
        Constraint::Length(if compact || app.split { 1 } else { 3 }),
        Constraint::Min(1),
    ])
    .split(area);
    let name = app
        .current_console()
        .map(|c| c.name.clone())
        .unwrap_or_else(|| tr("app.title"));
    let mode = tr(if app.split { "ui.split" } else { "ui.tabs" });
    let header = if f.area().width < 72 || app.sidebar_collapsed {
        format!("  {name} · {mode}")
    } else {
        format!("  {mode}")
    };
    paragraph(
        f,
        parts[0],
        format!("{header}{}", if app.demo { " · DEMO" } else { "" }),
        MUTED,
    );
    let ids = app.visible_ids();
    let active = ids
        .iter()
        .position(|id| Some(*id) == app.active)
        .unwrap_or(0);
    if app.split {
        let tools = Layout::horizontal([
            Constraint::Length(3),
            Constraint::Length(3),
            Constraint::Min(1),
            Constraint::Length(5),
        ])
        .split(parts[1]);
        for (r, label, action) in [(tools[0], " ‹ ", "previous"), (tools[1], " › ", "next")] {
            paragraph(f, r, label.to_owned(), FG);
            hit(app, r, Hit::Action(action.into()));
        }
        paragraph(
            f,
            tools[2],
            trf(
                "ui.responsive",
                &[
                    (active + usize::from(!ids.is_empty())).to_string(),
                    ids.len().to_string(),
                ],
            ),
            MUTED,
        );
        paragraph(f, tools[3], " + ".to_owned(), FG);
        hit(app, tools[3], Hit::New);
    } else {
        let slots = ((area.width.saturating_sub(10)) / 18).max(1) as usize;
        let start = active / slots * slots;
        let end = (start + slots).min(ids.len());
        let mut tab_constraints = vec![];
        if start > 0 {
            tab_constraints.push(Constraint::Length(3));
        }
        tab_constraints.extend((start..end).map(|_| Constraint::Length(18)));
        if end < ids.len() {
            tab_constraints.push(Constraint::Length(3));
        }
        tab_constraints.push(Constraint::Length(if compact { 3 } else { 5 }));
        let tab_regions = Layout::horizontal(tab_constraints).split(parts[1]);
        let mut index = 0;
        if start > 0 {
            paragraph(f, tab_regions[0], " ‹ ".to_owned(), FG);
            hit(app, tab_regions[0], Hit::Action("previous".into()));
            index += 1;
        }
        for id in ids.iter().take(end).skip(start) {
            let p = app.panes.iter().find(|p| p.id == *id).unwrap();
            let active = Some(*id) == app.active;
            let r = tab_regions[index];
            let b = block("", active);
            let tab_inner = if compact { r } else { b.inner(r) };
            let inner_width = tab_inner.width.saturating_sub(3);
            let label = format!(
                "{}{}",
                fit(
                    &p.title,
                    inner_width.saturating_sub(if p.unread { 2 } else { 0 })
                ),
                if p.unread { " •" } else { "" }
            );
            let widget = Paragraph::new(label).style(
                Style::default()
                    .fg(if active { FG } else { MUTED })
                    .bg(if active { BORDER } else { PANEL }),
            );
            f.render_widget(if compact { widget } else { widget.block(b) }, r);
            hit(app, r, Hit::Pane(*id));
            let close = Rect::new(
                tab_inner.right().saturating_sub(3),
                tab_inner.y,
                3.min(tab_inner.width),
                1.min(tab_inner.height),
            );
            paragraph(f, close, " × ".to_owned(), MUTED);
            hit(app, close, Hit::Close(*id));
            index += 1;
        }
        if end < ids.len() {
            paragraph(f, tab_regions[index], " › ".to_owned(), FG);
            hit(app, tab_regions[index], Hit::Action("next".into()));
        }
        if let Some(r) = tab_regions.last() {
            let widget = Paragraph::new(" + ").style(Style::default().fg(FG).bg(BORDER));
            f.render_widget(
                if compact {
                    widget
                } else {
                    widget.block(block("", false))
                },
                *r,
            );
            hit(app, *r, Hit::New);
        }
    }
    if ids.is_empty() {
        welcome(f, parts[2]);
        return;
    }
    let capacity = split_capacity(parts[2]);
    let visible: Vec<u64> = if app.split {
        ids.iter()
            .skip(active / capacity * capacity)
            .take(capacity)
            .copied()
            .collect()
    } else {
        vec![ids[active]]
    };
    let regions = pane_regions(parts[2], visible.len());
    let allow_cursor = matches!(app.overlay, Overlay::None) && !app.sidebar_focus;
    for (id, r) in visible.iter().zip(regions) {
        let is_active = Some(*id) == app.active;
        let p = app.panes.iter_mut().find(|p| p.id == *id).unwrap();
        let scroll = p.parser.screen().scrollback();
        let title = if app.split {
            format!(
                " {}{} ",
                fit(
                    &p.title,
                    r.width.saturating_sub(if p.unread { 9 } else { 7 })
                ),
                if p.unread { " •" } else { "" }
            )
        } else {
            String::new()
        };
        let bottom = if scroll > 0 {
            trf("ui.scroll", &[scroll.to_string()])
        } else if app.demo {
            String::new()
        } else {
            p.status.clone()
        };
        let b = block(title, is_active).title_bottom(Line::from(Span::styled(
            format!(" {bottom} "),
            Style::default().fg(if scroll > 0 { FG } else { MUTED }),
        )));
        let inner = b.inner(r);
        f.render_widget(b, r);
        p.resize(inner.height, inner.width);
        f.render_widget(ScreenWidget(p.parser.screen()), inner);
        if is_active
            && allow_cursor
            && scroll == 0
            && !p.parser.screen().hide_cursor()
            && (p.online || app.demo)
        {
            let (y, x) = p.parser.screen().cursor_position();
            if x < inner.width && y < inner.height {
                f.set_cursor_position((inner.x + x, inner.y + y));
            }
        }
        hit(app, r, Hit::Pane(*id));
        if app.split {
            let close = Rect::new(r.right().saturating_sub(4), r.y, 3.min(r.width), 1);
            paragraph(f, close, " × ".to_owned(), MUTED);
            hit(app, close, Hit::Close(*id));
        }
    }
}
fn pane_regions(area: Rect, count: usize) -> Vec<Rect> {
    if count == 1 {
        return vec![area];
    }
    if count == 2 {
        if area.width < 88 {
            return Layout::vertical([Constraint::Percentage(50), Constraint::Percentage(50)])
                .split(area)
                .to_vec();
        }
        return Layout::horizontal([Constraint::Percentage(50), Constraint::Percentage(50)])
            .split(area)
            .to_vec();
    }
    let rows =
        Layout::vertical([Constraint::Percentage(50), Constraint::Percentage(50)]).split(area);
    let mut result = Vec::new();
    for r in rows.iter() {
        result.extend(
            Layout::horizontal([Constraint::Percentage(50), Constraint::Percentage(50)])
                .split(*r)
                .iter()
                .copied(),
        );
    }
    result.truncate(count);
    result
}
fn split_capacity(area: Rect) -> usize {
    match (area.width >= 88, area.height >= 26) {
        (true, true) => 4,
        (true, false) | (false, true) => 2,
        _ => 1,
    }
}
fn welcome(f: &mut Frame, area: Rect) {
    let b = block(" PSXTERM ", false);
    let inner = b.inner(area);
    f.render_widget(b, area);
    let mut lines = vec![Line::from("")];
    for (i, text) in BANNER.iter().enumerate() {
        lines.push(Line::from(Span::styled(
            *text,
            Style::default().fg(Color::Rgb(
                101 + i as u8 * 23,
                214 - i as u8 * 15,
                232 + i as u8 * 5,
            )),
        )));
    }
    lines.extend([
        Line::from(Span::styled(tr("brand.credit"), Style::default().fg(FG))),
        Line::from(tr("brand.license")),
        Line::from(""),
        Line::from(Span::styled(
            tr("ui.welcome"),
            Style::default().fg(FG).bold(),
        )),
        Line::from(""),
    ]);
    lines.extend(
        tr("ui.welcome_detail")
            .lines()
            .map(|l| Line::from(l.to_owned())),
    );
    lines.extend([
        Line::from(""),
        Line::from(Span::styled(tr("ui.port_note"), Style::default().fg(FG))),
    ]);
    f.render_widget(
        Paragraph::new(lines)
            .wrap(Wrap { trim: false })
            .style(Style::default().fg(FG)),
        inner,
    );
}

pub struct ScreenWidget<'a>(pub &'a vt100::Screen);
fn color(c: vt100::Color, default: Color) -> Color {
    match c {
        vt100::Color::Default => default,
        vt100::Color::Idx(i) => Color::Indexed(i),
        vt100::Color::Rgb(r, g, b) => Color::Rgb(r, g, b),
    }
}
impl Widget for ScreenWidget<'_> {
    fn render(self, area: Rect, buffer: &mut Buffer) {
        for y in 0..area.height {
            for x in 0..area.width {
                if let Some(c) = self.0.cell(y, x) {
                    if c.is_wide_continuation() {
                        continue;
                    }
                    let mut style = Style::default()
                        .fg(color(c.fgcolor(), FG))
                        .bg(color(c.bgcolor(), PANEL));
                    for (on, modifier) in [
                        (c.bold(), Modifier::BOLD),
                        (c.dim(), Modifier::DIM),
                        (c.italic(), Modifier::ITALIC),
                        (c.underline(), Modifier::UNDERLINED),
                        (c.inverse(), Modifier::REVERSED),
                    ] {
                        if on {
                            style = style.add_modifier(modifier);
                        }
                    }
                    buffer[(area.x + x, area.y + y)]
                        .set_symbol(if c.has_contents() { c.contents() } else { " " })
                        .set_style(style);
                }
            }
        }
    }
}
fn modal(f: &mut Frame, width: u16, height: u16, title: String) -> Rect {
    let a = f.area();
    let w = width.min(a.width);
    let h = height.min(a.height);
    let r = Rect::new(a.x + (a.width - w) / 2, a.y + (a.height - h) / 2, w, h);
    f.render_widget(Clear, r);
    let b = block(format!(" {title} "), true);
    let inner = b.inner(r);
    f.render_widget(b, r);
    inner
}
fn confirm_buttons(f: &mut Frame, inner: Rect, hits: &mut Vec<(Rect, Hit)>, save: bool) {
    let rr = row(inner, inner.height.saturating_sub(1));
    let buttons =
        Layout::horizontal([Constraint::Percentage(50), Constraint::Percentage(50)]).split(rr);
    let yes = format!(
        " {}  Enter ",
        tr(if save {
            "button.save"
        } else {
            "button.confirm"
        })
    );
    let no = format!(" {}  Esc ", tr("button.cancel"));
    for (r, label, confirm) in [(buttons[0], yes, true), (buttons[1], no, false)] {
        f.render_widget(
            Paragraph::new(label).style(
                Style::default()
                    .fg(if confirm { FG } else { MUTED })
                    .bg(BORDER),
            ),
            r,
        );
        if r.width > 0 && r.height > 0 {
            hits.push((r, Hit::Confirm(confirm)));
        }
    }
}
fn overlay(f: &mut Frame, app: &mut App) {
    if matches!(app.overlay, Overlay::None) {
        return;
    }
    app.hits.clear();
    match &app.overlay {
        Overlay::Consoles { selected } => {
            let a = f.area();
            let offset = 3.min(a.height.saturating_sub(3));
            let h = (app.config.consoles.len() as u16 + 4).min(a.height.saturating_sub(offset + 1));
            let r = Rect::new(
                a.x + 1.min(a.width),
                a.y + offset,
                52.min(a.width.saturating_sub(2)),
                h,
            );
            f.render_widget(Clear, r);
            let b = block(format!(" {} ", tr("ui.choose")), true);
            let inner = b.inner(r);
            f.render_widget(b, r);
            let avail = inner.height.saturating_sub(1) as usize;
            let start = selected.saturating_sub(avail.saturating_sub(1));
            let mut choices = app
                .config
                .consoles
                .iter()
                .map(|c| format!("{}  ·  {}", c.name, c.address()))
                .collect::<Vec<_>>();
            choices.push(tr("ui.add"));
            for (n, label) in choices.iter().enumerate().skip(start).take(avail) {
                let rr = row(inner, (n - start) as u16);
                f.render_widget(
                    Paragraph::new(format!(
                        "{} {label}",
                        if n == *selected { "›" } else { " " }
                    ))
                    .style(Style::default().fg(FG).bg(if n == *selected {
                        BORDER
                    } else {
                        PANEL
                    })),
                    rr,
                );
                if rr.height > 0 {
                    app.hits.push((rr, Hit::Choice(n)));
                }
            }
            paragraph(
                f,
                row(inner, inner.height.saturating_sub(1)),
                tr("ui.modal_hint"),
                MUTED,
            );
        }
        Overlay::Palette { query, selected } => {
            let inner = modal(f, 80, 24, tr("ui.palette"));
            paragraph(
                f,
                row(inner, 0),
                format!(
                    "› {}",
                    if query.is_empty() {
                        tr("ui.search")
                    } else {
                        query.clone()
                    }
                ),
                FG,
            );
            let items = filtered(query);
            let avail = inner.height.saturating_sub(7) as usize;
            let start = selected.saturating_sub(avail.saturating_sub(1));
            for (n, i) in items.iter().enumerate().skip(start).take(avail) {
                let action = &ACTIONS[*i];
                let disabled = app.disabled_reason(action.id).is_some();
                let rr = row(inner, (n - start + 2) as u16);
                let line = Line::from(vec![Span::styled(
                    format!(
                        "{} {}",
                        if n == *selected { "›" } else { " " },
                        fit(&tr(action.title), rr.width.saturating_sub(8))
                    ),
                    Style::default().fg(if disabled { MUTED } else { FG }),
                )]);
                f.render_widget(
                    Paragraph::new(line).style(Style::default().bg(if n == *selected {
                        BORDER
                    } else {
                        PANEL
                    })),
                    rr,
                );
                let label = if disabled { "—" } else { action.shortcut() };
                paragraph(
                    f,
                    Rect::new(
                        rr.right().saturating_sub(5),
                        rr.y,
                        5.min(rr.width),
                        rr.height,
                    ),
                    label.to_owned(),
                    MUTED,
                );
                if rr.height > 0 {
                    app.hits.push((rr, Hit::Choice(n)));
                }
            }
            if items.is_empty() {
                paragraph(f, row(inner, 2), tr("ui.no_matches"), FG);
            }
            if let Some(i) = items.get(*selected) {
                paragraph(
                    f,
                    row(inner, inner.height.saturating_sub(5)),
                    format!("{}  ·  {}", tr("ui.detail"), tr(ACTIONS[*i].category())),
                    MUTED,
                );
                f.render_widget(
                    Paragraph::new(
                        app.disabled_reason(ACTIONS[*i].id)
                            .unwrap_or_else(|| tr(ACTIONS[*i].description)),
                    )
                    .wrap(Wrap { trim: false })
                    .style(Style::default().fg(FG)),
                    Rect::new(
                        inner.x,
                        inner.y + inner.height.saturating_sub(4),
                        inner.width,
                        3.min(inner.height),
                    ),
                );
            }
            paragraph(
                f,
                row(inner, inner.height.saturating_sub(1)),
                tr("ui.modal_hint"),
                MUTED,
            );
        }
        Overlay::Form {
            editing,
            fields,
            field,
            error,
        } => {
            let inner = modal(
                f,
                68,
                21,
                tr(if editing.is_some() {
                    "action.edit"
                } else {
                    "action.add"
                }),
            );
            for (i, key) in ["form.name", "form.host", "form.port", "form.token"]
                .iter()
                .enumerate()
            {
                paragraph(f, row(inner, (i * 3) as u16), tr(key), MUTED);
                let value = if i == 3 {
                    "•".repeat(fields[i].chars().count())
                } else {
                    fields[i].clone()
                };
                let rr = row(inner, (i * 3 + 1) as u16);
                let value = fit_tail(&value, rr.width.saturating_sub(3));
                f.render_widget(
                    Paragraph::new(format!(
                        "{} {value}{}",
                        if i == *field { "›" } else { " " },
                        if i == *field { "▏" } else { "" }
                    ))
                    .style(Style::default().fg(FG).bg(BORDER)),
                    rr,
                );
                if rr.height > 0 {
                    app.hits.push((rr, Hit::Choice(i)));
                }
            }
            paragraph(f, row(inner, 12), tr("form.token_hint"), MUTED);
            let compact = inner.height < 19;
            if compact {
                paragraph(
                    f,
                    row(inner, 13),
                    if error.is_empty() {
                        tr("ui.port_note")
                    } else {
                        error.clone()
                    },
                    FG,
                );
            } else {
                paragraph(f, row(inner, 13), tr("ui.port_note"), FG);
                paragraph(f, row(inner, 14), tr("form.edit_note"), MUTED);
                paragraph(f, row(inner, 16), error.clone(), FG);
            }
            paragraph(
                f,
                row(inner, inner.height.saturating_sub(2)),
                tr("form.navigation"),
                MUTED,
            );
            confirm_buttons(f, inner, &mut app.hits, true);
        }
        Overlay::Rename(text) => {
            let inner = modal(f, 55, 7, tr("action.rename"));
            paragraph(f, row(inner, 1), format!("› {text}▏"), FG);
            confirm_buttons(f, inner, &mut app.hits, true);
        }
        Overlay::Text {
            title,
            text,
            scroll,
        } => {
            let height = f.area().height.saturating_sub(4);
            let inner = modal(f, 90, height, title.clone());
            let body = Rect::new(
                inner.x,
                inner.y,
                inner.width,
                inner.height.saturating_sub(1),
            );
            let body_widget = Paragraph::new(text.clone()).wrap(Wrap { trim: false });
            app.text_scroll_limit = body_widget
                .line_count(body.width)
                .saturating_sub(body.height as usize)
                .min(u16::MAX as usize) as u16;
            f.render_widget(
                body_widget
                    .scroll(((*scroll).min(app.text_scroll_limit), 0))
                    .style(Style::default().fg(FG)),
                body,
            );
            paragraph(
                f,
                row(inner, inner.height.saturating_sub(1)),
                tr("ui.close_hint"),
                FG,
            );
        }
        Overlay::ConfirmClose(_)
        | Overlay::ConfirmRemove(_)
        | Overlay::ConfirmQuit
        | Overlay::ConfirmPaste(_) => {
            let key = match app.overlay {
                Overlay::ConfirmClose(_) => "confirm.close",
                Overlay::ConfirmRemove(_) => "confirm.remove",
                Overlay::ConfirmQuit => "confirm.quit",
                _ => "confirm.paste",
            };
            let text = if let Overlay::ConfirmPaste(text) = &app.overlay {
                trf(key, &[text.lines().count().to_string()])
            } else {
                tr(key)
            };
            let inner = modal(f, 68, 9, tr("app.title"));
            f.render_widget(
                Paragraph::new(text)
                    .wrap(Wrap { trim: false })
                    .style(Style::default().fg(FG)),
                Rect::new(
                    inner.x,
                    inner.y + 1,
                    inner.width,
                    inner.height.saturating_sub(3),
                ),
            );
            confirm_buttons(f, inner, &mut app.hits, false);
        }
        Overlay::None => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        config::{Config, Console},
        i18n::Language,
    };
    use ratatui::{Terminal, backend::TestBackend};
    #[test]
    fn all_layouts_render_including_small_windows_and_overlays() {
        let mut a = App::new(
            Config {
                version: 1,
                language: Language::En,
                consoles: vec![Console {
                    id: 1,
                    name: "Studio".into(),
                    host: "127.0.0.1".into(),
                    port: 2323,
                    token: None,
                    token_env: None,
                }],
            },
            "unused".into(),
            true,
        );
        for _ in 0..5 {
            a.new_pane().unwrap();
        }
        a.split = true;
        for (w, h) in [(140, 44), (80, 24), (55, 18), (20, 8)] {
            let mut t = Terminal::new(TestBackend::new(w, h)).unwrap();
            t.draw(|f| draw(f, &mut a)).unwrap();
            a.overlay = Overlay::Consoles { selected: 1 };
            t.draw(|f| draw(f, &mut a)).unwrap();
            a.overlay = Overlay::Palette {
                query: "files".into(),
                selected: 0,
            };
            t.draw(|f| draw(f, &mut a)).unwrap();
            a.action("add");
            t.draw(|f| draw(f, &mut a)).unwrap();
            a.overlay = Overlay::None;
        }
    }
    #[test]
    fn terminal_widget_preserves_attributes_and_wide_characters() {
        let mut p = vt100::Parser::new(2, 10, 0);
        p.process("\x1b[38;2;1;2;3m界é\x1b[1;4mX".as_bytes());
        let mut buffer = Buffer::empty(Rect::new(0, 0, 10, 2));
        ScreenWidget(p.screen()).render(buffer.area, &mut buffer);
        assert_eq!(buffer[(0, 0)].symbol(), "界");
        assert_eq!(buffer[(0, 0)].fg, Color::Rgb(1, 2, 3));
        assert_eq!(buffer[(2, 0)].symbol(), "é");
        assert!(
            buffer[(3, 0)]
                .modifier
                .contains(Modifier::BOLD | Modifier::UNDERLINED)
        );
    }
}
