# Launching a second payload on the console

Status: DESIGN, derived from a working implementation (zftpd)

This note records how the ecosystem actually runs a second payload on a PS5,
because PSXTerm spent a lot of hardware time trying to relay stdio through the
payload arguments and the answer is that a production payload does not do that
at all.

## The reference: zftpd

`zftpd` (github.com/seregonwar/zftpd) needs privileged work done outside its
own process, so it ships a second, small payload (`install_helper.elf`, built
from `src/platform/ps5/install_helper/install_helper.c`, 305 lines) and embeds
its bytes in the main binary through `tools/gen_blob.py`.

Launching it is two steps, both visible in
`src/http/games/ps5_install_helper.c`:

```c
#define HELPER_IPC_PORT    39481   /* where the helper calls back */
#define HELPER_LOADER_PORT 9021    /* the loader, i.e. elfldr */

static int deliver_helper(int loader_fd) {
  while (sent < install_helper_elf_size) {
    ssize_t n = send(loader_fd, install_helper_elf + sent,
                     install_helper_elf_size - sent, 0);
    ...
```

1. **Connect to the loader on port 9021 and send the ELF bytes.** That is the
   same loader PSXTerm already uses for deployment; no private loader is
   involved.
2. The new payload is started by that loader, and it **connects back to the
   parent on a loopback port of its own** (`connect_back()`, `127.0.0.1`,
   `HELPER_IPC_PORT`) and does its work over that socket.

The helper's stdio is therefore never relayed by the loader: the child owns a
socket it opened itself.

## Why this answers the PSXTerm stdio problem

PSXTerm spawns payloads through its own loader (victim process, ptrace, payload
args) and then tried to recover the payload runtime's libc stdout by relaying
`rwpipe` and then the overlapped socket pair. Both were measured on hardware
and both are wrong: the first leaves the output missing, the second stops even
the payload's raw descriptor writes.

With the loader path the question disappears:

- the loader wires **its own accepted socket** into the new payload as standard
  io - that is what `elfldr_spawn(stdio, ...)` does with the connection it
  accepted - and a TCP socket is exactly the descriptor shape this kernel
  handles correctly for a payload;
- the parent (PSXTerm) still holds the connecting socket, so it can pump it:
  session input into the socket, socket data out as ordinary `STDOUT` frames,
  exactly like the relay plumbing that is already in the tree, but with a
  socket the child definitely writes to.

This is the same shape as the roadmap's execution broker, and it needs no new
kernel primitives.

## What PSXTerm should implement

1. `psx_platform_loader_spawn(path, argv)`: read the ELF, connect to
   `127.0.0.1:9021`, send the bytes, keep the socket. Loopback only, like
   zftpd.
2. Session: when a foreground CLI is spawned on a console and the loader is
   reachable, use this path and pump the socket into the session's stdout and
   stderr (the child decides which stream by writing to the socket; a wrapper
   can distinguish them with a one-byte marker if needed).
3. Exit status: the loader does not report one. zftpd's helper reports its own
   outcome over the callback socket, so PSXTerm should do the same: a small
   wrapper payload that runs the CLI, waits for it and sends a final line with
   the status before closing. That wrapper is also where `sh`-style features
   can grow later.
4. Fall back to the existing injection path when port 9021 is not reachable,
   so a console without a loader behaves as it does today.

## Verification plan

- `curl --version` and `curl -o file https://example.com` through the loader
  path: the version banner must appear in the session, because the child's
  stdout is the loader socket.
- A certificate failure (`https://expired.badssl.com/`) must still fail, which
  is what proves the CA bundle is being used rather than `-k`.
- Only then `curl ... | sh`, which additionally needs the shell substrate
  (pipelines and a standalone `sh`), tracked separately.

## Verified on the console

Measured against the loader on the test console, from the host machine:

| Attempt | Result |
|---|---|
| Raw ELF bytes sent to port 9021 (`hello.elf`, 112 KB) | **The payload runs and all five of its lines come back on that socket**, including the two that go through libc (`printf`, `fflush` on stdout) which never reach the session through the injection path. This is the proof the mechanism is right |
| Same with `curl` (8.1 MB) | The loader closes the connection: the raw path is not meant for payloads that size |
| URI form (`"file"` magic + `file://data/...` request line) | `[elfldr.elf] Error reading URI payload` - the installed loader rejects it, so the request-line protocol (which is what carries `?args=`) is not available on this console |

Consequences for the plan below:

1. The raw path is the one to use, and it is proven to deliver a payload's
   libc stdio. It carries no arguments, so arguments need a wrapper payload:
   the daemon sends the wrapper, and the wrapper receives the command and its
   arguments over the same socket before doing the privileged part.
2. The wrapper is also where the exit status comes from, which the loader does
   not report, and where the shell substrate can grow later.
3. `curl` is 8 MB and cannot be sent raw; the wrapper can fetch it from the
   console filesystem itself and spawn it with the existing injection code,
   which is exactly what it is there for.

## Sources

- zftpd `src/http/games/ps5_install_helper.c` (loader port, callback port,
  `deliver_helper`)
- zftpd `src/platform/ps5/install_helper/install_helper.c` (`connect_back`)
- zftpd `Makefile` (blob generation for the helper ELF)
- ps5-payload-dev `elfldr` (`elfldr_spawn(stdio, ...)`, the socket handed to
  the payload)
