use psxterm_tui::{
    app::App,
    config::{Config, Console},
    i18n::Language,
    protocol::*,
};
use std::{
    net::TcpListener,
    path::PathBuf,
    thread,
    time::{Duration, Instant},
};

fn console(port: u16) -> Console {
    Console {
        id: 1,
        name: "Host test".into(),
        host: "127.0.0.1".into(),
        port,
        token: None,
        token_env: None,
    }
}

#[test]
fn worker_handshake_binary_streams_eof_resize_ping_and_close() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let server = thread::spawn(move || {
        let (mut s, _) = listener.accept().unwrap();
        s.set_read_timeout(Some(Duration::from_secs(4))).unwrap();
        let h = read_frame(&mut s).unwrap();
        assert_eq!(h.kind, HELLO);
        assert_eq!(h.payload[0], 11);
        send_frame(&mut s, HELLO_ACK, 71, b"\0HOST|PSXTerm test".to_vec()).unwrap();
        let open = read_frame(&mut s).unwrap();
        assert_eq!(open.kind, OPEN);
        assert_eq!(&open.payload[..4], &dimensions(24, 80));
        send_frame(&mut s, OPEN_OK, 71, 71u32.to_le_bytes().to_vec()).unwrap();
        send_frame(&mut s, CAPS, 71, 18u32.to_le_bytes().to_vec()).unwrap();
        let mut info = 71u32.to_le_bytes().to_vec();
        info.extend_from_slice(&[9; 16]);
        send_frame(&mut s, SESSION_INFO, 71, info).unwrap();
        // Byte-by-byte output tests the worker's incremental decoder.
        use std::io::Write;
        for b in (Frame {
            kind: STDOUT,
            sid: 71,
            payload: b"\x1b[31mout\0\xff\x1b[0m\n".to_vec(),
        })
        .encode()
        .unwrap()
        {
            s.write_all(&[b]).unwrap();
        }
        send_frame(&mut s, STDERR, 71, b"stderr\n".to_vec()).unwrap();
        send_frame(&mut s, PING, 71, b"server-ping".to_vec()).unwrap();
        let mut seen = [false; 4];
        loop {
            let frame = read_frame(&mut s).unwrap();
            match frame.kind {
                STDIN => {
                    assert!(frame.payload.is_empty());
                    seen[0] = true;
                }
                RESIZE => {
                    assert_eq!(frame.payload, dimensions(30, 100));
                    seen[1] = true;
                }
                PONG => {
                    assert_eq!(frame.payload, b"server-ping");
                    seen[2] = true;
                }
                PING => {
                    seen[3] = true;
                    send_frame(&mut s, PONG, 71, frame.payload).unwrap();
                }
                CLOSE => {
                    assert!(seen.iter().all(|s| *s));
                    break;
                }
                _ => panic!("unexpected frame {}", frame.kind),
            }
        }
    });
    let c = Connection::start(console(port), 24, 80, None);
    let mut output = Vec::new();
    let mut ready = false;
    let mut ping = false;
    let mut token = false;
    let deadline = Instant::now() + Duration::from_secs(4);
    while Instant::now() < deadline && !(output.ends_with(b"stderr\n") && ping && token) {
        match c.rx.recv_timeout(Duration::from_millis(200)).unwrap() {
            Event::Connected(sid, _) => {
                assert_eq!(sid, 71);
                ready = true;
                c.send(Command::Input(vec![])).unwrap();
                c.send(Command::Resize(30, 100)).unwrap();
                c.send(Command::Ping).unwrap();
            }
            Event::Output(b) => output.extend_from_slice(&b),
            Event::Pong(_) => ping = true,
            Event::Resume(r) => {
                assert_eq!(r.token, [9; 16]);
                token = true;
            }
            Event::Capabilities(c) => assert_eq!(c, 18),
            Event::Error(e) => panic!("{e}"),
            _ => {}
        }
    }
    assert!(ready && ping && token);
    assert!(output.contains(&255));
    c.close();
    server.join().unwrap();
}

#[test]
#[ignore = "requires PSXTERM_TEST_PORT pointing to the running host daemon"]
fn actual_host_daemon_multiple_terminals_and_resume() {
    // Optional developer check against the repository's real POSIX daemon.
    let port = std::env::var("PSXTERM_TEST_PORT").expect("set PSXTERM_TEST_PORT");
    let c = console(port.parse().unwrap());
    let mut app = App::new(
        Config {
            version: 1,
            language: Language::En,
            consoles: vec![c.clone()],
        },
        PathBuf::new(),
        false,
    );
    let first = app.new_pane().unwrap();
    let second = app.new_pane().unwrap();
    wait(&mut app, |a| {
        a.panes.iter().all(|p| p.online && p.resume.is_some())
    });
    assert_ne!(app.panes[0].remote_sid, app.panes[1].remote_sid);
    app.select(first);
    app.current_mut()
        .unwrap()
        .send(Command::Input(b"echo FIRST-SESSION\n".to_vec()))
        .unwrap();
    app.select(second);
    app.current_mut()
        .unwrap()
        .send(Command::Input(b"echo SECOND-SESSION\n".to_vec()))
        .unwrap();
    wait(&mut app, |a| {
        a.panes[0]
            .parser
            .screen()
            .contents()
            .contains("FIRST-SESSION")
            && a.panes[1]
                .parser
                .screen()
                .contents()
                .contains("SECOND-SESSION")
    });
    assert!(
        !app.panes[0]
            .parser
            .screen()
            .contents()
            .contains("SECOND-SESSION")
    );
    assert!(app.panes[0].unread);
    let sid = app.current().unwrap().remote_sid;
    let old = app.current_mut().unwrap().connection.take().unwrap();
    drop(old);
    thread::sleep(Duration::from_millis(150));
    app.current_mut().unwrap().online = false;
    app.current_mut().unwrap().reconnect(&c);
    wait(&mut app, |a| a.current().unwrap().online);
    assert_eq!(app.current().unwrap().remote_sid, sid);
    app.action("pwd");
    wait(&mut app, |a| {
        a.current().unwrap().online && a.current().unwrap().pending_command.is_none()
    });
    app.current_mut().unwrap().send(Command::Ping).unwrap();
    wait(&mut app, |a| a.current().unwrap().status.contains(" ms"));
    for p in &app.panes {
        if let Some(c) = &p.connection {
            c.close();
        }
    }
    wait(&mut app, |a| {
        a.panes
            .iter()
            .all(|p| p.connection.as_ref().is_none_or(|c| c.finished()))
    });
}
fn wait(app: &mut App, condition: impl Fn(&App) -> bool) {
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        app.poll();
        if condition(app) {
            return;
        }
        thread::sleep(Duration::from_millis(15));
    }
    panic!(
        "condition timed out; statuses: {:?}",
        app.panes.iter().map(|p| &p.status).collect::<Vec<_>>()
    );
}
