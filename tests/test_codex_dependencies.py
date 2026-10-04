"""Validate target dependency evidence and prevent workspace-wide overclaims."""

from contextlib import redirect_stderr
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from ports.codex import dependencies


class DependencyTests(unittest.TestCase):
    def test_duplicates_versions_features_and_shortest_paths(self):
        tree = ("0codex-exec v0.0.0 (/repo/exec)|\n"
                "1long-path v1.0.0|default\n"
                "2middle v1.0.0|\n"
                "3getrandom v0.3.4|std\n"
                "1getrandom v0.3.4|std (*)\n"
                "1getrandom v0.2.17|std\n")
        report = dependencies.tree_inventory(tree)
        self.assertEqual(report["package_count"], 5)
        self.assertEqual(report["edge_count"], 5)
        self.assertFalse(report["native_execution_verified"])
        versions = {item["version"]: item for item in report["focus"]["getrandom"]}
        self.assertEqual(versions["0.3.4"]["shortest_dependency_path"],
                         ["codex-exec v0.0.0", "getrandom v0.3.4"])
        self.assertEqual(versions["0.3.4"]["feature_variants"], [["std"]])
        self.assertEqual(report["focus"]["v8"], [])

    def test_cycles_terminate_without_inventing_missing_packages(self):
        tree = "0codex-exec v0.0.0|\n1mio v1.2.0|\n2codex-exec v0.0.0| (*)\n"
        report = dependencies.tree_inventory(tree)
        self.assertEqual(report["package_count"], 2)
        self.assertEqual(report["focus"]["mio"][0]["shortest_dependency_path"],
                         ["codex-exec v0.0.0", "mio v1.2.0"])

    def test_invalid_root_depth_build_sections_and_proc_macros_fail(self):
        fixtures = ["", "0other v1.0.0|", "1codex-exec v0.0.0|",
                    "0codex-exec v0.0.0|\n2missing-parent v1.0.0|",
                    "0codex-exec v0.0.0|\n[build-dependencies]",
                    "0codex-exec v0.0.0|\n1macro v1.0.0 (proc-macro)|",
                    "0codex-exec v0.0.0|\n0codex-exec v0.0.0|"]
        for tree in fixtures:
            with self.subTest(tree=tree), self.assertRaises(ValueError):
                dependencies.tree_inventory(tree)

    def run_cli(self, directory, head="pinned", lock_hash="locked", status=""):
        root = Path(directory)
        pin, tree, output = root / "pin.json", root / "tree.txt", root / "report.json"
        pin.write_text(json.dumps({"commit": "pinned", "lockfile_sha256": "locked",
                                   "entry_package": "codex-exec"}), encoding="utf-8")
        tree.write_text("0codex-exec v0.0.0|\n1mio v1.2.0|default\n", encoding="utf-8")
        args = ["dependencies", "--source", str(root), "--tree", str(tree),
                "--pin", str(pin), "--output", str(output)]
        with patch("sys.argv", args), patch.object(dependencies, "digest", return_value=lock_hash), \
                patch.object(dependencies, "command", side_effect=[head, status]), \
                redirect_stderr(io.StringIO()):
            code = dependencies.main()
        return code, output

    def test_cli_requires_matching_clean_source_provenance(self):
        invalid = [("other", "locked", ""), ("pinned", "other-lock", ""),
                   ("pinned", "locked", " M file.rs\n")]
        for head, lock_hash, status in invalid:
            with self.subTest(head=head, lock_hash=lock_hash, status=status), \
                    tempfile.TemporaryDirectory() as directory:
                code, output = self.run_cli(directory, head, lock_hash, status)
                self.assertEqual(code, 1)
                self.assertFalse(output.exists())

    def test_cli_writes_pinned_inventory_without_claiming_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            code, output = self.run_cli(directory)
            self.assertEqual(code, 0)
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(report["package_count"], 2)
        self.assertEqual(report["provenance"]["commit"], "pinned")
        self.assertFalse(report["native_execution_verified"])


if __name__ == "__main__":
    unittest.main()
