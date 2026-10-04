#!/usr/bin/env python3
"""Compare compile-only x86_64 little-endian ABI constants; never run binaries."""

import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import sys


OBJECT = re.compile(r"^psx_abi_(\w+):\s*\n(.*?)^\s*\.size\s+psx_abi_\1,\s*8\b", re.M | re.S)


def constants(assembly):
    values = {}
    labels = re.findall(r"^psx_abi_(\w+):", assembly, re.M)
    for name, body in OBJECT.findall(assembly):
        if name in values:
            raise ValueError(f"duplicate ABI constant: {name}")
        data = bytearray()
        for line in body.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            directive, _, operand = line.partition("\t")
            if not operand:
                parts = line.split(None, 1)
                directive, operand = parts if len(parts) == 2 else (line, "")
            if directive in (".ascii", ".asciz"):
                literal = ast.literal_eval("b" + operand.strip())
                if not isinstance(literal, bytes):
                    raise ValueError(f"invalid byte string for {name}")
                data.extend(literal)
                if directive == ".asciz":
                    data.append(0)
            elif directive == ".quad":
                number = int(operand.split("#", 1)[0].strip(), 0)
                data.extend((number & ((1 << 64) - 1)).to_bytes(8, "little"))
            elif directive == ".zero":
                parts = operand.split("#", 1)[0].split(",")
                count = int(parts[0], 0)
                fill = int(parts[1], 0) if len(parts) == 2 else 0
                if len(parts) > 2 or not 0 <= count <= 8 or not 0 <= fill <= 255:
                    raise ValueError(f"invalid zero/fill directive for {name}")
                data.extend(bytes([fill]) * count)
            else:
                raise ValueError(f"unsupported assembly directive for {name}: {directive}")
        if len(data) != 8:
            raise ValueError(f"ABI constant {name} has {len(data)} bytes instead of 8")
        values[name] = int.from_bytes(data, "little")
    if not labels or len(labels) != len(values) or set(labels) != set(values):
        raise ValueError("ABI inventory is empty, duplicated or contains incomplete objects")
    return values


def compare(sdk, rust):
    if set(sdk) != set(rust):
        raise ValueError(f"different inventory keys: SDK-only={sorted(set(sdk) - set(rust))}, "
                         f"Rust-only={sorted(set(rust) - set(sdk))}")
    if not sdk:
        raise ValueError("empty ABI inventory")
    differences = [{"name": name, "sdk": sdk[name], "rust": rust[name]}
                   for name in sorted(sdk) if sdk[name] != rust[name]]
    return {
        "schema_version": 1,
        "native_execution_verified": False,
        "method": "compile-only named 64-bit constants; x86_64 little-endian; "
                  "a match does not establish complete ABI compatibility or runtime behavior",
        "constant_count": len(sdk),
        "measured_headers_match": not differences,
        "mismatches": differences,
        "sdk_constants": sdk,
        "rust_constants": rust,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-assembly", type=Path, required=True)
    parser.add_argument("--rust-assembly", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        sdk_text = args.sdk_assembly.read_text(encoding="utf-8")
        rust_text = args.rust_assembly.read_text(encoding="utf-8")
        report = compare(constants(sdk_text), constants(rust_text))
        report["inputs"] = {
            "sdk": {"path": str(args.sdk_assembly), "sha256": hashlib.sha256(args.sdk_assembly.read_bytes()).hexdigest()},
            "rust": {"path": str(args.rust_assembly), "sha256": hashlib.sha256(args.rust_assembly.read_bytes()).hexdigest()},
        }
        text = json.dumps(report, indent=2) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
    except (OSError, ValueError, SyntaxError) as error:
        print(f"compare-abi: {error}", file=sys.stderr)
        return 1
    return 0 if report["measured_headers_match"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
