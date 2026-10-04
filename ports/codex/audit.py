#!/usr/bin/env python3
"""Offline source/dependency and SDK export inventory for the native port.

This reads manifests and library symbols. It never runs an agent, a target
binary, a loader, or a network/authentication request. Export declarations are
not evidence that an ABI or a function works on hardware.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tomllib


POSIX_CANDIDATES = {
    "threads": "pthread_create pthread_join pthread_detach pthread_once pthread_self "
               "pthread_mutex_lock pthread_mutex_unlock pthread_cond_wait pthread_cond_timedwait "
               "pthread_key_create pthread_getspecific pthread_setspecific".split(),
    "event_loop": "kqueue kevent poll fcntl".split(),
    "network": "socket connect recv send getaddrinfo freeaddrinfo".split(),
    "memory": "mmap munmap mprotect malloc free".split(),
    "clock_entropy": "clock_gettime gettimeofday arc4random_buf getrandom getentropy".split(),
    "process": "fork execve posix_spawn posix_spawnp posix_spawn_file_actions_init "
               "posix_spawn_file_actions_addchdir_np waitpid kill sigaction setrlimit".split(),
    "files_stdio": "read write close open pipe dup2 rename fsync ioctl tcgetattr".split(),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(args, cwd=None):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=60)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed: {result.stderr.strip()}")
    return result.stdout


def read_manifest(path):
    with path.open("rb") as handle:
        return tomllib.load(handle)


def declared_dependencies(manifest, workspace, workspace_root, manifest_path):
    """Keep all platform and optional guards; do not pretend to resolve Cargo."""
    scopes = [(None, manifest)] + list(manifest.get("target", {}).items())
    result = []
    for selector, tables in scopes:
        for kind in ("dependencies", "build-dependencies"):
            for alias, raw in tables.get(kind, {}).items():
                spec = {"version": raw} if isinstance(raw, str) else dict(raw)
                owner = manifest_path.parent
                if spec.get("workspace"):
                    base = workspace.get(alias)
                    if base is None:
                        raise ValueError(f"missing workspace dependency {alias} in {manifest_path}")
                    base = {"version": base} if isinstance(base, str) else dict(base)
                    features = sorted(set(base.get("features", [])) | set(spec.get("features", [])))
                    base.update({k: v for k, v in spec.items() if k not in ("workspace", "features")})
                    base["features"] = features
                    spec = base
                    owner = workspace_root
                local = (owner / spec["path"] / "Cargo.toml").resolve() if "path" in spec else None
                result.append({
                    "alias": alias,
                    "package": spec.get("package", alias),
                    "kind": kind,
                    "target_guard": selector,
                    "optional": spec.get("optional", False),
                    "features": spec.get("features", []),
                    "default_features": spec.get("default-features", True),
                    "version": spec.get("version"),
                    "git": spec.get("git"),
                    "revision": spec.get("rev"),
                    "manifest": str(local) if local else None,
                })
    return result


def source_inventory(source):
    source = source.resolve()
    root = source / "codex-rs"
    workspace = read_manifest(root / "Cargo.toml")["workspace"]
    deps = workspace.get("dependencies", {})
    pending = [root / "exec" / "Cargo.toml"]
    seen = set()
    crates = []
    while pending:
        path = pending.pop().resolve()
        if path in seen:
            continue
        if not path.is_relative_to(source):
            raise ValueError(f"dependency leaves the source checkout: {path}")
        manifest = read_manifest(path)
        seen.add(path)
        declared = declared_dependencies(manifest, deps, root, path)
        for item in declared:
            if item["manifest"]:
                child = Path(item["manifest"])
                if not child.is_relative_to(source):
                    raise ValueError(f"dependency leaves the source checkout: {child}")
                pending.append(child)
                item["manifest"] = child.relative_to(source).as_posix()
        crates.append({"name": manifest["package"]["name"],
                       "manifest": path.relative_to(source).as_posix(),
                       "sha256": digest(path), "dependencies": declared})
    toolchain_file = root / "rust-toolchain.toml"
    toolchain = read_manifest(toolchain_file)["toolchain"]
    lock = root / "Cargo.lock"
    return {
        "repository": command(["git", "remote", "get-url", "origin"], source).strip(),
        "commit": command(["git", "rev-parse", "HEAD"], source).strip(),
        "checkout_status": command(["git", "status", "--porcelain"], source).splitlines(),
        "toolchain": toolchain,
        "toolchain_sha256": digest(toolchain_file),
        "lockfile_sha256": digest(lock),
        "entry_package": "codex-exec",
        "method": "conservative manifest closure including optional/target/build dependencies; "
                  "not a Cargo feature resolution or a successful compilation",
        "local_package_count": len(crates),
        "local_packages": sorted(crates, key=lambda item: item["name"]),
    }


def parse_nm(text):
    """Defined GNU/LLVM nm rows; skip archive member headings and undefineds."""
    symbols = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 3 and len(fields[-2]) == 1 and fields[-2].upper() != "U":
            symbols.add(fields[-1].split("@", 1)[0])
    return symbols


def sdk_inventory(sdk, nm):
    sdk = sdk.resolve()
    lib = sdk / "target" / "lib"
    files = sorted(set(lib.glob("*.a")) | set(lib.glob("*.so")))
    if not files:
        raise ValueError(f"SDK has no target libraries: {lib}")
    candidates = {name for names in POSIX_CANDIDATES.values() for name in names}
    providers = {name: [] for name in sorted(candidates)}
    libraries = []
    complete = True
    for path in files:
        args = [nm, "-g", "--defined-only"]
        if path.suffix == ".so":
            args.append("-D")
        args.append(str(path))
        item = {"name": path.name, "sha256": digest(path)}
        try:
            exports = parse_nm(command(args))
            item.update({"status": "inspected", "export_count": len(exports)})
            for name in candidates & exports:
                providers[name].append(path.name)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            complete = False
            item.update({"status": "not_inspected", "error": str(error)})
        libraries.append(item)
    return {
        "path": str(sdk),
        "method": "candidate POSIX APIs and declared SDK exports only; does not resolve target guards "
                  "or establish required imports, runtime binding, ABI layouts or hardware behavior",
        "inventory_complete": complete,
        "libraries": libraries,
        "groups": {group: {name: providers[name] for name in names}
                   for group, names in POSIX_CANDIDATES.items()},
        "not_declared_in_inspected_libraries": sorted(name for name, libs in providers.items() if not libs),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = {"schema_version": 1, "native_execution_verified": False,
                  "source": source_inventory(args.source)}
        if args.sdk:
            report["sdk"] = sdk_inventory(args.sdk, args.nm)
        text = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"audit: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
