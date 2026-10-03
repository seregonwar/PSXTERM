use crate::i18n::{tr, trf};
use crate::{
    config::Console,
    protocol::{self, *},
};
use anyhow::{Result, bail};
use std::{
    io::{self, Write},
    time::{Duration, Instant},
};

#[derive(Clone, Copy)]
pub struct Action {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
    pub command: Option<&'static str>,
}
impl Action {
    pub fn shortcut(&self) -> &'static str {
        match self.id {
            "new" => "F4",
            "console" => "F3",
            "reconnect" => "F5",
            "split" => "F6",
            "rename" => "F7",
            "close" => "F8",
            "help" => "F1",
            _ => "",
        }
    }
    pub fn category(&self) -> &'static str {
        if self.command.is_some() || matches!(self.id, "ping" | "sessions") {
            "category.remote"
        } else if matches!(self.id, "console" | "add" | "edit" | "remove") {
            "category.console"
        } else if matches!(self.id, "new" | "reconnect" | "rename" | "close") {
            "category.terminal"
        } else {
            "category.interface"
        }
    }
}
pub const ACTIONS: &[Action] = &[
    Action {
        id: "new",
        title: "action.new",
        description: "desc.new",
        command: None,
    },
    Action {
        id: "console",
        title: "action.console",
        description: "desc.console",
        command: None,
    },
    Action {
        id: "add",
        title: "action.add",
        description: "desc.add",
        command: None,
    },
    Action {
        id: "edit",
        title: "action.edit",
        description: "desc.edit",
        command: None,
    },
    Action {
        id: "remove",
        title: "action.remove",
        description: "desc.remove",
        command: None,
    },
    Action {
        id: "reconnect",
        title: "action.reconnect",
        description: "desc.reconnect",
        command: None,
    },
    Action {
        id: "split",
        title: "action.split",
        description: "desc.split",
        command: None,
    },
    Action {
        id: "rename",
        title: "action.rename",
        description: "desc.rename",
        command: None,
    },
    Action {
        id: "pwd",
        title: "action.pwd",
        description: "desc.pwd",
        command: Some("pwd"),
    },
    Action {
        id: "files",
        title: "action.files",
        description: "desc.files",
        command: Some("ls"),
    },
    Action {
        id: "runtime",
        title: "action.runtime",
        description: "desc.runtime",
        command: Some("ls /data/psxterm"),
    },
    Action {
        id: "system",
        title: "action.system",
        description: "desc.system",
        command: Some("uname"),
    },
    Action {
        id: "commands",
        title: "action.commands",
        description: "desc.commands",
        command: Some("help"),
    },
    Action {
        id: "ping",
        title: "action.ping",
        description: "desc.ping",
        command: None,
    },
    Action {
        id: "sessions",
        title: "action.sessions",
        description: "desc.sessions",
        command: None,
    },
    Action {
        id: "clear",
        title: "action.clear",
        description: "desc.clear",
        command: None,
    },
    Action {
        id: "banner",
        title: "action.banner",
        description: "desc.banner",
        command: None,
    },
    Action {
        id: "close",
        title: "action.close",
        description: "desc.close",
        command: None,
    },
    Action {
        id: "help",
        title: "action.help",
        description: "desc.help",
        command: None,
    },
    Action {
        id: "license",
        title: "action.license",
        description: "desc.license",
        command: None,
    },
    Action {
        id: "language",
        title: "action.language",
        description: "desc.language",
        command: None,
    },
];
pub fn filtered(query: &str) -> Vec<usize> {
    let query = query.trim().trim_start_matches('/').to_lowercase();
    let mut matches = Vec::new();
    for (i, a) in ACTIONS.iter().enumerate() {
        let title = tr(a.title).to_lowercase();
        let description = tr(a.description).to_lowercase();
        let text = format!("{} {title} {description}", a.id);
        let mut score = 0;
        let found = query.split_whitespace().all(|word| {
            if a.id == word {
                score += 100;
                true
            } else if a.id.contains(word) || title.contains(word) {
                score += 60;
                true
            } else if description.contains(word) {
                score += 20;
                true
            } else if let Some(gaps) = subsequence(word, a.id) {
                score += 40 - gaps.min(15);
                true
            } else if let Some(gaps) = subsequence(word, &title) {
                score += 20 - gaps.min(15);
                true
            } else if subsequence(word, &text).is_some() {
                score += 1;
                true
            } else {
                false
            }
        });
        if found {
            matches.push((i, score));
        }
    }
    matches.sort_by_key(|(i, score)| (std::cmp::Reverse(*score), *i));
    matches.into_iter().map(|(i, _)| i).collect()
}
fn subsequence(needle: &str, haystack: &str) -> Option<i32> {
    let mut last = 0;
    let mut gaps = 0;
    let mut chars = haystack.chars().enumerate();
    for wanted in needle.chars() {
        let (index, _) = chars.find(|(_, c)| *c == wanted)?;
        gaps += index.saturating_sub(last);
        last = index + 1;
    }
    Some(gaps as i32)
}
pub fn run(console: &Console, id: &str) -> Result<()> {
    let action = ACTIONS
        .iter()
        .find(|a| a.id == id)
        .ok_or_else(|| anyhow::anyhow!(trf("error.flash_unknown", &[id.into()])))?;
    if id == "banner" {
        print!("{}", crate::terminal::banner());
        return Ok(());
    }
    if id == "license" {
        println!("{}", include_str!("../../../LICENSE"));
        return Ok(());
    }
    if id == "help" {
        println!(
            "PSXTERM · {} · GPLv3\n\n{}\n",
            tr("brand.credit"),
            tr("cli.usage")
        );
        for a in ACTIONS.iter().filter(|a| {
            a.command.is_some() || matches!(a.id, "ping" | "sessions" | "banner" | "license")
        }) {
            println!("  {:10} {}", a.id, tr(a.description));
        }
        println!("\n{}", tr("cli.port"));
        return Ok(());
    }
    if action.command.is_none() && !matches!(id, "ping" | "sessions") {
        bail!(tr("error.flash_ui"));
    }
    let (mut stream, sid, _) = protocol::connect(console, 24, 80, None)?;
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let start = Instant::now();
    let result = (|| -> Result<()> {
        let mut out = io::stdout().lock();
        match id {
            "ping" => send_frame(&mut stream, PING, sid, b"flash-ping".to_vec())?,
            "sessions" => send_frame(&mut stream, SESSIONS_REQUEST, sid, vec![])?,
            _ => send_frame(
                &mut stream,
                STDIN,
                sid,
                format!("{}\nexit\n", action.command.unwrap()).into_bytes(),
            )?,
        }
        let mut output = 0usize;
        loop {
            if start.elapsed() > Duration::from_secs(15) {
                bail!(tr("error.timeout"));
            }
            let frame = read_frame(&mut stream)?;
            match frame.kind {
                PONG if id == "ping" && frame.payload == b"flash-ping" => {
                    writeln!(out, "{} · {} ms", console.name, start.elapsed().as_millis())?;
                    break;
                }
                SESSIONS_DATA if id == "sessions" => {
                    output += frame.payload.len();
                    if output > MAX_PAYLOAD {
                        bail!(tr("error.sessions"));
                    }
                    out.write_all(&frame.payload)?;
                }
                SESSIONS_DONE if id == "sessions" => break,
                STDOUT | STDERR if action.command.is_some() => {
                    out.write_all(&frame.payload)?;
                }
                EXIT if frame.payload.len() == 5 && frame.payload[4] == 1 => break,
                CLOSE if action.command.is_some() => break,
                PING => send_frame(&mut stream, PONG, sid, frame.payload)?,
                _ => {}
            }
        }
        out.flush()?;
        Ok(())
    })();
    let _ = send_frame(&mut stream, CLOSE, sid, vec![]);
    result
}
