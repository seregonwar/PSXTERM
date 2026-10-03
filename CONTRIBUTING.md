# Contributing to PSXTerm

Use the [issue chooser](https://github.com/seregonwar/PSXTERM/issues/new/choose)
for bugs, console crashes, features, documentation, and translations. Include
the release/nightly tag or commit, affected component, and shortest reproduction.
For client display problems include the terminal application, columns × rows,
and selected language. Remove access tokens and resume tokens from attachments.

## Local checks

For the Rust workspace client (Windows, Linux, macOS):

```console
cargo fmt --manifest-path client/tui/Cargo.toml -- --check
cargo clippy --locked --manifest-path client/tui/Cargo.toml --all-targets -- -D warnings
cargo test --locked --manifest-path client/tui/Cargo.toml
python3 tools/check_locales.py
```

For the C host daemon and CLI (Linux, macOS, WSL):

```console
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host --parallel
ctest --test-dir build-host --output-on-failure
python3 tests/integration/test_e2e.py --build build-host
```

For nightly packaging and translation validation tools, run on Linux/macOS:

```console
python3 -m unittest discover -s tests -p test_github_tools.py
```

Use `python` instead of `python3` on Windows. Keep changes focused, explain the
resulting behavior, and report checks actually run in the PR template. UI changes
should include screenshots or snapshots at 55×18, 80×24, and 140×44 in both
English and Italian. See [the client guide](docs/CLIENT_TUI.md) for snapshot and
native terminal checks. Describe untested configurations explicitly; a build or
host test alone does not establish PS4/PS5 hardware behavior.

## Translation changes

Edit `client/tui/locales/en.json` and `it.json` together. Keep keys and indexed
placeholders such as `{0}` consistent; preserve repeated placeholders. The locale
checker also rejects duplicate keys, non-string values, and empty translations.
Protocol command IDs and the original GPL text are not translated.

New languages also need registration in `client/tui/src/i18n.rs`; adding a JSON
file alone does not expose that language in the client.

## Builds and releases

See [CI and nightly downloads](docs/CI.md) for workflow responsibilities,
nightly identity, supported download targets, and checksums. Nightly artifacts
contain the exact source commit in `BUILD_INFO.json`; include it in regression
reports. Dependency update PRs are reviewed normally and never merged by these
workflows automatically.
