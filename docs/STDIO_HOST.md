# Host stdio acceptance

Status: IMPLEMENTED, HOST TESTED. This covers ordinary host fork/exec with
PipeTTY; it does not establish that the PS4/PS5 process backends pass the
same matrix.

The earlier stdout/stderr integration test exercised a shell builtin. It did
not catch that external programs received the same descriptor for both
streams, which placed their stderr in `PTTY_MSG_STDOUT`.

Host PipeTTY sessions now give an external program a separate stderr pipe.
The session polls its read end and emits `PTTY_MSG_STDERR`, drains it before
process exit frames and closes it on completion or session destruction.
Detached stderr is drained into the existing bounded scrollback. Scrollback
stores terminal bytes and still does not preserve channel tags.

A real PTY continues to provide a conventional combined terminal stream and
keeps `isatty()` semantics. Separate stderr setup is enabled only in host
builds; this is not a change to console descriptor installation.

`cli_test` reports the exact bytes consumed by `fread` in hexadecimal,
distinguishes stdin EOF from a read error and reports both flush results.
The host integration matrix verifies:

- `fgets` returns a complete line while input remains open;
- binary bytes, including NUL and high bytes, reach `fread` unchanged;
- raw write, printf, fprintf and puts markers survive stdin EOF;
- stdout and stderr reach their respective protocol channels;
- exit code 7 reaches the independent client and the actual client binary;
- repeated EOF and process execution leave the session usable with no extra
  descriptors;
- stderr produced while detached is retained for reattachment.

The test harness also waits for a completed readiness handshake and server
closure of the probe connection. Its previous delay occurred while the
probe socket was still open and could make the first client hit the session
limit. Authenticated daemons are ready after an authentication refusal and
clean server closure as well; the readiness check needs no token.

Local GCC AddressSanitizer/UndefinedBehaviorSanitizer checks use a non-PIE
test build (`-fno-pie` and `-no-pie`). The initial PIE sanitizer build showed
intermittent startup crashes across unchanged and changed test binaries in
this WSL environment. This is a local test configuration; repository CI and
normal builds retain their existing options.

The sanitized real-client check also exposed a leaked PTTY reader payload
on client exit. All connected client exit paths now destroy the reader before
closing its socket. The targeted stdio, client and authenticated-readiness
checks pass with both sanitizers enabled.

The test console daemon is not running, as confirmed by the operator on
2026-10-03. Physical PS5 stdio acceptance remains pending; host test success
must not promote it to HARDWARE TESTED.
