# Native Codex port: compatibility evidence

Status: **not built or executed as a native Codex agent on PS5**. This directory
contains offline inventories and compile-only ABI and entropy checks. It does not contain a
loader, deployment command, process broker, or an implementation of a PS5 Rust
target. The upstream checkout and generated reports stay in ignored `build-*`
directories; upstream source is not vendored.

## Pinned upstream

`upstream.json` identifies the official OpenAI source commit inspected on
2026-10-03, its Rust 1.95.0 toolchain, lockfile hashes, and the locked `libc`
dependency. `codex-exec` is the first candidate entry point because it provides
non-interactive agent execution. It still requires the core agent, local files,
child commands, authentication, state, and model communication.

The [official Codex CLI documentation](https://learn.chatgpt.com/docs/codex/cli)
documents installation on macOS, Linux, and Windows; it does not establish PS5
support. Rust's [FreeBSD target documentation](https://doc.rust-lang.org/rustc/platform-support/freebsd.html)
also does not establish compatibility with the console's ABI.

## Findings from this SDK

The available SDK's compiler reports `x86_64-sie-ps5`, and its CMake toolchain
sets FreeBSD version 9. The Rust `libc` 0.2.186 build script selects FreeBSD 12
by default, including when cross-compiling from Windows.

Compiling `header_abi.c` against the SDK and `abi-check` against that locked
Rust dependency measured 36 sizes, alignments, field offsets, and constants.
Nine differed under the default FreeBSD configuration:

| Measurement | SDK headers | Rust default |
| --- | ---: | ---: |
| `ino_t` size | 4 | 8 |
| `nlink_t` size | 2 | 8 |
| `struct stat` size | 120 | 224 |
| `stat.st_ino` offset | 4 | 8 |
| `stat.st_nlink` offset | 10 | 16 |
| `stat.st_atim` offset | 24 | 48 |
| `stat.st_mtim` offset | 40 | 64 |
| `stat.st_size` offset | 72 | 112 |
| `struct kevent` size | 32 | 64 |

The dependency's **testing override**
`RUST_LIBC_UNSTABLE_FREEBSD_VERSION=11` matched all 36 measured values. It also
changed representative imports from `stat`, `fstat`, and `kevent` to
`stat@FBSD_1.0`, `fstat@FBSD_1.0`, and `kevent@FBSD_1.0`. These names were
observed in the compiled archive with `llvm-nm`, without running it.

This override is **not a port**: it does not rebuild Rust's precompiled standard
library or prove that these versioned imports bind to SDK libraries. Nor does
the small inventory cover every ABI type, error convention, TLS/thread-local
storage behavior, unwinding, startup, or function used by Codex. Using the
default precompiled FreeBSD libraries directly is not justified by this evidence.

The separate SDK export inventory inspected all 40 libraries. Thread primitives
were declared in the `libkernel` variants despite the empty `libpthread.a`.
`getrandom`, `getentropy`, and `posix_spawn_file_actions_addchdir_np` were not
declared in the inspected libraries. The dependency and entropy checks below
confirm that the selected default FreeBSD entropy backend imports `getrandom`.
The other two are candidate APIs: `codex-utils-pty` gates its direct POSIX child
implementation to macOS/Linux, so `addchdir_np` alone is not a FreeBSD build blocker.

The source manifest closure contains 125 local packages, conservatively including
optional, platform-guarded, and build dependencies. It is not a resolved Cargo
feature graph. A separate Cargo tree for the selected `codex-exec` package,
default features and FreeBSD target resolves **797 normal dependency packages**,
excluding proc-macros and host build dependencies. `dependencies.py` records
versions, feature variants, and shortest paths from the entry package.

| Selected dependency | Version(s) | Porting requirement established by this graph |
| --- | --- | --- |
| `getrandom` | 0.2.17, 0.3.4, 0.4.2 | Default FreeBSD entropy backend imports `getrandom`, absent from the inspected SDK exports |
| `mio` | 1.2.0 | Event-loop interfaces used by Tokio need ABI and hardware checks |
| `portable-pty` | 0.9.0 | Included through `codex-utils-pty`; inclusion does not prove a hardware PTY is available |
| `openssl-sys` | 0.9.111 | Native TLS includes OpenSSL; the resolved feature set does not vendor it |
| `aws-lc-sys`, `ring` | 0.45.0, 0.17.14 | Both crypto implementations are selected; cross-compilation needs their C/assembly support |
| `keyring`, `libdbus-sys` | 3.6.3, 0.2.7 | FreeBSD enables Secret Service and a non-vendored D-Bus library |
| `libsqlite3-sys` | 0.37.0 | State storage selects bundled SQLite and its C build |

`v8`, `codex-code-mode-runtime`, and `codex-code-mode-host` are absent from this
selected normal dependency graph. They exist elsewhere in the workspace; their
presence there is not a blocker for compiling the main agent. This says nothing
about invoking optional Code Mode functionality at runtime.

The supported file-based authentication configuration avoids needing a running
keyring service for that storage path. It **does not remove** the unconditional
keyring/D-Bus build dependencies, including those selected through `codex-rmcp-client`.
TLS certificate validation and the required P-521 support of the installed
AWS-LC crypto provider must be preserved during porting.

The isolated `entropy-check` compiles ordinary safe fill calls using the three
locked `getrandom` versions, `libc` 0.2.186 and `cfg-if` 1.0.4. Their checksums
match upstream. Its archive has an undefined `getrandom` import; no target code
was executed. The probe uses minimal features to isolate backend selection and
does not reproduce the full agent's feature set or supply replacement entropy.
Resolving this requires a verified secure entropy API in the target runtime;
an SDK declaration of `arc4random_buf` alone is not that verification.

## Reproduce without executing target binaries

Requires Python 3.11+, Git, LLVM `nm`, Rust 1.95.0 with the FreeBSD standard
library, and the SDK compiler. The commands below use PowerShell and the SDK
already present in Debian WSL. Adjust paths for another development machine.

```powershell
git clone --filter=blob:none --no-checkout https://github.com/openai/codex.git build-codex-port-source
git -C build-codex-port-source -c core.longpaths=true checkout --detach b741e480e203f037ca726bc2a76d99a8e8668e66

$sdkPath = '\\wsl.localhost\Debian\home\seregon\psxterm-tools\sdk-ps5\ps5-payload-sdk'
$nmPath = 'C:\Program Files\LLVM\bin\llvm-nm.exe'
python ports/codex/audit.py --source build-codex-port-source --sdk $sdkPath --nm $nmPath --output build-codex-audit/report.json

rustup toolchain install 1.95.0 --profile minimal --target x86_64-unknown-freebsd
cargo +1.95.0 rustc --manifest-path ports/codex/abi-check/Cargo.toml --locked --target x86_64-unknown-freebsd --target-dir build-codex-abi --release -- --emit=asm
wsl -d Debian -- /home/seregon/psxterm-tools/sdk-ps5/ps5-payload-sdk/bin/prospero-clang -std=gnu11 -Wall -Wextra -Werror -S ports/codex/header_abi.c -o build-codex-audit/header_abi.s
$rustAssembly = Get-ChildItem build-codex-abi/x86_64-unknown-freebsd/release/deps/psxterm_codex_abi_check-*.s
python ports/codex/compare_abi.py --sdk-assembly build-codex-audit/header_abi.s --rust-assembly $rustAssembly.FullName --output build-codex-audit/abi-default-freebsd.json
```

The comparator returns 2 for a complete inventory with mismatches, 1 for an
invalid/incomplete inventory, and 0 for a match of the measured constants only.
For the separate FreeBSD 11 **diagnostic** comparison, restore the environment
after the build and keep artifacts in a different directory:

```powershell
$previousLibcAbi = $env:RUST_LIBC_UNSTABLE_FREEBSD_VERSION
try {
    $env:RUST_LIBC_UNSTABLE_FREEBSD_VERSION = '11'
    cargo +1.95.0 rustc --manifest-path ports/codex/abi-check/Cargo.toml --locked --target x86_64-unknown-freebsd --target-dir build-codex-abi-freebsd11 --release -- --emit=asm
} finally {
    $env:RUST_LIBC_UNSTABLE_FREEBSD_VERSION = $previousLibcAbi
}
$rustAssembly = Get-ChildItem build-codex-abi-freebsd11/x86_64-unknown-freebsd/release/deps/psxterm_codex_abi_check-*.s
python ports/codex/compare_abi.py --sdk-assembly build-codex-audit/header_abi.s --rust-assembly $rustAssembly.FullName --output build-codex-audit/abi-freebsd11.json
& $nmPath --undefined-only build-codex-abi-freebsd11/x86_64-unknown-freebsd/release/libpsxterm_codex_abi_check.rlib

python -m unittest discover -s tests -p 'test_codex*.py' -v
```

`audit.py` records checkout modifications and hashes but does not enforce the
pin; check its commit and hashes against `upstream.json` before comparing results.
An incomplete symbol inspection remains incomplete, including when a tool times
out. Both reports explicitly keep `native_execution_verified` false.

## Resolve the selected dependency and entropy requirements

Cargo tree fetches source dependencies but does not build or run the agent.
The normal-only tree intentionally excludes proc-macros and host build
dependencies; use a separate `--edges normal,build` tree to inspect those.

```powershell
cargo +1.95.0 tree --manifest-path build-codex-port-source/codex-rs/Cargo.toml --package codex-exec --target x86_64-unknown-freebsd --edges normal,no-proc-macro --locked --prefix depth --format '{p}|{f}' > build-codex-audit/tree-freebsd-runtime.txt
if ($LASTEXITCODE -ne 0) { throw 'Dependency resolution failed; do not use the output as evidence.' }
python ports/codex/dependencies.py --tree build-codex-audit/tree-freebsd-runtime.txt --source build-codex-port-source --output build-codex-audit/dependencies-freebsd.json

cargo +1.95.0 build --manifest-path ports/codex/entropy-check/Cargo.toml --locked --target x86_64-unknown-freebsd --target-dir build-codex-entropy --release
& $nmPath --undefined-only build-codex-entropy/x86_64-unknown-freebsd/release/libpsxterm_codex_entropy_check.rlib
```

The dependency inventory requires the pinned commit, matching lockfile, a clean
upstream checkout, one matching entry package, and a complete tree shape. It
does not attest how an externally supplied input tree was generated; preserve
the successful Cargo command result with the generated report. Empty or partial
trees, build section markers and proc-macros are rejected rather than treated
as a successful resolution. The report also keeps native execution unverified.

## Next gates for the original native execution objective

1. Identify an authorized userspace execution environment and its documented
   SDK/ABI, startup, library binding, and child-process interfaces. This work
   cannot proceed by extending exploit, sandbox-escape, or injection paths.
2. Establish a coherent Rust standard-library and dependency ABI for that
   environment. Confirm representative imports and all necessary layouts,
   rather than applying the FreeBSD 11 test override to dependencies alone.
3. Execute independent userspace checks on the console for threads, event loop,
   stdin/stdout/stderr, ordinary child commands, files, secure entropy, DNS,
   certificate-verified TLS, and state storage, with bounded failures.
4. Use the resolved pinned `codex-exec` dependency graph, build against the verified
   runtime, then demonstrate a console-resident agent reading and changing a
   console file, running a child command, and completing a model request.

None of these gates is completed by running Codex on the PC, producing an ELF
file, matching header constants, or listing SDK stubs.
