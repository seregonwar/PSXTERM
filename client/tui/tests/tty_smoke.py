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
        key(b"\x1bOQ", "flash palette", expected=translations["ui.search"])  # F2
        key(b"\x1b", "dismiss palette", expected="DEMO", absent=translations["ui.search"])
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
        print(f"PASS {cols}x{rows} {language}: palette, resize, split, close/cancel, quit, restore", flush=True)


if __name__ == "__main__":
    main()
