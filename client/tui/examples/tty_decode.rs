//! Decode a smoke-test PTY transcript into its current screen for assertions.
use anyhow::{Context, Result};
use std::io::{Read, Write};

fn main() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let cols = args.next().context("columns required")?.parse::<u16>()?;
    let rows = args.next().context("rows required")?.parse::<u16>()?;
    anyhow::ensure!(cols > 0 && rows > 0, "nonzero dimensions required");
    let mut bytes = Vec::new();
    std::io::stdin()
        .lock()
        .take(8 * 1024 * 1024)
        .read_to_end(&mut bytes)?;
    let mut parser = vt100::Parser::new(rows, cols, 0);
    parser.process(&bytes);
    std::io::stdout().write_all(parser.screen().contents().as_bytes())?;
    Ok(())
}
