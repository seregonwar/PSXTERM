//! Plain-text terminal exports. File I/O runs outside the UI event loop.
use std::{
    io::{self, Write},
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread::{self, JoinHandle},
    time::{SystemTime, UNIX_EPOCH},
};

pub const MAX_PATH_CHARS: usize = 4096;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Scope {
    Visible,
    #[default]
    History,
}
impl Scope {
    pub fn toggle(self) -> Self {
        match self {
            Self::Visible => Self::History,
            Self::History => Self::Visible,
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Self::Visible => "export.visible",
            Self::History => "export.history",
        }
    }
}

pub struct Dialog {
    pub pane: u64,
    pub title: String,
    pub path: crate::input::Input,
    pub scope: Scope,
    pub field: usize,
    pub error: String,
}
impl Dialog {
    pub fn append_path(&mut self, text: &str) {
        if self.path.insert(text) {
            self.error.clear();
        }
    }
}

pub fn suggested_path(pane: u64) -> io::Result<PathBuf> {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis();
    Ok(std::env::current_dir()?.join(format!("psxterm-terminal-{pane}-{stamp}.txt")))
}

/// Serialize a captured screen without UI controls, ANSI escapes or any profile
/// metadata. Consumes the clone so serialization needs no second history copy.
pub fn text(mut screen: vt100::Screen, scope: Scope) -> String {
    let (rows, cols) = screen.size();
    if rows == 0 || cols == 0 {
        return String::new();
    }
    let bottom = match scope {
        Scope::Visible => screen.scrollback(),
        Scope::History => {
            screen.set_scrollback(usize::MAX);
            0
        }
    };
    let top = screen.scrollback() + usize::from(rows) - 1;
    let mut output = String::new();
    for line in (bottom..=top).rev() {
        screen.set_scrollback(line.saturating_sub(usize::from(rows) - 1));
        let row = usize::from(rows) - 1 - (line - screen.scrollback());
        let mut content = String::new();
        let mut written_end = 0;
        for col in 0..cols {
            if let Some(cell) = screen.cell(row as u16, col)
                && !cell.is_wide_continuation()
            {
                content.push_str(if cell.has_contents() {
                    cell.contents()
                } else {
                    " "
                });
                if cell.has_contents() {
                    written_end = content.len();
                }
            }
        }
        let wraps = line != bottom && screen.row_wrapped(row as u16);
        content.truncate(if wraps {
            written_end
        } else {
            content.trim_end_matches(' ').len()
        });
        output.push_str(&content);
        if !wraps {
            output.push('\n');
        }
    }
    output.truncate(output.trim_end_matches('\n').len());
    if !output.is_empty() {
        output.push('\n');
    }
    output
}

#[derive(Debug)]
pub enum Error {
    Exists,
    InvalidPath,
    Cancelled,
    WorkerStopped,
    Io(io::Error),
}
impl Error {
    pub fn message(&self) -> String {
        use crate::i18n::{tr, trf};
        match self {
            Self::Exists => tr("export.exists"),
            Self::InvalidPath => tr("export.invalid_path"),
            Self::Cancelled => tr("export.cancelled"),
            Self::WorkerStopped => tr("export.worker_stopped"),
            Self::Io(e) => trf("export.failed", &[e.to_string()]),
        }
    }
}

pub struct Saved {
    pub path: PathBuf,
    pub bytes: usize,
}

fn write(path: PathBuf, contents: String, cancelled: &AtomicBool) -> Result<Saved, Error> {
    let check = || {
        if cancelled.load(Ordering::Acquire) {
            Err(Error::Cancelled)
        } else {
            Ok(())
        }
    };
    check()?;
    if path.file_name().is_none() || path.as_os_str().is_empty() {
        return Err(Error::InvalidPath);
    }
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    std::fs::create_dir_all(parent).map_err(Error::Io)?;
    let mut temp = tempfile::NamedTempFile::new_in(parent).map_err(Error::Io)?;
    check()?;
    temp.write_all(contents.as_bytes()).map_err(Error::Io)?;
    temp.as_file().sync_all().map_err(Error::Io)?;
    check()?;
    temp.persist_noclobber(&path).map_err(|e| {
        if e.error.kind() == io::ErrorKind::AlreadyExists {
            Error::Exists
        } else {
            Error::Io(e.error)
        }
    })?;
    Ok(Saved {
        path,
        bytes: contents.len(),
    })
}

pub struct Job {
    rx: mpsc::Receiver<Result<Saved, Error>>,
    worker: JoinHandle<()>,
    cancelled: Arc<AtomicBool>,
}
impl Job {
    pub fn start(screen: vt100::Screen, scope: Scope, path: PathBuf) -> io::Result<Self> {
        Self::spawn(move |cancelled| {
            if cancelled.load(Ordering::Acquire) {
                return Err(Error::Cancelled);
            }
            let contents = text(screen, scope);
            write(path, contents, cancelled)
        })
    }
    pub(crate) fn spawn(
        task: impl FnOnce(&AtomicBool) -> Result<Saved, Error> + Send + 'static,
    ) -> io::Result<Self> {
        let (tx, rx) = mpsc::channel();
        let cancelled = Arc::new(AtomicBool::new(false));
        let flag = Arc::clone(&cancelled);
        let worker = thread::Builder::new()
            .name("psxterm-export".into())
            .spawn(move || {
                let _ = tx.send(task(&flag));
            })?;
        Ok(Self {
            rx,
            worker,
            cancelled,
        })
    }
    pub fn poll(&self) -> Option<Result<Saved, Error>> {
        match self.rx.try_recv() {
            Ok(result) => Some(result),
            Err(mpsc::TryRecvError::Empty) => None,
            Err(mpsc::TryRecvError::Disconnected) => Some(Err(Error::WorkerStopped)),
        }
    }
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }
    pub fn finished(&self) -> bool {
        self.worker.is_finished()
    }
}
impl Drop for Job {
    fn drop(&mut self) {
        self.cancel();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn text_preserves_history_unicode_wrapping_and_the_captured_view() {
        let mut parser = vt100::Parser::new(3, 10, 20);
        parser.process("oldest\r\n\x1b[34m界é\x1b[0m\r\n123456789 tail\r\nlast".as_bytes());
        parser.screen_mut().set_scrollback(2);
        let view = parser.screen().contents_formatted();
        let history = text(parser.screen().clone(), Scope::History);
        assert_eq!(history, "oldest\n界é\n123456789 tail\nlast\n");
        let visible = text(parser.screen().clone(), Scope::Visible);
        assert_eq!(visible, "oldest\n界é\n123456789\n");
        assert_eq!(parser.screen().contents_formatted(), view);
        assert_eq!(parser.screen().scrollback(), 2);
    }

    #[test]
    fn history_export_includes_exactly_the_rows_still_retained() {
        let mut parser = vt100::Parser::new(3, 80, 5000);
        for n in 0..6100 {
            parser.process(format!("line{n}\r\n").as_bytes());
        }
        let history = text(parser.screen().clone(), Scope::History);
        assert_eq!(history.lines().count(), 5002);
        assert_eq!(history.lines().next(), Some("line1098"));
        assert_eq!(history.lines().last(), Some("line6099"));
    }

    #[test]
    fn existing_files_and_cancelled_exports_stay_untouched() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("output.txt");
        std::fs::write(&path, "existing").unwrap();
        let flag = AtomicBool::new(false);
        assert!(matches!(
            write(path.clone(), "replacement".into(), &flag),
            Err(Error::Exists)
        ));
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "existing");
        flag.store(true, Ordering::Release);
        let cancelled = dir.path().join("cancelled.txt");
        assert!(matches!(
            write(cancelled.clone(), "text".into(), &flag),
            Err(Error::Cancelled)
        ));
        assert!(!cancelled.exists());
        assert_eq!(std::fs::read_dir(dir.path()).unwrap().count(), 1);
    }

    #[test]
    fn background_export_creates_utf8_paths_and_reports_io_failures() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("folder with spaces/出力.txt");
        let mut parser = vt100::Parser::new(3, 10, 20);
        parser.process("界é".as_bytes());
        let job = Job::start(parser.screen().clone(), Scope::History, path.clone()).unwrap();
        let saved = job
            .rx
            .recv_timeout(std::time::Duration::from_secs(3))
            .unwrap()
            .unwrap();
        assert_eq!(saved.bytes, "界é\n".len());
        assert_eq!(std::fs::read_to_string(path).unwrap(), "界é\n");
        let blocker = dir.path().join("file");
        std::fs::write(&blocker, "blocker").unwrap();
        let job = Job::start(
            parser.screen().clone(),
            Scope::Visible,
            blocker.join("out.txt"),
        )
        .unwrap();
        assert!(matches!(
            job.rx
                .recv_timeout(std::time::Duration::from_secs(3))
                .unwrap(),
            Err(Error::Io(_))
        ));
    }

    #[test]
    fn a_pending_job_can_be_polled_and_cancelled_without_blocking() {
        let (gate_tx, gate_rx) = mpsc::channel();
        let job = Job::spawn(move |cancelled| {
            gate_rx.recv().unwrap();
            assert!(cancelled.load(Ordering::Acquire));
            Err(Error::Cancelled)
        })
        .unwrap();
        assert!(job.poll().is_none());
        assert!(!job.finished());
        job.cancel();
        gate_tx.send(()).unwrap();
        assert!(matches!(
            job.rx
                .recv_timeout(std::time::Duration::from_secs(3))
                .unwrap(),
            Err(Error::Cancelled)
        ));
    }
}
