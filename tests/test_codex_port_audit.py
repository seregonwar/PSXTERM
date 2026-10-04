"""Evidence classification tests for the offline Codex port inventory."""

from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from ports.codex import audit


class ManifestTests(unittest.TestCase):
    def test_workspace_alias_features_and_guards(self):
        root = Path("workspace").resolve()
        manifest = {
            "dependencies": {
                "renamed": {"workspace": True, "features": ["extra"], "optional": True},
                "version_only": "1.2",
            },
            "target": {'cfg(target_os = "freebsd")': {
                "build-dependencies": {"helper": {"path": "../helper", "default-features": False}},
            }},
            "dev-dependencies": {"test_only": {"path": "../test-only"}},
        }
        workspace = {"renamed": {"package": "actual-name", "path": "shared", "features": ["base"]}}
        deps = audit.declared_dependencies(manifest, workspace, root, root / "entry" / "Cargo.toml")
        self.assertEqual({item["alias"] for item in deps}, {"renamed", "version_only", "helper"})
        renamed = next(item for item in deps if item["alias"] == "renamed")
        self.assertEqual(renamed["package"], "actual-name")
        self.assertEqual(renamed["features"], ["base", "extra"])
        self.assertTrue(renamed["optional"])
        self.assertEqual(Path(renamed["manifest"]), root / "shared" / "Cargo.toml")
        helper = next(item for item in deps if item["alias"] == "helper")
        self.assertEqual(helper["target_guard"], 'cfg(target_os = "freebsd")')
        self.assertEqual(helper["kind"], "build-dependencies")
        self.assertFalse(helper["default_features"])
        self.assertEqual(Path(helper["manifest"]), root / "helper" / "Cargo.toml")

    def test_missing_workspace_dependency_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "missing workspace dependency absent"):
            audit.declared_dependencies({"dependencies": {"absent": {"workspace": True}}},
                                        {}, Path("."), Path("entry/Cargo.toml"))

    def make_source(self, base, child_dependency="../exec"):
        root = base / "codex-rs"
        (root / "exec").mkdir(parents=True)
        (root / "optional").mkdir()
        (root / "Cargo.toml").write_text('[workspace]\n', encoding="utf-8")
        (root / "Cargo.lock").write_text('version = 4\n', encoding="utf-8")
        (root / "rust-toolchain.toml").write_text('[toolchain]\nchannel = "1.95.0"\n', encoding="utf-8")
        (root / "exec/Cargo.toml").write_text(
            '[package]\nname = "codex-exec"\n'
            '[target.\'cfg(target_os = "freebsd")\'.dependencies]\n'
            'optional = { path = "../optional", optional = true }\n'
            '[dev-dependencies]\nnonexistent = { path = "../does-not-exist" }\n', encoding="utf-8")
        (root / "optional/Cargo.toml").write_text(
            '[package]\nname = "optional"\n[dependencies]\n'
            f'entry = {{ path = "{child_dependency}" }}\n', encoding="utf-8")

    def test_closure_includes_guarded_optional_and_terminates_cycles(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            self.make_source(source)
            with patch.object(audit, "command", side_effect=["official-repo\n", "pinned-commit\n", " M modified\n"]):
                report = audit.source_inventory(source)
        self.assertEqual(report["local_package_count"], 2)
        self.assertEqual(report["commit"], "pinned-commit")
        self.assertEqual(report["checkout_status"], [" M modified"])
        self.assertIn("not a Cargo feature resolution", report["method"])
        entry = next(item for item in report["local_packages"] if item["name"] == "codex-exec")
        self.assertEqual(entry["dependencies"][0]["manifest"], "codex-rs/optional/Cargo.toml")
        self.assertEqual(len(entry["sha256"]), 64)

    def test_dependency_outside_checkout_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            self.make_source(source, "../../../outside")
            with self.assertRaisesRegex(ValueError, "dependency leaves the source checkout"):
                audit.source_inventory(source)


class ExportTests(unittest.TestCase):
    def test_nm_excludes_undefineds_and_normalizes_symbol_versions(self):
        listing = ('archive.a(member.o):\n'
                   '00000000 T pthread_create\n'
                   '00000001 W arc4random_buf@@LIBC_1.0\n'
                   '         U getentropy\n'
                   '         w weak_undefined\n'
                   '00000002 U another_undefined\n')
        self.assertEqual(audit.parse_nm(listing), {"pthread_create", "arc4random_buf"})

    def make_sdk(self, directory):
        sdk = Path(directory)
        lib = sdk / "target/lib"
        lib.mkdir(parents=True)
        (lib / "libempty.a").write_bytes(b"!<arch>\n")
        (lib / "libkernel.so").write_bytes(b"fixture")
        return sdk

    def test_empty_archive_is_not_evidence_of_unavailable_threads(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = self.make_sdk(directory)
            with patch.object(audit, "command", side_effect=["", "00000000 T pthread_create\n"]) as run:
                report = audit.sdk_inventory(sdk, "llvm-nm")
        self.assertTrue(report["inventory_complete"])
        self.assertEqual(report["groups"]["threads"]["pthread_create"], ["libkernel.so"])
        self.assertNotIn("pthread_create", report["not_declared_in_inspected_libraries"])
        self.assertIn("-D", run.call_args_list[1].args[0])
        self.assertNotIn("-D", run.call_args_list[0].args[0])
        self.assertIn("does not resolve target guards", report["method"])

    def test_failed_or_timed_out_inspection_produces_incomplete_inventory(self):
        failures = [RuntimeError("nm failed"), subprocess.TimeoutExpired(["nm"], 60)]
        for error in failures:
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as directory:
                sdk = self.make_sdk(directory)
                with patch.object(audit, "command", side_effect=["", error]):
                    report = audit.sdk_inventory(sdk, "nm")
                self.assertFalse(report["inventory_complete"])
                self.assertEqual(report["libraries"][1]["status"], "not_inspected")
                self.assertEqual(report["groups"]["threads"]["pthread_create"], [])

    def test_sdk_without_libraries_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "SDK has no target libraries"):
                audit.sdk_inventory(Path(directory), "nm")


if __name__ == "__main__":
    unittest.main()
