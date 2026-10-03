use anyhow::Result;
use clap::{Parser, Subcommand};
use crossterm::{
    event::{
        self, DisableBracketedPaste, DisableMouseCapture, EnableBracketedPaste, EnableMouseCapture,
    },
    execute,
};
use psxterm_tui::{
    app::{App, Overlay},
    config::{self, Config, Console},
    i18n::{self, Language, tr, trf},
    ui,
};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

#[derive(Parser)]
#[command(
    name = "psxterm-tui",
    version,
    disable_help_flag = true,
    disable_help_subcommand = true
)]
struct Args {
    #[arg(long, short = 'h')]
    help: bool,
    #[arg(long, global = true)]
    host: Option<String>,
    #[arg(long, short = 'p', global = true, default_value_t = 2323)]
    port: u16,
    #[arg(long, global = true)]
    console: Option<String>,
    #[arg(long, global = true)]
    name: Option<String>,
    #[arg(long, global = true, value_enum)]
    lang: Option<Language>,
    #[arg(long, global = true)]
    config: Option<PathBuf>,
    #[arg(long)]
    demo: bool,
    #[arg(long)]
    snapshot: Option<PathBuf>,
    #[arg(long,default_value="tabs",value_parser=["tabs","split","palette","consoles","form"])]
    snapshot_view: String,
    #[arg(long,default_value_t=140,value_parser=clap::value_parser!(u16).range(40..=240))]
    snapshot_cols: u16,
    #[arg(long,default_value_t=44,value_parser=clap::value_parser!(u16).range(12..=80))]
    snapshot_rows: u16,
    #[arg(long,default_value_t=2,value_parser=clap::value_parser!(u16).range(1..=8))]
    snapshot_terminals: u16,
    #[command(subcommand)]
    command: Option<Mode>,
}
#[derive(Subcommand)]
enum Mode {
    Flash {
        #[arg(default_value = "help")]
        action: String,
    },
}

struct TerminalGuard;
impl Drop for TerminalGuard {
    fn drop(&mut self) {
        let _ = execute!(
            std::io::stdout(),
            DisableBracketedPaste,
            DisableMouseCapture
        );
        ratatui::restore();
    }
}
fn main() {
    if let Err(e) = run() {
        eprintln!("PSXTERM: {e:#}");
        std::process::exit(1);
    }
}
fn run() -> Result<()> {
    let args = Args::parse();
    if let Some(lang) = args.lang {
        i18n::set(lang);
    }
    if args.help {
        println!("{}", tr("cli.help"));
        return Ok(());
    }
    let demo = args.demo || args.snapshot.is_some();
    let config_path = args.config.unwrap_or(config::default_path()?);
    let mut config = if demo {
        demo_config()
    } else {
        Config::load(&config_path)?
    };
    if let Some(lang) = args.lang {
        config.language = lang;
    }
    i18n::set(config.language);
    let mut selected = config.consoles.first().map(|c| c.id);
    if let Some(name) = args.console {
        selected = Some(
            config
                .consoles
                .iter()
                .find(|c| c.name.eq_ignore_ascii_case(&name))
                .ok_or_else(|| anyhow::anyhow!(trf("cli.console_unknown", &[name])))?
                .id,
        );
    }
    if let Some(host) = args.host {
        let host = host.trim_matches(['[', ']']).to_owned();
        if let Some(c) = config
            .consoles
            .iter()
            .find(|c| c.host == host && c.port == args.port)
        {
            selected = Some(c.id);
        } else {
            if config.consoles.len() >= 32 {
                anyhow::bail!(tr("error.console_limit"));
            }
            let c = Console {
                id: config.next_id(),
                name: args.name.unwrap_or_else(|| host.clone()),
                host,
                port: args.port,
                token: None,
                token_env: None,
            };
            c.validate()?;
            selected = Some(c.id);
            config.consoles.push(c);
        }
    }
    if let Some(Mode::Flash { action }) = args.command {
        let c = config
            .consoles
            .iter()
            .find(|c| Some(c.id) == selected)
            .cloned();
        if matches!(action.as_str(), "help" | "license" | "banner") {
            return psxterm_tui::flash::run(&c.unwrap_or_else(dummy_console), &action);
        }
        return psxterm_tui::flash::run(
            &c.ok_or_else(|| anyhow::anyhow!(tr("cli.no_console")))?,
            &action,
        );
    }
    if !demo {
        config.save(&config_path)?;
    }
    let mut app = App::new(config, config_path, demo);
    if let Some(id) = selected {
        app.switch(id);
        app.new_pane()?;
    }
    if demo && (args.snapshot.is_none() || args.snapshot_terminals > 1) {
        let first = app.active;
        app.new_pane()?;
        if let Some(p) = app.current_mut() {
            p.title = tr("demo.logs");
        }
        if let Some(id) = first {
            app.select(id);
        }
    }
    if let Some(path) = args.snapshot {
        while app.panes.len() < args.snapshot_terminals as usize {
            app.new_pane()?;
        }
        let first = app.panes.first().map(|p| p.id);
        if let Some(id) = first {
            app.select(id);
        }
        match args.snapshot_view.as_str() {
            "split" => app.split = true,
            "palette" => {
                app.overlay = Overlay::Palette {
                    query: String::new(),
                    selected: 0,
                }
            }
            "consoles" => app.action("console"),
            "form" => app.action("add"),
            _ => {}
        }
        psxterm_tui::snapshot::save_size(&mut app, &path, args.snapshot_cols, args.snapshot_rows)?;
        println!("{}", path.display());
        return Ok(());
    }
    let mut terminal = ratatui::try_init()?;
    let guard = TerminalGuard;
    execute!(std::io::stdout(), EnableMouseCapture, EnableBracketedPaste)?;
    let result = (|| -> Result<()> {
        while !app.quit {
            app.poll();
            terminal.draw(|f| ui::draw(f, &mut app))?;
            if event::poll(Duration::from_millis(33))? {
                app.handle(event::read()?);
            }
        }
        Ok(())
    })();
    drop(guard);
    // Keep workers alive long enough to deliver explicit CLOSE on normal quit.
    if app.quit {
        let deadline = Instant::now() + Duration::from_secs(4);
        while Instant::now() < deadline
            && app
                .panes
                .iter()
                .any(|p| p.connection.as_ref().is_some_and(|c| !c.finished()))
        {
            std::thread::sleep(Duration::from_millis(20));
        }
    }
    result
}
fn dummy_console() -> Console {
    Console {
        id: 1,
        name: "PSXTerm".into(),
        host: "localhost".into(),
        port: 2323,
        token: None,
        token_env: None,
    }
}
fn demo_config() -> Config {
    Config {
        version: 1,
        language: Language::En,
        consoles: vec![
            Console {
                id: 1,
                name: "PS5 · Studio".into(),
                host: "192.168.1.20".into(),
                port: 2323,
                token: None,
                token_env: None,
            },
            Console {
                id: 2,
                name: "PS4 · Lab".into(),
                host: "192.168.1.21".into(),
                port: 2323,
                token: None,
                token_env: None,
            },
        ],
    }
}
