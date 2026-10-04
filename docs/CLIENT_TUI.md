# PSXTerm workspace client

The Ratatui client is a separate portable executable, `psxterm-tui`. It talks
PTTY/1 to an **already running PSXTerm daemon**. Windows and Linux (WSL) have
been tested locally; macOS is included in the CI matrix. The existing C client remains
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
- The interface uses a restrained charcoal and gray palette. The PSXTERM
  logo uses a gradient. By default, the 16 basic ANSI foreground colors use a
  readable palette with at least 4.5:1 contrast against the terminal's default
  charcoal background. This makes directory names and executable files in `ls`
  easier to distinguish. RGB colors, extended indexed colors, backgrounds and
  text attributes retain their original values. Custom backgrounds and dim text
  can have different contrast. Use F2 → `/colors` to switch to original colors,
  or start with `--output-colors original`; `readable` restores the default.
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
| F11 | Search terminal output and retained history locally |
| F12 | Export terminal text to a local file |
| Alt+1…8, Ctrl+PageUp/PageDown | Switch terminal |
| Shift+PageUp/PageDown, mouse wheel | Scroll local history |
| Ctrl+End | Return to live output |
| Mouse drag, then release | Select and copy terminal text |
| Ctrl+Shift+C | Copy selection, or the visible terminal when nothing is selected |
| Ctrl+Q | Quit, after confirming that all open sessions will end |

In sidebar focus, use arrows to select, Enter to return to the terminal, or N
for a new terminal. Mouse clicks select profiles, tabs, panes and actions.
Tab, Esc and Ctrl+C reach the remote program when no dialog is open. Ctrl+D
sends the protocol's explicit EOF marker. Multiline paste requires confirmation;
the profile form also accepts paste and masks the token field.

Copy reads terminal cells directly, excluding workspace borders and controls.
It preserves Unicode, combining characters and the order of multiple lines,
including retained history. Automatic line wrapping does not add a newline to
the copied text. Scrolling preserves the selection; new output in its terminal
or a change in pane dimensions cancels it to avoid copying shifted content.
Clipboard delivery uses OSC 52 and requires support from the hosting terminal.

The workspace redraws when input, output or connection state changes. It
coalesces bursts into frames, with a normal maximum of about 60 frames per
second; clicks and resizing can refresh mouse targets immediately. An unchanged
workspace does not redraw during idle polling.

Parsing busy connections also yields to input. One update shares a 6 ms,
64-event and 256 KiB budget across the workspace; an individual frame already
started may finish beyond those limits. Each pane handles at most eight events
or 64 KiB of output before yielding (finishing the current frame), and the next update starts with the
next pane. Unconsumed events stay in their bounded FIFO; output is not dropped.
When the output queue is full, the connection worker still forwards input and
resize commands, so a command such as Ctrl+C does not wait for it to drain.

The queued-output regression test fills eight terminals with 64 mixed-size
frames each. In a local Windows release run, the first update fell from about
175 ms before this change to 3.6 ms afterwards; draining the whole burst took
about 179 ms. These are measurements for that synthetic workload and machine,
not a timing guarantee. Reproduce the current measurement with:

```console
cargo test --release --locked --manifest-path client/tui/Cargo.toml --lib busy_poll_yields -- --nocapture
```

In split view, the mouse wheel scrolls the terminal under the pointer and keeps
keyboard focus in the selected terminal. The wheel over workspace controls
does not scroll a terminal. New output preserves the history viewport being
read until those rows are evicted from the retained history.

Dialog fields share a UTF-8-safe editor: Left/Right moves the caret, Home/End
or Ctrl+A/E goes to the beginning/end, Backspace/Delete removes behind/ahead,
and Ctrl+U clears the field. Paste inserts at the caret. Clicking inside a
field places the caret in its visible text; clipped fields keep it on screen.
Token fields remain masked while editing and scrolling. Name limits are 48
characters for consoles and 32 for terminals; host/token limits are 253/255
UTF-8 bytes, and ports accept at most five characters. Limits apply equally to
typing and paste, without splitting a Unicode character.

F11 or F2 → `/search` opens a search bar below the workspace. Type or paste a
literal phrase; search ignores case and includes the terminal's retained
history. It joins rows marked as automatically wrapped by the terminal parser,
keeps explicit spaces and highlights complete wide/combining character cells.
The most recent match is selected first. Enter/Down moves forward, Shift+Enter/Up
moves backward, and Home/End selects the oldest/newest result. The arrow buttons
provide the same navigation with the mouse. Left/Right and mouse clicks edit
the query; Ctrl+Home/End or Ctrl+A/E moves its caret to the beginning/end.
The palette uses the same distinction between result and caret navigation.
Moving the search caret preserves the selected match and scroll position,
including when output has changed; an actual text edit refreshes stale results.
Ctrl+U clears the query; Esc, F11
or × closes the bar and leaves the current scroll position in place. Ctrl+End
after closing the bar returns to live output. F11 is reserved locally; Ctrl+F still reaches the
remote program when no dialog is open.

Search indexes the terminal only on opening, editing after output changes, or
refresh/navigation after output changes. It does not rescan history on each
frame or network event. New output or a pane resize marks results stale and
removes their highlights. F5, Enter or the refresh button updates them; F5 in
the search bar never reconnects. Limits are 256 query characters and the latest
10,000 non-overlapping matches; the bar reports a truncated result set. Queries
are kept only for the open bar and are not saved or sent to the console.
Case matching uses Unicode lowercase conversion without accent normalization.
As with copying, line boundaries follow `vt100`; a wide character moved past
an empty final column is treated as a separate row by that parser.

F12 or F2 → `/export` opens a file dialog for the active terminal. Choose all
retained history (the default), or only the currently visible rows, including
the scrolled viewport. The file is a UTF-8 text snapshot captured when you save;
it contains terminal cells without ANSI formatting, workspace borders or
profile metadata. This is not a continuous recording: overwritten cells and
evicted history cannot be recovered. Line wrapping follows the same parser
rules as copying/search, and unused trailing blank rows are omitted.

The suggested filename is unique to the terminal and time, in the current
working directory. Enter a relative or absolute path; missing parent folders
are created on save. Paths can contain spaces and Unicode and are treated
literally (no shell, environment-variable or `~` expansion). Left/Right,
Home/End, Backspace and Delete edit the path; Ctrl+U clears it; paste inserts
at the cursor. Tab switches focus to the content choice, arrows/Space switch
its value, and mouse clicks position the path caret or select content and Save/Cancel.
Enter saves from either field. Paths are limited to 4,096 characters, subject
to the filesystem's own limits.

Serialization and file I/O run in one background worker while terminal input
and output continue. Existing files are never overwritten, including when a
destination appears during saving. Completion reports the path and byte count.
After an error, F12 retains the path and scope for retry without interrupting
the workspace with a new dialog. Only one export runs at a time. Closing its
terminal leaves the captured export running. Quitting requests cooperative
cancellation before committing the file and waits for workers up to the
client's shutdown deadline; a file already committed remains saved. Demo mode
also supports an explicitly saved local export, without writing console profiles.

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
console editing, language selection and output color selection work through the
same palette.
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

Profiles, language and output color preference are written atomically to the platform's user configuration
directory (`ProjectDirs` for `dev / seregonwar / PSXTerm`). Override with
`--config PATH`. Starting the UI with `--host` saves that profile; CLI flashes
do not modify the profile file. Editing a profile affects new connections and
reconnections, while existing terminals keep their current connection.
Existing profile files without a color preference use `readable`. Demo mode
allows changing the palette for its current run without saving a preference.

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

Snapshot views: `tabs`, `split`, `consoles`, `palette`, `form`, `search`, `export`.
The search preview finds `projects` in the demo directory listing.
The export preview opens its dialog without saving terminal text.
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

The Linux build also has an executable smoke test using a real POSIX PTY at all
eight sizes. It decodes captured ANSI output with `vt100` and checks the actual
screens for the flash palette, dismissing it, the small-window hint, restoring
dimensions, both color modes, middle-of-query editing, Unicode terminal renaming,
search/paste/no-match/resize/refresh, split view,
export/paste/content selection/no-overwrite/retry/cancel, closing/cancelling
and quit confirmation.
It also checks that an unchanged workspace emits no idle redraw output. It then verifies
the alternate screen and original terminal attributes are restored. Demo mode
keeps this test independent of console availability. Run on Linux with:

```console
cargo build --release --locked --manifest-path client/tui/Cargo.toml
cargo build --release --locked --manifest-path client/tui/Cargo.toml --example tty_decode
python3 client/tui/tests/tty_smoke.py --output build-tui-tty-smoke
```

The optional output directory contains ANSI transcripts and a JSON result matrix.
When sharing a checkout between Windows and WSL, use a separate `CARGO_TARGET_DIR`
and pass its executable and decoder paths through `--executable` and `--decoder`.

The optional real-daemon test requires the host daemon running on localhost:

```console
PSXTERM_TEST_PORT=29323 cargo test --manifest-path client/tui/Cargo.toml --test network actual_host -- --ignored
```

PowerShell equivalent: set `$env:PSXTERM_TEST_PORT='29323'` before the Cargo command.
Local Windows-to-WSL validation covered multiple independent sessions, output,
resize, ping and detach/resume with the same server session ID. **This is host
validation, not PS4/PS5 hardware validation.**
The current frontend passed 72 default tests on Windows and native Linux using
the declared minimum Rust 1.88 toolchain. These cover direct Unicode/history
copy, soft wrapping, color contrast and preservation, preference migration and
rollback, demo reconnect staying offline, redraw scheduling, bounded history
search, Unicode matches/highlights, stale results, keyboard/mouse navigation,
field caret placement, masked tokens, consistent UTF-8 typing/paste limits,
fair polling of eight busy terminals, input delivery through a full output queue,
close/detach while that queue is full, and mouse-wheel routing in split view
and search/export previews in both languages. Export checks cover exact
retained history, Unicode file paths, background worker progress, failure/retry,
no overwrite, scrolled view preservation, and cancellation on quit. Three optional
real-daemon tests are ignored by the default run. Earlier local host validation
also exercised real-daemon sessions and the `pwd` and `ping` CLI flashes.
Cancellation tests cover fragmented HELLO/OPEN/ATTACH replies, close and detach,
and waiting for connections belonging to removed tabs. The real-daemon test also
withholds OPEN_OK after creating a session, cancels the client and verifies the
closed session cannot be resumed.
