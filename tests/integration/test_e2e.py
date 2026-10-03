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
import tempfile
import time

MAGIC = 0x59545450  # wire bytes 'P','T','T','Y'
VERSION = 1
HEADER_SIZE = 16
MAX_PAYLOAD = 65536

(HELLO, HELLO_ACK, OPEN, OPEN_OK, CLOSE, STDIN, STDOUT, STDERR, RESIZE, SIGNAL,
 EXEC, EXIT, PING, PONG, DIAG_REQUEST, DIAG_DATA, DIAG_DONE, CAPS, DETACH,
 ATTACH, ATTACH_OK, ATTACH_FAIL, SESSION_INFO, SESSIONS_REQUEST, SESSIONS_DATA,
 SESSIONS_DONE, FILE_OPEN, FILE_OPEN_OK, FILE_DATA, FILE_SEEK, FILE_CLOSE,
 FILE_RESULT, FILE_STAT) = range(1, 34)

(FILE_MODE_READ, FILE_MODE_WRITE) = (0, 1)
FILE_SIZE_UNKNOWN = 0xffffffffffffffff
FILE_TYPE_REGULAR = 0


def file_open_payload(mode, size, path):
    return struct.pack("<BQ", mode, size) + path.encode()


def file_data_payload(offset, data):
    return struct.pack("<Q", offset) + data

(CAP_REAL_PTY, CAP_PIPE_TTY, CAP_EXEC, CAP_FILE_TRANSFER, CAP_SESSION_RESUME,
 CAP_JOB_CONTROL, CAP_AUTH_CHALLENGE, CAP_COMPRESSION,
 CAP_JSON_DIAGNOSTICS) = (1, 2, 4, 8, 16, 32, 64, 128, 256)

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
    def __init__(self, *args, wait=True, env=None):
        self.port = free_port()
        self.log_path = os.path.join(
            os.environ.get("PSXTERM_SCRATCH", "/tmp"),
            "psxtermd-test-%d.log" % os.getpid())
        self.log_file = open(self.log_path, "wb")
        process_env = dict(os.environ)
        process_env.update(env or {})
        process_env["PSXTERM_PID_FILE"] = os.path.join(
            os.environ.get("PSXTERM_SCRATCH", "/tmp"),
            "psxtermd-test-%d.pid" % self.port)
        self.proc = subprocess.Popen(
            [os.path.join(BUILD, "psxtermd"), "-p", str(self.port)] + list(args),
            stdout=subprocess.DEVNULL, stderr=self.log_file, env=process_env)
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
        self.caps = 0
        self.resume_token = None
        self.info_session_id = None
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

    def detach(self):
        """Detaches without closing the session, then drops the connection."""
        self.send(DETACH)
        self.sock.close()

    def attach(self, session_id, token):
        """Attaches to a detached session; returns (ok, reason, truncated)."""
        payload = struct.pack("<I", session_id) + bytes(token)
        self.send(ATTACH, payload)

        deadline = time.time() + 5
        while time.time() < deadline:
            header, data = self.read_frame(
                timeout=max(0.1, deadline - time.time()))

            if header[0] == ATTACH_OK:
                self.session_id = session_id
                return True, None, bool(data[4]) if len(data) >= 5 else False
            if header[0] == ATTACH_FAIL:
                return False, (data[0] if data else None), False
            if header[0] == CLOSE:
                return False, None, False

        return False, None, False

    def sessions_listing(self, timeout=5.0):
        """Returns the daemon's session listing as text."""
        self.send(SESSIONS_REQUEST)
        chunks = []
        deadline = time.time() + timeout

        while time.time() < deadline:
            header, payload = self.read_frame(
                timeout=max(0.1, deadline - time.time()))
            if header[0] == SESSIONS_DATA:
                chunks.append(payload.decode(errors="replace"))
            elif header[0] == SESSIONS_DONE:
                return "".join(chunks)

        raise ProtocolError("no SESSIONS_DONE received")

    def read_caps(self, timeout=3.0):
        """Reads frames until the capability advertisement arrives."""
        deadline = time.time() + timeout

        while time.time() < deadline:
            header, payload = self.read_frame(
                timeout=max(0.1, deadline - time.time()))
            if header[0] == CAPS:
                return struct.unpack("<I", payload[:4])[0]

        raise ProtocolError("no CAPS frame received")

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

            # Capture session metadata wherever a frame is read.
            if msg_type == CAPS and len(payload) >= 4:
                self.caps = struct.unpack("<I", payload[:4])[0]
            elif msg_type == SESSION_INFO and len(payload) >= 20:
                self.info_session_id = struct.unpack("<I", payload[:4])[0]
                self.resume_token = payload[4:20]

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

    def drain(self, quiet=0.3, max_total=5.0):
        """Read frames until the socket has been quiet for `quiet` seconds.

        `max_total` bounds the read even while data keeps arriving, so a
        flooding remote process cannot hang a test.
        """
        out = []
        start = time.time()
        deadline = start + quiet

        while time.time() < deadline and time.time() - start < max_total:
            self.sock.settimeout(max(0.01, deadline - time.time()))
            try:
                header, payload = self.read_frame()
            except (socket.timeout, TimeoutError):
                break
            except ProtocolError:
                break
            out.append((header, payload))
            deadline = min(time.time() + quiet, start + max_total)

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


def daemon_rss_kb(daemon):
    """Resident set size of the daemon in KiB (Linux)."""
    try:
        with open("/proc/%d/status" % daemon.proc.pid) as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def test_capabilities_advertised():
    with Daemon() as daemon:
        client = Client(daemon)
        caps = client.read_caps()

        check(caps & CAP_EXEC, "EXEC capability not advertised: 0x%08x" % caps)
        check(caps & CAP_JSON_DIAGNOSTICS, "JSON diagnostics not advertised")
        check(caps & (CAP_REAL_PTY | CAP_PIPE_TTY),
              "no tty backend advertised: 0x%08x" % caps)
        check(caps & CAP_REAL_PTY and caps & CAP_JOB_CONTROL,
              "real pty without job control: 0x%08x" % caps)
        check(not (caps & CAP_COMPRESSION),
              "COMPRESSION must not be advertised while unimplemented")
        check(caps & CAP_FILE_TRANSFER, "FILE_TRANSFER not advertised")
        check(caps & CAP_SESSION_RESUME, "SESSION_RESUME not advertised")

        text, status = client.diag()
        check("capabilities" in text and "job-control" in text,
              "doctor does not report capabilities")
        client.close()


def test_capabilities_pipe_backend():
    with Daemon("--tty", "pipe") as daemon:
        client = Client(daemon)
        caps = client.read_caps()

        check(caps & CAP_PIPE_TTY, "PIPE_TTY not advertised on the fallback")
        check(not (caps & CAP_REAL_PTY), "REAL_PTY advertised on PipeTTY")
        check(not (caps & CAP_JOB_CONTROL),
              "JOB_CONTROL must not be advertised on PipeTTY")
        client.close()


def test_input_backpressure_is_bounded():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        # A foreground process that never reads stdin.
        client.send(EXEC, b"sleep 30")
        time.sleep(0.4)

        baseline = daemon_rss_kb(daemon)
        payload = b"x" * MAX_PAYLOAD
        frame = struct.pack("<IBBHII", MAGIC, VERSION, STDIN, 0,
                            client.session_id, MAX_PAYLOAD) + payload

        client.sock.setblocking(False)
        sent = 0
        deadline = time.time() + 5
        while time.time() < deadline and sent < 200:
            try:
                client.sock.send(frame)
                sent += 1
            except BlockingIOError:
                time.sleep(0.05)
            except OSError:
                break
        peak = daemon_rss_kb(daemon)
        client.sock.setblocking(True)

        check(sent > 0, "could not send any input")
        check(peak - baseline < 8 * 1024,
              "daemon RSS grew by %d KiB during an input flood (%d frames)"
              % (peak - baseline, sent))

        client.sock.close()

        other = Client(daemon)
        check(other.ack_status == ACK_OK, "daemon unhealthy after input flood")
        other.close()


def test_output_backpressure_is_bounded():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        baseline = daemon_rss_kb(daemon)
        client.send(EXEC, b"/usr/bin/yes")
        time.sleep(2.0)  # never read the socket while yes floods stdout
        peak = daemon_rss_kb(daemon)

        check(peak - baseline < 24 * 1024,
              "daemon RSS grew by %d KiB during an output flood"
              % (peak - baseline))

        client.sock.close()

        other = Client(daemon)
        check(other.ack_status == ACK_OK, "daemon unhealthy after output flood")
        other.close()


def test_detach_attach_roundtrip():
    with Daemon() as daemon:
        a = Client(daemon)
        a.drain()

        check(a.resume_token is not None, "no resume token delivered")
        session_id = a.session_id
        token = a.resume_token

        text, status, _ = a.execute("pwd")
        check(status == 0, "pwd failed before detach")
        a.execute("cd /")

        a.detach()
        time.sleep(0.3)

        # Attach connections must not OPEN a session first.
        b = Client(daemon, open_session=False)
        check(b.session_id != session_id,
              "attach must not reuse the requester's session id")
        ok, reason, _ = b.attach(session_id, token)
        check(ok, "attach failed (reason %r)" % reason)

        b.drain(quiet=0.4)
        text, status, _ = b.execute("pwd")
        check(status == 0, "reattached session cannot run commands")
        check("\n/\n" in text or text.strip().endswith("/"),
              "session cwd not preserved across detach: %r" % text)

        check(b.caps & CAP_SESSION_RESUME, "SESSION_RESUME not advertised")
        b.close()


def test_attach_rejects_bad_token_and_unknown_session():
    with Daemon() as daemon:
        a = Client(daemon)
        a.drain()
        a.detach()
        time.sleep(0.3)

        bad = Client(daemon, open_session=False)
        ok, reason, _ = bad.attach(a.session_id, b"\x00" * 16)
        check(not ok, "attach with a bad token was accepted")
        check(reason == 1, "expected BAD_TOKEN, got %r" % reason)

        unknown = Client(daemon, open_session=False)
        ok, reason, _ = unknown.attach(999999, a.resume_token)
        check(not ok, "attach to an unknown session was accepted")
        check(reason == 0, "expected UNKNOWN_SESSION, got %r" % reason)

        good = Client(daemon, open_session=False)
        ok, reason, _ = good.attach(a.session_id, a.resume_token)
        check(ok, "valid attach rejected: %r" % reason)
        good.close()


def test_scrollback_delivered_after_detach():
    with Daemon() as daemon:
        a = Client(daemon)
        a.drain()

        # Output produced while nobody is attached.
        a.send(EXEC, b'/bin/sh -c "sleep 1; echo BUFFERED-OUTPUT"')
        time.sleep(0.2)
        a.detach()
        time.sleep(1.6)

        b = Client(daemon, open_session=False)
        ok, reason, _ = b.attach(a.session_id, a.resume_token)
        check(ok, "attach failed: %r" % reason)

        frames = b.drain(quiet=0.6)
        text = b.text_of(frames)
        check("BUFFERED-OUTPUT" in text, "scrollback lost: %r" % text)
        b.close()


def test_scrollback_truncation_notice():
    with Daemon() as daemon:
        a = Client(daemon)
        a.drain()

        a.send(EXEC, b"/usr/bin/yes")
        time.sleep(0.3)
        a.detach()
        time.sleep(1.6)  # yes floods the bounded scrollback

        b = Client(daemon, open_session=False)
        ok, reason, truncated = b.attach(a.session_id, a.resume_token)
        check(ok, "attach failed: %r" % reason)
        check(truncated, "daemon did not report scrollback truncation")

        frames = b.drain(quiet=0.6)
        text = b.text_of(frames)
        check("output truncated while detached" in text,
              "missing truncation notice: %r" % text[:200])
        b.close()


def test_sessions_listing_reports_detached():
    with Daemon() as daemon:
        a = Client(daemon)
        a.drain()
        a.detach()
        time.sleep(0.3)

        b = Client(daemon)
        b.drain()
        listing = b.sessions_listing()

        check("ID" in listing and "STATE" in listing,
              "listing header missing: %r" % listing)
        check("detached" in listing, "no detached session listed: %r" % listing)
        check(str(a.session_id) in listing, "detached id missing from listing")
        check("running" in listing, "requester session not listed")
        check("psh" in listing, "command column missing")
        b.close()


def test_no_persist_destroys_session_on_disconnect():
    with Daemon("--no-persist") as daemon:
        a = Client(daemon)
        a.drain()
        session_id = a.session_id
        a.detach()
        time.sleep(0.4)

        b = Client(daemon)
        b.drain()
        listing = b.sessions_listing()
        check(str(session_id) not in listing,
              "session survived a disconnect with --no-persist: %r" % listing)
        b.close()


def test_client_binary_sessions_and_attach_errors():
    with Daemon() as daemon:
        psxterm = os.path.join(BUILD, "psxterm")

        listing = subprocess.run(
            [psxterm, "sessions", "127.0.0.1", "-p", str(daemon.port)],
            capture_output=True, text=True, timeout=30)
        check(listing.returncode == 0,
              "sessions rc=%d stderr=%s" % (listing.returncode, listing.stderr))
        check("STATE" in listing.stdout and "COMMAND" in listing.stdout,
              "sessions output unexpected: %r" % listing.stdout)

        bad = subprocess.run(
            [psxterm, "attach", "127.0.0.1", "999999", "--resume", "00" * 16,
             "-p", str(daemon.port)],
            capture_output=True, text=True, timeout=30)
        check(bad.returncode == 2, "attach rc=%d" % bad.returncode)
        check("unknown session" in bad.stderr,
              "attach error message: %r" % bad.stderr)

        missing = subprocess.run(
            [psxterm, "attach", "127.0.0.1", "1", "-p", str(daemon.port)],
            capture_output=True, text=True, timeout=30)
        check(missing.returncode == 64,
              "missing --resume rc=%d" % missing.returncode)


def test_file_push_pull_roundtrip():
    remote = "/tmp/psxterm-push-pull.bin"

    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        data = bytes(range(256)) * 800  # 204800 bytes: several frames
        if os.path.exists(remote):
            os.unlink(remote)

        client.send(FILE_OPEN, file_open_payload(FILE_MODE_WRITE, len(data),
                                                 remote))
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_OPEN_OK and payload[0] == 0,
              "push open failed: %r" % (payload[:16],))

        for offset in range(0, len(data), 32768):
            client.send(FILE_DATA,
                        file_data_payload(offset, data[offset:offset + 32768]))
            time.sleep(0.005)

        client.send(FILE_CLOSE)
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_RESULT and payload[0] == 0,
              "push failed: %r" % (payload[:32],))
        check(os.path.exists(remote), "pushed file missing on disk")
        with open(remote, "rb") as handle:
            check(handle.read() == data, "pushed content differs")

        # Pull it back through the protocol.
        client.send(FILE_OPEN, file_open_payload(FILE_MODE_READ,
                                                 FILE_SIZE_UNKNOWN, remote))
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_OPEN_OK, "pull open failed")
        check(struct.unpack("<Q", payload[1:9])[0] == len(data),
              "reported size differs")

        got = bytearray()
        while True:
            client.send(FILE_DATA, struct.pack("<Q", len(got)))
            header, payload = client.read_frame(timeout=10)

            if header[0] == FILE_DATA:
                check(struct.unpack("<Q", payload[:8])[0] == len(got),
                      "download offset mismatch")
                got += payload[8:]
                continue

            check(header[0] == FILE_RESULT and payload[0] == 0,
                  "pull failed: %r" % (payload[:32],))
            break

        check(bytes(got) == data, "pulled content differs (%d bytes)" % len(got))
        os.unlink(remote)
        client.close()


def test_file_upload_is_atomic_on_failure():
    target = "/tmp/psxterm-atomic.bin"

    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        if os.path.exists(target):
            os.unlink(target)

        client.send(FILE_OPEN, file_open_payload(FILE_MODE_WRITE, 1000, target))
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_OPEN_OK, "open failed")

        client.send(FILE_DATA, file_data_payload(0, b"x" * 100))
        client.send(FILE_CLOSE)
        header, payload = client.read_frame(timeout=10)

        check(header[0] == FILE_RESULT and payload[0] != 0,
              "size mismatch was accepted")
        check(not os.path.exists(target),
              "a partial upload was published as the final file")

        leftovers = [name for name in os.listdir("/tmp")
                     if name.startswith("psxterm-atomic.bin.psxterm-upload")]
        check(not leftovers, "temporary upload file left behind: %r" % leftovers)
        client.close()


def test_file_stat_and_missing_file():
    known = "/tmp/psxterm-stat.bin"

    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        with open(known, "wb") as handle:
            handle.write(b"st" * 40)

        client.send(FILE_STAT, known.encode())
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_RESULT and payload[0] == 0, "stat failed")
        check(struct.unpack("<Q", payload[1:9])[0] == 80, "stat size wrong")
        check(payload[9] == FILE_TYPE_REGULAR, "stat type wrong")

        client.send(FILE_STAT, b"/tmp/psxterm-does-not-exist.bin")
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_RESULT and payload[0] != 0,
              "stat of a missing file succeeded")

        client.send(FILE_OPEN, file_open_payload(FILE_MODE_READ,
                                                 FILE_SIZE_UNKNOWN,
                                                 "/tmp/psxterm-does-not-exist.bin"))
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_RESULT and payload[0] != 0,
              "open of a missing file succeeded")

        os.unlink(known)
        client.close()


def test_file_path_safety():
    with Daemon() as daemon:
        client = Client(daemon)
        client.drain()

        # Empty path.
        client.send(FILE_STAT, b"")
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_RESULT and payload[0] != 0,
              "empty path accepted")

        # Embedded NUL byte.
        client.send(FILE_STAT, b"/tmp/bad\x00name")
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_RESULT and payload[0] != 0,
              "path with an embedded NUL accepted")

        # Path longer than the resolver limit.
        client.send(FILE_STAT, b"/tmp/" + b"a" * 900)
        header, payload = client.read_frame(timeout=10)
        check(header[0] == FILE_RESULT and payload[0] != 0,
              "oversized path accepted")

        client.close()


def test_client_binary_push_pull():
    src = "/tmp/psxterm-client-src.bin"
    dst = "/tmp/psxterm-client-dst.bin"
    remote = "/tmp/psxterm-client-remote.bin"
    payload = bytes((i * 7 + 3) & 0xff for i in range(100000))

    with Daemon() as daemon:
        psxterm = os.path.join(BUILD, "psxterm")

        with open(src, "wb") as handle:
            handle.write(payload)

        for path in (dst, remote):
            if os.path.exists(path):
                os.unlink(path)

        pushed = subprocess.run(
            [psxterm, "push", "127.0.0.1", src, remote, "-p", str(daemon.port)],
            capture_output=True, text=True, timeout=60)
        check(pushed.returncode == 0,
              "push rc=%d stderr=%s" % (pushed.returncode, pushed.stderr))
        check("pushed" in pushed.stdout, "push output: %r" % pushed.stdout)

        pulled = subprocess.run(
            [psxterm, "pull", "127.0.0.1", remote, dst, "-p", str(daemon.port)],
            capture_output=True, text=True, timeout=60)
        check(pulled.returncode == 0,
              "pull rc=%d stderr=%s" % (pulled.returncode, pulled.stderr))

        with open(dst, "rb") as handle:
            check(handle.read() == payload, "client roundtrip content differs")

        failed = subprocess.run(
            [psxterm, "push", "127.0.0.1", src, "/nonexistent-dir/x.bin",
             "-p", str(daemon.port)],
            capture_output=True, text=True, timeout=60)
        check(failed.returncode == 1, "push to a bad path rc=%d"
              % failed.returncode)
        check(failed.stderr.strip() != "", "no error message for a failed push")

        for path in (src, dst, remote):
            if os.path.exists(path):
                os.unlink(path)


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


def test_runtime_manifest_survives_restart():
    # Quotes, backslashes and a control character require real JSON escaping.
    with tempfile.TemporaryDirectory(prefix='psxterm-"\\\n-') as base:
        env = {"PSXTERM_RUNTIME_BASE": base}
        root = os.path.join(base, "runtime")
        manifest = os.path.join(root, "runtime.json")
        with Daemon(env=env):
            with open(manifest, "r") as handle:
                data = json.load(handle)
            check(data["root"] == root, "manifest root did not roundtrip")
            check(data["packages"] == {}, "default manifest contains packages")

            # Simulate a bundle installer, including an unknown metadata field.
            data["packages"] = {"curl": "test-version"}
            data["bundle_revision"] = "preserve-me"
            installed = json.dumps(data, indent=2).encode()
            with open(manifest, "wb") as handle:
                handle.write(installed)

        with Daemon(env=env):
            with open(manifest, "rb") as handle:
                check(handle.read() == installed,
                      "daemon restart changed installed runtime metadata")


def test_runtime_manifest_failure_is_reported():
    with tempfile.TemporaryDirectory(prefix="psxterm-test-") as base:
        manifest = os.path.join(base, "runtime", "runtime.json")
        os.makedirs(manifest)
        with Daemon(env={"PSXTERM_RUNTIME_BASE": base}) as daemon:
            check(os.path.isdir(manifest), "startup replaced manifest directory")
            check("runtime manifest unavailable" in daemon.read_log(),
                  "manifest write failure was not reported")
            client = Client(daemon)
            try:
                output, code, _ = client.execute("pwd")
                check(code == 0 and output,
                      "manifest failure prevented shell operation")
            finally:
                client.close()


TESTS = [
    test_runtime_manifest_survives_restart,
    test_runtime_manifest_failure_is_reported,
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
    test_capabilities_advertised,
    test_capabilities_pipe_backend,
    test_detach_attach_roundtrip,
    test_attach_rejects_bad_token_and_unknown_session,
    test_scrollback_delivered_after_detach,
    test_scrollback_truncation_notice,
    test_sessions_listing_reports_detached,
    test_no_persist_destroys_session_on_disconnect,
    test_client_binary_sessions_and_attach_errors,
    test_file_push_pull_roundtrip,
    test_file_upload_is_atomic_on_failure,
    test_file_stat_and_missing_file,
    test_file_path_safety,
    test_client_binary_push_pull,
    test_input_backpressure_is_bounded,
    test_output_backpressure_is_bounded,
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
