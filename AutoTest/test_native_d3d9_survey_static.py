"""Pure contract/fake tests: no Windows API, executable, game, GPU, or build."""
import ast
import copy
import ctypes
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_native_d3d9_survey as survey
from test_d3d9_migration_census_integration import sample_snapshot, sample_snapshot_v2


def launch_sample(executable):
    return dict(pid=123, creation100ns=456, actualExecutable=str(executable),
        jobAtomic=True, handleListApplied=True, desktop="WarVK_Survey_abc", inputBefore="Default",
        restrictedToken=dict(childTokenVerifiedBeforeResume=True, resumedAfterVerification=True,
                             resumePreviousCount=1))


def receipt_sample():
    return dict(gameLaunch=dict(ok=True, exitCode=0, timedOut=False, forcedTermination=False,
                    processSettled=True, jobEmpty=True), desktopSafety=dict(ok=True),
                observerJoined=True, protectedUnchanged=True, cleanupErrors=[],
                observer=dict(ready=True, modulesVerified=True, screenshotVerified=True, cleanupErrors=[]),
                census=[dict(rows=[dict(census={"schema": survey.CENSUS_SCHEMA})], errors=[])])


class PureContracts(unittest.TestCase):
    def test_import_has_no_native_call(self):
        source = Path(survey.__file__).read_text("utf-8")
        tree = ast.parse(source)
        for node in tree.body:
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
                self.fail("top-level call on import")

    def test_only_known_observer_commands(self):
        self.assertEqual(survey.COMMANDS, {"ping", "get_runtime_status", "wait_until", "capture_final_frame"})
        obj = survey.NativeObserver.__new__(survey.NativeObserver)
        for command in ("shutdown_session", "game.end_for_exit_test", "invoke_test_command", "frame_evidence"):
            with self.subTest(command=command), self.assertRaises(ValueError):
                obj.request(command, {})

    def test_strict_json_rejects_duplicates_and_nonfinite(self):
        for text in ('{"a":1,"a":2}', '{"a":NaN}', '{"a":Infinity}', '{"a":-Infinity}'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                survey.strict_json(text)

    def test_exit_opt_in_authorizes_only_exact_stock_endgame(self):
        payload = dict(command="game.end_for_exit_test", payload={}, timeoutMs=3000)
        self.assertTrue(survey.exit_request_allowed(True, "invoke_test_command", payload))
        for enabled, command, data in (
                (False, "invoke_test_command", payload), (1, "invoke_test_command", payload),
                (True, "shutdown_session", payload), (True, "invoke_test_command", {}),
                (True, "invoke_test_command", dict(payload, command="shutdown.session")),
                (True, "invoke_test_command", dict(payload, payload={"force": True})),
                (True, "invoke_test_command", dict(payload, timeoutMs=3000.0)),
                (True, "invoke_test_command", dict(payload, timeoutMs=True)),
                (True, "invoke_test_command", dict(payload, extra=True))):
            with self.subTest(enabled=enabled, command=command, payload=data):
                self.assertFalse(survey.exit_request_allowed(enabled, command, data))

    def test_explicit_exit_gate_still_requires_observations_cleanup_and_real_exit(self):
        row = receipt_sample()
        row["normalExitTestRequested"] = True
        row["observer"]["normalExitTest"] = dict(ok=True, normalExitProven=True,
            endGameAccepted=True, mapTeardownObserved=True)
        result = survey.classify(copy.deepcopy(row))
        self.assertTrue(result["ok"])
        self.assertEqual(result["verdict"], "LOCAL_GAME_START_CAPTURE_EXIT_PASSED")
        self.assertFalse(result["x64GameRenderingProven"] or result["visualAcceptance"])
        for group, field, value in (
                ("gameLaunch", "timedOut", True), ("gameLaunch", "forcedTermination", True),
                ("gameLaunch", "exitCode", 1), ("gameLaunch", "jobEmpty", False),
                ("observer", "screenshotVerified", False), ("observer", "ready", False),
                ("observer", "cleanupErrors", ["failure"]), ("desktopSafety", "ok", False)):
            bad = copy.deepcopy(row)
            bad[group][field] = value
            with self.subTest(field=field):
                self.assertFalse(survey.classify(bad)["ok"])
        for field in row["observer"]["normalExitTest"]:
            bad = copy.deepcopy(row)
            bad["observer"]["normalExitTest"][field] = False
            with self.subTest(exit_field=field):
                self.assertFalse(survey.classify(bad)["ok"])
        bad = copy.deepcopy(row)
        bad["protectedUnchanged"] = False
        self.assertFalse(survey.classify(bad)["ok"])
        for top_level in (True, False):
            bad = copy.deepcopy(row)
            (bad if top_level else bad["observer"])["error"] = "OSError: journal close failed"
            self.assertFalse(survey.classify(bad)["ok"])

    def test_memory_metadata_gate_does_not_claim_census_or_savings(self):
        row = receipt_sample()
        row.update(normalExitTestRequested=True, memoryReviewRequested=True, census=[])
        row['observer']['normalExitTest'] = dict(ok=True, normalExitProven=True,
            endGameAccepted=True, mapTeardownObserved=True)
        row['observer']['memoryReview'] = dict(complete=True)
        result = survey.classify(copy.deepcopy(row))
        self.assertTrue(result['ok'])
        self.assertFalse(result['observationAvailable'] or result['gameMemorySavingsProven'])
        self.assertEqual(result['verdict'], 'LOCAL_GAME_MEMORY_METADATA_AND_EXIT_PASSED')
        for key in ('ready', 'screenshotVerified', 'modulesVerified'):
            bad = copy.deepcopy(row)
            bad['observer'][key] = False
            self.assertFalse(survey.classify(bad)['ok'])
        for complete in (False, 1, None):
            bad = copy.deepcopy(row)
            bad['observer']['memoryReview']['complete'] = complete
            self.assertFalse(survey.classify(bad)['ok'])
        row['memoryReviewRequested'] = False
        self.assertFalse(survey.classify(row)['ok'])

    def test_memory_recorder_authority_is_bounded(self):
        check = survey.memory_recorder_request_allowed
        self.assertTrue(check(True, 'frame_evidence', dict(action='arm', capacity=262144)))
        self.assertTrue(check(True, 'frame_evidence', dict(action='freeze', session='1')))
        self.assertTrue(check(True, 'frame_evidence', dict(action='discard', session='1')))
        for payload in ({}, dict(action='arm', capacity=8192), dict(action='arm', capacity=262144.0),
                        dict(action='freeze', session=1), dict(action='freeze', session='0'),
                        dict(action='export', session='1'), dict(action='discard', session=str(1 << 64))):
            self.assertFalse(check(True, 'frame_evidence', payload))
        self.assertFalse(check(False, 'frame_evidence', dict(action='arm', capacity=262144)))

    def test_identity_requires_owned_exact_process(self):
        exe = Path(__file__).resolve()
        valid = launch_sample(exe)
        self.assertEqual(survey.launch_identity(valid, exe)["pid"], 123)
        for field, value in (("pid", True), ("creation100ns", 0), ("pid", 1 << 32),
                             ("jobAtomic", False), ("handleListApplied", False),
                             ("desktop", "Default"), ("actualExecutable", str(exe.parent / "wrong.exe"))):
            bad = copy.deepcopy(valid)
            bad[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                survey.launch_identity(bad, exe)

    def test_identity_requires_verified_restricted_resume(self):
        exe = Path(__file__).resolve()
        for field, value in (("childTokenVerifiedBeforeResume", False),
                             ("resumedAfterVerification", False), ("resumePreviousCount", 2)):
            bad = launch_sample(exe)
            bad["restrictedToken"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                survey.launch_identity(bad, exe)

    def test_pipe_reply_matches_nonce_command_and_server(self):
        request = dict(requestId="one", command="ping", pid=123)
        reply = dict(requestId="one", command="ping", ok=True, result=dict(pid=123))
        self.assertIs(survey.validate_reply(reply, request, 123), reply)
        for field, value in (("requestId", "old"), ("command", "other"), ("ok", 1),
                             ("result", []), ("result", {"pid": True}), ("result", {"pid": 124})):
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                survey.validate_reply(dict(reply, **{field: value}), request, 123)
        with self.assertRaises(ValueError):
            survey.validate_reply(reply, request, 124)

    def test_environment_does_not_inherit_experiments_or_secrets(self):
        env = survey.safe_environment(dict(SystemRoot="C:/Windows", PATH="C:/Windows/System32",
            DSH_TOKEN="secret", DXVK_WAR3_FRAME_EVIDENCE="1", WARVK_D3D9_HOST_EXE="wrong",
            VK_INSTANCE_LAYERS="bad", PYTHONPATH="bad"), Path("E:/instance"), Path("E:/artifact/logs"))
        for field in ("DSH_TOKEN", "WARVK_D3D9_HOST_EXE", "VK_INSTANCE_LAYERS", "PYTHONPATH"):
            self.assertNotIn(field, env)
        self.assertEqual(env["DXVK_WAR3_FRAME_EVIDENCE"], "0")
        self.assertEqual(env["DXVK_WAR3_FRAME_EVIDENCE_INPUTS"], "0")
        self.assertNotIn("DXVK_WAR3_INTERNAL_EXIT_TEST", env)
        self.assertFalse(any("CENSUS" in field for field in env))

    def test_timeout_and_force_never_become_pass(self):
        for field, value in (("timedOut", True), ("forcedTermination", True),
                             ("exitCode", 2), ("processSettled", False), ("jobEmpty", False)):
            row = receipt_sample()
            row["gameLaunch"][field] = value
            result = survey.classify(row)
            with self.subTest(field=field):
                self.assertTrue(result["observationAvailable"])
                self.assertFalse(result["normalExitProven"])
                self.assertFalse(result["cleanupPassed"])
                self.assertFalse(result["ok"])
                self.assertEqual(result["verdict"], "OBSERVATIONS_RETAINED_CLEANUP_FAILED")

    def test_even_natural_exit_does_not_certify_render_migration(self):
        result = survey.classify(receipt_sample())
        self.assertTrue(result["normalExitProven"])
        for field in ("ok", "fullGameCaptureProven", "x64GameRenderingProven", "gameMemorySavingsProven",
                      "performanceAcceptance", "visualAcceptance"):
            self.assertIs(result[field], False)

    def test_cleanup_or_preservation_failure_cannot_pass_cleanup(self):
        for field, value in (("observerJoined", False), ("protectedUnchanged", False),
                             ("cleanupErrors", ["unresolved handle"])):
            result = receipt_sample()
            result[field] = value
            self.assertFalse(survey.classify(result)["cleanupPassed"])
        result = receipt_sample()
        result["observer"]["cleanupErrors"] = ["pending IO"]
        self.assertFalse(survey.classify(result)["cleanupPassed"])

    def test_boolean_exit_code_is_not_native_zero(self):
        result = receipt_sample()
        result["gameLaunch"]["exitCode"] = False
        self.assertFalse(survey.classify(result)["normalExitProven"])

    def test_empty_missing_and_malformed_census_are_not_coverage(self):
        for census in ([], [dict(rows=[], errors=[])], [dict(rows=[{}], errors=["bad"]) ]):
            row = receipt_sample()
            row["census"] = census
            self.assertFalse(survey.classify(row)["observationAvailable"])
        for field in ("ready", "modulesVerified", "screenshotVerified"):
            row = receipt_sample()
            row["observer"][field] = False
            self.assertFalse(survey.classify(row)["observationAvailable"])

    def test_log_line_provenance_and_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "native.log"
            path.write_text("ordinary log\ninfo: " + survey.CENSUS_MARKER +
                json.dumps(sample_snapshot(draws=8)) + "\n" +
                survey.CENSUS_MARKER + '{"schema":"wrong"}\n' +
                survey.CENSUS_MARKER + '{"truncated":\n', encoding="utf-8")
            result = survey.parse_census(path)
            self.assertEqual(len(result["rows"]), 1)
            self.assertEqual(result["rows"][0]["line"], 2)
            self.assertEqual(result["rows"][0]["census"]["observedCalls"], 10)
            self.assertEqual([item["line"] for item in result["errors"]], [3, 4])

    def test_schema_only_or_inconsistent_record_is_not_usable(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "native.log"
            for record in (dict(schema=survey.CENSUS_SCHEMA, observedCalls=10),
                           sample_snapshot() | dict(observedCalls=999)):
                with self.subTest(record=record):
                    path.write_text(survey.CENSUS_MARKER + json.dumps(record) + "\n",
                                    encoding="utf-8")
                    result = survey.parse_census(path)
                    self.assertEqual(result["rows"], [])
                    self.assertTrue(result["errors"])

    def test_both_frozen_versions_are_strictly_readable(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "native.log"
            for record in (sample_snapshot(), sample_snapshot_v2(kinds=(1,1,1,1))):
                with self.subTest(schema=record["schema"]):
                    path.write_text(survey.CENSUS_MARKER + json.dumps(record) + "\n", encoding="utf-8")
                    result = survey.parse_census(path)
                    self.assertEqual(result["errors"], [])
                    self.assertEqual(result["rows"][0]["census"], record)
            old, new = sample_snapshot(), sample_snapshot_v2(sequence=2)
            path.write_text(survey.CENSUS_MARKER + json.dumps(old) + "\n" +
                            survey.CENSUS_MARKER + json.dumps(new) + "\n", encoding="utf-8")
            self.assertTrue(survey.parse_census(path)["errors"])

    def test_duplicate_sequence_and_truncated_marker_remain_visible(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "native.log"
            row = survey.CENSUS_MARKER + json.dumps(sample_snapshot()) + "\n"
            for content in (row + row, row + survey.CENSUS_MARKER.rstrip()):
                with self.subTest(content=content):
                    path.write_text(content, encoding="utf-8")
                    result = survey.parse_census(path)
                    self.assertTrue(result["rows"])
                    self.assertTrue(result["errors"])
                    receipt = receipt_sample()
                    receipt["census"] = [result]
                    self.assertFalse(survey.classify(receipt)["observationAvailable"])

    def test_overlong_log_line_is_visible(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "native.log"
            path.write_text("x" * (survey.MAX_LOG_LINE + 1), encoding="utf-8")
            self.assertTrue(survey.parse_census(path)["errors"])

    def test_paths_cannot_escape_or_equal_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(survey.within(root / "new", root), root.resolve() / "new")
            for path in (root, root / ".." / "outside"):
                with self.subTest(path=path), self.assertRaises(ValueError):
                    survey.within(path, root)

    def test_reparse_attributes_rejected_before_resolve(self):
        fake = types.SimpleNamespace(st_mode=stat_regular(), st_file_attributes=0x400)
        with mock.patch.object(Path, "lstat", return_value=fake), self.assertRaises(ValueError):
            survey.checked_path(Path(__file__).resolve())

    def test_module_matches_instance_not_just_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            expected = Path(tmp) / "d3d9.dll"
            expected.write_bytes(b"candidate")
            rows = [dict(name="d3d9.dll", path=str(expected))]
            result = survey.verify_modules(rows, {"d3d9.dll": expected})
            self.assertEqual(result["d3d9.dll"]["bytes"], 9)
            for bad in ([], rows + rows, [dict(name="d3d9.dll", path=str(expected.parent / "other" / "d3d9.dll"))]):
                with self.subTest(rows=bad), self.assertRaises(ValueError):
                    survey.verify_modules(bad, {"d3d9.dll": expected})

    def test_actual_bmp_dimensions_and_truncation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "frame.bmp"
            width, height = 4, 2
            size = 54 + width * 3 * height
            header = struct.pack("<2sIHHI", b"BM", size, 0, 0, 54)
            header += struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, size - 54, 0, 0, 0, 0)
            path.write_bytes(header + bytes(size - 54))
            self.assertEqual(survey.bmp_dimensions(path), [4, 2])
            path.write_bytes(header)
            with self.assertRaises(ValueError):
                survey.bmp_dimensions(path)

    def test_loadfile_modes_bind_the_same_deployed_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            instance = Path(tmp).resolve()
            target = instance / "Maps" / "AutoTest" / "test.w3m"
            target.parent.mkdir(parents=True)
            target.write_bytes(b"protected map copy")
            deployed = dict(target=str(target), sha256=survey.identity(target)["sha256"],
                            loadfileArg="Maps\\AutoTest\\test.w3m")
            relative = survey.loadfile_argument(instance, deployed, False)
            absolute = survey.loadfile_argument(instance, deployed, True)
            self.assertEqual((instance / relative.replace("\\", "/")).resolve(), Path(absolute))
            self.assertEqual(Path(absolute), target)
            self.assertEqual(relative, deployed["loadfileArg"])
            for field, value in (("sha256", "0" * 64), ("loadfileArg", "Maps\\elsewhere.w3m"),
                                 ("target", str(instance.parent / "outside.w3m"))):
                with self.subTest(field=field), self.assertRaises(ValueError):
                    survey.loadfile_argument(instance, dict(deployed, **{field: value}), True)

    def test_production_capture_is_bmp_and_shutdown_is_only_soft(self):
        source = (survey.ROOT / "src/d3d9/war3/tools/war3_frame_capture.cpp").read_text("utf-8")
        self.assertIn('if (ext != ".bmp")', source)
        command = (survey.ROOT / "src/d3d9/war3/tools/war3_internal_test_api.cpp").read_text("utf-8")
        body = command.split('state->request.command == "shutdown.session"', 1)[1].split("} else {", 1)[0]
        self.assertIn('response["softOnly"] = true', body)
        self.assertNotIn("ExitProcess", body)

    def test_instance_artifacts_include_production_crash_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            instance = Path(temporary)
            paths = [instance / "WarVK/Crash/fatal.json", instance / "WarVK/Crash/fatal.dmp",
                     instance / "WarVK/Log/session.log", instance / "Crash/legacy.json"]
            for path in paths:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"synthetic evidence")
            self.assertEqual(survey.instance_artifacts(instance),
                             [survey.identity(path) for path in (paths[2], paths[1], paths[0], paths[3])])
            self.assertEqual(survey.instance_artifacts(instance / "empty"), [])

    def test_source_uses_only_owned_launcher_and_no_input_registry(self):
        tree = ast.parse(Path(survey.__file__).read_text("utf-8"))
        calls = {node.func.attr if isinstance(node.func, ast.Attribute) else node.func.id
                 for node in ast.walk(tree) if isinstance(node, ast.Call) and
                 isinstance(node.func, (ast.Name, ast.Attribute))}
        forbidden = {"Popen", "CreateProcessW", "TerminateProcess", "TerminateJobObject", "PostMessageW",
                     "SendMessageW", "SwitchDesktop", "SetForegroundWindow", "SendInput", "keybd_event",
                     "mouse_event", "SetValueEx", "launch_war3_instance", "_set_process_priority_high"}
        self.assertFalse(calls & forbidden)
        source = Path(survey.__file__).read_text("utf-8")
        self.assertIn('restricted_medium=True', source)
        self.assertIn('timeout_s=args.timeout_seconds', source)
        self.assertIn('validate_probe_receipt(', source)
        self.assertIn('GetNamedPipeServerProcessId', source)
        self.assertNotIn('import war3_autotest_mcp', source)


def stat_regular():
    import stat
    return stat.S_IFREG | 0o644


class FakeKernel:
    def __init__(self, waits):
        self.waits = iter(waits)
        self.closed = []
        self.cancelled = False

    def CreateEventW(self, *args):
        return 100

    def ReadFile(self, *args):
        return False

    def WriteFile(self, *args):
        return False

    def WaitForSingleObject(self, *args):
        return next(self.waits)

    def CancelIoEx(self, *args):
        self.cancelled = True
        return True

    def GetOverlappedResult(self, handle, overlap, transferred, wait):
        transferred._obj.value = 3
        return True

    def CloseHandle(self, handle):
        self.closed.append(handle)
        return True


class FakeIoTests(unittest.TestCase):
    def observer(self, waits):
        obj = survey.NativeObserver.__new__(survey.NativeObserver)
        obj.k = FakeKernel(waits)
        obj.pending = []
        return obj

    def test_pending_io_completion_closes_event(self):
        obj = self.observer([0])
        with mock.patch.object(ctypes, "get_last_error", return_value=997, create=True):
            count, more = obj._transfer(5, ctypes.create_string_buffer(16), 16, False, survey.time.monotonic() + 2)
        self.assertEqual((count, more), (3, False))
        self.assertEqual(obj.k.closed, [100])
        self.assertFalse(obj.pending)

    def test_immediate_more_data_uses_completed_count_not_readfile_zero(self):
        obj = self.observer([])
        def read(handle, buffer, size, transferred, overlap):
            transferred._obj.value = 0
            overlap._obj.InternalHigh = size
            return False
        def completed(handle, overlap, transferred, wait):
            transferred._obj.value = overlap._obj.InternalHigh
            return False
        obj.k.ReadFile = mock.Mock(side_effect=read)
        obj.k.GetOverlappedResult = mock.Mock(side_effect=completed)
        with mock.patch.object(ctypes, "get_last_error", return_value=234, create=True):
            result = obj._transfer(5, ctypes.create_string_buffer(65536), 65536,
                                   False, survey.time.monotonic() + 2)
        self.assertEqual(result, (65536, True))
        obj.k.GetOverlappedResult.assert_called_once()
        self.assertEqual(obj.k.closed, [100])
        self.assertFalse(obj.pending)

    def test_pending_more_data_uses_completion_count(self):
        obj = self.observer([0])
        last_error = [997]
        def completed(handle, overlap, transferred, wait):
            transferred._obj.value = 65536
            last_error[0] = 234
            return False
        obj.k.GetOverlappedResult = mock.Mock(side_effect=completed)
        with mock.patch.object(ctypes, "get_last_error", side_effect=lambda: last_error[0], create=True):
            result = obj._transfer(5, ctypes.create_string_buffer(65536), 65536,
                                   False, survey.time.monotonic() + 2)
        self.assertEqual(result, (65536, True))
        obj.k.GetOverlappedResult.assert_called_once()
        self.assertEqual(obj.k.closed, [100])

    def test_more_data_rejects_zero_or_oversize_completion(self):
        for count, message in ((0, "no progress"), (17, "exceeds buffer")):
            obj = self.observer([])
            def completed(handle, overlap, transferred, wait):
                transferred._obj.value = count
                return False
            obj.k.GetOverlappedResult = mock.Mock(side_effect=completed)
            with self.subTest(count=count), \
                 mock.patch.object(ctypes, "get_last_error", return_value=234, create=True), \
                 self.assertRaisesRegex(ValueError, message):
                obj._transfer(5, ctypes.create_string_buffer(16), 16,
                              False, survey.time.monotonic() + 2)
            self.assertEqual(obj.k.closed, [100])
            self.assertFalse(obj.pending)

    def test_write_more_data_is_not_a_successful_completion(self):
        obj = self.observer([])
        obj.k.GetOverlappedResult = mock.Mock()
        with mock.patch.object(ctypes, "get_last_error", return_value=234, create=True), \
             self.assertRaisesRegex(ValueError, "IPC IO failure: 234"):
            obj._transfer(5, ctypes.create_string_buffer(16), 16,
                          True, survey.time.monotonic() + 2)
        obj.k.GetOverlappedResult.assert_not_called()
        self.assertEqual(obj.k.closed, [100])

    def test_timeout_drains_cancel_before_freeing_buffer(self):
        obj = self.observer([258, 0])
        with mock.patch.object(ctypes, "get_last_error", return_value=997, create=True), self.assertRaises(TimeoutError):
            obj._transfer(5, ctypes.create_string_buffer(16), 16, False, survey.time.monotonic())
        self.assertTrue(obj.k.cancelled)
        self.assertEqual(obj.k.closed, [100])
        self.assertFalse(obj.pending)

    def test_unsettled_cancel_retains_overlapped_and_buffer(self):
        obj = self.observer([258, 258])
        buffer = ctypes.create_string_buffer(16)
        with mock.patch.object(ctypes, "get_last_error", return_value=997, create=True), self.assertRaises(RuntimeError):
            obj._transfer(5, buffer, 16, False, survey.time.monotonic())
        self.assertEqual(obj.k.closed, [])
        self.assertEqual(obj.pending[0][:2], (5, 100))
        self.assertIs(obj.pending[0][3], buffer)

    def test_createfile_failure_retains_numeric_error_and_never_widens_access(self):
        for code in (2, 5, 87, 231):
            obj = survey.NativeObserver.__new__(survey.NativeObserver)
            obj.expected = dict(pid=123, creation100ns=456)
            obj.verify_process = lambda: None
            obj.k = types.SimpleNamespace(CreateFileW=mock.Mock(return_value=ctypes.c_void_p(-1).value))
            with self.subTest(code=code), mock.patch.object(ctypes, "get_last_error", return_value=code, create=True):
                with self.assertRaises(survey.PipeOpenError) as raised:
                    obj.request("ping", {})
                row = raised.exception.diagnostic
                self.assertEqual(row["win32Error"], code)
                self.assertEqual(row["expectedCreation100ns"], 456)
                self.assertEqual(row["expectedPid"], 123)
                self.assertEqual(row["pipeName"], r"\\.\pipe\War3ControlPlane_123")
                self.assertEqual(row["startupRetryAllowed"], code in (2, 231))
                obj.k.CreateFileW.assert_called_once_with(row["pipeName"], 0xC0000000, 0, None, 3, 0x40000000, None)

    def test_observer_handoff_uses_published_identity_and_internal_bmp_only(self):
        commands = []
        bindings = []
        class StopAfterOneSample:
            stopped = False
            def wait(self, seconds):
                if seconds == 1.0:
                    self.stopped = True
                return self.stopped
            def is_set(self):
                return self.stopped
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            instance = root / "instance"
            output = root / "artifacts"
            instance.mkdir()
            output.mkdir()
            for name in ("war3.exe", "Game.dll", "Storm.dll", "d3d9.dll"):
                (instance / name).write_bytes(b"test-only")
            launch = launch_sample(instance / "war3.exe")
            desktop = types.SimpleNamespace(receipt=dict(launches=[launch]))
            class FakeNative:
                pending = []
                def bind(self, expected):
                    bindings.append(expected)
                def modules(self):
                    return [dict(name=name, path=str(instance / name)) for name in ("war3.exe", "Game.dll", "Storm.dll", "d3d9.dll")]
                def request(self, command, payload):
                    commands.append(command)
                    result = dict(pid=launch["pid"])
                    if command == "wait_until":
                        result = dict(ready=True)
                    if command == "capture_final_frame":
                        path = Path(payload["outputPath"])
                        self_path = output / "game-frame.bmp"
                        if path != self_path:
                            raise AssertionError("incorrect capture path")
                        size = 54 + 24
                        header = struct.pack("<2sIHHI", b"BM", size, 0, 0, 54)
                        header += struct.pack("<IiiHHIIiiII", 40, 4, 2, 1, 24, 0, 24, 0, 0, 0, 0)
                        path.write_bytes(header + bytes(24))
                        result = dict(outputPath=str(path), width=4, height=2)
                    return dict(response=dict(ok=True, result=result), request=dict(command=command))
                def memory(self):
                    return dict(privateBytes=100, workingSetBytes=90, peakWorkingSetBytes=110)
                def close(self):
                    return []
            receipt = {}
            with mock.patch.object(survey, "NativeObserver", FakeNative):
                survey.observe(desktop, 0, instance / "war3.exe", instance, output, StopAfterOneSample(), receipt)
            self.assertEqual(commands, ["ping", "get_runtime_status", "wait_until", "capture_final_frame"])
            self.assertEqual(bindings[0]["creation100ns"], launch["creation100ns"])
            self.assertEqual(receipt["samples"], 1, receipt)
            self.assertTrue(receipt["ready"] and receipt["modulesVerified"] and receipt["screenshotVerified"])
            self.assertEqual(receipt["actualResolution"], [4, 2])
            self.assertTrue(receipt["finished"])
            self.assertEqual(len((output / "ipc.jsonl").read_text("utf-8").splitlines()), 6)

    def test_pipe_failure_does_not_hide_initial_or_periodic_memory_and_modules(self):
        for error_code in (2, 5):
            commands = []
            class StopAfterTwoLoops:
                loops = 0
                def wait(self, seconds):
                    if seconds == 1.0:
                        self.loops += 1
                    return self.is_set()
                def is_set(self):
                    return self.loops >= 2
            with self.subTest(error_code=error_code), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp).resolve()
                instance, output = root / "instance", root / "artifacts"
                instance.mkdir()
                output.mkdir()
                for name in ("war3.exe", "Game.dll", "Storm.dll", "d3d9.dll"):
                    (instance / name).write_bytes(b"fake")
                launch = launch_sample(instance / "war3.exe")
                desktop = types.SimpleNamespace(receipt=dict(launches=[launch]))
                class NoPipeNative:
                    pending = []
                    def bind(self, expected):
                        self.expected = expected
                    def modules(self):
                        return [dict(name=name, path=str(instance / name)) for name in ("war3.exe", "Game.dll", "Storm.dll", "d3d9.dll")]
                    def memory(self):
                        return dict(privateBytes=100, workingSetBytes=90, peakWorkingSetBytes=110)
                    def request(self, command, payload):
                        commands.append(command)
                        raise survey.PipeOpenError(error_code, r"\\.\pipe\War3ControlPlane_123", self.expected)
                    def close(self):
                        return []
                receipt = {}
                with mock.patch.object(survey, "NativeObserver", NoPipeNative), \
                     mock.patch.object(survey.time, "monotonic", side_effect=iter(range(100))):
                    survey.observe(desktop, 0, instance / "war3.exe", instance, output, StopAfterTwoLoops(), receipt)
                self.assertTrue(receipt["modulesVerified"])
                self.assertEqual(receipt["samples"], 2)
                self.assertEqual(receipt["firstMemorySample"]["phase"], "initial")
                self.assertEqual(receipt["lastMemorySample"]["phase"], "periodic")
                self.assertFalse(receipt["memorySavingsProven"] or receipt["ready"] or receipt["screenshotVerified"])
                self.assertEqual(receipt["lastPipeOpenFailure"]["win32Error"], error_code)
                self.assertEqual(receipt["ipcDisabled"], error_code == 5)
                self.assertEqual(commands, ["ping"] * (2 if error_code == 2 else 1))
                records = [json.loads(line) for line in (output / "ipc.jsonl").read_text("utf-8").splitlines()]
                self.assertIn("memory", records[0])
                self.assertIn("moduleIdentity", records[1])


if __name__ == "__main__":
    unittest.main()
