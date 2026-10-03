"""Contracts for downloadable nightlies and translation contributions."""

from datetime import datetime
import hashlib
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import check_locales
import nightly

SHA = "abcdef0" + "1" * 33
TAG = "nightly-20261003-gabcdef0-r123"


class NightlyIdentityTests(unittest.TestCase):
    def test_rome_schedule_selects_one_cron_across_dst_changes(self):
        for now, active in [
            ("2026-03-28T20:00:00+00:00", "0 21 * * *"),
            ("2026-03-29T20:00:00+00:00", "0 20 * * *"),
            ("2026-10-24T20:00:00+00:00", "0 20 * * *"),
            ("2026-10-25T21:00:00+00:00", "0 21 * * *"),
        ]:
            for cron in ("0 20 * * *", "0 21 * * *"):
                with self.subTest(now=now, cron=cron):
                    result = nightly.identity(SHA, "123", datetime.fromisoformat(now), cron)
                    self.assertEqual(result["should_run"], str(cron == active).lower())

    def test_scheduled_duplicate_is_skipped_but_manual_run_can_rebuild(self):
        now = datetime.fromisoformat("2026-10-03T20:05:00+00:00")
        self.assertEqual(nightly.identity(SHA, "124", now, "0 20 * * *", [TAG])["should_run"], "false")
        manual = nightly.identity(SHA, "124", now, existing_tags=[TAG])
        self.assertEqual(manual["should_run"], "true")
        self.assertNotEqual(manual["tag"], TAG)
        self.assertEqual(manual["sha"], SHA)

    def test_manual_date_uses_rome_and_invalid_inputs_fail(self):
        now = datetime.fromisoformat("2026-10-03T23:30:00+00:00")
        self.assertEqual(nightly.identity(SHA, "123", now)["date"], "2026-10-04")
        with self.assertRaises(ValueError):
            nightly.identity("main", "123", now)
        with self.assertRaises(ValueError):
            nightly.identity(SHA, "1\nsha=other", now)


class NightlyPackageTests(unittest.TestCase):
    def test_archives_include_correct_binaries_provenance_and_checksums(self):
        for system, machine, target, binary in [
            ("Linux", "x86_64", "linux-x86_64", "psxterm-tui"),
            ("Darwin", "arm64", "macos-arm64", "psxterm-tui"),
            ("Windows", "AMD64", "windows-x86_64", "psxterm-tui.exe"),
        ]:
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                tui = root / "client/tui/target/release"
                tui.mkdir(parents=True)
                (tui / binary).write_bytes(b"fixture client")
                host = root / "build-nightly-host"
                host.mkdir()
                for name in ("psxtermd", "psxterm", "psxterm-ttyprobe", "cli_test"):
                    (host / name).write_bytes(b"fixture host")
                (root / "LICENSE").write_text("fixture license", encoding="utf-8")
                (root / "README.md").write_text("fixture guide", encoding="utf-8")
                (root / "docs").mkdir()
                (root / "docs/CLIENT_TUI.md").write_text("Italiano: console à", encoding="utf-8")
                output = root / "dist"
                with patch("nightly.platform.system", return_value=system), patch("nightly.platform.machine", return_value=machine):
                    archive = nightly.package(root, output, target, TAG, SHA)
                prefix = f"psxterm-{TAG}-{target}/"
                if system == "Windows":
                    with zipfile.ZipFile(archive) as bundle:
                        names = bundle.namelist()
                        metadata = json.loads(bundle.read(prefix + "BUILD_INFO.json"))
                    self.assertNotIn(prefix + "psxtermd", names)
                else:
                    with tarfile.open(archive) as bundle:
                        names = bundle.getnames()
                        metadata = json.load(bundle.extractfile(prefix + "BUILD_INFO.json"))
                        self.assertEqual(bundle.getmember(prefix + binary).mode & 0o777, 0o755)
                    self.assertIn(prefix + "psxtermd", names)
                self.assertIn(prefix + binary, names)
                self.assertIn(prefix + "LICENSE", names)
                self.assertEqual(metadata["commit"], SHA)
                self.assertEqual(metadata["target"], target)
                checksum = (output / f"SHA256SUMS-{target}.txt").read_text().split()
                self.assertEqual(checksum, [hashlib.sha256(archive.read_bytes()).hexdigest(), archive.name])

    def test_mismatched_architecture_and_commit_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch("nightly.platform.system", return_value="Linux"), patch("nightly.platform.machine", return_value="arm64"):
                with self.assertRaises(ValueError):
                    nightly.package(root, root / "dist", "linux-x86_64", TAG, SHA)
            with self.assertRaises(ValueError):
                nightly.package(root, root / "dist", "windows-x86_64", TAG, "0" * 40)


class LocaleTests(unittest.TestCase):
    def test_duplicate_keys_and_non_string_values_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "en.json"
            for text in ('{"title":"one","title":"two"}', '{"title":42}'):
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    check_locales.read_locale(path)

    def test_missing_keys_and_placeholder_counts_are_reported(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "en.json").write_text('{"title":"Hello {0} {0}","body":"World"}', encoding="utf-8")
            (root / "it.json").write_text('{"title":"Ciao {0}"}', encoding="utf-8")
            errors = check_locales.validate(root)
            self.assertIn("it.json: missing body", errors)
            self.assertIn("it.json: placeholders differ for title", errors)


if __name__ == "__main__":
    unittest.main()
