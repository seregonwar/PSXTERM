//! Clipboard support for terminal clients.
//!
//! A terminal application has no direct access to the system clipboard, and
//! PSXTerm is usually driven from a different machine than the one showing the
//! window. The portable answer is OSC 52: the terminal emulator that displays
//! this client puts the payload into its own clipboard. It is understood by
//! Windows Terminal, iTerm2, kitty, WezTerm, foot and others, and it survives
//! SSH and tmux.

use std::io::{self, Write};

const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Standard base64, padded. Small enough to keep here instead of adding a
/// dependency to a client that must build on every platform.
fn base64(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = *chunk.get(1).unwrap_or(&0) as u32;
        let b2 = *chunk.get(2).unwrap_or(&0) as u32;
        let triple = (b0 << 16) | (b1 << 8) | b2;

        out.push(ALPHABET[((triple >> 18) & 0x3f) as usize] as char);
        out.push(ALPHABET[((triple >> 12) & 0x3f) as usize] as char);
        out.push(if chunk.len() > 1 {
            ALPHABET[((triple >> 6) & 0x3f) as usize] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            ALPHABET[(triple & 0x3f) as usize] as char
        } else {
            '='
        });
    }
    out
}

/// Put `text` into the clipboard of the terminal displaying this client.
/// Returns the number of characters handed over.
pub fn copy(text: &str) -> io::Result<usize> {
    if text.is_empty() {
        return Ok(0);
    }

    // OSC 52 ; clipboard-selector ; base64 payload BEL. The BEL terminator is
    // accepted by every implementation; the sequence is a control sequence, so
    // it does not disturb the rendered screen.
    let sequence = format!("\x1b]52;c;{}\x07", base64(text.as_bytes()));
    let mut out = io::stdout();
    out.write_all(sequence.as_bytes())?;
    out.flush()?;

    Ok(text.chars().count())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_matches_known_vectors() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foob"), "Zm9vYg==");
        assert_eq!(base64(b"fooba"), "Zm9vYmE=");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn copy_reports_character_count() {
        assert_eq!(copy("").expect("empty copy"), 0);
        assert_eq!(copy("ciao").expect("copy"), 4);
    }
}
