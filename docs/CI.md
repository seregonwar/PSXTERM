# CI, nightly builds, and contribution forms

The `.github` setup follows the issue forms, localization checks, and dated
nightly approach used by [MemDBG](https://github.com/seregonwar/MemDBG/tree/main/.github),
adapted to PSXTerm's C host tools and Rust/Ratatui workspace client.

## Workflows

| Workflow | Trigger | Result |
|---|---|---|
| Verify | PR, main push, manual | Existing client/host tests, sanitizers, and SDK build checks |
| Build | Changes to implementation/build files | Debug and Release host builds and tests on Linux/macOS |
| Docs Check | Markdown changes | Relative links and tracked-file checks |
| Locale Check | Client resource changes, manual | Translation keys, types, duplicates, and placeholder parity |
| GitHub Configuration | GitHub/tool changes, manual | Actionlint and nightly/locale tool tests |
| Nightly | Daily schedule or manual | Desktop archives, PS4/PS5 ELF payloads, and immutable prerelease |
| Release | `v*` tag or manual | Existing stable release workflow |

Dependabot proposes weekly updates to Cargo dependencies and GitHub Actions.
Compatible Cargo updates are grouped; major updates remain separate. Release
notes use `.github/release.yml` categories. No workflow automatically merges PRs
or commits generated locale files.

Verify and Nightly use Rust 1.88.0, the client's declared minimum version,
including rustfmt and Clippy. SDK jobs export an absolute `PS4_PAYLOAD_SDK` or
`PS5_PAYLOAD_SDK` path for compiler/linker wrappers as well as the CMake
toolchain. Keep these environment variables when reproducing an SDK build.

Build checks both optimization profiles before Nightly packages Release tools.
To reproduce Linux Release diagnostics with fortified libc checks locally:

```console
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-D_FORTIFY_SOURCE=3
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
```

## Nightly downloads

The schedule targets **22:00 Europe/Rome**, using two UTC cron entries and a
preflight daylight-saving check. GitHub may delay scheduled runs. Only the
matching UTC entry builds; scheduled runs also skip a main commit that already
has a nightly tag. Manual runs can build that commit again with a new run ID.

Every run resolves `main` once and all matrix jobs check out that full SHA.
Tags follow `nightly-YYYYMMDD-g<7-character-SHA>-r<run-ID>`. Nightlies are
prereleases, do not replace stable Latest, and never overwrite published assets.
They appear in [Releases](https://github.com/seregonwar/PSXTERM/releases) after
all three desktop builds, both console builds, and required tests succeed.

| Archive target | Contents |
|---|---|
| `windows-x86_64.zip` | `psxterm-tui.exe`, docs, license, build metadata |
| `linux-x86_64.tar.gz` | Workspace client, native host daemon/CLI/probe/test tool, docs, license, metadata |
| `macos-arm64.tar.gz` | Workspace client, native host daemon/CLI/probe/test tool, docs, license, metadata |
| `ps4.tar.gz` | PS4 daemon/probe ELFs, execution wrapper, CLI/network probes, docs, license, SDK metadata |
| `ps5.tar.gz` | PS5 daemon/probe ELFs, execution wrapper, CLI/network/process probes, docs, license, SDK metadata |

The release also offers `psxtermd-ps4.elf` and `psxtermd-ps5.elf` as direct
downloads. The archives include `cli_test.elf`, `exec_wrapper.elf`, `hello.elf`,
`netprobe.elf`, and `pslist.elf` on PS5, plus any optional ELF tools built.

Each console job downloads its SDK on demand from the latest release of
`ps4-payload-dev/sdk` or `ps5-payload-dev/sdk`, resolves that release tag before
downloading, and records the SDK repository, version and ZIP SHA256 in
`BUILD_INFO.json`. The SDK is not committed or bundled. LLVM 18 and the SDK's
Orbis/Prospero toolchain compile Release payloads from the same source SHA as
the desktop builds. Packaging rejects missing tools and invalid ELF headers.
Nightly tests do not validate execution on PS4/PS5 hardware. Archives are
unsigned development builds.

Each archive includes `BUILD_INFO.json` with the source commit, target, channel,
and binary list. `SHA256SUMS.txt` lists every archive and standalone ELF. On Linux:

```console
sha256sum -c SHA256SUMS.txt
```

On macOS use `shasum -a 256 -c SHA256SUMS.txt`. On Windows use
`Get-FileHash <archive.zip> -Algorithm SHA256` and compare with the manifest.
Download all five archives and both standalone ELFs for a complete manifest check, or select the line
for the archive you downloaded.

The Actions run also keeps downloadable packages for 14 days and native terminal
diagnostics for 7 days. Publication requires `contents: write` only in the final
job; compilation and checks have read-only tokens. A failed build publishes no
release. The workflow creates a tag on the tested SHA through GitHub's release
API and does not push branches.

## Local checks

```console
python3 tools/check_locales.py
python3 -m unittest discover -s tests -p test_github_tools.py
python3 -m unittest discover -s client/tui/tests -p test_tty_smoke.py
actionlint -shellcheck= -pyflakes=
```

Tests cover summer/winter schedules and transitions, duplicate scheduled builds,
manual identities, OS/architecture mismatch, archive contents, executable modes,
commit/SDK provenance, console ELF validation, checksums, and malformed translation resources.

The terminal test helpers cover asynchronous completion across quiet redraw
intervals, missing results, dialog dismissal, idle drains, and early client exit.
Host CTest suites have a 60-second limit per test. The maximum-frame check uses
a small socket send buffer and drains while writing, including on macOS. Shell
output tests use the same approach for long output and never block the writer
before reading. Spawn diagnostics use `/bin/sh` on both supported host systems.
Listing tests cover a working directory reached through a symbolic link and
operands ending in `/`, `/.`, and `/..`, preserving filesystem path semantics.

Workflow files take effect after they are committed and pushed to the repository.
Adding them locally does not run Actions or publish a nightly.
