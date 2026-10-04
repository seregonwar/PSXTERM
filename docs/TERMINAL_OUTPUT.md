# Terminal output

The built-in commands render inside the session's virtual terminal. `ls` uses
the width reported by the client, sorted names, two spaces between columns and
no trailing padding. Columns fill downward by default; `-x` fills across.
`-1` prints one name per line. A name wider than the terminal remains complete
and can wrap; it is never silently cut off.

## Listing and colours

| Entry | Default style |
| --- | --- |
| Directory | Bold bright blue (`1;94`) |
| Executable regular file | Bold bright green (`1;92`) |
| Symbolic link | Bold bright cyan (`1;96`) |
| Broken symbolic link | Bold bright red (`1;91`) |
| Archive/package | Bold bright red (`1;91`) |
| Image/audio/video or socket | Bold bright magenta (`1;95`) |
| Regular file | Terminal's normal foreground |

Special directory permissions, setuid/setgid files, devices and FIFOs use
distinct styles. Each coloured name ends with a reset, so its style cannot
carry into the next file or the shell prompt.

Personalise the palette inside psh, for example with yellow directories:

```console
export LS_COLORS="di=1;93:ex=1;92:ln=1;96:*.zip=1;91:*.tar.gz=1;91"
ls
```

`LS_COLORS` supports file-class keys and literal suffix patterns beginning
with `*`. Later matching rules win. Unspecified classes keep their defaults.
An empty value disables that class's colour. Values are SGR numbers separated
by semicolons; arbitrary terminal sequences and `ln=target` are not supported.

```console
ls --color=never
export NO_COLOR=1
ls
ls --color=always
unset NO_COLOR
```

The default and `--color=auto` respect a nonempty `NO_COLOR` and `TERM=dumb`.
`--color=always` overrides those settings. A bare `--color` means `always`.
Built-ins write to a virtual terminal, including EXEC sessions; use
`--color=never -1` for plain listing output in scripts.

Supported listing options are `-a`, `-A`, `-1`, `-C`, `-x`, `-l`, `-h`, `-n`,
`-F`, `-p`, `-d`, `-r`, `-S`, `-t`, `--all`, `--almost-all`, `--color` and
`--help`. Options can follow operands; `--` ends option parsing. Unsupported
options fail explicitly instead of being silently ignored.

Long listings show aligned permissions, link counts, owner/group, size,
modification date, name and symbolic-link target. Host builds resolve owner
and group names; console builds use IDs, and `-n` always uses IDs. `-h` rounds
sizes upward in binary units. Directory totals use allocated blocks rather
than apparent file sizes. Multiple directory operands have separated headings.

Names with spaces or quotes use psh-compatible double quoting. Invalid UTF-8,
control bytes and directional formatting controls are shown as literal
`\xNN` escapes so they cannot change the terminal display. Common CJK,
accented letters, combining accents and emoji have cell-aware widths.
Complex grapheme clusters and terminal-specific emoji fonts can still differ;
`-1` is available when an exact column layout is unsuitable. Escaped control
bytes are a readable representation, not a shell escape syntax for reopening
such names.

## Other built-ins

- All 13 built-ins have `--help`. `help COMMAND` shows command usage.
- `cat` preserves binary file contents by default. `-n`, `-b`, `-s`, `-E`,
  `-T`, `-v` and `-A` provide numbered or visible text output, with state
  preserved across file and read boundaries. Missing files produce stderr
  diagnostics while remaining files are still read. Session stdin is not
  supported; `cat -` reports that limitation.
- `env` prints sorted, complete `NAME=value` entries; `-0`/`--null` uses NUL
  separators. `export`/`export -p` prints readable assignments; `export` and
  `unset` validate identifiers.
- `pwd` prints the session path. `cd` updates `PWD` and `OLDPWD`; `cd -`
  returns to the previous directory and prints its path.
- `whoami` prints the session user without interpreting control characters.
  `uname` and `uname -a` retain the PSXTerm platform description. Other uname
  field options are not implemented.
- `ps` describes PSXTerm sessions and foreground processes, with aligned
  columns and printable client names. It is not a system-wide process list.
- `clear` clears the visible screen and moves the cursor home. `exit` is
  silent on success and reports invalid arguments on stderr.

Formatted shell output grows as needed instead of truncating at 1,023 bytes.
Existing protocol frame and output-queue bounds still apply. External command
output remains a byte stream passed through the existing TTY/client path.

## Verification

The normal host integration suite includes four output matrices:
GNU comparisons run when GNU `ls`/`cat` (or `gls`/`gcat`) are installed;
other hosts explicitly skip those oracles while retaining the output contracts.

- Contracts and stdout/stderr/exit status for every built-in, including
  10,000-byte environment values.
- 95 exact comparisons with GNU `ls` in the C locale, including empty
  directories, multiple operands, links, long/human listings and sorting.
- 13 exact comparisons with GNU `cat`, including all 256 byte values,
  multiple files, CRLF and buffer boundaries.
- Colours, overrides, resets, quoted/control/invalid UTF-8 names and layouts
  at 1, 12, 24, 55, 80 and 140 columns.

The C suites cover printable UTF-8, malformed/truncated sequences, long
formatted output, directory changes and option defaults. The Rust client
includes a real-daemon test for bright directory cells, bold style, resets and
colour disabling. It runs with the other ignored `actual_host` network tests
when `PSXTERM_TEST_PORT` is set.

Validation on 2026-10-03: all nine host unit suites and all four output matrices
passed, also under ASan/UBSan; the Windows client tests and the real-daemon
colour test passed. PS4 and PS5 payload builds passed. The complete host
integration run passed 45/49 tests; the four failures below were reproduced
against an unmodified HEAD build in the same WSL environment:

- `test_resize_and_cli_test`
- `test_session_limit`
- `test_pipe_tty_forced_pty`
- `test_pipe_tty_stdio_matrix_and_reuse`

Physical PS4/PS5 display validation remains required. These checks establish
the tested output contracts, not full GNU coreutils compatibility.
Strict Clippy is also blocked by two existing `useless_conversion` diagnostics
in `client/tui/src/app.rs`. The network test passes Clippy with that existing
diagnostic category allowed.
