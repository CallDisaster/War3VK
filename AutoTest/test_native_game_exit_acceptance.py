"""Pure normal-exit contract tests; no game, Desktop, Win32 call, or build."""
import ast
import copy
import ctypes
from ctypes import wintypes as wt
import os
from pathlib import Path
import struct
import tempfile
import threading
import types
import unittest
from unittest import mock

import native_game_exit_acceptance as gate
import run_native_d3d9_survey as survey


def status(stamp=1, state=(True, True, False, True)):
    return dict(source="control-plane", timestampMs=stamp,
        runtime=dict(gameStarted=state[0]), render=dict(isInGame=state[1],
        isLoading=state[2], inGameRenderReady=state[3]))


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class Observer:
    def __init__(self, expected):
        self.expected = copy.deepcopy(expected)
        self.process, self.pending = 44, []
        self.requests, self.verifications = [], 0
        self.transform = None
        self.status_count = 0
        self.failure = None

    def verify_process(self):
        self.verifications += 1
        if self.failure:
            raise ValueError(self.failure)

    def request(self, command, payload, timeout=3.0):
        self.requests.append((command, copy.deepcopy(payload), timeout))
        request = dict(requestId="unique-" + str(len(self.requests)), command=command,
                       payload=copy.deepcopy(payload), pid=self.expected["pid"])
        if command == "get_runtime_status":
            self.status_count += 1
            result = status(self.status_count, (True, True, False, True) if self.status_count == 1
                else (False, False, False, False))
        elif command == gate.EXIT_COMMAND and payload == gate.EXIT_PAYLOAD:
            result = dict(handled=True, command="game.end_for_exit_test",
                          stockEndGameInvoked=True, processExitProven=False)
        else:
            raise AssertionError("unexpected operation")
        row = dict(request=request, pipeServerPid=self.expected["pid"],
            response=dict(requestId=request["requestId"], command=command, ok=True, result=result))
        if self.transform:
            self.transform(row, len(self.requests))
        return row


class ExitApi:
    def __init__(self):
        self.calls = []
        self.close = dict(posted=True, workerDesktopRestored=True, desktopHandleClosed=True,
                          workerPending=False, cleanupErrors=[], hwnd=777)
        self.exit = dict(settled=True, timedOut=False, exitCode=0)
        self.guard_error_at = None
        self.after_close = None

    def guard_desktop(self, target, original):
        self.calls.append(("guard", target, original))
        if self.guard_error_at == sum(row[0] == "guard" for row in self.calls):
            raise ValueError("input desktop changed")

    def close_window(self, native, expected, original):
        self.calls.append(("close", native.process, copy.deepcopy(expected), original))
        if self.after_close:
            self.after_close(native)
        return copy.deepcopy(self.close)

    def wait_exit(self, handle, seconds):
        self.calls.append(("wait", handle, seconds))
        if isinstance(self.exit, Exception):
            raise self.exit
        return copy.deepcopy(self.exit)


class ExitContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.instance = self.root / gate.DEFAULT_INSTANCE_POOL_NAME / "run-01" / "session-01" / "root"
        self.instance.mkdir(parents=True)
        self.exe = self.instance / "war3.exe"
        self.exe.write_bytes(b"fixture-executable-not-runnable")
        picture = self.root / "capture.bmp"
        header = bytearray(58)
        struct.pack_into("<2sI", header, 0, b"BM", len(header))
        struct.pack_into("<I", header, 10, 54)
        struct.pack_into("<IiiHHI", header, 14, 40, 1, 1, 1, 24, 0)
        picture.write_bytes(header)
        self.launch = dict(pid=123, creation100ns=456, actualExecutable=str(self.exe),
            jobAtomic=True, handleListApplied=True, desktop="WarVK_Test_abcdef", inputBefore="Default",
            restrictedToken=dict(childTokenVerifiedBeforeResume=True, resumedAfterVerification=True,
                                 resumePreviousCount=1))
        expected = survey.launch_identity(self.launch, self.exe)
        self.observation = dict(processIdentity=copy.deepcopy(expected), ready=True, modulesVerified=True,
            screenshotVerified=True, screenshot=survey.identity(picture), actualResolution=[1, 1],
            capture=dict(outputPath=str(picture)), cleanupErrors=[], ipcDisabled=False)
        self.native = Observer(expected)
        self.desktop = types.SimpleNamespace(_entered=True, _desktop=99,
            desktop_name=expected["desktop"], input_before="Default", receipt=dict(
                desktop=expected["desktop"], inputBefore="Default", windowStation="WinSta0",
                desktopCreated=True, desktopClosed=False, restrictedMediumRequested=True,
                globalInputUsed=False, desktopSwitchRequested=False, cleanupErrors=[], launches=[self.launch]))
        self.api, self.clock = ExitApi(), Clock()
        self.patch = mock.patch.object(gate, "DEFAULT_SANDBOX_ROOT", self.root)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def run_gate(self, **kwargs):
        return gate.accept_normal_exit(self.native, launch=self.launch, observation=self.observation,
            instance=self.instance, desktop=self.desktop, _api=self.api,
            _clock=self.clock, _sleep=self.clock.sleep, **kwargs)

    def assert_no_close(self, result):
        self.assertFalse(result["ok"])
        self.assertFalse(result["normalExitProven"])
        self.assertFalse(result["forcedTermination"])
        self.assertFalse(any(row[0] in ("close", "wait") for row in self.api.calls))

    def test_exact_chain_waits_original_retained_handle(self):
        result = self.run_gate()
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["normalExitProven"])
        self.assertTrue(result["mapTeardownObserved"])
        self.assertEqual(result["phase"], "complete")
        self.assertEqual([row[0] for row in self.native.requests],
            ["get_runtime_status", gate.EXIT_COMMAND, "get_runtime_status", "get_runtime_status"])
        self.assertEqual(self.native.requests[1][1], gate.EXIT_PAYLOAD)
        self.assertIn(("wait", 44, 10.0), self.api.calls)
        self.assertEqual(self.native.process, 44)
        self.assertFalse(result["globalInputUsed"])
        self.assertFalse(result["desktopSwitchRequested"])

    def test_one_shot_does_not_reissue_endgame_or_window_close(self):
        self.assertTrue(self.run_gate()["ok"])
        count = len(self.native.requests), len(self.api.calls)
        self.assertFalse(self.run_gate()["ok"])
        self.assertEqual((len(self.native.requests), len(self.api.calls)), count)

    def test_bound_identity_and_ipc_are_mandatory(self):
        for field, value in (("process", 0), ("expected", None), ("pending", [object()])):
            old = getattr(self.native, field)
            with self.subTest(field=field):
                setattr(self.native, field, value)
                self.assert_no_close(self.run_gate())
                self.assertFalse(self.native.requests)
                setattr(self.native, field, old)

    def test_exact_creation_not_only_pid(self):
        self.native.expected["creation100ns"] += 1
        self.assert_no_close(self.run_gate())
        self.assertFalse(self.native.requests)

    def test_live_handle_identity_failure_precedes_ipc(self):
        self.native.failure = "creation/path mismatch"
        self.assert_no_close(self.run_gate())
        self.assertFalse(self.native.requests)

    def test_exact_ready_module_capture_observation_required(self):
        for field, value in (("ready", False), ("ready", 1), ("modulesVerified", False),
                ("screenshotVerified", False), ("processIdentity", {}), ("ipcDisabled", True),
                ("cleanupErrors", ["old close failed"])):
            original = copy.deepcopy(self.observation)
            with self.subTest(field=field, value=value):
                self.observation[field] = value
                self.assert_no_close(self.run_gate())
                self.assertFalse(self.native.requests)
                self.observation = original

    def test_launch_restricted_atomic_authority_required(self):
        for field, value in (("pid", True), ("creation100ns", 0), ("jobAtomic", False),
                ("handleListApplied", False), ("restrictedToken", {})):
            old = self.launch[field]
            with self.subTest(field=field):
                self.launch[field] = value
                self.assert_no_close(self.run_gate())
                self.assertFalse(self.native.requests)
                self.launch[field] = old

    def test_timedout_forced_or_settled_launch_is_not_normal_exit_candidate(self):
        for field in ("timedOut", "forcedTermination", "processSettled"):
            with self.subTest(field=field):
                self.launch[field] = True
                self.assert_no_close(self.run_gate())
                self.assertFalse(self.native.requests)
                del self.launch[field]

    def test_sandbox_base_is_not_a_disposable_instance(self):
        self.instance = self.root
        self.assert_no_close(self.run_gate())

    def test_non_sandbox_and_wrong_layout_rejected(self):
        for instance in (self.root.parent, self.root / gate.DEFAULT_INSTANCE_POOL_NAME / "run-01"):
            with self.subTest(instance=str(instance)):
                self.instance = instance
                self.assert_no_close(self.run_gate())

    def test_hardlinked_executable_rejected(self):
        os.link(self.exe, self.root / "war3-alias.exe")
        self.assert_no_close(self.run_gate())
        self.assertFalse(self.native.requests)

    def test_capture_hash_drift_rejected(self):
        with Path(self.observation["screenshot"]["path"]).open("ab") as stream:
            stream.write(b"drift")
        self.assert_no_close(self.run_gate())

    def test_capture_path_and_dimensions_rejected(self):
        self.observation["capture"]["outputPath"] = str(self.exe)
        self.assert_no_close(self.run_gate())
        self.observation["capture"]["outputPath"] = self.observation["screenshot"]["path"]
        self.observation["actualResolution"] = [2, 1]
        self.assert_no_close(self.run_gate())

    def test_default_and_current_input_desktop_rejected(self):
        for name in ("Default", "default", "InputOther"):
            self.launch["desktop"] = name
            if name == "InputOther":
                self.launch["inputBefore"] = name
            self.native.expected["desktop"] = name
            self.observation["processIdentity"]["desktop"] = name
            with self.subTest(name=name):
                self.assert_no_close(self.run_gate())
                self.assertFalse(self.native.requests)

    def test_desktop_ownership_receipt_required(self):
        for field, value in (("desktopCreated", False), ("desktopClosed", True),
                ("restrictedMediumRequested", False), ("globalInputUsed", True),
                ("desktopSwitchRequested", True), ("inputBefore", "Other"),
                ("windowStation", "Other"), ("cleanupErrors", ["error"])):
            original = copy.deepcopy(self.desktop.receipt)
            with self.subTest(field=field):
                self.desktop.receipt[field] = value
                self.assert_no_close(self.run_gate())
                self.assertFalse(self.native.requests)
                self.desktop.receipt = original

    def test_live_desktop_context_required(self):
        for field, value in (("_entered", False), ("_desktop", 0), ("desktop_name", "Other"),
                             ("input_before", "Other")):
            old = getattr(self.desktop, field)
            with self.subTest(field=field):
                setattr(self.desktop, field, value)
                self.assert_no_close(self.run_gate())
                setattr(self.desktop, field, old)

    def test_unique_launch_of_exact_executable_required(self):
        self.desktop.receipt["launches"].append(copy.deepcopy(self.launch))
        self.assert_no_close(self.run_gate())
        self.desktop.receipt["launches"] = []
        self.assert_no_close(self.run_gate())

    def test_request_and_response_identity_not_trusted(self):
        mutations = (("request", "command", "shutdown_session"), ("request", "payload", {}),
            ("request", "pid", 456), ("request", "pid", True), ("request", "requestId", ""),
            ("response", "requestId", "old"), ("response", "command", "ping"),
            ("response", "ok", 1), ("response", "result", []),
            (None, "pipeServerPid", 124), (None, "pipeServerPid", True))
        for scope, field, value in mutations:
            with self.subTest(scope=scope, field=field, value=value):
                self.native = Observer(self.observation["processIdentity"])
                def change(row, index):
                    if index == 2:
                        (row if scope is None else row[scope])[field] = value
                self.native.transform = change
                self.assert_no_close(self.run_gate())

    def test_replayed_request_nonce_rejected(self):
        def change(row, index):
            if index == 2:
                row["request"]["requestId"] = row["response"]["requestId"] = "unique-1"
        self.native.transform = change
        self.assert_no_close(self.run_gate())

    def test_internal_refusal_or_timeout_cannot_post_close(self):
        for timed_out in (False, True):
            with self.subTest(timed_out=timed_out):
                self.native = Observer(self.observation["processIdentity"])
                def change(row, index):
                    if index == 2:
                        if timed_out:
                            raise TimeoutError("internal test deadline")
                        row["response"]["ok"] = False
                self.native.transform = change
                self.assert_no_close(self.run_gate())

    def test_internal_endgame_result_contract(self):
        for field, value in (("handled", False), ("command", "shutdown.session"),
                             ("stockEndGameInvoked", False), ("processExitProven", True)):
            with self.subTest(field=field):
                self.native = Observer(self.observation["processIdentity"])
                def change(row, index):
                    if index == 2:
                        row["response"]["result"][field] = value
                self.native.transform = change
                self.assert_no_close(self.run_gate())

    def test_state_boolean_shape_source_and_timestamp(self):
        mutations = (("source", "cached"), ("timestampMs", True), ("timestampMs", -1),
                     ("runtime", {"gameStarted": 1}), ("render", {}))
        for field, value in mutations:
            with self.subTest(field=field):
                self.native = Observer(self.observation["processIdentity"])
                def change(row, index):
                    if index == 1:
                        row["response"]["result"][field] = value
                self.native.transform = change
                self.assert_no_close(self.run_gate())
                self.assertEqual(len(self.native.requests), 1)

    def test_menu_before_endgame_is_not_ready_game_acceptance(self):
        def change(row, index):
            if index == 1:
                row["response"]["result"] = status(1, (False,) * 4)
        self.native.transform = change
        self.assert_no_close(self.run_gate())

    def test_no_map_teardown_is_bounded_and_not_a_fixed_sleep_pass(self):
        def change(row, index):
            if index >= 3:
                row["response"]["result"] = status(index)
        self.native.transform = change
        self.assert_no_close(self.run_gate())
        self.assertLessEqual(len(self.native.requests), 102)
        self.assertLessEqual(self.clock.now, 10.001)

    def test_only_one_fresh_menu_status_is_insufficient(self):
        def change(row, index):
            if index >= 3:
                row["response"]["result"] = status(2, (False,) * 4)
        self.native.transform = change
        self.assert_no_close(self.run_gate())

    def test_loading_mixed_or_regressed_status_does_not_close(self):
        for state, stamp in (((False, False, True, False), 9), ((False,) * 4, 0)):
            with self.subTest(state=state, stamp=stamp):
                self.native = Observer(self.observation["processIdentity"])
                self.clock = Clock()
                def change(row, index):
                    if index >= 3:
                        row["response"]["result"] = status(stamp, state)
                self.native.transform = change
                self.assert_no_close(self.run_gate())

    def test_authority_or_pending_change_after_ipc_rejected(self):
        for pending in (False, True):
            with self.subTest(pending=pending):
                self.native = Observer(self.observation["processIdentity"])
                def change(row, index):
                    if index == 2:
                        if pending:
                            self.native.pending.append(object())
                        else:
                            self.native.expected["creation100ns"] += 1
                self.native.transform = change
                self.assert_no_close(self.run_gate())

    def test_input_change_prevents_side_effect(self):
        self.api.guard_error_at = 1
        self.assert_no_close(self.run_gate())
        self.assertFalse(self.native.requests)

    def test_input_change_after_status_prevents_endgame(self):
        self.api.guard_error_at = 2
        self.assert_no_close(self.run_gate())
        self.assertEqual(len(self.native.requests), 1)

    def test_handle_change_during_status_prevents_endgame(self):
        def change(row, index):
            if index == 1:
                self.native.process = 888
        self.native.transform = change
        self.assert_no_close(self.run_gate())
        self.assertEqual(len(self.native.requests), 1)

    def test_close_worker_error_pending_or_cleanup_failure_not_exit_acceptance(self):
        for field, value in (("posted", False), ("workerDesktopRestored", False),
                ("desktopHandleClosed", False), ("workerPending", True),
                ("error", "owner changed"), ("cleanupErrors", ["close error"])):
            with self.subTest(field=field):
                self.native = Observer(self.observation["processIdentity"])
                self.api = ExitApi()
                self.api.close[field] = value
                result = self.run_gate()
                self.assertFalse(result["ok"])
                self.assertFalse(any(row[0] == "wait" for row in self.api.calls))

    def test_handle_change_after_close_cannot_wait_wrong_handle(self):
        self.api.after_close = lambda native: setattr(native, "process", 777)
        result = self.run_gate()
        self.assertFalse(result["ok"])
        self.assertFalse(any(row[0] == "wait" for row in self.api.calls))

    def test_nonzero_timeout_wait_error_and_boolean_exit_rejected(self):
        for exit_row in (dict(settled=True, timedOut=False, exitCode=1),
                dict(settled=False, timedOut=True, exitCode=None),
                dict(settled=True, timedOut=False, exitCode=False), RuntimeError("wait failed")):
            with self.subTest(exit_row=exit_row):
                self.native = Observer(self.observation["processIdentity"])
                self.api = ExitApi()
                self.api.exit = exit_row
                result = self.run_gate()
                self.assertFalse(result["ok"])
                self.assertFalse(result["forcedTermination"])
                self.assertEqual(result["phase"], "retained-handle-exit")

    def test_input_change_after_exit_does_not_certify_isolation(self):
        self.api.guard_error_at = 4
        result = self.run_gate()
        self.assertFalse(result["ok"])

    def test_bad_wait_parameters_fail_without_ipc(self):
        for value in (0, -1, 31, True, "10", float("nan"), float("inf")):
            with self.subTest(value=value):
                self.assert_no_close(self.run_gate(wait_seconds=value))
                self.assertFalse(self.native.requests)


class WorkerApi:
    def __init__(self):
        self.calls, self.windows = [], [(777, 88)]
        self.owner = (123, 88)
        self.title = "Warcraft III"
        self.expected = dict(pid=123, creation100ns=456, actualExecutable="owned.exe", desktop="Owned_Desk")
        self.names = {11: "Default", 22: "Owned_Desk", 33: "Owned_Desk"}
        self.fail_operation = None

    def call(self, name, *args):
        self.calls.append((name, *args))
        if self.fail_operation == name:
            raise ValueError("injected " + name)

    def verify_handle(self, handle, expected):
        self.call("verify", handle, expected)
        if handle != 500 or expected != self.expected:
            raise ValueError("bad retained identity")

    def guard_desktop(self, target, original):
        self.call("guard", target, original)

    def thread_desktop(self, thread=None):
        self.call("thread_desktop", thread)
        return 11 if thread is None else 33

    def open_desktop(self, name):
        self.call("open", name)
        return 22

    def name(self, handle):
        self.call("name", handle)
        return self.names[handle]

    def bind_desktop(self, handle):
        self.call("restore" if handle == 11 else "bind", handle)

    def owned_windows(self, handle, pid):
        self.call("enumerate", handle, pid)
        return self.windows

    def window_owner(self, hwnd):
        self.call("owner", hwnd)
        return self.owner

    def window_title(self, hwnd):
        self.call("title", hwnd)
        return self.title

    def post_close(self, hwnd):
        self.call("post", hwnd)

    def close_desktop(self, handle):
        self.call("close_desktop", handle)

    def close_process_copy(self, handle):
        self.call("close_copy", handle)


class WorkerContracts(unittest.TestCase):
    def run_worker(self, api, cancelled=False):
        cancel = threading.Event()
        if cancelled:
            cancel.set()
        result = dict(posted=False, workerDesktopRestored=False, desktopHandleClosed=False, cleanupErrors=[])
        gate._close_worker(api, 500, api.expected, "Default", cancel, result)
        return result

    def test_exact_window_and_worker_restored(self):
        api = WorkerApi()
        result = self.run_worker(api)
        self.assertTrue(result["posted"])
        self.assertTrue(result["workerDesktopRestored"])
        self.assertTrue(result["desktopHandleClosed"])
        self.assertEqual([x[0] for x in api.calls][-3:], ["restore", "close_desktop", "close_copy"])
        self.assertIn(("enumerate", 22, 123), api.calls)

    def test_zero_or_two_windows_refuse_post(self):
        for windows in ([], [(777, 88), (778, 89)]):
            api = WorkerApi()
            api.windows = windows
            with self.subTest(windows=windows):
                result = self.run_worker(api)
                self.assertFalse(result["posted"])
                self.assertFalse(any(row[0] == "post" for row in api.calls))
                self.assertTrue(result["workerDesktopRestored"])

    def test_wrong_owner_thread_title_or_desktop_refuse_post(self):
        for field, value in (("owner", (124, 88)), ("owner", (123, 89)),
                             ("title", "Different"), ("names", {11: "Default", 22: "Other", 33: "Other"})):
            api = WorkerApi()
            setattr(api, field, value)
            with self.subTest(field=field, value=value):
                result = self.run_worker(api)
                self.assertFalse(result["posted"])
                self.assertFalse(any(row[0] == "post" for row in api.calls))

    def test_window_thread_on_wrong_desktop_refuse_post(self):
        api = WorkerApi()
        # It was on the owned Desktop at first enumeration, but no longer is
        # at the final membership check. Do not infer membership from PID.
        replies = iter([[(777, 88)], []])
        api.owned_windows = lambda handle, pid: next(replies)
        self.assertFalse(self.run_worker(api)["posted"])

    def test_same_pid_replaced_window_or_thread_refuses_post(self):
        for final in ([(778, 88)], [(777, 89)], [(777, 88), (779, 90)]):
            api = WorkerApi()
            replies = iter([[(777, 88)], final])
            api.owned_windows = lambda handle, pid: next(replies)
            self.assertFalse(self.run_worker(api)["posted"])

    def test_remote_thread_desktop_access_not_required(self):
        api = WorkerApi()
        def current_only(thread=None):
            self.assertIsNone(thread, "must use owned Desktop enumeration, not remote TID query")
            return 11
        api.thread_desktop = current_only
        self.assertTrue(self.run_worker(api)["posted"])
        self.assertEqual(api.calls.count(("enumerate", 22, 123)), 2)

    def test_worker_cancelled_before_final_check_does_not_post(self):
        api = WorkerApi()
        result = self.run_worker(api, cancelled=True)
        self.assertFalse(result["posted"])
        self.assertTrue(result["desktopHandleClosed"])
        self.assertFalse(any(row[0] == "post" for row in api.calls))

    def test_each_native_failure_remains_failure(self):
        for operation in ("verify", "guard", "thread_desktop", "open", "name", "bind",
                          "enumerate", "owner", "title", "post"):
            api = WorkerApi()
            api.fail_operation = operation
            with self.subTest(operation=operation):
                result = self.run_worker(api)
                self.assertFalse(result["posted"])
                self.assertIn("error", result)
                self.assertIn(("close_copy", 500), api.calls)

    def test_restore_failure_does_not_close_bound_desktop_or_pass(self):
        api = WorkerApi()
        api.fail_operation = "restore"
        result = self.run_worker(api)
        self.assertTrue(result["posted"])
        self.assertFalse(result["workerDesktopRestored"])
        self.assertTrue(result["cleanupErrors"])
        self.assertFalse(any(row[0] == "close_desktop" for row in api.calls))
        self.assertIn(("close_copy", 500), api.calls)

    def test_close_handle_failure_cannot_pass(self):
        for operation in ("close_desktop", "close_copy"):
            api = WorkerApi()
            api.fail_operation = operation
            with self.subTest(operation=operation):
                self.assertTrue(self.run_worker(api)["cleanupErrors"])

    def test_wait_uses_given_handle_without_opening_pid(self):
        api = gate._WindowsExitApi.__new__(gate._WindowsExitApi)
        calls = []
        def wait(handle, ms):
            calls.append((handle, ms))
            return 0
        def code(handle, output):
            ctypes.cast(output, ctypes.POINTER(wt.DWORD)).contents.value = 0
            return True
        api.k = types.SimpleNamespace(WaitForSingleObject=wait, GetExitCodeProcess=code)
        self.assertEqual(api.wait_exit(44, 1.25), dict(settled=True, timedOut=False, exitCode=0))
        self.assertEqual(calls, [(44, 1250)])
        api.k.WaitForSingleObject = lambda handle, ms: 258
        self.assertEqual(api.wait_exit(44, 1), dict(settled=False, timedOut=True, exitCode=None))
        api.k.WaitForSingleObject = lambda handle, ms: 0xFFFFFFFF
        with self.assertRaises(ValueError):
            api.wait_exit(44, 1)

    def test_module_has_no_launch_input_switch_force_or_second_transport(self):
        source = Path(gate.__file__).read_text("utf-8")
        tree = ast.parse(source)
        forbidden = {"TerminateProcess", "TerminateJobObject", "CreateProcessW", "SwitchDesktop",
            "SendInput", "mouse_event", "keybd_event", "SetForegroundWindow", "OpenProcess",
            "CreateFileW", "WriteFile", "ReadFile", "shell", "system", "Popen"}
        called = {node.func.attr for node in ast.walk(tree)
                  if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)}
        self.assertFalse(forbidden & called)
        for node in tree.body:
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
                self.fail("native action at import")
        self.assertNotIn("__main__", source)


if __name__ == "__main__":
    unittest.main()
