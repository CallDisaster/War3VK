"""Pure startup snapshot tests: no WinAPI, game, build, or screenshot capture."""
import ast
import copy
import os
from pathlib import Path
import tempfile
import threading
import types
import unittest
from unittest import mock

import native_game_startup_snapshot as gate
import run_native_d3d9_survey as survey


class Native:
    def __init__(self, expected):
        self.process, self.expected, self.pending = 44, copy.deepcopy(expected), []
        self.verifications, self.failure = 0, None

    def verify_process(self):
        self.verifications += 1
        if self.failure:
            raise ValueError(self.failure)


class FakeApi:
    def __init__(self, expected):
        self.expected, self.calls = copy.deepcopy(expected), []
        self.row = dict(hwnd=777, pid=expected["pid"], thread=88, title="Warcraft III",
            visible=True, desktop=expected["desktop"], rect=[0, 0, 4, 3])
        self.rows = [self.row]
        self.failure = None
        self.after_print = None
        self.output_drift = False

    def call(self, name, *args):
        self.calls.append((name, *args))
        if self.failure == name:
            raise ValueError("injected " + name)

    def guard_desktop(self, target, input_name):
        self.call("guard", target, input_name)

    def duplicate_process(self, process):
        self.call("duplicate", process)
        if process != 44:
            raise ValueError("wrong retained process")
        return 500

    def verify_handle(self, process, expected):
        self.call("verify", process)
        if process != 500 or expected != self.expected:
            raise ValueError("wrong duplicate identity")

    def thread_desktop(self):
        self.call("thread_desktop")
        return 11

    def open_desktop(self, name):
        self.call("open", name)
        return 22

    def name(self, handle):
        self.call("name", handle)
        return self.expected["desktop"]

    def bind_desktop(self, handle):
        self.call("restore" if handle == 11 else "bind", handle)

    def close_desktop(self, handle):
        self.call("close_desktop", handle)

    def close_process_copy(self, handle):
        self.call("close_copy", handle)

    def startup_windows(self, desktop, expected):
        self.call("windows", desktop, expected)
        return copy.deepcopy(self.rows)

    def window_details(self, hwnd):
        self.call("details", hwnd)
        return copy.deepcopy(self.row)

    def print_window_to(self, hwnd, width, height, spool, cancelled):
        self.call("print", hwnd, width, height)
        if self.output_drift:
            spool.write(b"not-a-bitmap")
        else:
            _, pixels = gate._layout(width, height)
            spool.write(gate._bitmap_header(width, height))
            # Test images are tiny. No game image is rendered/read here.
            spool.write(b"\x27" * pixels)
        if self.after_print:
            self.after_print(cancelled)


class InlineThread:
    def __init__(self, *, target, args, **unused):
        self.target, self.args, self.started = target, args, False

    def start(self):
        self.started = True
        self.target(*self.args)

    def join(self, timeout):
        if timeout != gate.WORKER_TIMEOUT_SECONDS:
            raise AssertionError("timeout contract changed")

    def is_alive(self):
        return False


class DelayedThread(InlineThread):
    def start(self):
        self.started = True

    def is_alive(self):
        return self.started

    def settle(self):
        self.target(*self.args)
        self.started = False


class SnapshotContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.instance = self.root / gate.DEFAULT_INSTANCE_POOL_NAME / "run-01" / "session-01" / "root"
        self.instance.mkdir(parents=True)
        self.exe = self.instance / "war3.exe"
        self.exe.write_bytes(b"non-executable-fixture")
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        self.output = self.artifacts / "startup.bmp"
        self.launch = dict(pid=123, creation100ns=456, actualExecutable=str(self.exe),
            desktop="WarVK_Test_abcdef", inputBefore="Default", jobAtomic=True, handleListApplied=True,
            restrictedToken=dict(childTokenVerifiedBeforeResume=True, resumedAfterVerification=True,
                                 resumePreviousCount=1))
        expected = survey.launch_identity(self.launch, self.exe)
        self.native, self.api = Native(expected), FakeApi(expected)
        self.desktop = types.SimpleNamespace(_entered=True, _desktop=99,
            desktop_name=expected["desktop"], input_before="Default", receipt=dict(
                desktop=expected["desktop"], inputBefore="Default", windowStation="WinSta0",
                desktopCreated=True, desktopClosed=False, restrictedMediumRequested=True,
                globalInputUsed=False, desktopSwitchRequested=False, cleanupErrors=[], launches=[self.launch]))
        for field, value in (("DEFAULT_SANDBOX_ROOT", self.root), ("ARTIFACT_ROOT", self.artifacts)):
            patch = mock.patch.object(gate, field, value)
            patch.start()
            self.addCleanup(patch.stop)
        gate._UNSETTLED_WORKERS.clear()
        self.addCleanup(gate._UNSETTLED_WORKERS.clear)

    def capture(self, **kwargs):
        return gate.capture_startup_snapshot(self.native, launch=self.launch, instance=self.instance,
            desktop=self.desktop, output=self.output, _api=self.api,
            _thread_factory=kwargs.get("thread_factory", InlineThread))

    def assert_failure(self, result):
        self.assertFalse(result["ok"], result)
        self.assertFalse(result["imageAvailable"])
        self.assertFalse(result["readyProven"])
        self.assertFalse(result["gpuRenderingProven"])
        self.assertFalse(result["globalInputUsed"])
        self.assertFalse(result["forcedTermination"])

    def test_pre_ready_capture_has_exact_process_and_no_ready_claim(self):
        result = self.capture()
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["processIdentity"], self.native.expected)
        self.assertEqual(result["image"], survey.identity(self.output))
        self.assertEqual(survey.bmp_dimensions(self.output), [4, 3])
        self.assertFalse(result["readyProven"])
        self.assertFalse(result["visualContentProven"])
        self.assertIn(("duplicate", 44), self.api.calls)
        self.assertIn(("close_copy", 500), self.api.calls)
        self.assertEqual([call[0] for call in self.api.calls if call[0] in
            ("bind", "print", "restore", "close_desktop")], ["bind", "print", "restore", "close_desktop"])

    def test_second_attempt_refused_without_another_capture(self):
        self.assertTrue(self.capture()["ok"])
        self.output = self.artifacts / "second.bmp"
        before = len(self.api.calls)
        self.assert_failure(self.capture())
        self.assertEqual(len(self.api.calls), before)
        self.assertFalse(self.output.exists())

    def test_no_process_pending_or_identity_drift_refused(self):
        for field, value in (("process", 0), ("expected", {}), ("pending", [object()]),
                             ("failure", "creation time changed")):
            with self.subTest(field=field):
                original = getattr(self.native, field)
                setattr(self.native, field, value)
                self.assert_failure(self.capture())
                setattr(self.native, field, original)
                self.assertFalse(self.api.calls)

    def test_invalid_launch_authority_is_not_window_authority(self):
        for field, value in (("pid", True), ("creation100ns", 0), ("jobAtomic", False),
                ("handleListApplied", False), ("restrictedToken", {}), ("timedOut", True),
                ("forcedTermination", True), ("processSettled", True)):
            with self.subTest(field=field):
                old = copy.deepcopy(self.launch)
                self.launch[field] = value
                self.assert_failure(self.capture())
                self.launch.clear()
                self.launch.update(old)
                self.assertFalse(self.api.calls)

    def test_unique_non_hardlinked_sandbox_instance_required(self):
        os.link(self.exe, self.root / "alias.exe")
        self.assert_failure(self.capture())
        self.assertFalse(self.api.calls)

    def test_non_sandbox_and_wrong_layout_refused(self):
        for instance in (self.root, self.root / gate.DEFAULT_INSTANCE_POOL_NAME / "run-01"):
            with self.subTest(instance=instance):
                self.instance = instance
                self.assert_failure(self.capture())
                self.assertFalse(self.api.calls)

    def test_desktop_active_receipt_and_single_launch_required(self):
        for field, value in (("desktopCreated", False), ("desktopClosed", True),
                ("restrictedMediumRequested", False), ("globalInputUsed", True),
                ("desktopSwitchRequested", True), ("windowStation", "Other"),
                ("cleanupErrors", ["unsafe"]), ("launches", []),
                ("launches", [self.launch, self.launch])):
            with self.subTest(field=field):
                old = self.desktop.receipt[field]
                self.desktop.receipt[field] = value
                self.assert_failure(self.capture())
                self.desktop.receipt[field] = old
                self.assertFalse(self.api.calls)

    def test_live_desktop_fields_required(self):
        for field, value in (("_entered", False), ("_desktop", 0),
                             ("desktop_name", "Other"), ("input_before", "Other")):
            with self.subTest(field=field):
                old = getattr(self.desktop, field)
                setattr(self.desktop, field, value)
                self.assert_failure(self.capture())
                setattr(self.desktop, field, old)
                self.assertFalse(self.api.calls)

    def test_default_and_input_target_refused(self):
        for name in ("Default", "default", "InputDesktop"):
            with self.subTest(name=name):
                self.launch["desktop"] = name
                self.launch["inputBefore"] = name
                self.native.expected["desktop"] = name
                self.assert_failure(self.capture())
                self.assertFalse(self.api.calls)

    def test_output_must_be_new_bmp_under_artifacts(self):
        occupied = self.artifacts / "old.bmp"
        occupied.write_bytes(b"keep")
        for output in (self.root / "outside.bmp", self.artifacts / "x.png", occupied,
                       self.artifacts / "missing-parent" / "x.bmp"):
            with self.subTest(output=output):
                self.output = output
                self.assert_failure(self.capture())
                self.assertFalse(self.api.calls)
        self.assertEqual(occupied.read_bytes(), b"keep")

    def test_window_count_title_pid_desktop_and_rect_are_checked(self):
        base = copy.deepcopy(self.api.row)
        mutations = (("pid", 124), ("pid", True), ("desktop", "Default"),
            ("thread", 0), ("hwnd", 0), ("title", "a" * 256), ("visible", 1),
            ("rect", [0, 0, 1]), ("rect", [0, 0, True, 3]), ("rect", [0, 0, 0, 3]),
            ("rect", [0, 0, 8193, 1]), ("rect", [0, 0, 8192, 8192]))
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                row = copy.deepcopy(base)
                row[field] = value
                with self.assertRaises(ValueError):
                    gate._select_window([row], self.native.expected)
        for rows in ([], [base, base], [dict(base, hwnd=800 + x) for x in range(33)]):
            with self.assertRaises(ValueError):
                gate._select_window(rows, self.native.expected)

    def test_multiple_main_windows_refuse_print(self):
        self.api.rows.append(dict(self.api.row, hwnd=778))
        self.assert_failure(self.capture())
        self.assertFalse(any(call[0] == "print" for call in self.api.calls))

    def test_unique_error_dialog_without_game_title_is_inspectable(self):
        self.api.row["title"] = "Map failed to load"
        result = self.capture()
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["windowObservation"]["selectedWindow"]["title"], "Map failed to load")

    def test_multiple_visible_non_main_windows_refuse_guessing(self):
        self.api.row["title"] = "Unknown"
        self.api.rows.append(dict(self.api.row, hwnd=778, title="Other"))
        self.assert_failure(self.capture())
        self.assertFalse(any(call[0] == "print" for call in self.api.calls))

    def test_hidden_window_cannot_be_selected(self):
        self.api.row["visible"] = False
        self.assert_failure(self.capture())
        self.assertFalse(any(call[0] == "print" for call in self.api.calls))

    def test_each_worker_native_error_refuses_final_output(self):
        for operation in ("verify", "thread_desktop", "open", "name", "bind", "windows", "details", "print",
                          "restore", "close_desktop", "close_copy"):
            with self.subTest(operation=operation):
                self.native._startup_snapshot_attempted = False
                self.api = FakeApi(self.native.expected)
                self.api.failure = operation
                self.assert_failure(self.capture())
                self.assertFalse(self.output.exists())
                self.assertIn(("close_copy", 500), self.api.calls)

    def test_restore_failure_does_not_close_bound_desktop(self):
        self.api.failure = "restore"
        result = self.capture()
        self.assert_failure(result)
        self.assertTrue(result["worker"]["cleanupErrors"])
        self.assertFalse(any(call[0] == "close_desktop" for call in self.api.calls))

    def test_print_failure_has_no_screen_copy_fallback(self):
        self.api.failure = "print"
        self.assert_failure(self.capture())
        self.assertEqual(sum(call[0] == "print" for call in self.api.calls), 1)
        self.assertFalse(self.output.exists())

    def test_post_print_window_change_refuses_publication(self):
        self.api.after_print = lambda cancel: self.api.row.update(title="Changed")
        self.assert_failure(self.capture())
        self.assertFalse(self.output.exists())

    def test_post_print_process_authority_change_refuses_publication(self):
        self.api.after_print = lambda cancel: setattr(self.native, "process", 55)
        self.assert_failure(self.capture())
        self.assertFalse(self.output.exists())

    def test_cancellation_after_print_never_publishes(self):
        self.api.after_print = lambda cancel: cancel.set()
        self.assert_failure(self.capture())
        self.assertFalse(self.output.exists())

    def test_wrong_bmp_bytes_cannot_become_image(self):
        self.api.output_drift = True
        self.assert_failure(self.capture())
        self.assertFalse(self.output.exists())

    def test_thread_timeout_and_eventual_completion_cannot_publish_late_image(self):
        result = self.capture(thread_factory=DelayedThread)
        self.assert_failure(result)
        self.assertTrue(result["workerPending"])
        self.assertFalse(self.output.exists())
        worker, state = gate._UNSETTLED_WORKERS[0]
        self.assertTrue(state["cancel"].is_set())
        worker.settle()
        self.assertIsNone(state["spool"])
        self.assertFalse(self.output.exists())
        self.assertIn(("close_copy", 500), self.api.calls)
        self.assertFalse(any(call[0] == "print" for call in self.api.calls))

    def test_inflight_print_timeout_late_return_cannot_publish(self):
        entered, release = threading.Event(), threading.Event()
        class InFlightApi(FakeApi):
            def print_window_to(self, *args):
                super().print_window_to(*args)
                entered.set()
                if not release.wait(3):
                    raise RuntimeError("fake PrintWindow release deadline")
        class InFlightThread(threading.Thread):
            def join(self, timeout=None):
                if not entered.wait(2):
                    raise RuntimeError("fake PrintWindow never entered")
                return super().join(timeout)
        self.api = InFlightApi(self.native.expected)
        try:
            with mock.patch.object(gate, "WORKER_TIMEOUT_SECONDS", 0.01):
                result = self.capture(thread_factory=InFlightThread)
            self.assertTrue(entered.is_set())
            self.assert_failure(result)
            self.assertTrue(result["workerPending"])
            self.assertEqual(result["windowObservation"]["selectedWindow"]["hwnd"], 777)
            self.assertFalse(self.output.exists())
        finally:
            release.set()
            for worker, state in gate._UNSETTLED_WORKERS:
                worker.join(2)
                self.assertFalse(worker.is_alive())
                self.assertIsNone(state["spool"])
        self.assertFalse(self.output.exists())
        self.assertEqual(self.api.calls.count(("close_copy", 500)), 1)

    def test_second_live_pending_worker_cannot_expand_budget(self):
        self.capture(thread_factory=DelayedThread)
        self.native._startup_snapshot_attempted = False
        before = len(self.api.calls)
        self.assert_failure(self.capture())
        self.assertEqual(len(self.api.calls), before)
        gate._UNSETTLED_WORKERS[0][0].settle()

    def test_thread_start_failure_closes_duplicate(self):
        class FailsStart(InlineThread):
            def start(self):
                raise RuntimeError("cannot start")
        self.assert_failure(self.capture(thread_factory=FailsStart))
        self.assertIn(("close_copy", 500), self.api.calls)
        self.assertFalse(self.output.exists())

    def test_thread_constructor_failure_closes_duplicate(self):
        def fail_constructor(**unused):
            raise RuntimeError("cannot construct")
        self.assert_failure(self.capture(thread_factory=fail_constructor))
        self.assertIn(("close_copy", 500), self.api.calls)
        self.assertFalse(self.output.exists())

    def test_unexpected_join_failure_cancels_and_retains_pending_worker(self):
        class FailsJoin(DelayedThread):
            def join(self, timeout):
                raise RuntimeError("join failed")
        result = self.capture(thread_factory=FailsJoin)
        self.assert_failure(result)
        self.assertTrue(result["workerPending"])
        worker, state = gate._UNSETTLED_WORKERS[0]
        self.assertTrue(state["cancel"].is_set())
        worker.settle()
        self.assertIsNone(state["spool"])
        self.assertFalse(self.output.exists())

    def test_dimensions_budget_covers_1440p_but_not_huge_window(self):
        stride, pixels = gate._layout(2560, 1440)
        self.assertEqual(stride, 7680)
        self.assertEqual(pixels, 11059200)
        self.assertLess(gate.MAX_PIXEL_BYTES + gate.COPY_CHUNK + 54, 16 * 1024 * 1024)
        for width, height in ((True, 3), (0, 3), (-1, 3), (3, 0), (8193, 1), (8192, 8192)):
            with self.assertRaises(ValueError):
                gate._layout(width, height)

    def test_module_has_no_pid_reopen_input_launch_screen_copy_or_force(self):
        source = Path(gate.__file__).read_text("utf-8")
        tree = ast.parse(source)
        forbidden = {"OpenProcess", "CreateProcessW", "SwitchDesktop", "SendInput", "BitBlt",
            "CopyFromScreen", "GetDC", "GetWindowDC", "TerminateProcess", "TerminateJobObject",
            "SetForegroundWindow", "PostMessageW", "Popen", "system", "shell"}
        calls = {node.func.attr for node in ast.walk(tree)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)}
        self.assertFalse(forbidden & calls)
        self.assertNotIn("__main__", source)


if __name__ == "__main__":
    unittest.main()
