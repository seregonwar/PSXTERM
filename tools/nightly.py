"""Nightly identity and desktop packaging; never modifies remote Git state."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import tarfile
import zipfile
from zoneinfo import ZoneInfo


def identity(sha, run_id, now, schedule="", existing_tags=()):
    if not re.fullmatch(r"[0-9a-f]{40}", sha) or not run_id.isdigit():
        raise ValueError("Expected a full commit SHA and numeric workflow run ID")
    local = now.astimezone(ZoneInfo("Europe/Rome"))
    expected_cron = "0 20 * * *" if local.utcoffset().total_seconds() == 7200 else "0 21 * * *"
    scheduled = bool(schedule)
    already_built = any(re.fullmatch(r"nightly-\d{8}-g[0-9a-f]{7}-r\d+", tag) for tag in existing_tags)
    return {
        "should_run": str(not scheduled or (schedule == expected_cron and not already_built)).lower(),
        "sha": sha,
        "tag": f"nightly-{local:%Y%m%d}-g{sha[:7]}-r{run_id}",
        "date": local.date().isoformat(),
    }


def package(root, output, target, tag, sha):
    if target not in {"linux-x86_64", "windows-x86_64", "macos-arm64"}:
        raise ValueError("Unsupported desktop target")
    if not re.fullmatch(r"nightly-\d{8}-g[0-9a-f]{7}-r\d+", tag):
        raise ValueError("Invalid nightly tag")
    if not re.fullmatch(r"[0-9a-f]{40}", sha) or f"-g{sha[:7]}-" not in tag:
        raise ValueError("Package commit does not match its tag")
    expected_machine = "arm64" if target == "macos-arm64" else "x86_64"
    actual_machine = {"amd64": "x86_64", "aarch64": "arm64"}.get(platform.machine().lower(), platform.machine().lower())
    expected_system = target.split("-")[0]
    actual_system = {"Darwin": "macos", "Windows": "windows", "Linux": "linux"}.get(platform.system())
    if (actual_system, actual_machine) != (expected_system, expected_machine):
        raise ValueError("Runner OS/architecture does not match the package target")
    name = f"psxterm-{tag}-{target}"
    destination = output / name
    destination.mkdir(parents=True, exist_ok=False)
    windows = target.startswith("windows")
    binary_name = "psxterm-tui.exe" if windows else "psxterm-tui"
    binaries = [root / "client/tui/target/release" / binary_name]
    if not windows:
        binaries.extend(root / "build-nightly-host" / name for name in ("psxtermd", "psxterm", "psxterm-ttyprobe", "cli_test"))
    for binary in binaries:
        shutil.copy2(binary, destination / binary.name)
        if not windows:
            (destination / binary.name).chmod(0o755)
    shutil.copy2(root / "LICENSE", destination / "LICENSE")
    shutil.copy2(root / "README.md", destination / "README.md")
    shutil.copytree(root / "docs", destination / "docs")
    metadata = {
        "project": "PSXTerm", "channel": "nightly", "tag": tag,
        "commit": sha, "target": target,
        "binaries": [binary.name for binary in binaries],
        "hardware_validation": "not performed by this workflow",
    }
    (destination / "BUILD_INFO.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    if windows:
        archive = output / f"{name}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            for file in sorted(destination.rglob("*")):
                if file.is_file():
                    bundle.write(file, file.relative_to(output).as_posix())
    else:
        archive = output / f"{name}.tar.gz"
        with tarfile.open(archive, "w:gz") as bundle:
            bundle.add(destination, arcname=name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / f"SHA256SUMS-{target}.txt").write_text(f"{digest}  {archive.name}\n", encoding="utf-8")
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    resolve = modes.add_parser("identity")
    resolve.add_argument("--sha", required=True)
    resolve.add_argument("--run-id", required=True)
    resolve.add_argument("--now", default=None)
    resolve.add_argument("--schedule", default="")
    resolve.add_argument("--existing-tags", default="")
    bundle = modes.add_parser("package")
    bundle.add_argument("--target", required=True)
    bundle.add_argument("--tag", required=True)
    bundle.add_argument("--sha", required=True)
    bundle.add_argument("--output", type=Path, default=Path("dist"))
    args = parser.parse_args()
    if args.mode == "identity":
        now = datetime.fromisoformat(args.now) if args.now else datetime.now(timezone.utc)
        if now.tzinfo is None:
            parser.error("--now requires a timezone")
        for key, value in identity(args.sha, args.run_id, now, args.schedule, args.existing_tags.splitlines()).items():
            print(f"{key}={value}")
    else:
        root = Path(__file__).resolve().parents[1]
        print(package(root, args.output, args.target, args.tag, args.sha))


if __name__ == "__main__":
    main()
