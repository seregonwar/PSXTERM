#!/usr/bin/env python3
"""Inventory a selected Cargo normal-dependency tree, without executing an agent."""

import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

if __package__:
    from .audit import command, digest
else:
    from audit import command, digest


ROW = re.compile(r"^(\d+)([\w-]+) v([^\s|]+)(.*?)\|(.*)$")
FOCUS = ("getrandom", "mio", "portable-pty", "native-tls", "openssl-sys",
         "aws-lc-sys", "ring", "keyring", "libdbus-sys", "libsqlite3-sys",
         "v8", "codex-code-mode-runtime", "codex-code-mode-host")


def tree_inventory(text, entry="codex-exec"):
    """Require --prefix depth --edges normal,no-proc-macro --format '{p}|{f}'."""
    packages, edges, stack = {}, {}, []
    root = None
    for line in text.splitlines():
        if not line.strip():
            continue
        row = ROW.fullmatch(line)
        if not row:
            raise ValueError(f"unexpected tree line (normal-only tree required): {line}")
        depth, name, version, origin, features = row.groups()
        depth = int(depth)
        origin = origin.strip()
        if "(proc-macro)" in origin:
            raise ValueError("proc-macros must be excluded from this runtime dependency inventory")
        features = features.removesuffix(" (*)").strip()
        feature_set = sorted(filter(None, features.split(",")))
        identity = (name, version, origin)
        if depth == 0:
            if root is not None or name != entry:
                raise ValueError(f"expected one root package named {entry}")
            root = identity
        elif root is None or depth > len(stack):
            raise ValueError(f"invalid dependency depth: {line}")
        if identity not in packages:
            packages[identity] = {"name": name, "version": version, "origin": origin,
                                  "feature_variants": []}
            edges[identity] = set()
        if feature_set not in packages[identity]["feature_variants"]:
            packages[identity]["feature_variants"].append(feature_set)
        if depth:
            edges[stack[depth - 1]].add(identity)
        stack[depth:] = [identity]
    if root is None:
        raise ValueError("empty dependency tree")

    paths = {root: [root]}
    queue = deque([root])
    while queue:
        parent = queue.popleft()
        for child in sorted(edges[parent]):
            if child not in paths:
                paths[child] = paths[parent] + [child]
                queue.append(child)
    focus = {}
    for name in FOCUS:
        focus[name] = [{"version": key[1], "feature_variants": packages[key]["feature_variants"],
                        "shortest_dependency_path": [f"{item[0]} v{item[1]}" for item in paths[key]]}
                       for key in sorted(packages) if key[0] == name]
    return {
        "schema_version": 1,
        "native_execution_verified": False,
        "entry_package": entry,
        "target": "x86_64-unknown-freebsd",
        "method": "Cargo selected package, default features, normal dependency edges, "
                  "proc-macros excluded; omits host build dependencies; not compilation or execution",
        "package_count": len(packages),
        "edge_count": sum(len(children) for children in edges.values()),
        "focus": focus,
        "packages": [packages[key] for key in sorted(packages)],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--pin", type=Path, default=Path(__file__).with_name("upstream.json"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        pin = json.loads(args.pin.read_text(encoding="utf-8"))
        head = command(["git", "rev-parse", "HEAD"], args.source).strip()
        lock_hash = digest(args.source / "codex-rs/Cargo.lock")
        if head != pin["commit"] or lock_hash != pin["lockfile_sha256"]:
            raise ValueError("source commit or lockfile differs from the inspected upstream pin")
        status = command(["git", "status", "--porcelain"], args.source).splitlines()
        if status:
            raise ValueError("upstream checkout has modifications; cannot attribute tree to the clean pin")
        report = tree_inventory(args.tree.read_text(encoding="utf-8-sig"), pin["entry_package"])
        report["provenance"] = {
            "commit": head, "lockfile_sha256": lock_hash,
            "tree_sha256": hashlib.sha256(args.tree.read_bytes()).hexdigest(),
            "tree_generation_command": "cargo +1.95.0 tree --package codex-exec "
                "--target x86_64-unknown-freebsd --edges normal,no-proc-macro "
                "--locked --prefix depth --format '{p}|{f}'",
            "limitation": "checks the source pin and tree shape; does not attest how the input tree was generated",
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"dependencies: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
