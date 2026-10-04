"""Nightly identity and binary packaging; never modifies remote Git state."""

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


def validate_payload(path):
    with path.open("rb") as binary:
        header = binary.read(64)
    if (len(header) != 64 or header[:6] != b"\x7fELF\x02\x01"
            or int.from_bytes(header[16:18], "little") not in (2, 3)
            or int.from_bytes(header[18:20], "little") != 62):
        raise ValueError(f"Expected a little-endian x86-64 ELF payload: {path.name}")


def package(root, output, target, tag, sha, sdk=None):
    console = target in {"ps4", "ps5"}
    if target not in {"linux-x86_64", "windows-x86_64", "macos-arm64", "ps4", "ps5"}:
        raise ValueError("Unsupported package target")
    if not re.fullmatch(r"nightly-\d{8}-g[0-9a-f]{7}-r\d+", tag):
        raise ValueError("Invalid nightly tag")
    if not re.fullmatch(r"[0-9a-f]{40}", sha) or f"-g{sha[:7]}-" not in tag:
        raise ValueError("Package commit does not match its tag")
    expected_machine = "arm64" if target == "macos-arm64" else "x86_64"
    actual_machine = {"amd64": "x86_64", "aarch64": "arm64"}.get(platform.machine().lower(), platform.machine().lower())
    expected_system = "linux" if console else target.split("-")[0]
    actual_system = {"Darwin": "macos", "Windows": "windows", "Linux": "linux"}.get(platform.system())
    if (actual_system, actual_machine) != (expected_system, expected_machine):
        raise ValueError("Runner OS/architecture does not match the package target")
    if console and (not sdk or sdk.get("repository") != f"{target}-payload-dev/sdk"
                    or not sdk.get("version")
                    or not re.fullmatch(r"[0-9a-f]{64}", sdk.get("sha256", ""))):
        raise ValueError("Console packages require SDK repository, version and SHA256")
    package_name = f"psxterm-{tag}-{target}"
    destination = output / package_name
    destination.mkdir(parents=True, exist_ok=False)
    windows = target.startswith("windows")
    if console:
        build = root / f"build-nightly-{target}"
        names = ["cli_test.elf", "exec_wrapper.elf", "hello.elf", "netprobe.elf"]
        if target == "ps5":
            names.append("pslist.elf")
        binaries = [(build / "psxtermd", f"psxtermd-{target}.elf"),
                    (build / "psxterm-ttyprobe", f"psxterm-ttyprobe-{target}.elf")]
        binaries.extend((build / name, name) for name in names)
        binaries.extend((binary, binary.name) for binary in sorted(build.glob("*.elf"))
                        if binary.name not in names)
        for binary, _ in binaries:
            validate_payload(binary)
    else:
        binary_name = "psxterm-tui.exe" if windows else "psxterm-tui"
        sources = [root / "client/tui/target/release" / binary_name]
        if not windows:
            sources.extend(root / "build-nightly-host" / name for name in ("psxtermd", "psxterm", "psxterm-ttyprobe", "cli_test"))
        binaries = [(binary, binary.name) for binary in sources]
    for binary, name in binaries:
        shutil.copy2(binary, destination / name)
        if not windows:
            (destination / name).chmod(0o755)
    shutil.copy2(root / "LICENSE", destination / "LICENSE")
    shutil.copy2(root / "README.md", destination / "README.md")
    shutil.copytree(root / "docs", destination / "docs")
    metadata = {
        "project": "PSXTerm", "channel": "nightly", "tag": tag,
        "commit": sha, "target": target,
        "binaries": [name for _, name in binaries],
        "hardware_validation": "not performed by this workflow",
    }
    if console:
        metadata["sdk"] = sdk
    (destination / "BUILD_INFO.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    if windows:
        archive = output / f"{package_name}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            for file in sorted(destination.rglob("*")):
                if file.is_file():
                    bundle.write(file, file.relative_to(output).as_posix())
    else:
        archive = output / f"{package_name}.tar.gz"
        executable_members = {f"{package_name}/{name}" for _, name in binaries}

        def archive_modes(member):
            member.mode = 0o755 if member.isdir() or member.name in executable_members else 0o644
            return member

        with tarfile.open(archive, "w:gz") as bundle:
            bundle.add(destination, arcname=package_name, filter=archive_modes)
    assets = [archive]
    if console:
        payload = output / f"psxtermd-{target}.elf"
        shutil.copy2(destination / payload.name, payload)
        assets.append(payload)
    checksums = "".join(f"{hashlib.sha256(asset.read_bytes()).hexdigest()}  {asset.name}\n"
                        for asset in assets)
    (output / f"SHA256SUMS-{target}.txt").write_text(checksums, encoding="utf-8")
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
    bundle.add_argument("--sdk-repo")
    bundle.add_argument("--sdk-version")
    bundle.add_argument("--sdk-sha256")
    args = parser.parse_args()
    if args.mode == "identity":
        now = datetime.fromisoformat(args.now) if args.now else datetime.now(timezone.utc)
        if now.tzinfo is None:
            parser.error("--now requires a timezone")
        for key, value in identity(args.sha, args.run_id, now, args.schedule, args.existing_tags.splitlines()).items():
            print(f"{key}={value}")
    else:
        root = Path(__file__).resolve().parents[1]
        sdk = None
        if args.target in {"ps4", "ps5"}:
            sdk = {"repository": args.sdk_repo, "version": args.sdk_version,
                   "sha256": args.sdk_sha256 or ""}
        print(package(root, args.output, args.target, args.tag, args.sha, sdk))


if __name__ == "__main__":
    main()
