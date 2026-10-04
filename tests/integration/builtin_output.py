"""Output contracts and GNU oracles, exercised through a real daemon."""

import errno
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time
import unicodedata


ANSI = re.compile(r"\x1b\[[0-9;]*m")


def gnu_tool(name):
    for candidate in (name, "g" + name):
        path = shutil.which(candidate)
        if path and b"GNU coreutils" in subprocess.run([path, "--version"], capture_output=True).stdout:
            return path
    print(f"    SKIP GNU {name} oracle: GNU coreutils unavailable (output contracts still run)", flush=True)
    return None


def run(wire, daemon, command, width=80):
    client = wire.Client(daemon)
    stdout, stderr = bytearray(), bytearray()
    code = None
    try:
        client.drain(quiet=0.01)
        client.send(wire.RESIZE, struct.pack("<HH", 24, width))
        client.send(wire.EXEC, command.encode())
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            header, payload = client.read_frame(timeout=max(0.1, deadline - time.monotonic()))
            if header[0] == wire.STDOUT:
                stdout.extend(payload)
            elif header[0] == wire.STDERR:
                stderr.extend(payload)
            elif header[0] == wire.EXIT:
                code = struct.unpack("<i", payload[:4])[0]
                break
        assert code is not None, f"no exit status for {command}"
        return bytes(stdout), bytes(stderr), code
    finally:
        client.close()


def invoke(wire, daemon, args, width=80):
    # Use the actual psh grammar: double quotes and escaped quotes/backslashes.
    return run(wire, daemon, " ".join('"' + str(arg).replace('\\', '\\\\').replace('"', '\\"') + '"'
                                     for arg in args), width)


def create_listing(root):
    for name, size in [("alpha", 0), ("beta.txt", 12), ("gamma.dat", 1025),
                       ("delta.bin", 20480), ("epsilon", 10240), ("zeta", 1024),
                       (".hidden", 5), ("runner", 10)]:
        path = root / name
        path.write_bytes(b"x" * size)
        path.chmod(0o755 if name == "runner" else 0o644)
    (root / "folder").mkdir()
    (root / "empty").mkdir()
    (root / "link").symlink_to("beta.txt")
    (root / "broken").symlink_to("missing")
    (root / "dirlink").symlink_to("folder")
    os.mkfifo(root / "fifo")
    stamp = int(time.time()) - 86400
    for index, path in enumerate(sorted(root.iterdir())):
        os.utime(path, (stamp + index, stamp + index), follow_symlinks=False)
    os.link(root / "beta.txt", root / "hardlink")


def ls_gnu_matrix(wire):
    ls = gnu_tool("ls")
    if not ls:
        return
    with tempfile.TemporaryDirectory(prefix="psxterm-output-") as directory:
        root = Path(directory)
        create_listing(root)
        with wire.Daemon(env={"LC_ALL": "C", "TZ": "UTC", "NO_COLOR": ""}) as daemon:
            modes = [[], ["-1"], ["-x"], ["-a1"], ["-A1"], ["-1F"], ["-1p"],
                     ["-r1"], ["-S1"], ["-t1"], ["-ln"], ["-lnh"], ["-ln1"],
                     ["-1C"], ["-lC"], ["-x1"], ["-aA1"], ["-Aa1"]]
            operands = [[str(root)], [str(root / "empty")],
                        [str(root / "link"), str(root / "broken")],
                        [str(root / "beta.txt"), str(root / "alpha")],
                        [str(root / "folder"), str(root / "alpha"), str(root / "empty")]]
            checked = 0
            for mode in modes:
                for paths in operands:
                    width = 55
                    args = ["ls", "--color=never", *mode, *paths]
                    actual, error, code = invoke(wire, daemon, args, width)
                    expected = subprocess.run([ls, "--color=never", "-T0", "-w", str(width),
                                               *(mode or ["-C"]), *paths],
                                              capture_output=True, env={**os.environ, "LC_ALL": "C", "TZ": "UTC"})
                    assert code == expected.returncode and not error, (args, error, code)
                    assert actual == expected.stdout, f"{args}\nactual: {actual!r}\nGNU: {expected.stdout!r}"
                    checked += 1
            for mode in [[], ["-d"], ["-ldn"], ["-dF"]]:
                args = ["ls", "--color=never", *mode, str(root / "dirlink")]
                actual, error, code = invoke(wire, daemon, args)
                expected = subprocess.run([ls, "--color=never", "-T0", *(mode or ["-C"]), args[-1]],
                                          capture_output=True, env={**os.environ, "LC_ALL": "C", "TZ": "UTC"})
                assert code == 0 and not error and actual == expected.stdout, (args, actual, expected.stdout)
            actual, error, code = invoke(wire, daemon, ["ls", str(root), "-1", "--color=never"])
            expected = subprocess.check_output([ls, "-1", "--color=never", str(root)], env={**os.environ, "LC_ALL": "C"})
            assert code == 0 and not error and actual == expected
            for option in ["-R", "-q", "--colorful", "--color=invalid", "-z"]:
                actual, error, code = invoke(wire, daemon, ["ls", option, str(root)])
                assert code == 2 and not actual and error, (option, actual, error, code)
            actual, error, code = invoke(wire, daemon, ["ls", "--color=never", "-1", root / "missing", root / "alpha"])
            assert code == 1 and actual.endswith(b"/alpha\n") and b"cannot access" in error
            print(f"    ls: {checked + 5} exact GNU output comparisons", flush=True)


def cat_gnu_matrix(wire):
    cat = gnu_tool("cat")
    if not cat:
        return
    with tempfile.TemporaryDirectory(prefix="psxterm-cat-output-") as directory:
        root = Path(directory)
        files = [root / "one", root / "two", root / "three"]
        files[0].write_bytes(b"first\n\n\n\tsecond\r\n" + bytes(range(256)) + b"\npartial")
        files[1].write_bytes(b" continuation\n\n\nend\r")
        files[2].write_bytes(b"\n\n\tfinal\n" + b"long" * 3000 + b"\r\n")
        with wire.Daemon() as daemon:
            checked = 0
            for mode in [[], ["-n"], ["-b"], ["-s"], ["-E"], ["-T"], ["-v"], ["-A"],
                         ["-nbs"], ["-nsE"], ["-bnA"], ["-nb"], ["-bn"]]:
                actual, error, code = invoke(wire, daemon, ["cat", *mode, *files])
                expected = subprocess.check_output([cat, *mode, *map(str, files)])
                assert not error and code == 0 and actual == expected, (mode, actual[:100], expected[:100])
                checked += 1
            actual, error, code = invoke(wire, daemon, ["cat", root / "missing", files[0]])
            assert code == 1 and error and actual == files[0].read_bytes()
            actual, error, code = invoke(wire, daemon, ["cat", "-"])
            assert code == 1 and not actual and b"stdin is not supported" in error
            dash = root / "-n"
            dash.write_bytes(b"literal option filename\n")
            actual, error, code = invoke(wire, daemon, ["cat", "--", dash])
            assert code == 0 and not error and actual == dash.read_bytes()
            print(f"    cat: {checked} exact GNU output comparisons including all 256 byte values", flush=True)


def cells(text):
    return sum(0 if unicodedata.combining(c) else 2 if unicodedata.east_asian_width(c) in "WF" else 1
               for c in text)


def ls_terminal_matrix(wire):
    with tempfile.TemporaryDirectory(prefix="psxterm-ls-terminal-") as directory:
        root = Path(directory)
        names = ["alpha", "beta", "gamma", "中文", "caffè", "e\u0301", "📁", "two words", "a'quote",
                 "z" * 200, "bad\nname", "tab\tname", "escape\x1b[31m", "archive.tar.gz"]
        for name in names:
            (root / name).touch()
        (root / "folder").mkdir()
        (root / "runner").touch()
        (root / "runner").chmod(0o755)
        (root / "link").symlink_to("alpha")
        (root / "broken").symlink_to("missing")
        invalid_names = True
        try:
            os.close(os.open(os.fsencode(root) + b"/invalid-\xff", os.O_CREAT | os.O_WRONLY, 0o644))
            os.close(os.open(os.fsencode(root) + b"/" + b"\xff" * 255, os.O_CREAT | os.O_WRONLY, 0o644))
        except OSError as error:
            if error.errno not in (errno.EILSEQ, errno.EINVAL):
                raise
            invalid_names = False  # Some host filesystems require valid Unicode names.
        long_target = "/".join(["t" * 200] * 4)
        (root / "long-link").symlink_to(long_target)
        with wire.Daemon(env={"NO_COLOR": ""}) as daemon:
            out, err, code = invoke(wire, daemon, ["ls", "-ln", "--color=never", root])
            assert code == 0 and not err
            if invalid_names:
                assert b"\\xff" * 255 + b"\n" in out
            assert b" -> " + long_target.encode() + b"\n" in out
        for width in [1, 12, 24, 55, 80, 140]:
            with wire.Daemon(env={"NO_COLOR": ""}) as daemon:
                for mode in [[], ["-x"], ["-F"]]:
                    actual, error, code = invoke(wire, daemon, ["ls", *mode, root], width)
                    assert code == 0 and not error
                    text = actual.decode()
                    plain = ANSI.sub("", text)
                    assert "\\x1b[31m" in plain and "\\x0a" in plain and "\\x09" in plain
                    assert "two words" in plain and "中文" in plain
                    if invalid_names:
                        assert "\\xff" in plain
                    assert "\x1b" not in plain
                    assert "\x1b[1;94mfolder\x1b[0m" in text or "\x1b[1;94mfolder/\x1b[0m" in text
                    assert "\x1b[1;92mrunner" in text and "\x1b[1;96mlink" in text
                    assert "\x1b[1;91mbroken" in text and "\x1b[1;91marchive.tar.gz" in text
                    for line in plain.splitlines():
                        assert not line.endswith(" "), repr(line)
                        if cells(line) > width:
                            # A single filename longer than the terminal must be preserved.
                            assert "  " not in line, (width, repr(line))
        for colors, expected in [("di=1;93:*.tar.gz=1;96", b"\x1b[1;93mfolder"),
                                 ("di=0", b"\x1b[0mfolder"),
                                 ("di=", b"folder\n")]:
            with wire.Daemon(env={"LS_COLORS": colors, "NO_COLOR": ""}) as custom:
                out, err, code = invoke(wire, custom, ["ls", "-1", root])
                assert code == 0 and not err and expected in out
                if "*.tar.gz" in colors:
                    assert b"\x1b[1;96marchive.tar.gz" in out
        with wire.Daemon(env={"NO_COLOR": "1"}) as daemon:
            out, err, code = invoke(wire, daemon, ["ls", root])
            assert code == 0 and not err and b"\x1b" not in out
            out, err, code = invoke(wire, daemon, ["ls", "--color=always", root])
            assert code == 0 and not err and b"\x1b[1;94m" in out



def builtin_contracts(wire):
    commands = ["help", "pwd", "cd", "ls", "cat", "clear", "env", "export", "unset", "uname", "whoami", "ps", "exit"]
    with tempfile.TemporaryDirectory(prefix="psxterm-contracts-") as directory:
        with wire.Daemon(env={"PSXTERM_OUTPUT_LONG": "x" * 10000, "PSXTERM_RUNTIME_BASE": directory}) as daemon:
            for command in commands:
                out, err, code = run(wire, daemon, command + " --help")
                assert code == 0 and not err and ("usage: " + command).encode() in out, (command, out, err, code)
            out, err, code = run(wire, daemon, "help")
            assert code == 0 and not err and all(("  " + command).encode() in out for command in commands)
            out, err, code = run(wire, daemon, "help missing pwd")
            assert code == 1 and b"no such built-in" in err and b"usage: pwd" in out
            out, err, code = run(wire, daemon, "env")
            assert code == 0 and not err and b"PSXTERM_OUTPUT_LONG=" + b"x" * 10000 + b"\n" in out
            assert out.splitlines() == sorted(out.splitlines())
            out, err, code = run(wire, daemon, "env -0")
            assert code == 0 and not err and out.endswith(b"\0") and b"PSXTERM_OUTPUT_LONG=" + b"x" * 10000 + b"\0" in out
            out, err, code = run(wire, daemon, "clear")
            assert (out, err, code) == (b"\x1b[2J\x1b[H", b"", 0)
            for command in ["pwd", "uname", "whoami", "ps"]:
                out, err, code = run(wire, daemon, command)
                assert code == 0 and not err and out.endswith(b"\n") and b"\x1b" not in out
                out, err, code = run(wire, daemon, command + " --invalid")
                assert code == 2 and not out and b"unexpected argument" in err
            out, err, code = run(wire, daemon, "export -p")
            assert code == 0 and not err and out.startswith(b"export ") and len(out) > 10000
            for command in ["export 1BAD=value", "unset BAD-NAME", "exit no-number"]:
                out, err, code = run(wire, daemon, command)
                assert code != 0 and not out and err, command
            out, err, code = run(wire, daemon, "exit 7")
            assert (out, err, code) == (b"", b"", 7)
