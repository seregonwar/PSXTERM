use std::process::Command;
#[test]
fn help_and_flash_are_localized_without_network_or_config() {
    let exe = env!("CARGO_BIN_EXE_psxterm-tui");
    for (language, word) in [("en", "Usage"), ("it", "Uso")] {
        let out = Command::new(exe)
            .args(["--lang", language, "flash", "help"])
            .output()
            .unwrap();
        assert!(out.status.success());
        let text = String::from_utf8(out.stdout).unwrap();
        assert!(text.contains(word));
        assert!(text.contains("seregonwar"));
        assert!(text.contains("2323"));
    }
    let out = Command::new(exe)
        .args(["--lang", "it", "--help"])
        .output()
        .unwrap();
    assert!(out.status.success());
    assert!(String::from_utf8(out.stdout).unwrap().contains("Opzioni"));
}
#[test]
fn snapshot_is_network_free_and_contains_brand_and_palette() {
    let d = tempfile::tempdir().unwrap();
    let path = d.path().join("preview.svg");
    let out = Command::new(env!("CARGO_BIN_EXE_psxterm-tui"))
        .args(["--lang", "it", "--snapshot"])
        .arg(&path)
        .args(["--snapshot-view", "tabs"])
        .output()
        .unwrap();
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let svg = std::fs::read_to_string(path).unwrap();
    assert!(svg.contains("seregonwar"));
    assert!(svg.contains("#dcdee2"));
    assert!(svg.contains("no connection"));
}
