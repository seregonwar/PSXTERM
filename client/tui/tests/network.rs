use psxterm_tui::{
    app::App,
    config::{Config, Console},
    i18n::Language,
    protocol::*,
};
use std::{
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    path::PathBuf,
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

fn finish(connection: &Connection) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while !connection.finished() && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(5));
    }
    assert!(
        connection.finished(),
        "cancelled connection did not finish promptly"
    );
}

#[test]
fn closing_a_connection_with_a_full_output_queue_finishes_without_draining_it() {
    for detach in [false, true] {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let (ready, receive) = mpsc::channel();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(3)))
                .unwrap();
            assert_eq!(read_frame(&mut stream).unwrap().kind, HELLO);
            send_frame(&mut stream, HELLO_ACK, 0, b"\0HOST".to_vec()).unwrap();
            assert_eq!(read_frame(&mut stream).unwrap().kind, OPEN);
            send_frame(&mut stream, OPEN_OK, 11, vec![]).unwrap();
            let mut output = Vec::new();
            for _ in 0..256 {
                output.extend(
                    Frame {
                        kind: STDOUT,
                        sid: 11,
                        payload: b"ordinary output\r\n".to_vec(),
                    }
                    .encode()
                    .unwrap(),
                );
            }
            stream.write_all(&output).unwrap();
            ready.send(()).unwrap();
            let control = read_frame(&mut stream).unwrap();
            assert_eq!(control.kind, if detach { DETACH } else { CLOSE });
            assert_eq!(control.sid, 11);
        });
        let connection = Connection::start(console(port), 24, 80, None);
        assert!(matches!(
            connection.rx.recv_timeout(Duration::from_secs(3)).unwrap(),
            Event::Connected(11, _)
        ));
        receive.recv_timeout(Duration::from_secs(3)).unwrap();
        thread::sleep(Duration::from_millis(50));
        if detach {
            drop(connection);
        } else {
            connection.close();
            finish(&connection);
        }
        server.join().unwrap();
    }
}

#[test]
fn cancellation_during_fragmented_hello_never_opens_a_terminal() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let (ready, receive) = mpsc::channel();
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(3)))
            .unwrap();
        assert_eq!(read_frame(&mut stream).unwrap().kind, HELLO);
        let ack = Frame {
            kind: HELLO_ACK,
            sid: 0,
            payload: b"\0HOST".to_vec(),
        }
        .encode()
        .unwrap();
        stream.write_all(&ack[..8]).unwrap();
        ready.send(()).unwrap();
        let mut byte = [0];
        assert_eq!(
            stream.read(&mut byte).unwrap(),
            0,
            "OPEN was sent after cancellation"
        );
    });
    let connection = Connection::start(console(port), 24, 80, None);
    receive.recv_timeout(Duration::from_secs(3)).unwrap();
    connection.close();
    finish(&connection);
    assert!(
        !connection
            .rx
            .try_iter()
            .any(|e| matches!(e, Event::Connected(..)))
    );
    server.join().unwrap();
}

#[test]
fn cancelled_open_and_attach_send_control_without_waiting_for_reply() {
    for resume in [
        None,
        Some(Resume {
            sid: 71,
            token: [9; 16],
        }),
    ] {
        for detach in [false, true] {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let port = listener.local_addr().unwrap().port();
            let (ready, receive) = mpsc::channel();
            let attaching = resume.is_some();
            let server = thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                stream
                    .set_read_timeout(Some(Duration::from_secs(3)))
                    .unwrap();
                assert_eq!(read_frame(&mut stream).unwrap().kind, HELLO);
                send_frame(&mut stream, HELLO_ACK, 0, b"\0HOST".to_vec()).unwrap();
                assert_eq!(
                    read_frame(&mut stream).unwrap().kind,
                    if attaching { ATTACH } else { OPEN }
                );
                let reply = Frame {
                    kind: if attaching { ATTACH_OK } else { OPEN_OK },
                    sid: 71,
                    payload: vec![],
                }
                .encode()
                .unwrap();
                stream.write_all(&reply[..9]).unwrap();
                ready.send(()).unwrap();
                let control = read_frame(&mut stream).unwrap();
                assert_eq!(control.kind, if detach { DETACH } else { CLOSE });
                assert_eq!(control.sid, 0);
            });
            let connection = Connection::start(console(port), 24, 80, resume.clone());
            receive.recv_timeout(Duration::from_secs(3)).unwrap();
            if detach {
                drop(connection);
            } else {
                connection.close();
                finish(&connection);
            }
            server.join().unwrap();
        }
    }
}

#[test]
fn closed_tab_finishes_its_connection_even_after_all_tabs_are_removed() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let (ready, receive) = mpsc::channel();
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(3)))
            .unwrap();
        assert_eq!(read_frame(&mut stream).unwrap().kind, HELLO);
        send_frame(&mut stream, HELLO_ACK, 0, b"\0HOST".to_vec()).unwrap();
        assert_eq!(read_frame(&mut stream).unwrap().kind, OPEN);
        ready.send(()).unwrap();
        assert_eq!(read_frame(&mut stream).unwrap().kind, CLOSE);
    });
    let mut app = App::new(
        Config {
            version: 1,
            language: Language::En,
            output_colors: Default::default(),
            consoles: vec![console(port)],
        },
        PathBuf::new(),
        false,
    );
    app.new_pane().unwrap();
    receive.recv_timeout(Duration::from_secs(3)).unwrap();
    app.action("close");
    app.handle(crossterm::event::Event::Key(
        crossterm::event::KeyEvent::new(
            crossterm::event::KeyCode::Enter,
            crossterm::event::KeyModifiers::NONE,
        ),
    ));
    assert!(app.panes.is_empty());
    let deadline = Instant::now() + Duration::from_secs(2);
    while !app.connections_finished() && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(5));
    }
    assert!(app.connections_finished());
    server.join().unwrap();
    assert!(
        Instant::now() < deadline,
        "shutdown waited for the old handshake timeout"
    );
}

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
fn actual_host_cancelled_open_does_not_leave_a_resumable_session() {
    let host_port: u16 = std::env::var("PSXTERM_TEST_PORT")
        .expect("set PSXTERM_TEST_PORT")
        .parse()
        .unwrap();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let proxy_port = listener.local_addr().unwrap().port();
    let (ready, receive) = mpsc::channel();
    let proxy = thread::spawn(move || {
        let (mut client, _) = listener.accept().unwrap();
        client
            .set_read_timeout(Some(Duration::from_secs(3)))
            .unwrap();
        assert_eq!(read_frame(&mut client).unwrap().kind, HELLO);
        send_frame(&mut client, HELLO_ACK, 0, b"\0HOST".to_vec()).unwrap();
        assert_eq!(read_frame(&mut client).unwrap().kind, OPEN);
        let (mut backend, sid, _) = connect(&console(host_port), 24, 80, None).unwrap();
        let info = read_frame(&mut backend).unwrap();
        assert_eq!(info.kind, SESSION_INFO);
        let resume = Resume {
            sid,
            token: info.payload[4..].try_into().unwrap(),
        };
        // Deliberately withhold OPEN_OK from the client after the real daemon
        // has created the session, exercising cancellation at the critical point.
        ready.send(()).unwrap();
        let close = read_frame(&mut client).unwrap();
        assert_eq!(close.kind, CLOSE);
        assert_eq!(close.sid, 0);
        backend.write_all(&close.encode().unwrap()).unwrap();
        wait_for_eof(&mut backend);
        resume
    });
    let connection = Connection::start(console(proxy_port), 24, 80, None);
    receive.recv_timeout(Duration::from_secs(3)).unwrap();
    connection.close();
    finish(&connection);
    let old_session = proxy.join().unwrap();
    let result = connect(&console(host_port), 24, 80, Some(&old_session));
    assert_eq!(
        result
            .expect_err("closed session was still resumable")
            .to_string(),
        psxterm_tui::i18n::tr("error.attach")
    );
}

fn wait_for_eof(stream: &mut TcpStream) {
    stream
        .set_read_timeout(Some(Duration::from_secs(3)))
        .unwrap();
    let mut byte = [0; 4096];
    loop {
        match stream.read(&mut byte).unwrap() {
            0 => break,
            _ => continue,
        }
    }
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
            output_colors: Default::default(),
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
#[test]
#[ignore = "requires PSXTERM_TEST_PORT pointing to the running host daemon"]
fn actual_host_daemon_builtin_listing_colors_and_reset() {
    let port = std::env::var("PSXTERM_TEST_PORT").expect("set PSXTERM_TEST_PORT");
    let c = console(port.parse().unwrap());
    let mut app = App::new(
        Config {
            version: 1,
            language: Language::En,
            output_colors: Default::default(),
            consoles: vec![c],
        },
        PathBuf::new(),
        false,
    );
    app.new_pane().unwrap();
    wait(&mut app, |a| a.current().unwrap().online);
    app.current_mut().unwrap().resize(18, 55);
    app.current_mut()
        .unwrap()
        .send(Command::Input(b"clear\nls -d --color=always /\n".to_vec()))
        .unwrap();
    wait(&mut app, |a| {
        let pane = a.current().unwrap();
        (0..18).any(|row| {
            pane.parser.screen().cell(row, 0).is_some_and(|cell| {
                cell.contents() == "/" && cell.fgcolor() == vt100::Color::Idx(12) && cell.bold()
            })
        })
    });
    let pane = app.current().unwrap();
    let screen = pane.parser.screen();
    let colored_row = (0..18)
        .find(|row| screen.cell(*row, 0).unwrap().fgcolor() == vt100::Color::Idx(12))
        .unwrap();
    let after_name = screen.cell(colored_row, 1).unwrap();
    assert_eq!(after_name.fgcolor(), vt100::Color::Default);
    assert!(
        !after_name.bold(),
        "listing style leaked beyond the filename"
    );
    app.current_mut()
        .unwrap()
        .send(Command::Input(b"ls -d --color=never /\n".to_vec()))
        .unwrap();
    wait(&mut app, |a| {
        let pane = a.current().unwrap();
        (colored_row + 1..18).any(|row| {
            pane.parser.screen().cell(row, 0).is_some_and(|cell| {
                cell.contents() == "/" && cell.fgcolor() == vt100::Color::Default && !cell.bold()
            })
        })
    });
    app.current().unwrap().connection.as_ref().unwrap().close();
    wait(&mut app, |a| a.current().unwrap().connection.is_none());
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
