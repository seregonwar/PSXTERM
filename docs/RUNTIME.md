# Runtime layout and metadata

Status: IMPLEMENTED, HOST TESTED. The updated `core/runtime.c` and daemon
startup source compile with both installed PS4/PS5 SDKs. This is source
compilation evidence; these changes remain HARDWARE TEST REQUIRED.

The default console layout is:

```text
/data/psxterm/
    bin/
    runtime/
        bin/
        lib/
        etc/ca-bundle.crt
        share/
        tmp/
        var/
        home/.config/
        home/.local/share/
        cache/
        runtime.json
    agents/
    workspaces/
```

`PSXTERM_RUNTIME_BASE` overrides `/data/psxterm`; host builds default to the
platform home directory. Runtime paths are initialized once per process,
so set the override before starting the daemon. The base's parent must
already exist. Startup creates the base and runtime directories; a regular
file where a directory belongs is an error, reported with `ENOTDIR`.

The session supplies `HOME`, `TMPDIR`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`,
`XDG_DATA_HOME`, `SSL_CERT_FILE` and `CURL_CA_BUNDLE` from this layout. `PATH`
starts with the runtime bin directory and the platform bin directory, then
retains the session's search list. The client's terminal type remains the
session's `TERM`. Environment vectors reserve space for their terminating
NULL pointer, including when the caller supplies a small buffer.

## Manifest lifecycle

At startup, the daemon creates `runtime.json` only if it is missing. A
default manifest records version 1, platform, architecture, runtime root and
an empty packages object. Existing regular manifests are preserved byte for
byte, including installer-specific metadata. Startup neither validates nor
repairs an existing manifest.

Explicit package metadata updates through `psx_runtime_manifest_write()`
require a valid JSON object supplied by the caller. They write a temporary
file in the runtime directory, flush and synchronize it, close it, then
rename it over the manifest. A failed write leaves the existing manifest
intact and removes the temporary file. The API checks write, flush, sync,
close and rename errors. Paths containing quotes, backslashes or control
characters are escaped in generated JSON.

An explicit update generates a new manifest from the current layout and
supplied packages; unlike startup, it does not retain unknown fields. The
manifest is metadata, not proof that a listed package runs on the console.
Layout preparation does not install a certificate bundle or any CLI.

Startup logs incomplete directory preparation or manifest creation and
continues serving the shell. This lets the operator inspect a failed
runtime without losing terminal access.

## Validation on 2026-10-03

Host regression checks cover directory/file collisions, every environment
capacity boundary, manifest initialization and preservation, failure during
flush with retention of previous metadata, and temporary-file cleanup.
The runtime test passed 134 checks, including AddressSanitizer and
UndefinedBehaviorSanitizer.

Integration tests start and restart a real host daemon with a runtime base
containing quotes, a backslash and a newline, parse its manifest with an
independent JSON parser and verify installed metadata survives unchanged.
They also verify a manifest directory produces a warning while shell
commands remain usable.

The test console at `192.168.1.20` accepted a TCP connection on port `9021`;
port `2323` refused the connection. No new runtime code was executed on the
console during these checks. The separately documented PS5 stdio limitations
remain open. The experimental bundle builder in `tools/ps5-runtime-build.sh`
still requires its own build and hardware acceptance tests.
