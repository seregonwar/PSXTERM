#!/usr/bin/env python3
"""Exercise the compiled client through a real POSIX PTY, without a console."""

import argparse
import errno
import json
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import tempfile
import time


SIZES = [(55, 18), (60, 20), (72, 20), (80, 24), (100, 30), (120, 35), (140, 44), (200, 60)]


def run_case(executable, decoder, cols, rows, language, output):
    import fcntl
    import pty
    import termios

    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    transcript = bytearray()
    process = None
    started = time.monotonic()
    viewport = [cols, rows]
    export_directory = tempfile.TemporaryDirectory(prefix="psxterm-export-")
    translations = json.loads((Path(__file__).resolve().parents[1] / "locales" / f"{language}.json").read_text(encoding="utf-8"))

    def size(width, height):
        viewport[:] = [width, height]
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))
        if process is not None:
            os.kill(process.pid, signal.SIGWINCH)

    def controlling_terminal():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)

    def redraw(stage, expected=None, absent=None, required=True):
        received = bytearray()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            readable, _, _ = select.select([master], [], [], 0.15)
            if not readable:
                if received or not required:
                    break
                if process.poll() is not None:
                    raise AssertionError(f"client exited during {stage}: {process.returncode}")
                continue
            try:
                data = os.read(master, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            if not data:
                break
            received.extend(data)
            if expected is not None or absent is not None:
                screen = subprocess.run(
                    [str(decoder), *map(str, viewport)], input=transcript + received,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=2,
                ).stdout.decode("utf-8")
                # Wrapped lines in a dialog have vertical borders between
                # their text; omit those edges when matching the whole message.
                plain = " ".join(" ".join(line.strip(" │") for line in screen.splitlines()).split())
                matches = expected is None or " ".join(expected.split()) in plain
                disappeared = absent is None or " ".join(absent.split()) not in plain
                if matches and disappeared:
                    transcript.extend(received)
                    return
        transcript.extend(received)
        if required and not received:
            raise AssertionError(f"no terminal redraw during {stage}")
        if expected is not None or absent is not None:
            raise AssertionError(f"incorrect screen during {stage}: {screen!r}")

    def key(sequence, stage, expected=None, absent=None):
        os.write(master, sequence)
        redraw(stage, expected=expected, absent=absent)

    try:
        size(cols, rows)
        environment = os.environ.copy()
        environment["TERM"] = "xterm-256color"
        process = subprocess.Popen(
            [str(executable), "--demo", "--lang", language],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            env=environment,
            preexec_fn=controlling_terminal,
        )
        redraw("startup", expected="DEMO")
        redraw("settle startup", required=False)
        assert not select.select([master], [], [], 0.2)[0], "unchanged workspace redraws during idle"
        key(b"\x1bOQ", "flash palette", expected=translations["ui.search"])  # F2
        key(b"\x1b", "dismiss palette", expected="DEMO", absent=translations["ui.search"])
        key(b"\x1bOQ", "color palette", expected=translations["ui.search"])
        key(b"/colorX", "palette query", expected="/colorX")
        key(b"\x1b[D\x1b[3~s", "edit palette query", expected="/colors")
        key(b"\r", "original colors", expected=translations["notice.colors_original"])
        key(b"\x1bOQ", "color palette again", expected=translations["ui.search"])
        key(b"/colors\r", "readable colors", expected=translations["notice.colors_readable"])
        key(b"\x1b[18~", "rename dialog", expected=translations["action.rename"])  # F7
        key(b"\x15\x1b[200~" + "Edit 界".encode("utf-8") + b"\x1b[201~", "rename text", expected="Edit 界")
        key(b"\x01\x1b[3~\x1b[200~R\x1b[201~", "edit beginning of name", expected="Rdit 界")
        key(b"\r", "save renamed terminal", expected="Rdit 界")
        key(b"\x1b[23~", "terminal search", expected=translations["search.label"])  # F11
        key(b"\x1b[200~projects\x1b[201~", "search result", expected=translations["search.count"].format("1", "1"))
        key(b"\x01\x1b[3~", "edit search beginning", expected="rojects")
        key(b"\x1b[200~p\x1b[201~", "paste at search caret", expected="projects")
        key(b"\x15missing-pattern", "search no result", expected=translations["search.none"])
        key(b"\x15projects", "search again", expected=translations["search.count"].format("1", "1"))
        size(cols + 1, rows)
        redraw("search after resize", expected=translations["search.changed"].split(" · ")[0])
        key(b"\x1b[15~", "refresh search", expected=translations["search.count"].format("1", "1"))  # F5
        size(cols, rows)
        redraw("search dimensions restored", expected=translations["search.changed"].split(" · ")[0])
        key(b"\x1b", "dismiss search", expected="DEMO", absent=translations["search.label"])
        key(b"\x1b[24~", "export dialog", expected=translations["action.export"])  # F12
        export_path = Path(export_directory.name) / "output 界.txt"
        key(b"\x15\x1b[200~" + str(export_path).encode("utf-8") + b"\x1b[201~", "export path", expected=export_path.name)
        key(b"\t\x1b[B", "visible output scope", expected="● " + translations["export.visible"])
        saved_prefix = translations["export.saved"].split("{1}")[0].strip()
        key(b"\r", "save terminal text", expected=saved_prefix)
        exported = export_path.read_text(encoding="utf-8")
        assert "projects/" in exported and "cli_test" in exported, "export missed terminal output"
        assert "\x1b" not in exported and "CONSOLE" not in exported, "export includes control codes or workspace chrome"
        key(b"\x1b[24~", "export again", expected=translations["action.export"])
        key(b"\x15\x1b[200~" + str(export_path).encode("utf-8") + b"\x1b[201~", "existing export path", expected=export_path.name)
        key(b"\r", "existing file protected", expected=translations["export.exists"].split(".")[0])
        assert export_path.read_text(encoding="utf-8") == exported, "existing output file was overwritten"
        key(b"\x1b[24~", "retry export dialog", expected=translations["action.export"])
        key(b"\x1b", "cancel export", expected="DEMO", absent=translations["action.export"])
        assert len(list(Path(export_directory.name).iterdir())) == 1, "cancel created another export"
        size(40, 12)
        redraw("small-window hint", expected=translations["ui.tiny"].splitlines()[0])
        size(cols, rows)
        redraw("restore dimensions", expected="DEMO", absent=translations["ui.tiny"].splitlines()[0])
        key(b"\x1b[17~", "split view", expected=translations["ui.split"])  # F6
        key(b"\x1b[19~", "close confirmation", expected=translations["confirm.close"])  # F8
        key(b"\x1b", "cancel close", expected="DEMO", absent=translations["confirm.close"])
        key(b"\x11", "quit confirmation", expected=translations["confirm.quit"])  # Ctrl+Q
        os.write(master, b"\r")
        process.wait(timeout=3)
        redraw("terminal restoration", required=False)
        assert process.returncode == 0, f"client exit code {process.returncode}"
        assert b"\x1b[?1049h" in transcript, "alternate screen was not entered"
        assert b"\x1b[?1049l" in transcript, "alternate screen was not restored"
        assert termios.tcgetattr(slave) == original, "terminal attributes were not restored"
        return {"columns": cols, "rows": rows, "language": language,
                "seconds": round(time.monotonic() - started, 2), "status": "passed"}
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if output is not None:
            (output / f"{cols}x{rows}-{language}.ansi").write_bytes(transcript)
        termios.tcsetattr(slave, termios.TCSANOW, original)
        os.close(master)
        os.close(slave)
        export_directory.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path,
                        default=Path("client/tui/target/release/psxterm-tui"))
    parser.add_argument("--output", type=Path, help="save ANSI transcripts and results.json")
    parser.add_argument("--decoder", type=Path,
                        default=Path("client/tui/target/release/examples/tty_decode"))
    args = parser.parse_args()
    if os.name != "posix":
        parser.error("this smoke test requires a POSIX PTY")
    executable = args.executable.resolve(strict=True)
    decoder = args.decoder.resolve(strict=True)
    if args.output is not None:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "results.json").write_text("[]\n", encoding="utf-8")
    results = []
    for index, (cols, rows) in enumerate(SIZES):
        language = "it" if index % 2 == 0 else "en"
        results.append(run_case(executable, decoder, cols, rows, language, args.output))
        if args.output is not None:
            (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        print(f"PASS {cols}x{rows} {language}: idle, colors, rename/query editing, search, export/no-overwrite, palette, resize, split, close/cancel, quit, restore", flush=True)


if __name__ == "__main__":
    main()
