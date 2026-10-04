"""Regression checks for the real-terminal test's asynchronous screen waits."""

import unittest
from unittest.mock import Mock, patch

import tty_smoke


class RedrawWaitTests(unittest.TestCase):
    def test_export_completion_can_follow_a_quiet_interval(self):
        transcript = bytearray(b"startup\n")
        process = Mock()
        process.poll.return_value = None
        with patch.object(tty_smoke.time, "monotonic", side_effect=[0, .1, .3, .5]), \
             patch.object(tty_smoke.select, "select", side_effect=[([7], [], []), ([], [], []), ([7], [], [])]), \
             patch.object(tty_smoke.os, "read", side_effect=[b"Saving terminal text\n", b"File already exists\n"]):
            tty_smoke.wait_for_redraw(7, process, transcript, bytearray.decode,
                                     "protected file", expected="File already exists")
        self.assertIn(b"Saving terminal text\nFile already exists\n", transcript)

    def test_missing_completion_still_fails_at_the_deadline(self):
        transcript = bytearray()
        process = Mock()
        process.poll.return_value = None
        with patch.object(tty_smoke.time, "monotonic", side_effect=[0, .1, .3, 2.1]), \
             patch.object(tty_smoke.select, "select", side_effect=[([7], [], []), ([], [], [])]), \
             patch.object(tty_smoke.os, "read", return_value=b"Saving terminal text\n"):
            with self.assertRaisesRegex(AssertionError, "incorrect screen.*Saving terminal text"):
                tty_smoke.wait_for_redraw(7, process, transcript, bytearray.decode,
                                         "export", expected="File already exists")
        self.assertEqual(transcript, b"Saving terminal text\n")

    def test_dialog_disappearance_can_follow_a_quiet_interval(self):
        transcript = bytearray()
        process = Mock()
        process.poll.return_value = None
        # The decoder represents two full screen states, not concatenated text.
        decode = Mock(side_effect=["DEMO · dialog", "DEMO"])
        with patch.object(tty_smoke.time, "monotonic", side_effect=[0, .1, .3, .5]), \
             patch.object(tty_smoke.select, "select", side_effect=[([7], [], []), ([], [], []), ([7], [], [])]), \
             patch.object(tty_smoke.os, "read", side_effect=[b"first", b"second"]):
            tty_smoke.wait_for_redraw(7, process, transcript, decode,
                                     "dismiss", expected="DEMO", absent="dialog")
        self.assertEqual(transcript, b"firstsecond")

    def test_idle_drain_does_not_wait_for_the_deadline(self):
        transcript = bytearray()
        with patch.object(tty_smoke.time, "monotonic", side_effect=[0, .1]), \
             patch.object(tty_smoke.select, "select", return_value=([], [], [])):
            tty_smoke.wait_for_redraw(7, Mock(), transcript, bytearray.decode,
                                     "settle", required=False)
        self.assertEqual(transcript, b"")

    def test_client_exit_reports_the_stage_and_retains_partial_output(self):
        transcript = bytearray()
        process = Mock(returncode=1)
        process.poll.return_value = 1
        with patch.object(tty_smoke.time, "monotonic", side_effect=[0, .1, .3]), \
             patch.object(tty_smoke.select, "select", side_effect=[([7], [], []), ([], [], [])]), \
             patch.object(tty_smoke.os, "read", return_value=b"Saving terminal text\n"):
            with self.assertRaisesRegex(AssertionError, "client exited during export: 1"):
                tty_smoke.wait_for_redraw(7, process, transcript, bytearray.decode,
                                         "export", expected="saved")
        self.assertEqual(transcript, b"Saving terminal text\n")


if __name__ == "__main__":
    unittest.main()
