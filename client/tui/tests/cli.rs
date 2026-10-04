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

#[test]
fn snapshots_apply_color_preferences_without_touching_profiles() {
    let dir = tempfile::tempdir().unwrap();
    let config = dir.path().join("profiles.json");
    for (palette, expected, absent) in [
        ("readable", "#80aaff", "#6ea6ff"),
        ("original", "#6ea6ff", "#80aaff"),
    ] {
        let snapshot = dir.path().join(format!("{palette}.svg"));
        let output = Command::new(env!("CARGO_BIN_EXE_psxterm-tui"))
            .args(["--output-colors", palette, "--config"])
            .arg(&config)
            .arg("--snapshot")
            .arg(&snapshot)
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        let svg = std::fs::read_to_string(snapshot).unwrap();
        assert!(svg.contains(expected));
        assert!(!svg.contains(absent));
        assert!(!config.exists());
    }
}

#[test]
fn search_previews_render_the_real_highlight_in_both_languages() {
    let dir = tempfile::tempdir().unwrap();
    let config = dir.path().join("profiles.json");
    for language in ["en", "it"] {
        for (cols, rows) in [(55, 18), (80, 24), (140, 44)] {
            let path = dir.path().join(format!("search-{language}-{cols}.svg"));
            let output = Command::new(env!("CARGO_BIN_EXE_psxterm-tui"))
                .args([
                    "--lang",
                    language,
                    "--snapshot-view",
                    "search",
                    "--snapshot-cols",
                    &cols.to_string(),
                    "--snapshot-rows",
                    &rows.to_string(),
                    "--config",
                ])
                .arg(&config)
                .arg("--snapshot")
                .arg(&path)
                .output()
                .unwrap();
            assert!(
                output.status.success(),
                "{}",
                String::from_utf8_lossy(&output.stderr)
            );
            let svg = std::fs::read_to_string(path).unwrap();
            assert!(svg.contains("#eecf76"));
            assert!(svg.contains("1/1"));
            assert!(svg.contains("no connection"));
            assert!(svg.contains(if language == "en" {
                "Find in terminal"
            } else {
                "Cerca nel testo"
            }));
            assert!(!config.exists());
        }
    }
}

#[test]
fn export_previews_show_the_dialog_without_creating_an_export_or_profiles() {
    let dir = tempfile::tempdir().unwrap();
    let config = dir.path().join("profiles.json");
    for language in ["en", "it"] {
        for (cols, rows) in [(55, 18), (80, 24), (140, 44)] {
            let path = dir.path().join(format!("export-{language}-{cols}.svg"));
            let output = Command::new(env!("CARGO_BIN_EXE_psxterm-tui"))
                .current_dir(dir.path())
                .args([
                    "--lang",
                    language,
                    "--snapshot-view",
                    "export",
                    "--snapshot-cols",
                    &cols.to_string(),
                    "--snapshot-rows",
                    &rows.to_string(),
                    "--config",
                ])
                .arg(&config)
                .arg("--snapshot")
                .arg(&path)
                .output()
                .unwrap();
            assert!(
                output.status.success(),
                "{}",
                String::from_utf8_lossy(&output.stderr)
            );
            let svg = std::fs::read_to_string(path).unwrap();
            assert!(svg.contains(if language == "en" {
                "Export terminal text"
            } else {
                "Esporta testo del terminale"
            }));
            assert!(svg.contains(if language == "en" {
                "Visible terminal only"
            } else {
                "Solo terminale visibile"
            }));
            assert!(svg.contains("no connection"));
            assert!(!config.exists());
        }
    }
    assert_eq!(std::fs::read_dir(dir.path()).unwrap().count(), 6);
}
