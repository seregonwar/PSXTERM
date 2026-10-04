# CI, nightly builds, and contribution forms

The `.github` setup follows the issue forms, localization checks, and dated
nightly approach used by [MemDBG](https://github.com/seregonwar/MemDBG/tree/main/.github),
adapted to PSXTerm's C host tools and Rust/Ratatui workspace client.

## Workflows

| Workflow | Trigger | Result |
|---|---|---|
| Verify | PR, main push, manual | Existing client/host tests, sanitizers, and SDK build checks |
| Build | Changes to implementation/build files | Existing host build and tests |
| Docs Check | Markdown changes | Relative links and tracked-file checks |
| Locale Check | Client resource changes, manual | Translation keys, types, duplicates, and placeholder parity |
| GitHub Configuration | GitHub/tool changes, manual | Actionlint and nightly/locale tool tests |
| Nightly | Daily schedule or manual | Tested desktop archives and immutable prerelease |
| Release | `v*` tag or manual | Existing stable release workflow |

Dependabot proposes weekly updates to Cargo dependencies and GitHub Actions.
Compatible Cargo updates are grouped; major updates remain separate. Release
notes use `.github/release.yml` categories. No workflow automatically merges PRs
or commits generated locale files.

Verify and Nightly use Rust 1.88.0, the client's declared minimum version,
including rustfmt and Clippy. SDK jobs export an absolute `PS4_PAYLOAD_SDK` or
`PS5_PAYLOAD_SDK` path for compiler/linker wrappers as well as the CMake
toolchain. Keep these environment variables when reproducing an SDK build.

## Nightly downloads

The schedule targets **22:00 Europe/Rome**, using two UTC cron entries and a
preflight daylight-saving check. GitHub may delay scheduled runs. Only the
matching UTC entry builds; scheduled runs also skip a main commit that already
has a nightly tag. Manual runs can build that commit again with a new run ID.

Every run resolves `main` once and all matrix jobs check out that full SHA.
Tags follow `nightly-YYYYMMDD-g<7-character-SHA>-r<run-ID>`. Nightlies are
prereleases, do not replace stable Latest, and never overwrite published assets.
They appear in [Releases](https://github.com/seregonwar/PSXTERM/releases) after
all three desktop builds and required tests succeed.

| Archive target | Contents |
|---|---|
| `windows-x86_64.zip` | `psxterm-tui.exe`, docs, license, build metadata |
| `linux-x86_64.tar.gz` | Workspace client, native host daemon/CLI/probe/test tool, docs, license, metadata |
| `macos-arm64.tar.gz` | Workspace client, native host daemon/CLI/probe/test tool, docs, license, metadata |

These nightly archives contain desktop binaries. The existing tagged Release
workflow handles console artifacts separately. Nightly tests do not validate
execution on PS4/PS5 hardware. Archives are unsigned development builds.

Each archive includes `BUILD_INFO.json` with the source commit, target, channel,
and binary list. `SHA256SUMS.txt` lists every archive. On Linux:

```console
sha256sum -c SHA256SUMS.txt
```

On macOS use `shasum -a 256 -c SHA256SUMS.txt`. On Windows use
`Get-FileHash <archive.zip> -Algorithm SHA256` and compare with the manifest.
Download all three archives for a complete manifest check, or select the line
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
actionlint -shellcheck= -pyflakes=
```

Tests cover summer/winter schedules and transitions, duplicate scheduled builds,
manual identities, OS/architecture mismatch, archive contents, executable modes,
commit provenance, checksums, and malformed translation resources.

Workflow files take effect after they are committed and pushed to the repository.
Adding them locally does not run Actions or publish a nightly.
