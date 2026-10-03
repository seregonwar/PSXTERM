# PSXTerm workspace client

The Ratatui client is a separate portable executable, `psxterm-tui`. It talks
PTTY/1 to an **already running PSXTerm daemon**. It runs natively on Windows;
Linux and macOS are included in the CI matrix. The existing C client remains
available for scripts, diagnostics and file transfers.

## Build and start

Use Rust 1.88 or newer and a terminal that supports UTF-8 and ANSI colors.

```console
cargo build --release --locked --manifest-path client/tui/Cargo.toml
cargo run --release --manifest-path client/tui/Cargo.toml -- --lang it
```

On Windows the executable is `client\tui\target\release\psxterm-tui.exe`.
On Linux/macOS it is `client/tui/target/release/psxterm-tui`.

For a first connection:

```console
psxterm-tui --host 192.168.1.20 --port 2323 --name PS5 --lang it
```

**2323 is the usual PSXTerm service port; 9021 is the payload loader port.**
Connecting to the loader does not start PSXTerm. This client has no loader,
injection or platform setup functions. If the service is stopped, the client
shows an actionable connection error while the interface stays responsive.

## Workspace

- Top-left dropdown: select a saved console or add a new profile.
- Left sidebar: the selected console and its address, flash palette, common
  actions and active transport details. Credits appear in each terminal's banner.
- Right side: terminal tabs with unread output indicators and a **× close button**.
  F6 switches to a split view with up to four panes per page; each pane has its
  own × and page controls replace the tab bar to avoid repeating terminal names.
  Closing requires confirmation and targets that terminal, including an inactive one.
  Narrow windows use a single pane or a vertical pair; wider windows use a
  horizontal pair or a grid. Sessions remain open when fewer panes fit.
- Below 72 columns the sidebar becomes a compact rail. F9 opens its full drawer;
  F10 toggles the sidebar manually. Short windows use flat tabs and shorter
  controls to give more rows to the terminal. Tab overflow has arrows
  and always keeps the selected tab visible.
- Switching consoles preserves all open connections and remembers the selected
  terminal on each console. Limits are eight terminals per console, 32 total,
  and 32 console profiles. The daemon may enforce a lower session limit.
- Every terminal starts with a colored PSXTERM banner, credits to **seregonwar**
  and a GPLv3 license notice. The palette also exposes the complete original
  license text, including its no-warranty terms.
- The interface uses a restrained charcoal and gray palette. Only the PSXTERM
  logo uses a gradient; program output retains its original ANSI colors.
  Connection state appears once in the terminal footer, console details in the
  sidebar, and keyboard hints in the workspace footer.
- ANSI colors, cursor movement, Unicode, combining characters, application
  cursor keys and bracketed paste are interpreted with `vt100`. Scrollback is
  bounded to 5,000 lines per terminal. This is a terminal client, not a complete
  implementation of every xterm extension.

| Key | Action |
|---|---|
| F1 | Help |
| F2 | Searchable flash palette |
| F3 | Console dropdown |
| F4 | New independent terminal |
| F5 | Retry connection / resume suspended session |
| F6 | Tabs / split view |
| F7 | Rename current terminal |
| F8 | Close current terminal, after confirmation |
| F9 | Move focus to/from the sidebar |
| F10 | Toggle full sidebar / compact rail |
| Alt+1…8, Ctrl+PageUp/PageDown | Switch terminal |
| Shift+PageUp/PageDown, mouse wheel | Scroll local history |
| Ctrl+End | Return to live output |
| Ctrl+Q | Quit, after confirming that all open sessions will end |

In sidebar focus, use arrows to select, Enter to return to the terminal, or N
for a new terminal. Mouse clicks select profiles, tabs, panes and actions.
Tab, Esc and Ctrl+C reach the remote program when no dialog is open. Ctrl+D
sends the protocol's explicit EOF marker. Multiline paste requires confirmation;
the profile form also accepts paste and masks the token field.

F5 reuses the session ID and private resume token while this client process is
running. A daemon restart, expired session or unsupported resume operation is
reported instead of silently creating another session. Resume tokens are not
saved. Normal quit and explicit close send CLOSE; abnormal connection loss may
leave a suspended session until the daemon's retention timeout expires.

Closing a tab also cancels an in-flight connection. Before OPEN/ATTACH it stops
without creating a shell; after sending OPEN/ATTACH it sends the appropriate
control frame even if the reply is incomplete. Closed tabs disappear immediately,
but their workers remain tracked until shutdown completes. The global limit of
32 includes connections still closing. Handshake replies have a five-second total
deadline and are read incrementally so cancellation does not wait for that timeout.

## Flash commands

Flash commands offer a short path to common read-only shell actions. In the UI
they open a **dedicated terminal** so they do not type into an existing command
or foreground application. Local actions such as rename, clear view, help,
console editing and language selection work through the same palette.
The palette accepts words, abbreviations and `/command` IDs, ranks name matches
first, displays shortcuts, and explains unavailable actions. Use arrows,
PageUp/PageDown, Home/End or the mouse wheel to move; descriptions wrap in a
separate detail area. Confirmations and profile saving also have mouse buttons.

The palette, contextual hints and collapsible sidebar take inspiration from
[OpenCode's TUI interaction patterns](https://opencode.ai/docs/tui/), implemented
here in Rust/Ratatui for console management and independent terminal sessions.

The command line supports the same useful remote shortcuts:

```console
psxterm-tui flash help
psxterm-tui --host 192.168.1.20 flash ping
psxterm-tui --console PS5 --lang it flash system
psxterm-tui --console PS5 flash files
psxterm-tui --console PS5 flash pwd
psxterm-tui --console PS5 flash commands
psxterm-tui --console PS5 flash runtime
psxterm-tui --console PS5 flash sessions
psxterm-tui flash license
```

`files` runs `ls`, `system` runs `uname`, `commands` runs `help`, and `runtime`
runs `ls /data/psxterm`. CLI flashes create a short-lived session and close it
after collecting their output. No command string supplied by a user is assembled
into these shortcuts. The command IDs remain the same in every language.

## Language and profiles

English is the initial default. Select `--lang en` or `--lang it`, or use
F2 → `language` to switch and save the language preference. UI, client errors,
help, confirmations, banner notices and action descriptions are stored in
`client/tui/locales/en.json` and `it.json`, not hardcoded in the UI. Remote shell
output, standard OS/library error details and the original GPL text retain their
original language. Existing terminal history is not rewritten on a language change.

Profiles and language are written atomically to the platform's user configuration
directory (`ProjectDirs` for `dev / seregonwar / PSXTerm`). Override with
`--config PATH`. Starting the UI with `--host` saves that profile; CLI flashes
do not modify the profile file. Editing a profile affects new connections and
reconnections, while existing terminals keep their current connection.

Tokens entered in the profile form live only in memory. By default the client
also reads `PSXTERM_TOKEN`; an optional `token_env` field in a profile can name
a different environment variable for that console. Neither access tokens nor
resume tokens are written by the client to disk. PTTY/1 shared-token
authentication does not encrypt transport; keep this service on a trusted network.

## Preview and verification

`--demo` runs a clearly marked workspace with sample output and **no network
connections**, without reading or writing saved profiles. Snapshots render the
actual Ratatui buffer to SVG and imply demo mode:

```console
psxterm-tui --demo --lang it
psxterm-tui --lang it --snapshot preview.svg --snapshot-view split
psxterm-tui --lang it --snapshot compact.svg --snapshot-cols 80 --snapshot-rows 24
cargo test --locked --manifest-path client/tui/Cargo.toml
cargo clippy --locked --manifest-path client/tui/Cargo.toml --all-targets -- -D warnings
```

Snapshot views: `tabs`, `split`, `consoles`, `palette`, `form`.
Use `--snapshot-terminals 8` to preview tab overflow. The minimum workspace size
is 55×18 cells; smaller windows display a resize hint and preserve sessions.
Tests cover framing, streamed binary output, ANSI rendering, keyboard sequences,
profile privacy and replacement, locale parity, console switching, flash
isolation, confirmation flows and layouts including small windows.

UX checks cover 55×18, 60×20, 72×20, 80×24, 100×30, 120×35, 140×44 and 200×60
cells, including selecting the first and eighth terminal, resizing an open
workspace, dropdowns, palette, forms and confirmations. Tests also verify mouse
hit regions stay inside the screen, compact drawer navigation, token masking,
disabled actions, close buttons on active and inactive terminals, cancellation,
unique metadata and scrolling the complete wrapped license. Native Windows
terminal smoke testing covered opening the palette, dismissing it, confirming
quit and restoring the terminal.

The optional real-daemon test requires the host daemon running on localhost:

```console
PSXTERM_TEST_PORT=29323 cargo test --manifest-path client/tui/Cargo.toml --test network actual_host -- --ignored
```

PowerShell equivalent: set `$env:PSXTERM_TEST_PORT='29323'` before the Cargo command.
Local Windows-to-WSL validation covered multiple independent sessions, output,
resize, ping and detach/resume with the same server session ID. **This is host
validation, not PS4/PS5 hardware validation.**
Cancellation tests cover fragmented HELLO/OPEN/ATTACH replies, close and detach,
and waiting for connections belonging to removed tabs. The real-daemon test also
withholds OPEN_OK after creating a session, cancels the client and verifies the
closed session cannot be resumed.
