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
                query: "/system".into(),
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
