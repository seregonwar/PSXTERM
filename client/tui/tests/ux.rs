use crossterm::event::{
    Event, KeyCode, KeyEvent, KeyModifiers, MouseButton, MouseEvent, MouseEventKind,
};
use psxterm_tui::{
    app::{App, Hit, Overlay},
    config::{Config, Console},
    flash,
    i18n::Language,
    ui,
};
use ratatui::{Terminal, backend::TestBackend, layout::Rect};

fn app() -> App {
    App::new(
        Config {
            version: 1,
            language: Language::En,
            output_colors: Default::default(),
            consoles: vec![Console {
                id: 1,
                name: "Studio".into(),
                host: "192.168.1.20".into(),
                port: 2323,
                token: None,
                token_env: None,
            }],
        },
        "unused".into(),
        true,
    )
}
fn key(a: &mut App, code: KeyCode) {
    a.handle(Event::Key(KeyEvent::new(code, KeyModifiers::NONE)));
}
fn click(a: &mut App, r: Rect) {
    a.handle(Event::Mouse(MouseEvent {
        kind: MouseEventKind::Down(MouseButton::Left),
        column: r.x,
        row: r.y,
        modifiers: KeyModifiers::NONE,
    }));
}
fn draw(a: &mut App, w: u16, h: u16) -> Terminal<TestBackend> {
    let mut t = Terminal::new(TestBackend::new(w, h)).unwrap();
    t.draw(|f| ui::draw(f, a)).unwrap();
    let screen = Rect::new(0, 0, w, h);
    for (r, _) in &a.hits {
        assert!(
            r.width > 0
                && r.height > 0
                && screen.contains((r.x, r.y).into())
                && r.right() <= w
                && r.bottom() <= h,
            "hit outside {w}x{h}: {r:?}"
        );
    }
    t
}
fn text(t: &Terminal<TestBackend>) -> String {
    let b = t.backend().buffer();
    b.content.iter().map(|c| c.symbol()).collect()
}

#[test]
fn unread_badge_and_picker_reach_other_consoles_without_replacing_sessions() {
    for (w, h) in [
        (55, 18),
        (60, 20),
        (72, 20),
        (80, 24),
        (100, 30),
        (120, 35),
        (140, 44),
        (200, 60),
    ] {
        let mut a = app();
        let first = a.new_pane().unwrap();
        let second = a.new_pane().unwrap();
        a.config.consoles.push(Console {
            id: 2,
            name: "Lab".into(),
            host: "192.168.1.21".into(),
            port: 2323,
            token: None,
            token_env: None,
        });
        a.switch(2);
        let third = a.new_pane().unwrap();
        a.select(first);
        for pane in &mut a.panes {
            pane.unread = pane.id != first;
        }
        assert_eq!(a.unread_count(None), 2);
        assert_eq!(a.unread_count(Some(1)), 1);
        assert_eq!(a.unread_count(Some(2)), 1);
        let t = draw(&mut a, w, h);
        assert!(text(&t).contains("● 2"));
        key(&mut a, KeyCode::F(3));
        let t = draw(&mut a, w, h);
        assert!(text(&t).contains("● 1  Studio"));
        assert!(text(&t).contains("● 1  Lab"));
        assert!(text(&t).contains("new output"));
        key(&mut a, KeyCode::Esc);
        draw(&mut a, w, h);
        let badge = a
            .hits
            .iter()
            .find(|(_, hit)| matches!(hit, Hit::Action(id) if id == "unread"))
            .unwrap()
            .0;
        click(&mut a, badge);
        assert_eq!(a.active, Some(second));
        assert_eq!(a.console, Some(1));
        assert_eq!(a.unread_count(None), 1);
        assert!(!a.sidebar_focus);
        // Palette access must work even when the remaining unread terminal is
        // on another console and its tab is not displayed in this workspace.
        key(&mut a, KeyCode::F(2));
        a.handle(Event::Paste("/unread".into()));
        key(&mut a, KeyCode::Enter);
        assert_eq!(a.active, Some(third));
        assert_eq!(a.console, Some(2));
        assert_eq!(a.unread_count(None), 0);
        assert!(a.disabled_reason("unread").is_some());
        draw(&mut a, w, h);
        assert!(
            !a.hits
                .iter()
                .any(|(_, hit)| matches!(hit, Hit::Action(id) if id == "unread"))
        );
        assert_eq!(a.panes.len(), 3);
        assert!(a.panes.iter().all(|p| p.connection.is_none()));
        // Switching back remembers the terminal reached via the activity badge.
        a.switch(1);
        assert_eq!(a.active, Some(second));
    }
}

#[test]
fn split_mouse_wheel_targets_the_hovered_terminal_and_new_output_keeps_history_stable() {
    for (w, h) in [
        (55, 18),
        (60, 20),
        (72, 20),
        (80, 24),
        (100, 30),
        (120, 35),
        (140, 44),
        (200, 60),
    ] {
        let mut a = app();
        a.new_pane().unwrap();
        a.new_pane().unwrap();
        a.split = true;
        draw(&mut a, w, h);
        for pane in &mut a.panes {
            for n in 0..150 {
                pane.output(format!("row {n}\r\n").as_bytes());
            }
        }
        draw(&mut a, w, h);
        let active = a.active;
        let (rect, target) = a
            .hits
            .iter()
            .find_map(|(rect, h)| {
                if let Hit::Pane(id) = h
                    && Some(*id) != active
                {
                    Some((*rect, *id))
                } else {
                    None
                }
            })
            .or_else(|| {
                a.hits.iter().find_map(|(rect, h)| {
                    if let Hit::Pane(id) = h {
                        Some((*rect, *id))
                    } else {
                        None
                    }
                })
            })
            .unwrap();
        a.handle(Event::Mouse(MouseEvent {
            kind: MouseEventKind::ScrollUp,
            column: rect.x + 1,
            row: rect.y + 1,
            modifiers: KeyModifiers::NONE,
        }));
        assert_eq!(a.active, active, "scrolling must not change input focus");
        for pane in &a.panes {
            assert_eq!(
                pane.parser.screen().scrollback(),
                if pane.id == target { 3 } else { 0 }
            );
        }
        a.handle(Event::Mouse(MouseEvent {
            kind: MouseEventKind::ScrollUp,
            column: 0,
            row: 0,
            modifiers: KeyModifiers::NONE,
        }));
        let pane = a.panes.iter_mut().find(|p| p.id == target).unwrap();
        assert_eq!(
            pane.parser.screen().scrollback(),
            3,
            "wheel on workspace chrome must not scroll a terminal"
        );
        let before = pane.parser.screen().contents();
        pane.output(b"new output\r\nmore output\r\n");
        assert_eq!(
            pane.parser.screen().contents(),
            before,
            "new output moved the history being read"
        );
        assert_eq!(pane.parser.screen().scrollback(), 5);
        draw(&mut a, w, h);
        a.handle(Event::Mouse(MouseEvent {
            kind: MouseEventKind::ScrollDown,
            column: rect.x + 1,
            row: rect.y + 1,
            modifiers: KeyModifiers::NONE,
        }));
        assert_eq!(
            a.panes
                .iter()
                .find(|p| p.id == target)
                .unwrap()
                .parser
                .screen()
                .scrollback(),
            2
        );
        assert!(a.panes.iter().all(|p| p.connection.is_none()));
    }
}

#[test]
fn dialog_carets_stay_inside_fields_and_tokens_stay_masked_at_all_sizes() {
    for (w, h) in [
        (55, 18),
        (60, 20),
        (72, 20),
        (80, 24),
        (100, 30),
        (120, 35),
        (140, 44),
        (200, 60),
    ] {
        let mut a = app();
        a.new_pane().unwrap();
        a.action("edit");
        key(&mut a, KeyCode::Home);
        key(&mut a, KeyCode::Delete);
        a.handle(Event::Paste("界".into()));
        let Overlay::Form { fields, .. } = &a.overlay else {
            panic!();
        };
        assert_eq!(fields[0].as_str(), "界tudio");
        let t = draw(&mut a, w, h);
        let name = a
            .hits
            .iter()
            .find(|(_, h)| matches!(h, Hit::Input(0)))
            .unwrap()
            .0;
        assert_eq!(t.backend().buffer()[(name.x, name.y)].symbol(), "界");
        assert_eq!(t.backend().buffer()[(name.x + 2, name.y)].symbol(), "t");
        for _ in 0..3 {
            key(&mut a, KeyCode::Tab);
        }
        a.handle(Event::Paste("sëcret".repeat(100)));
        let Overlay::Form { fields, .. } = &a.overlay else {
            panic!();
        };
        assert!(fields[3].as_str().len() <= 255);
        let mut t = draw(&mut a, w, h);
        assert!(!text(&t).contains("sëcret"));
        let field = a
            .hits
            .iter()
            .find(|(_, h)| matches!(h, Hit::Input(3)))
            .unwrap()
            .0;
        assert!(field.contains(t.get_cursor_position().unwrap()));
        click(&mut a, field);
        key(&mut a, KeyCode::Delete);
        let mut t = draw(&mut a, w, h);
        assert!(!text(&t).contains("sëcret"));
        assert!(field.contains(t.get_cursor_position().unwrap()));
        key(&mut a, KeyCode::Home);
        let mut t = draw(&mut a, w, h);
        assert_eq!(t.get_cursor_position().unwrap(), (field.x, field.y).into());
        assert!(text(&t).contains("•"));
        key(&mut a, KeyCode::Esc);
        assert!(a.config.consoles[0].token.is_none());
        key(&mut a, KeyCode::F(7));
        a.handle(Event::Key(KeyEvent::new(
            KeyCode::Char('u'),
            KeyModifiers::CONTROL,
        )));
        a.handle(Event::Paste("界terminal".into()));
        let mut t = draw(&mut a, w, h);
        let field = a
            .hits
            .iter()
            .find(|(_, h)| matches!(h, Hit::Input(0)))
            .unwrap()
            .0;
        assert!(field.contains(t.get_cursor_position().unwrap()));
        click(&mut a, field);
        key(&mut a, KeyCode::Delete);
        a.handle(Event::Paste("é".into()));
        key(&mut a, KeyCode::Enter);
        assert_eq!(a.current().unwrap().title, "éterminal");
        assert!(a.current().unwrap().connection.is_none());
    }
}

#[test]
fn moving_search_caret_preserves_selection_and_stale_results_until_edit() {
    let mut a = app();
    a.new_pane().unwrap();
    draw(&mut a, 80, 24);
    a.current_mut()
        .unwrap()
        .output(b"\r\nprojects\r\nprojects\r\n");
    key(&mut a, KeyCode::F(11));
    a.handle(Event::Paste("projects".into()));
    key(&mut a, KeyCode::Home);
    let scroll = a.current().unwrap().parser.screen().scrollback();
    a.current_mut().unwrap().output(b"projects\r\n");
    key(&mut a, KeyCode::Left);
    let Overlay::Search(search) = &a.overlay else {
        panic!();
    };
    assert_eq!(search.selected, 0);
    assert_eq!(search.matches.len(), 3);
    assert!(!search.is_current(
        a.current().unwrap().parser.screen(),
        a.current().unwrap().output_revision
    ));
    assert_eq!(a.current().unwrap().parser.screen().scrollback(), scroll);
    // No-op paste must not rebuild an index or jump to the newest match.
    a.handle(Event::Paste("\r\n".into()));
    let Overlay::Search(search) = &a.overlay else {
        panic!();
    };
    assert_eq!(search.selected, 0);
    assert_eq!(search.matches.len(), 3);
    a.handle(Event::Key(KeyEvent::new(
        KeyCode::Char('a'),
        KeyModifiers::CONTROL,
    )));
    key(&mut a, KeyCode::Delete);
    let Overlay::Search(search) = &a.overlay else {
        panic!();
    };
    assert_eq!(search.query.as_str(), "rojects");
    assert_eq!(search.matches.len(), 4);
    assert!(search.is_current(
        a.current().unwrap().parser.screen(),
        a.current().unwrap().output_revision
    ));
    a.handle(Event::Paste("p".into()));
    key(&mut a, KeyCode::End);
    let Overlay::Search(search) = &a.overlay else {
        panic!();
    };
    assert_eq!(search.selected, 3);
    assert_eq!(search.query.as_str(), "projects");
    key(&mut a, KeyCode::Esc);
    assert!(a.current().unwrap().connection.is_none());
}

#[test]
fn palette_edits_query_without_changing_keyboard_result_navigation() {
    let mut a = app();
    a.new_pane().unwrap();
    key(&mut a, KeyCode::F(2));
    a.handle(Event::Paste("/system".into()));
    key(&mut a, KeyCode::End);
    let Overlay::Palette { query, selected } = &a.overlay else {
        panic!();
    };
    let selected = *selected;
    assert_eq!(query.as_str(), "/system");
    key(&mut a, KeyCode::Left);
    let Overlay::Palette {
        selected: current, ..
    } = &a.overlay
    else {
        panic!();
    };
    assert_eq!(*current, selected);
    a.handle(Event::Key(KeyEvent::new(
        KeyCode::Home,
        KeyModifiers::CONTROL,
    )));
    key(&mut a, KeyCode::Delete);
    let Overlay::Palette { query, selected } = &a.overlay else {
        panic!();
    };
    assert_eq!(query.as_str(), "system");
    assert_eq!(*selected, 0);
    let mut t = draw(&mut a, 80, 24);
    let field = a
        .hits
        .iter()
        .find(|(_, h)| matches!(h, Hit::Input(0)))
        .unwrap()
        .0;
    assert_eq!(t.get_cursor_position().unwrap(), (field.x, field.y).into());
    click(&mut a, field);
    a.handle(Event::Paste("/".into()));
    let Overlay::Palette { query, .. } = &a.overlay else {
        panic!();
    };
    assert_eq!(query.as_str(), "/system");
    assert!(a.current().unwrap().connection.is_none());
}

#[test]
fn local_search_navigates_history_and_mouse_controls_across_all_layouts() {
    for (w, h) in [
        (55, 18),
        (60, 20),
        (72, 20),
        (80, 24),
        (100, 30),
        (120, 35),
        (140, 44),
        (200, 60),
    ] {
        let mut a = app();
        a.new_pane().unwrap();
        draw(&mut a, w, h);
        let p = a.current_mut().unwrap();
        let dimensions = p.dimensions;
        p.parser = vt100::Parser::new(dimensions.0, dimensions.1, 5000);
        let output_rows = usize::from(dimensions.0) + 40;
        for n in 0..output_rows {
            p.output(if n == 0 || n == output_rows - 5 {
                b"NEEDLE\r\n"
            } else {
                b"log\r\n"
            });
        }
        let revision = p.output_revision;
        key(&mut a, KeyCode::F(11));
        a.handle(Event::Paste("needle".into()));
        let t = draw(&mut a, w, h);
        let Overlay::Search(search) = &a.overlay else {
            panic!("search did not open");
        };
        assert_eq!(search.matches.len(), 2);
        assert_eq!(search.selected, 1);
        assert!(
            text(&t).contains("Esc"),
            "search close hint must fit at {w}x{h}"
        );
        assert_eq!(
            t.backend()
                .buffer()
                .content
                .iter()
                .filter(|c| c.bg == ratatui::style::Color::Rgb(238, 207, 118))
                .count(),
            6
        );
        let previous = a
            .hits
            .iter()
            .find(|(_, hit)| matches!(hit, Hit::SearchStep(-1)))
            .unwrap()
            .0;
        click(&mut a, previous);
        draw(&mut a, w, h);
        assert!(a.current().unwrap().parser.screen().scrollback() > 0);
        let Overlay::Search(search) = &a.overlay else {
            panic!();
        };
        assert_eq!(search.selected, 0);
        key(&mut a, KeyCode::Enter);
        let t = draw(&mut a, w, h);
        assert!(text(&t).contains("2/2"));
        assert_eq!(a.current().unwrap().output_revision, revision);
        assert_eq!(a.current().unwrap().dimensions, dimensions);
        assert_eq!(a.panes.len(), 1);
        assert!(a.current().unwrap().connection.is_none());
        let close = a
            .hits
            .iter()
            .find(|(_, hit)| matches!(hit, Hit::SearchClose))
            .unwrap()
            .0;
        click(&mut a, close);
        assert!(matches!(a.overlay, Overlay::None));
    }
}

#[test]
fn search_invalidates_shifted_highlights_and_refreshes_without_reconnecting() {
    let mut a = app();
    a.new_pane().unwrap();
    draw(&mut a, 80, 24);
    key(&mut a, KeyCode::F(11));
    a.handle(Event::Paste("projects".into()));
    let t = draw(&mut a, 80, 24);
    assert!(text(&t).contains("1/1"));
    a.current_mut().unwrap().output(b"\r\nprojects/new\r\n");
    let t = draw(&mut a, 80, 24);
    assert!(text(&t).contains("changed"));
    assert!(
        !t.backend()
            .buffer()
            .content
            .iter()
            .any(|c| c.bg == ratatui::style::Color::Rgb(238, 207, 118))
    );
    let refresh = a
        .hits
        .iter()
        .find(|(_, hit)| matches!(hit, Hit::SearchRefresh))
        .unwrap()
        .0;
    click(&mut a, refresh);
    let t = draw(&mut a, 80, 24);
    assert!(text(&t).contains("2/2"));
    assert!(a.current().unwrap().connection.is_none());
    let t = draw(&mut a, 60, 20);
    assert!(text(&t).contains("changed"));
    key(&mut a, KeyCode::Enter);
    let t = draw(&mut a, 60, 20);
    assert!(!text(&t).contains("changed"));
    a.handle(Event::Key(KeyEvent::new(
        KeyCode::Char('u'),
        KeyModifiers::CONTROL,
    )));
    a.handle(Event::Paste("不存在\r\n".into()));
    let t = draw(&mut a, 60, 20);
    assert!(text(&t).contains("No matches"));
    let Overlay::Search(search) = &a.overlay else {
        panic!();
    };
    assert_eq!(search.query.as_str(), "不存在");
    key(&mut a, KeyCode::Esc);
    assert!(matches!(a.overlay, Overlay::None));
    a.active = None;
    assert!(a.disabled_reason("search").is_some());
}

#[test]
fn search_highlights_whole_unicode_cells_across_wrapped_rows() {
    let mut a = app();
    a.new_pane().unwrap();
    draw(&mut a, 80, 24);
    let p = a.current_mut().unwrap();
    let (rows, cols) = p.dimensions;
    p.parser = vt100::Parser::new(rows, cols, 5000);
    p.output(format!("{} 界é", "x".repeat(usize::from(cols) - 1)).as_bytes());
    a.action("search");
    a.handle(Event::Paste("xx 界é".into()));
    let t = draw(&mut a, 80, 24);
    let highlighted: Vec<_> = t
        .backend()
        .buffer()
        .content
        .iter()
        .filter(|c| c.bg == ratatui::style::Color::Rgb(238, 207, 118))
        .collect();
    assert_eq!(
        highlighted
            .iter()
            .map(|c| unicode_width::UnicodeWidthStr::width(c.symbol()))
            .sum::<usize>(),
        6
    );
    assert!(highlighted.iter().any(|c| c.symbol() == "界"));
    assert!(highlighted.iter().any(|c| c.symbol() == "é"));
}

#[test]
fn export_dialog_supports_mouse_paths_and_scrolled_output_at_all_sizes() {
    let dir = tempfile::tempdir().unwrap();
    for (w, h) in [
        (55, 18),
        (60, 20),
        (72, 20),
        (80, 24),
        (100, 30),
        (120, 35),
        (140, 44),
        (200, 60),
    ] {
        let mut a = app();
        a.new_pane().unwrap();
        draw(&mut a, w, h);
        let p = a.current_mut().unwrap();
        let dimensions = p.dimensions;
        p.parser = vt100::Parser::new(dimensions.0, dimensions.1, 5000);
        for n in 0..usize::from(dimensions.0) + 40 {
            p.output(format!("line {n} 界é\r\n").as_bytes());
        }
        p.scroll(5);
        let offset = p.parser.screen().scrollback();
        let expected = psxterm_tui::export::text(
            p.parser.screen().clone(),
            psxterm_tui::export::Scope::Visible,
        );
        key(&mut a, KeyCode::F(12));
        let t = draw(&mut a, w, h);
        assert!(text(&t).contains("Export terminal text"));
        assert!(text(&t).contains("Enter") && text(&t).contains("Esc"));
        let visible = a
            .hits
            .iter()
            .find(|(_, hit)| matches!(hit, Hit::ExportScope(psxterm_tui::export::Scope::Visible)))
            .unwrap()
            .0;
        click(&mut a, visible);
        let field = a
            .hits
            .iter()
            .find(|(_, hit)| matches!(hit, Hit::ExportField(0)))
            .unwrap()
            .0;
        click(&mut a, field);
        a.handle(Event::Key(KeyEvent::new(
            KeyCode::Char('u'),
            KeyModifiers::CONTROL,
        )));
        let path = dir.path().join(format!("output {w}x{h} 界.txt"));
        a.handle(Event::Paste(path.display().to_string()));
        key(&mut a, KeyCode::Home);
        draw(&mut a, w, h);
        key(&mut a, KeyCode::End);
        draw(&mut a, w, h);
        let save = a
            .hits
            .iter()
            .find(|(_, hit)| matches!(hit, Hit::Confirm(true)))
            .unwrap()
            .0;
        click(&mut a, save);
        assert!(matches!(a.overlay, Overlay::None));
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(3);
        while a.export_pending() && std::time::Instant::now() < deadline {
            a.poll();
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        assert!(!a.export_pending());
        assert_eq!(std::fs::read_to_string(path).unwrap(), expected);
        assert_eq!(a.current().unwrap().dimensions, dimensions);
        assert_eq!(a.current().unwrap().parser.screen().scrollback(), offset);
        assert!(a.current().unwrap().connection.is_none());
        a.action("export");
        let cancelled = dir.path().join(format!("cancelled-{w}.txt"));
        a.handle(Event::Key(KeyEvent::new(
            KeyCode::Char('u'),
            KeyModifiers::CONTROL,
        )));
        a.handle(Event::Paste(cancelled.display().to_string()));
        draw(&mut a, w, h);
        let cancel = a
            .hits
            .iter()
            .find(|(_, hit)| matches!(hit, Hit::Confirm(false)))
            .unwrap()
            .0;
        click(&mut a, cancel);
        assert!(matches!(a.overlay, Overlay::None));
        assert!(!cancelled.exists());
        assert!(!a.export_pending());
    }
}

#[test]
fn resize_matrix_keeps_active_terminal_readable_and_reachable() {
    let mut a = app();
    for _ in 0..8 {
        a.new_pane().unwrap();
    }
    a.split = true;
    for (w, h) in [
        (55, 18),
        (60, 20),
        (72, 20),
        (80, 24),
        (100, 30),
        (120, 35),
        (140, 44),
        (200, 60),
    ] {
        for active in [1, 8] {
            a.select(active);
            let t = draw(&mut a, w, h);
            assert!(
                a.hits
                    .iter()
                    .any(|(_, hit)| matches!(hit,Hit::Pane(id) if *id==active)),
                "active pane unreachable at {w}x{h}"
            );
            assert!(
                a.current().unwrap().dimensions.0 >= 10 && a.current().unwrap().dimensions.1 >= 35,
                "unreadable viewport {w}x{h}"
            );
            assert!(
                text(&t).contains("DEMO"),
                "demo must stay visibly labeled at {w}x{h}"
            );
            assert_eq!(a.panes.len(), 8);
        }
        for overlay in [
            Overlay::Consoles { selected: 1 },
            Overlay::Palette {
                query: psxterm_tui::input::Input::new("/system".into(), 100),
                selected: 0,
            },
            Overlay::ConfirmQuit,
        ] {
            a.overlay = overlay;
            draw(&mut a, w, h);
        }
        a.action("add");
        draw(&mut a, w, h);
        a.overlay = Overlay::None;
    }
    draw(&mut a, 40, 12);
    assert_eq!(a.panes.len(), 8);
}

#[test]
fn mouse_form_and_confirmation_flow_in_compact_workspace() {
    let mut a = app();
    a.new_pane().unwrap();
    draw(&mut a, 60, 20);
    let r = a
        .hits
        .iter()
        .find(|(_, h)| matches!(h, Hit::Console))
        .unwrap()
        .0;
    click(&mut a, r);
    draw(&mut a, 60, 20);
    let r = a
        .hits
        .iter()
        .find(|(_, h)| matches!(h, Hit::Choice(1)))
        .unwrap()
        .0;
    click(&mut a, r);
    a.handle(Event::Paste("Lab".into()));
    key(&mut a, KeyCode::Tab);
    a.handle(Event::Paste("127.0.0.1".into()));
    key(&mut a, KeyCode::Tab);
    key(&mut a, KeyCode::Tab);
    a.handle(Event::Paste("VERY-PRIVATE-TOKEN".into()));
    let t = draw(&mut a, 60, 20);
    assert!(!text(&t).contains("VERY-PRIVATE-TOKEN"));
    let r = a
        .hits
        .iter()
        .find(|(_, h)| matches!(h, Hit::Confirm(true)))
        .unwrap()
        .0;
    click(&mut a, r);
    assert!(matches!(a.overlay, Overlay::None));
    assert_eq!(a.current_console().unwrap().name, "Lab");
    assert_eq!(a.config.consoles.len(), 2);
    a.new_pane().unwrap();
    a.action("close");
    draw(&mut a, 60, 20);
    let r = a
        .hits
        .iter()
        .find(|(_, h)| matches!(h, Hit::Confirm(false)))
        .unwrap()
        .0;
    click(&mut a, r);
    assert_eq!(a.panes.len(), 2);
    a.action("close");
    draw(&mut a, 60, 20);
    let r = a
        .hits
        .iter()
        .find(|(_, h)| matches!(h, Hit::Confirm(true)))
        .unwrap()
        .0;
    click(&mut a, r);
    assert_eq!(a.panes.len(), 1);
}

#[test]
fn sidebar_drawer_and_palette_explain_unavailable_actions() {
    let mut a = app();
    a.new_pane().unwrap();
    key(&mut a, KeyCode::F(9));
    let t = draw(&mut a, 60, 20);
    assert!(text(&t).contains("192.168.1.20"));
    key(&mut a, KeyCode::Enter);
    assert!(!a.sidebar_focus);
    key(&mut a, KeyCode::F(2));
    a.handle(Event::Paste("/ping".into()));
    draw(&mut a, 80, 24);
    key(&mut a, KeyCode::Enter);
    assert!(matches!(a.overlay, Overlay::Palette { .. }));
    assert_eq!(a.panes.len(), 1);
    assert!(!a.notice.is_empty());
    assert_eq!(flash::ACTIONS[flash::filtered("/system")[0]].id, "system");
    assert_eq!(flash::ACTIONS[flash::filtered("rnm")[0]].id, "rename");
    assert!(flash::filtered("zzzzzz").is_empty());
}

#[test]
fn close_cross_targets_that_terminal_and_cancel_preserves_selection() {
    for split in [false, true] {
        for (w, h) in [(55, 18), (80, 24), (140, 44)] {
            let mut a = app();
            a.new_pane().unwrap();
            a.new_pane().unwrap();
            a.select(1);
            a.split = split;
            let target = if split && w < 100 { 1 } else { 2 };
            draw(&mut a, w, h);
            let cross = a
                .hits
                .iter()
                .find(|(_, hit)| matches!(hit, Hit::Close(id) if *id == target))
                .unwrap()
                .0;
            click(&mut a, cross);
            assert!(matches!(a.overlay, Overlay::ConfirmClose(id) if id == target));
            key(&mut a, KeyCode::Esc);
            assert_eq!(a.panes.len(), 2);
            assert_eq!(a.active, Some(1));
            draw(&mut a, w, h);
            click(&mut a, cross);
            key(&mut a, KeyCode::Enter);
            assert_eq!(a.panes.len(), 1);
            assert!(!a.panes.iter().any(|p| p.id == target));
            assert_eq!(a.active, Some(if target == 1 { 2 } else { 1 }));
        }
    }
}

#[test]
fn workspace_has_one_place_for_each_detail_and_neutral_chrome() {
    let mut a = app();
    a.new_pane().unwrap();
    let t = draw(&mut a, 140, 44);
    let screen = text(&t);
    for word in [
        "Studio",
        "192.168.1.20",
        "Terminal 1",
        "by seregonwar",
        "GPLv3",
        "DEMO",
    ] {
        assert_eq!(screen.matches(word).count(), 1, "repeated detail: {word}");
    }
    let pane = a
        .hits
        .iter()
        .find(|(_, hit)| matches!(hit, Hit::Pane(1)))
        .unwrap()
        .0;
    for y in 0..44 {
        for x in 0..140 {
            if !pane.contains((x, y).into()) && x < 29 {
                let c = &t.backend().buffer()[(x, y)];
                assert!([ui::FG, ui::MUTED, ui::BORDER].contains(&c.fg));
                assert!([ui::BG, ui::PANEL, ui::BORDER].contains(&c.bg));
            }
        }
    }
}

#[test]
fn license_can_scroll_to_the_last_wrapped_line_after_resize() {
    let mut a = app();
    a.action("license");
    draw(&mut a, 55, 18);
    let narrow = a.text_scroll_limit;
    for _ in 0..150 {
        key(&mut a, KeyCode::PageDown);
    }
    let t = draw(&mut a, 55, 18);
    assert!(text(&t).contains("https://www.gnu.org/licenses/"));
    draw(&mut a, 140, 44);
    assert!(a.text_scroll_limit < narrow);
    key(&mut a, KeyCode::Home);
    assert!(matches!(a.overlay, Overlay::Text { scroll: 0, .. }));
}
