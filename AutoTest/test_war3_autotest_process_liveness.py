#!/usr/bin/env python
# -*- coding: utf-8 -*-

import unittest
from unittest import mock

import war3_autotest_mcp as autotest


class _FakeWitness:
    def __init__(
        self,
        *,
        pid: int,
        exit_code: int | None,
        available: bool = True,
        poll_error: bool = False,
    ) -> None:
        self.pid = int(pid)
        self.exit_code = exit_code
        self.available = bool(available)
        self.poll_error = bool(poll_error)

    def snapshot(self):
        return {"available": self.available, "pid": self.pid}

    def poll(self):
        if self.poll_error:
            raise OSError("synthetic witness failure")
        return self.exit_code


class StateOwnedProcessLivenessTests(unittest.TestCase):
    def setUp(self) -> None:
        self.previous_pid = autotest.STATE.war3_pid
        self.previous_witness = autotest.STATE.retained_native_process

    def tearDown(self) -> None:
        autotest.STATE.war3_pid = self.previous_pid
        autotest.STATE.retained_native_process = self.previous_witness

    def test_retained_handle_overrides_weak_pid_false_negative(self) -> None:
        autotest.STATE.war3_pid = 1234
        autotest.STATE.retained_native_process = _FakeWitness(
            pid=1234, exit_code=None
        )
        with (
            mock.patch.object(
                autotest, "_pid_alive_via_tasklist", return_value=False
            ) as tasklist_probe,
            mock.patch.object(
                autotest.ctypes.windll.kernel32,
                "OpenProcess",
                side_effect=AssertionError("weak probe must not run"),
            ),
        ):
            self.assertTrue(autotest._pid_alive(1234))
        tasklist_probe.assert_not_called()

    def test_signaled_retained_handle_reports_process_exit(self) -> None:
        autotest.STATE.war3_pid = 1234
        autotest.STATE.retained_native_process = _FakeWitness(
            pid=1234, exit_code=7
        )
        self.assertFalse(autotest._state_owned_process_alive(1234))
        self.assertFalse(autotest._pid_alive(1234))

    def test_identity_mismatch_and_witness_error_fall_back(self) -> None:
        autotest.STATE.war3_pid = 1234
        autotest.STATE.retained_native_process = _FakeWitness(
            pid=9999, exit_code=None
        )
        self.assertIsNone(autotest._state_owned_process_alive(1234))

        autotest.STATE.retained_native_process = _FakeWitness(
            pid=1234, exit_code=None, poll_error=True
        )
        self.assertIsNone(autotest._state_owned_process_alive(1234))


if __name__ == "__main__":
    unittest.main()
