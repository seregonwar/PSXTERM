//! Deterministic, network-free previews of the actual Ratatui buffer.
use crate::{app::App, ui};
use anyhow::Result;
use ratatui::{
    Terminal,
    backend::TestBackend,
    style::{Color, Modifier},
};
use std::{fmt::Write, path::Path};
fn escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}
fn css(c: Color) -> String {
    match c {
        Color::Rgb(r, g, b) => format!("#{r:02x}{g:02x}{b:02x}"),
        Color::Indexed(i) => {
            if i < 16 {
                [
                    "#101827", "#d85c70", "#76d6ad", "#f2b676", "#6ea6ff", "#c19bff", "#65d6e8",
                    "#dbe5f5", "#879ab8", "#f38da6", "#9ee5c5", "#ffdbac", "#a2c7ff", "#d6bdff",
                    "#a1edf4", "#ffffff",
                ][i as usize]
                    .into()
            } else if i < 232 {
                let n = i - 16;
                let c = |v| if v == 0 { 0 } else { 55 + 40 * v };
                format!("#{:02x}{:02x}{:02x}", c(n / 36), c(n / 6 % 6), c(n % 6))
            } else {
                let v = 8 + (i - 232) * 10;
                format!("#{v:02x}{v:02x}{v:02x}")
            }
        }
        Color::Reset => "#dbe5f5".into(),
        _ => "#dbe5f5".into(),
    }
}
pub fn save(app: &mut App, path: &Path) -> Result<()> {
    save_size(app, path, 140, 44)
}
pub fn save_size(app: &mut App, path: &Path, cols: u16, rows: u16) -> Result<()> {
    let mut terminal = Terminal::new(TestBackend::new(cols, rows))?;
    terminal.draw(|f| ui::draw(f, app))?;
    let buffer = terminal.backend().buffer();
    let mut accessible = String::new();
    for y in 0..rows {
        for x in 0..cols {
            accessible.push_str(buffer[(x, y)].symbol());
        }
        accessible.push('\n');
    }
    let width = u32::from(cols) * 10;
    let height = u32::from(rows) * 20;
    let mut svg = format!(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{width}\" height=\"{height}\" viewBox=\"0 0 {width} {height}\"><title>PSXTERM Ratatui demo · no connection</title><rect width=\"{width}\" height=\"{height}\" fill=\"#101827\"/><g font-family=\"Cascadia Mono,DejaVu Sans Mono,monospace\" font-size=\"16\">"
    );
    let _ = write!(svg, "<desc>{}</desc>", escape(&accessible));
    for y in 0..rows {
        for x in 0..cols {
            let cell = &buffer[(x, y)];
            let _ = write!(
                svg,
                "<rect x=\"{}\" y=\"{}\" width=\"10\" height=\"20\" fill=\"{}\"/>",
                x * 10,
                y * 20,
                css(cell.bg)
            );
        }
    }
    // Paint all backgrounds first, so following cells cannot cover wide glyphs.
    for y in 0..rows {
        for x in 0..cols {
            let cell = &buffer[(x, y)];
            if cell.symbol() != " " {
                let _ = write!(
                    svg,
                    "<text x=\"{}\" y=\"{}\" fill=\"{}\"{}>{}</text>",
                    x * 10,
                    y * 20 + 16,
                    css(cell.fg),
                    if cell.modifier.contains(Modifier::BOLD) {
                        " font-weight=\"bold\""
                    } else {
                        ""
                    },
                    escape(cell.symbol())
                );
            }
        }
    }
    svg.push_str("</g></svg>");
    std::fs::write(path, svg)?;
    Ok(())
}
