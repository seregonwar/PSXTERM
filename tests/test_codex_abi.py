"""Ensure compile-only ABI mismatches cannot be reported as successful ports."""

import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from ports.codex import compare_abi


def object_text(name, directive):
    return f"psx_abi_{name}:\n\t{directive}\n\t.size psx_abi_{name}, 8\n"


class AbiTests(unittest.TestCase):
    def test_clang_quad_and_rust_escaped_bytes_have_identical_values(self):
        c = object_text("size", ".quad 8 # alignment") + object_text("filter", ".quad -1")
        rust = object_text("size", r'.asciz "\b\000\000\000\000\000\000"')
        rust += object_text("filter", ".zero 8,255")
        report = compare_abi.compare(compare_abi.constants(c), compare_abi.constants(rust))
        self.assertTrue(report["measured_headers_match"])
        self.assertFalse(report["native_execution_verified"])
        self.assertEqual(report["constant_count"], 2)

    def test_numeric_mismatch_is_preserved(self):
        report = compare_abi.compare({"kevent_size": 32}, {"kevent_size": 64})
        self.assertFalse(report["measured_headers_match"])
        self.assertEqual(report["mismatches"], [{"name": "kevent_size", "sdk": 32, "rust": 64}])

    def test_missing_inventory_key_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "different inventory keys"):
            compare_abi.compare({"stat": 120}, {"kevent": 64})

    def test_empty_duplicate_and_truncated_objects_are_rejected(self):
        fixtures = ["", object_text("size", ".quad 8") * 2,
                    object_text("size", ".zero 7"), "psx_abi_size:\n\t.quad 8\n"]
        for fixture in fixtures:
            with self.subTest(fixture=fixture), self.assertRaises(ValueError):
                compare_abi.constants(fixture)

    def test_unknown_directives_and_oversized_fill_are_rejected(self):
        for directive in [".long 8", ".zero 100000000", ".zero 8,256"]:
            with self.subTest(directive=directive), self.assertRaises(ValueError):
                compare_abi.constants(object_text("size", directive))

    def test_cli_reports_match_or_mismatch_without_claiming_native_execution(self):
        for rust_size, expected_code in [(8, 0), (16, 2)]:
            with self.subTest(rust_size=rust_size), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                sdk, rust, output = root / "sdk.s", root / "rust.s", root / "report.json"
                sdk.write_text(object_text("size", ".quad 8"), encoding="utf-8")
                rust.write_text(object_text("size", f".quad {rust_size}"), encoding="utf-8")
                args = ["compare-abi", "--sdk-assembly", str(sdk), "--rust-assembly", str(rust),
                        "--output", str(output)]
                with patch("sys.argv", args):
                    self.assertEqual(compare_abi.main(), expected_code)
                report = json.loads(output.read_text(encoding="utf-8"))
                self.assertFalse(report["native_execution_verified"])
                self.assertEqual(len(report["inputs"]["sdk"]["sha256"]), 64)


if __name__ == "__main__":
    unittest.main()
