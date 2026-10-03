//! Portable PTTY/1 transport. No loader or platform-specific execution paths.
use crate::config::Console;
use crate::i18n::tr;
use anyhow::{Context, Result, bail};
use std::{
    io::{Read, Write},
    net::{TcpStream, ToSocketAddrs},
    sync::{
        Arc,
        atomic::{AtomicU8, Ordering},
        mpsc::{self, Receiver, SyncSender, TrySendError},
    },
    thread,
    time::{Duration, Instant},
};

pub const MAX_PAYLOAD: usize = 65536;
pub const HELLO: u8 = 1;
pub const HELLO_ACK: u8 = 2;
pub const OPEN: u8 = 3;
pub const OPEN_OK: u8 = 4;
pub const CLOSE: u8 = 5;
pub const STDIN: u8 = 6;
pub const STDOUT: u8 = 7;
pub const STDERR: u8 = 8;
pub const RESIZE: u8 = 9;
pub const EXIT: u8 = 12;
pub const PING: u8 = 13;
pub const PONG: u8 = 14;
pub const CAPS: u8 = 18;
pub const DETACH: u8 = 19;
pub const ATTACH: u8 = 20;
pub const ATTACH_OK: u8 = 21;
pub const ATTACH_FAIL: u8 = 22;
pub const SESSION_INFO: u8 = 23;
pub const SESSIONS_REQUEST: u8 = 24;
pub const SESSIONS_DATA: u8 = 25;
pub const SESSIONS_DONE: u8 = 26;

#[derive(Debug, PartialEq)]
pub struct Frame {
    pub kind: u8,
    pub sid: u32,
    pub payload: Vec<u8>,
}
impl Frame {
    pub fn encode(&self) -> Result<Vec<u8>> {
        if self.payload.len() > MAX_PAYLOAD {
            bail!(tr("error.frame"));
        }
        let mut bytes = Vec::with_capacity(16 + self.payload.len());
        bytes.extend_from_slice(b"PTTY");
        bytes.extend_from_slice(&[1, self.kind, 0, 0]);
        bytes.extend_from_slice(&self.sid.to_le_bytes());
        bytes.extend_from_slice(&(self.payload.len() as u32).to_le_bytes());
        bytes.extend_from_slice(&self.payload);
        Ok(bytes)
    }
}
#[derive(Default)]
pub struct Decoder {
    bytes: Vec<u8>,
}
impl Decoder {
    pub fn feed(&mut self, bytes: &[u8]) -> Result<()> {
        if self.bytes.len() + bytes.len() > MAX_PAYLOAD + 16 + 8192 {
            bail!(tr("error.buffer"));
        }
        self.bytes.extend_from_slice(bytes);
        Ok(())
    }
    pub fn next_frame(&mut self) -> Result<Option<Frame>> {
        if self.bytes.len() < 16 {
            return Ok(None);
        }
        if &self.bytes[..4] != b"PTTY" || self.bytes[4] != 1 {
            bail!(tr("error.protocol"));
        }
        let n = u32::from_le_bytes(self.bytes[12..16].try_into().unwrap()) as usize;
        if n > MAX_PAYLOAD {
            bail!(tr("error.payload"));
        }
        if self.bytes.len() < 16 + n {
            return Ok(None);
        }
        let frame = Frame {
            kind: self.bytes[5],
            sid: u32::from_le_bytes(self.bytes[8..12].try_into().unwrap()),
            payload: self.bytes[16..16 + n].to_vec(),
        };
        self.bytes.drain(..16 + n);
        Ok(Some(frame))
    }
}

#[derive(Clone)]
pub struct Resume {
    pub sid: u32,
    pub token: [u8; 16],
}
pub enum Command {
    Input(Vec<u8>),
    Resize(u16, u16),
    Ping,
    Sessions,
}
pub enum Event {
    Connected(u32, String),
    Output(Vec<u8>),
    Capabilities(u32),
    Resume(Resume),
    Exit(i32, bool),
    Pong(Duration),
    Sessions(String),
    Error(String),
    Disconnected,
}
pub struct Connection {
    tx: SyncSender<Command>,
    pub rx: Receiver<Event>,
    control: Arc<AtomicU8>,
    worker: thread::JoinHandle<()>,
}
impl Connection {
    pub fn start(console: Console, rows: u16, cols: u16, resume: Option<Resume>) -> Self {
        let (tx, commands) = mpsc::sync_channel(64);
        let (events, rx) = mpsc::sync_channel(64);
        let control = Arc::new(AtomicU8::new(0));
        let stop = control.clone();
        let worker = thread::spawn(move || {
            let result = worker(console, rows, cols, resume, commands, &events, &stop);
            if let Err(e) = result {
                emit(&events, &stop, Event::Error(format!("{e:#}")));
            }
            emit(&events, &stop, Event::Disconnected);
        });
        Self {
            tx,
            rx,
            control,
            worker,
        }
    }
    pub fn send(&self, command: Command) -> Result<()> {
        self.tx
            .try_send(command)
            .map_err(|_| anyhow::anyhow!(tr("error.command_queue")))
    }
    pub fn close(&self) {
        self.control.store(2, Ordering::Release);
    }
    pub fn finished(&self) -> bool {
        self.worker.is_finished()
    }
}
impl Drop for Connection {
    fn drop(&mut self) {
        let _ = self
            .control
            .compare_exchange(0, 1, Ordering::AcqRel, Ordering::Acquire);
    }
}
fn emit(tx: &SyncSender<Event>, stop: &AtomicU8, mut event: Event) -> bool {
    loop {
        match tx.try_send(event) {
            Ok(()) => return true,
            Err(TrySendError::Full(e)) => {
                if stop.load(Ordering::Acquire) != 0 {
                    return false;
                }
                event = e;
                thread::sleep(Duration::from_millis(5));
            }
            Err(TrySendError::Disconnected(_)) => return false,
        }
    }
}
pub fn send_frame(stream: &mut TcpStream, kind: u8, sid: u32, payload: Vec<u8>) -> Result<()> {
    stream.write_all(&Frame { kind, sid, payload }.encode()?)?;
    Ok(())
}
pub fn read_frame(stream: &mut TcpStream) -> Result<Frame> {
    let mut header = [0; 16];
    stream.read_exact(&mut header)?;
    let mut decoder = Decoder::default();
    decoder.feed(&header)?;
    // Validate before allocating a payload, including zero-length frames.
    if let Some(f) = decoder.next_frame()? {
        return Ok(f);
    }
    let n = u32::from_le_bytes(header[12..16].try_into().unwrap()) as usize;
    let mut payload = vec![0; n];
    stream.read_exact(&mut payload)?;
    decoder.feed(&payload)?;
    decoder.next_frame()?.context(tr("error.partial"))
}
pub fn connect(
    console: &Console,
    rows: u16,
    cols: u16,
    resume: Option<&Resume>,
) -> Result<(TcpStream, u32, String)> {
    console.validate()?;
    let addresses: Vec<_> = (console.host.as_str(), console.port)
        .to_socket_addrs()
        .context(tr("error.resolve"))?
        .collect();
    let mut stream = None;
    for address in addresses.into_iter().take(4) {
        if let Ok(s) = TcpStream::connect_timeout(&address, Duration::from_secs(3)) {
            stream = Some(s);
            break;
        }
    }
    let mut stream = stream.context(tr("error.connect"))?;
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    stream.set_write_timeout(Some(Duration::from_secs(3)))?;
    stream.set_nodelay(true)?;
    let name = b"psxterm-tui";
    let token = console.credential()?;
    let mut hello = vec![name.len() as u8];
    hello.extend_from_slice(name);
    hello.push(token.len() as u8);
    hello.extend_from_slice(token.as_bytes());
    send_frame(&mut stream, HELLO, 0, hello)?;
    let ack = read_frame(&mut stream)?;
    if ack.kind != HELLO_ACK || ack.payload.is_empty() {
        bail!(tr("error.hello"));
    }
    match ack.payload[0] {
        0 => {}
        1 | 2 => bail!(tr("error.auth")),
        3 => bail!(tr("error.busy")),
        _ => bail!(tr("error.ack")),
    }
    let server = String::from_utf8_lossy(&ack.payload[1..])
        .chars()
        .filter(|c| !c.is_control())
        .take(120)
        .collect();
    if let Some(r) = resume {
        let mut payload = r.sid.to_le_bytes().to_vec();
        payload.extend_from_slice(&r.token);
        send_frame(&mut stream, ATTACH, 0, payload)?;
    } else {
        let mut payload = dimensions(rows, cols);
        payload.extend_from_slice(b"xterm-256color");
        send_frame(&mut stream, OPEN, 0, payload)?;
    }
    let open = read_frame(&mut stream)?;
    if open.kind == ATTACH_FAIL {
        bail!(tr("error.attach"));
    }
    if open.kind != if resume.is_some() { ATTACH_OK } else { OPEN_OK } || open.sid == 0 {
        bail!(tr("error.open"));
    }
    Ok((stream, open.sid, server))
}
pub fn dimensions(rows: u16, cols: u16) -> Vec<u8> {
    [rows.max(1).to_le_bytes(), cols.max(1).to_le_bytes()].concat()
}
fn worker(
    console: Console,
    rows: u16,
    cols: u16,
    resume: Option<Resume>,
    commands: Receiver<Command>,
    events: &SyncSender<Event>,
    stop: &AtomicU8,
) -> Result<()> {
    let (mut stream, sid, server) = connect(&console, rows, cols, resume.as_ref())?;
    emit(events, stop, Event::Connected(sid, server));
    stream.set_read_timeout(Some(Duration::from_millis(25)))?;
    let mut decoder = Decoder::default();
    let mut buffer = [0u8; 8192];
    let mut ping = None;
    let mut sessions = Vec::new();
    loop {
        let action = stop.load(Ordering::Acquire);
        if action != 0 {
            send_frame(
                &mut stream,
                if action == 2 { CLOSE } else { DETACH },
                sid,
                vec![],
            )?;
            return Ok(());
        }
        for command in commands.try_iter().take(16) {
            let (kind, payload) = match command {
                Command::Input(bytes) => (STDIN, bytes),
                Command::Resize(r, c) => (RESIZE, dimensions(r, c)),
                Command::Ping => {
                    ping = Some(Instant::now());
                    (PING, b"psxterm-tui-ping".to_vec())
                }
                Command::Sessions => {
                    sessions.clear();
                    (SESSIONS_REQUEST, vec![])
                }
            };
            send_frame(&mut stream, kind, sid, payload)?;
        }
        match stream.read(&mut buffer) {
            Ok(0) => {
                if !decoder.bytes.is_empty() {
                    bail!(tr("error.partial"));
                }
                return Ok(());
            }
            Ok(n) => decoder.feed(&buffer[..n])?,
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock
                        | std::io::ErrorKind::TimedOut
                        | std::io::ErrorKind::Interrupted
                ) => {}
            Err(e) => return Err(e.into()),
        }
        while let Some(frame) = decoder.next_frame()? {
            if frame.sid != sid && frame.sid != 0 {
                bail!(tr("error.sid"));
            }
            let event = match frame.kind {
                PING => {
                    send_frame(&mut stream, PONG, sid, frame.payload)?;
                    None
                }
                CLOSE => return Ok(()),
                STDOUT | STDERR => Some(Event::Output(frame.payload)),
                CAPS if frame.payload.len() == 4 => Some(Event::Capabilities(u32::from_le_bytes(
                    frame.payload[..4].try_into().unwrap(),
                ))),
                SESSION_INFO if frame.payload.len() == 20 => Some(Event::Resume(Resume {
                    sid: u32::from_le_bytes(frame.payload[..4].try_into().unwrap()),
                    token: frame.payload[4..].try_into().unwrap(),
                })),
                EXIT if frame.payload.len() == 5 => Some(Event::Exit(
                    i32::from_le_bytes(frame.payload[..4].try_into().unwrap()),
                    frame.payload[4] == 1,
                )),
                PONG if frame.payload == b"psxterm-tui-ping" => {
                    ping.take().map(|p| Event::Pong(p.elapsed()))
                }
                SESSIONS_DATA => {
                    if sessions.len() + frame.payload.len() > MAX_PAYLOAD {
                        bail!(tr("error.sessions"));
                    }
                    sessions.extend_from_slice(&frame.payload);
                    None
                }
                SESSIONS_DONE => Some(Event::Sessions(
                    String::from_utf8_lossy(&sessions).into_owned(),
                )),
                _ => None,
            };
            if let Some(event) = event
                && !emit(events, stop, event)
            {
                let action = stop.load(Ordering::Acquire);
                if action != 0 {
                    send_frame(
                        &mut stream,
                        if action == 2 { CLOSE } else { DETACH },
                        sid,
                        vec![],
                    )?;
                }
                return Ok(());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fragmented_binary_frames_and_limits() {
        let original = Frame {
            kind: STDOUT,
            sid: 42,
            payload: vec![0, 255, 27, 10],
        };
        let bytes = original.encode().unwrap();
        let mut d = Decoder::default();
        for b in &bytes[..bytes.len() - 1] {
            d.feed(&[*b]).unwrap();
            assert!(d.next_frame().unwrap().is_none());
        }
        d.feed(&bytes[bytes.len() - 1..]).unwrap();
        assert_eq!(d.next_frame().unwrap().unwrap(), original);
        let mut bad = bytes[..16].to_vec();
        bad[12..16].copy_from_slice(&65537u32.to_le_bytes());
        d.feed(&bad).unwrap();
        assert!(d.next_frame().is_err());
        let mut d = Decoder::default();
        let mut bad = bytes;
        bad[4] = 2;
        d.feed(&bad).unwrap();
        assert!(d.next_frame().is_err());
    }
}
