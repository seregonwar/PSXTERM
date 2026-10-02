#!/usr/bin/env python3
"""
PSXTerm host integration tests.

Drives a real psxtermd over TCP with an independent PTTY/1 implementation, so
the C protocol code is exercised from the outside. Covers the happy path,
malformed input, authentication, session limits and resource cleanup.

Usage: python3 tests/integration/test_e2e.py [--build DIR] [--verbose]
"""

import argparse
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time

MAGIC = 0x59545450  # wire bytes 'P','T','T','Y'
VERSION = 1
HEADER_SIZE = 16
MAX_PAYLOAD = 65536

(HELLO, HELLO_ACK, OPEN, OPEN_OK, CLOSE, STDIN, STDOUT, STDERR, RESIZE, SIGNAL,
 EXEC, EXIT, PING, PONG, DIAG_REQUEST, DIAG_DATA, DIAG_DONE) = range(1, 18)

(ACK_OK, ACK_AUTH_REQUIRED, ACK_AUTH_FAILED, ACK_SERVER_BUSY) = (0, 1, 2, 3)
(EXIT_PROCESS, EXIT_SHELL) = (0, 1)

BUILD = os.path.abspath(
    os.environ.get("PSXTERM_BUILD",
                   os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "build-host")))

VERBOSE = False
FAILURES = []
PASSES = []


def log(message):
    if VERBOSE:
        print("    " + message, flush=True)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class ProtocolError(Exception):
    pass


class Daemon:
    def __init__(self, *args, wait=True):
        self.port = free_port()
        self.log_path = os.path.join(
            os.environ.get("PSXTERM_SCRATCH", "/tmp"),
            "psxtermd-test-%d.log" % os.getpid())
        self.log_file = open(self.log_path, "wb")
        self.proc = subprocess.Popen(
            [os.path.join(BUILD, "psxtermd"), "-p", str(self.port)] + list(args),
            stdout=subprocess.DEVNULL, stderr=self.log_file)
        if wait:
            self.wait_ready()

    def wait_ready(self, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("daemon exited early; log: %s" %
                                   self.read_log())
            try:
                with socket.create_connection(("127.0.0.1", self.port), 0.2):
                    # The probe connection is accepted as a session; give the
                    # daemon a tick to reap it before tests open real sessions.
                    time.sleep(0.2)
                    return
            except OSError:
                time.sleep(0.02)
        raise RuntimeError("daemon did not start listening")

    def read_log(self):
        try:
            with open(self.log_path, "r", errors="replace") as handle:
                return handle.read()
        except OSError:
            return ""

    def fd_count(self):
        return len(os.listdir("/proc/%d/fd" % self.proc.pid))

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.log_file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


class Client:
    """Minimal independent PTTY/1 client."""

    def __init__(self, daemon, token=None, name="psxterm-test/1.0",
                 open_session=True, timeout=5.0):
        self.sock = socket.create_connection(("127.0.0.1", daemon.port), 3.0)
        self.sock.settimeout(timeout)
        self.buffer = b""
        self.session_id = 0
        self.closed = False

        self.send(HELLO, self.encode_hello(name, token))
        status, text = self.read_ack()
        self.ack_status = status
        self.ack_text = text

        if open_session and status == ACK_OK:
            self.send(OPEN, struct.pack("<HH", 24, 80) + b"xterm-256color")
            header, payload = self.read_frame()
            if header[0] != OPEN_OK:
                raise ProtocolError("expected OPEN_OK, got %d" % header[0])
            self.session_id = struct.unpack("<I", payload)[0]

    # -- framing ---------------------------------------------------------

    @staticmethod
    def encode_hello(name, token):
        name = name.encode()
        token = (token or "").encode()
        return bytes([len(name)]) + name + bytes([len(token)]) + token

    def send(self, msg_type, payload=b"", session_id=None):
        if session_id is None:
            session_id = self.session_id
        header = struct.pack("<IBBHII", MAGIC, VERSION, msg_type, 0,
                             session_id, len(payload))
        self.sock.sendall(header + payload)

    def send_raw(self, data):
        self.sock.sendall(data)

    def read_frame(self, timeout=None):
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            while len(self.buffer) < HEADER_SIZE:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise ProtocolError("connection closed")
                self.buffer += chunk

            magic, version, msg_type, flags, session_id, length = \
                struct.unpack("<IBBHII", self.buffer[:HEADER_SIZE])
            if magic != MAGIC:
                raise ProtocolError("bad magic 0x%08x" % magic)
            if version != VERSION:
                raise ProtocolError("bad version %d" % version)
            if length > MAX_PAYLOAD:
                raise ProtocolError("oversized frame %d" % length)

            while len(self.buffer) < HEADER_SIZE + length:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise ProtocolError("connection closed mid-frame")
                self.buffer += chunk

            payload = self.buffer[HEADER_SIZE:HEADER_SIZE + length]
            self.buffer = self.buffer[HEADER_SIZE + length:]
            return (msg_type, flags, session_id, length), payload
        finally:
            if timeout is not None:
                self.sock.settimeout(5.0)

    def read_ack(self):
        header, payload = self.read_frame()
        if header[0] != HELLO_ACK:
            raise ProtocolError("expected HELLO_ACK, got %d" % header[0])
        status = payload[0]
        text = payload[1:].decode(errors="replace")
        return status, text

    def expect_eof(self, timeout=3.0):
        self.sock.settimeout(timeout)
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return True
        except socket.timeout:
            return False
        except ConnectionResetError:
            return True
        finally:
            self.sock.settimeout(5.0)

    def close(self):
        try:
            self.send(CLOSE)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass

    # -- convenience -----------------------------------------------------

    def drain(self, quiet=0.3, collect=True):
        """Read frames until the socket has been quiet for `quiet` seconds."""
        out = []
        deadline = time.time() + quiet
        while time.time() < deadline:
            self.sock.settimeout(max(0.01, deadline - time.time()))
            try:
                header, payload = self.read_frame()
            except (socket.timeout, TimeoutError):
                break
            except ProtocolError:
                break
            out.append((header, payload))
            deadline = time.time() + quiet
        self.sock.settimeout(5.0)
        return out

    def text_of(self, frames, channel=None):
        chunks = []
        for (msg_type, _flags, _sid, _length), payload in frames:
            if channel is not None and msg_type != channel:
                continue
            if channel is None and msg_type not in (STDOUT, STDERR):
                continue
            chunks.append(payload.decode(errors="replace"))
        return "".join(chunks)

    def diag(self, json_mode=False, timeout=30.0):
        """Requests a diagnostic report; returns (text, overall status)."""
        self.send(DIAG_REQUEST, b"\x01" if json_mode else b"\x00")
        chunks = []
        deadline = time.time() + timeout

        while time.time() < deadline:
            header, payload = self.read_frame(
                timeout=max(0.1, deadline - time.time()))
            if header[0] == DIAG_DATA:
                chunks.append(payload.decode(errors="replace"))
            elif header[0] == DIAG_DONE:
                status = payload[0] if payload else None
                return "".join(chunks), status

        raise ProtocolError("no DIAG_DONE received")

    def execute(self, command, timeout=10.0, eof=False):
        """EXEC a line; returns (stdout+stderr, status, kinds).

        With eof=True a zero-length STDIN frame follows, which delivers EOF to
        a foreground process that reads stdin.
        """
        self.send(EXEC, command.encode())
        if eof:
            self.send(STDIN, b"")
        out = []
        stdout = []
        kinds = []
        deadline = time.time() + timeout

        while time.time() < deadline:
            try:
                header, payload = self.read_frame(
                    timeout=max(0.05, deadline - time.time()))
            except (socket.timeout, TimeoutError):
                break
            except ProtocolError:
                break

            msg_type = header[0]
            if msg_type in (STDOUT, STDERR):
                out.append(payload.decode(errors="replace"))
                if msg_type == STDOUT:
                    stdout.append(payload.decode(errors="replace"))
            elif msg_type == EXIT and len(payload) >= 5:
                status = struct.unpack("<i", payload[:4])[0]
                kind = payload[4]
                kinds.append(kind)
                if kind == EXIT_SHELL:
                    return "".join(out), status, kinds
            elif msg_type == CLOSE:
                break

        return "".join(out), None, kinds


def check(condition, message):
    if condition:
        return True
    raise AssertionError(message)


# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------

def test_handshake_and_prompt():
    with Daemon() as daemon:
        client = Client(daemon)
        check(client.ack_status == ACK_OK, "handshake was not OK")
        check("PSXTerm" in client.ack_text, "missing server identity in ACK")
        frames = client.drain()
        text = client.text_of(frames)
        check("$ " in text, "no shell prompt received: %r" % text)
        client.close()


def test_builtins_and_exit_status():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        text, status, kinds = client.execute("pwd")
        check(status == 0, "pwd exit status %r" % status)
        check(os.getcwd() in text, "unexpected pwd output %r" % text)

        text, status, _ = client.execute("help")
        check("pwd" in text and "exit" in text, "help output incomplete")

        text, status, _ = client.execute("cd /")
        text, status, _ = client.execute("pwd")
        check("/\r\n" in text or "\n/\n" in text or text.rstrip().endswith("/"),
              "cd / did not change the directory: %r" % text)

        text, status, _ = client.execute("no_such_builtin_xyz")
        check(status == 127, "unknown command status %r" % status)
        check("command not found" in text, "missing not-found message")

        text, status, _ = client.execute("exit 5")
        check(status == 5, "exit 5 status %r" % status)
        client.close()


def test_ping_pong():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()
        client.send(PING, b"hello")
        deadline = time.time() + 3
        seen = False
        while time.time() < deadline:
            header, payload = client.read_frame()
            if header[0] == PONG:
                check(payload == b"hello", "PONG payload mismatch")
                seen = True
                break
        check(seen, "no PONG received")
        client.close()


def test_resize_and_cli_test():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        client.send(RESIZE, struct.pack("<HH", 40, 100))
        time.sleep(0.1)

        text, status, _ = client.execute(os.path.join(BUILD, "cli_test"),
                                         eof=True)
        check("isatty: stdin=1 stdout=1 stderr=1" in text,
              "cli_test did not run on a real PTY: %r" % text)
        check("winsize: rows=40 cols=100" in text,
              "resize did not reach the process: %r" % text)
        check(status == 7, "cli_test exit status %r" % status)
        check("cli_test: stderr works" in text, "stderr output missing")

        client.close()


def test_cli_test_stdin_and_args():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        text, status, _ = client.execute(
            "%s alpha \"beta gamma\"" % os.path.join(BUILD, "cli_test"),
            eof=True)
        check("argv[1]=alpha" in text, "argv[1] missing: %r" % text)
        check("argv[2]=beta gamma" in text,
              "quoted argument lost: %r" % text)
        check(status == 7, "cli_test exit status %r" % status)

        client.close()


def test_stdout_stderr_channels():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        client.send(EXEC, b"cat /definitely/not/here")
        stderr_text = ""
        stdout_text = ""
        deadline = time.time() + 5
        while time.time() < deadline:
            header, payload = client.read_frame()
            if header[0] == STDERR:
                stderr_text += payload.decode(errors="replace")
            elif header[0] == STDOUT:
                stdout_text += payload.decode(errors="replace")
            elif header[0] == EXIT and payload[4] == EXIT_SHELL:
                break

        check("No such file" in stderr_text or "not" in stderr_text,
              "error message not on STDERR: %r" % stderr_text)
        check("No such file" not in stdout_text,
              "error message leaked to STDOUT")
        client.close()


def test_malformed_magic_closes_connection():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()
        bad = struct.pack("<IBBHII", 0xDEADBEEF, VERSION, PING, 0, 0, 0)
        client.send_raw(bad)
        check(client.expect_eof(), "server kept a bad-magic connection open")
        client.close()

        # daemon still healthy
        other = Client(daemon)
        check(other.ack_status == ACK_OK, "daemon unhealthy after bad magic")
        other.close()


def test_oversized_frame_rejected():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()
        # payload_length = MAX_PAYLOAD + 1
        bad = struct.pack("<IBBHII", MAGIC, VERSION, STDOUT, 0, 0,
                          MAX_PAYLOAD + 1)
        client.send_raw(bad)
        check(client.expect_eof(), "server accepted an oversized frame")
        client.close()


def test_bad_version_and_session_id():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()
        bad = struct.pack("<IBBHII", MAGIC, 9, PING, 0, 0, 0)
        client.send_raw(bad)
        check(client.expect_eof(), "server accepted a bad version")
        client.close()

        client = Client(daemon)
        client.drain()
        client.send(PING, b"", session_id=0x12345678)
        check(client.expect_eof(), "server accepted a mismatched session id")
        client.close()


def test_truncated_frame_then_disconnect():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()
        # half a header, then disconnect
        client.send_raw(struct.pack("<I", MAGIC))
        client.sock.close()

        other = Client(daemon)
        check(other.ack_status == ACK_OK, "daemon unhealthy after truncation")
        other.close()


def test_partial_frame_writes():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        payload = b"partial-frame"
        frame = struct.pack("<IBBHII", MAGIC, VERSION, PING, 0, 0,
                            len(payload)) + payload
        for i in range(0, len(frame), 3):
            client.send_raw(frame[i:i + 3])
            time.sleep(0.01)

        header, response = client.read_frame()
        while header[0] != PONG:
            header, response = client.read_frame()
        check(response == payload, "PONG mismatch after partial writes")
        client.close()


def test_authentication():
    with Daemon("--token", "s3cret") as daemon:
        wrong = Client(daemon, token="nope")
        check(wrong.ack_status == ACK_AUTH_FAILED,
              "wrong token accepted: %r" % wrong.ack_status)
        wrong.close()

        missing = Client(daemon, token=None)
        check(missing.ack_status == ACK_AUTH_FAILED,
              "missing token accepted: %r" % missing.ack_status)
        missing.close()

        good = Client(daemon, token="s3cret")
        check(good.ack_status == ACK_OK, "valid token rejected")
        good.drain()
        text, status, _ = good.execute("whoami")
        check(status == 0, "authenticated session could not run a command")
        good.close()


def test_session_limit():
    with Daemon("--max-sessions", "1") as daemon:
        first = Client(daemon)
        check(first.ack_status == ACK_OK, "first session refused")
        first.drain()

        second = Client(daemon, open_session=False)
        check(second.ack_status == ACK_SERVER_BUSY,
              "second session not rejected: %r" % second.ack_status)
        second.close()

        first.close()


def test_multiple_sessions_are_isolated():
    with Daemon("--max-sessions", "4") as daemon:
        a = Client(daemon)
        b = Client(daemon)
        check(a.session_id != b.session_id, "session ids collide")
        a.drain()
        b.drain()

        text_a, _, _ = a.execute("export WHO=a")
        text_a, _, _ = a.execute("export MARK=alpha")

        # b must not have received anything from a's commands
        b_frames = b.drain(quiet=0.2)
        check(b.text_of(b_frames) == "", "session B received session A output")

        text_b, _, _ = b.execute("env")
        check("MARK=alpha" not in text_b, "environment leaked between sessions")
        check("MARK" not in text_b or "MARK=alpha" not in text_b,
              "environment leaked between sessions")

        text_a, _, _ = a.execute("env")
        check("MARK=alpha" in text_a, "environment missing in session A")

        text_ps, _, _ = a.execute("ps")
        check("running" in text_ps, "ps output missing session states")

        a.close()
        b.close()


def test_interrupt_via_signal_frame():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        client.send(EXEC, b"sleep 30")
        time.sleep(0.4)
        client.send(SIGNAL, bytes([signal.SIGINT]))

        deadline = time.time() + 5
        status = None
        while time.time() < deadline:
            header, payload = client.read_frame()
            if header[0] == EXIT and len(payload) >= 5 and payload[4] == EXIT_SHELL:
                status = struct.unpack("<i", payload[:4])[0]
                break

        check(status == 130, "SIGINT exit status %r (want 130)" % status)
        client.close()


def test_pipe_tty_fallback():
    with Daemon("--tty", "pipe") as daemon:
        client = Client(daemon)
        client.drain()

        # Builtins work without a kernel PTY.
        text, status, _ = client.execute("pwd")
        check(status == 0, "pwd failed on the PipeTTY fallback")
        check(os.getcwd() in text, "unexpected pwd output on PipeTTY")

        text, status, _ = client.execute("ps")
        check("pipe" in text, "ps does not report the pipe backend: %r" % text)

        # External execution works; isatty() is false by definition.
        text, status, _ = client.execute(os.path.join(BUILD, "cli_test"),
                                         eof=True)
        check("isatty: stdin=0 stdout=0 stderr=0" in text,
              "PipeTTY isatty expectation violated: %r" % text)
        check(status == 7, "cli_test exit status %r on PipeTTY" % status)
        check("read=<eof>" in text, "stdin EOF was not delivered on PipeTTY")

        # Ctrl+C is translated into SIGINT by the server on PipeTTY.
        client.send(EXEC, b"sleep 30")
        time.sleep(0.4)
        client.send(STDIN, b"\x03")

        deadline = time.time() + 5
        status = None
        while time.time() < deadline:
            header, payload = client.read_frame()
            if header[0] == EXIT and len(payload) >= 5 and payload[4] == EXIT_SHELL:
                status = struct.unpack("<i", payload[:4])[0]
                break
        check(status == 130, "PipeTTY Ctrl+C exit status %r" % status)

        client.close()


def test_pipe_tty_forced_pty():
    """--tty pty must not silently degrade on a host with PTY support."""
    with Daemon("--tty", "pty") as daemon:
        client = Client(daemon)
        client.drain()
        text, status, _ = client.execute(os.path.join(BUILD, "cli_test"),
                                         eof=True)
        check("isatty: stdin=1 stdout=1 stderr=1" in text,
              "forced PTY did not provide a terminal: %r" % text)
        client.close()


def test_doctor_human():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        text, status = client.diag()
        check(status == 0, "doctor overall status %r (want 0/READY)" % status)
        for marker in ("PSXTerm diagnostics", "Platform:", "Protocol:", "TTY",
                       "Process execution", "cli_test located", "spawn",
                       "stdio separation", "SIGINT", "Result:", "READY"):
            check(marker in text, "doctor output missing %r:\n%s" % (marker, text))

        client.close()


def test_doctor_json():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        text, status = client.diag(json_mode=True)
        check(status == 0, "doctor json status %r" % status)

        data = json.loads(text)
        check(data["result"] == "READY", "json result %r" % data["result"])
        check(data["protocol"] == "PTTY/1", "json protocol %r" % data["protocol"])
        check(len(data["groups"]) >= 5, "json groups missing")

        checks = [c for g in data["groups"] for c in g["checks"]]
        check(len(checks) >= 30, "json checks missing: %d" % len(checks))
        check(any(c["name"] == "session" and c["status"] == "PASS"
                  for c in checks), "session checks missing in json")
        check(any(c["name"] == "socket" and c["status"] == "PASS"
                  for c in checks), "socket check missing in json")
        check(any(c["name"] == "frames" for c in checks), "frame counters missing")

        client.close()


def test_doctor_reports_busy_session():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        client.send(EXEC, b"sleep 30")
        time.sleep(0.4)

        text, status = client.diag()
        check(status == 2, "busy doctor status %r (want 2)" % status)
        check("foreground process is running" in text,
              "busy notice missing: %r" % text)

        client.send(SIGNAL, bytes([signal.SIGINT]))
        deadline = time.time() + 5
        while time.time() < deadline:
            header, payload = client.read_frame()
            if header[0] == EXIT and len(payload) >= 5 and payload[4] == EXIT_SHELL:
                break

        client.close()


def test_doctor_client_binary():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()
        client.close()

        human = subprocess.run(
            [os.path.join(BUILD, "psxterm"), "doctor", "127.0.0.1",
             "-p", str(daemon.port)],
            capture_output=True, text=True, timeout=60)
        check(human.returncode == 0,
              "doctor client rc=%d stderr=%s" % (human.returncode, human.stderr))
        check("PSXTerm diagnostics" in human.stdout, "doctor client output empty")
        check("Result:" in human.stdout and "READY" in human.stdout,
              "doctor client result missing")

        machine = subprocess.run(
            [os.path.join(BUILD, "psxterm"), "doctor", "--json",
             "127.0.0.1", "-p", str(daemon.port)],
            capture_output=True, text=True, timeout=60)
        check(machine.returncode == 0,
              "doctor --json rc=%d stderr=%s" % (machine.returncode,
                                                 machine.stderr))
        data = json.loads(machine.stdout)
        check(data["result"] == "READY", "doctor --json result %r" % data["result"])
        check(machine.stdout.lstrip().startswith("{"),
              "doctor --json output is not pure JSON")


def test_handshake_timeout():
    with Daemon("--handshake-timeout", "300") as daemon:
        sock = socket.create_connection(("127.0.0.1", daemon.port), 3.0)
        sock.settimeout(3.0)
        start = time.time()
        try:
            data = sock.recv(64)
            saw_eof = data == b""
        except socket.timeout:
            saw_eof = False
        elapsed = time.time() - start
        sock.close()
        check(saw_eof, "handshake timeout did not close the connection")
        check(elapsed < 2.5, "handshake timeout too slow: %.2fs" % elapsed)


def test_idle_timeout():
    with Daemon("--idle-timeout", "400") as daemon:
        client = Client(daemon)
        client.drain()
        time.sleep(0.2)
        check(client.expect_eof(timeout=3.0),
              "idle session was not dropped after the idle timeout")
        client.close()


def test_disconnect_stress_and_fd_leak():
    with Daemon() as daemon:
        # warm up
        for _ in range(3):
            Client(daemon).close()
        time.sleep(0.3)
        baseline = daemon.fd_count()

        for _ in range(30):
            client = Client(daemon)
            client.execute("pwd")
            client.close()

        time.sleep(0.5)
        after = daemon.fd_count()
        check(after <= baseline + 3,
              "fd leak: %d -> %d descriptors" % (baseline, after))

        # the daemon is still able to serve
        client = Client(daemon)
        check(client.ack_status == ACK_OK, "daemon unhealthy after stress")
        client.close()


TESTS = [
    test_handshake_and_prompt,
    test_builtins_and_exit_status,
    test_ping_pong,
    test_resize_and_cli_test,
    test_cli_test_stdin_and_args,
    test_stdout_stderr_channels,
    test_malformed_magic_closes_connection,
    test_oversized_frame_rejected,
    test_bad_version_and_session_id,
    test_truncated_frame_then_disconnect,
    test_partial_frame_writes,
    test_authentication,
    test_session_limit,
    test_multiple_sessions_are_isolated,
    test_interrupt_via_signal_frame,
    test_pipe_tty_fallback,
    test_pipe_tty_forced_pty,
    test_doctor_human,
    test_doctor_json,
    test_doctor_reports_busy_session,
    test_doctor_client_binary,
    test_handshake_timeout,
    test_idle_timeout,
    test_disconnect_stress_and_fd_leak,
]


def main():
    global VERBOSE, BUILD

    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=BUILD)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    BUILD = os.path.abspath(args.build)
    VERBOSE = args.verbose

    for path in (os.path.join(BUILD, "psxtermd"),
                 os.path.join(BUILD, "cli_test")):
        if not os.path.exists(path):
            print("missing binary: %s (build first)" % path)
            return 2

    failures = 0
    for test in TESTS:
        name = test.__name__
        start = time.time()
        try:
            test()
            print("PASS %-45s (%.2fs)" % (name, time.time() - start))
        except Exception as exc:  # noqa: BLE001 - test harness
            failures += 1
            print("FAIL %-45s %s" % (name, exc))
            if VERBOSE:
                import traceback
                traceback.print_exc()

    print()
    print("%d/%d integration tests passed" % (len(TESTS) - failures,
                                              len(TESTS)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
