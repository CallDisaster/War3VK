"""Offline reader and transaction-shape regressions; never launches the game."""
from pathlib import Path
import json
import os
import struct
import sys
import tempfile
import inspect
import unittest
from types import SimpleNamespace
from unittest.mock import patch
import run_snapshot_pressure_canary as runner


def _cp_result(status):
    return {"transportOk": True, "ok": True, "result": status}


class Contract(unittest.TestCase):
    def test_actual_launch_signature(self):
        args = runner.launch_arguments('offline', {})
        inspect.signature(runner.war3.launch_war3_test).bind(**args)
        self.assertEqual(args['use_isolated_desktop'], True)
        self.assertEqual(args['deploy_d3d9_before_launch'], False)
    def parse(self, value):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'report.html'
            path.write_text(value, encoding='utf-8')
            return runner.read_report(path)

    def test_strict_root(self):
        self.assertEqual(self.parse('const data = {"x":1};'), {'x': 1})
        for text in ('const data={"x":1,"x":1};', 'const data={"x":{"y":1,"y":2}};',
                     'const data={"x":NaN};', 'const data={};const data={};',
                     'const data={} trailing'):
            with self.assertRaises((ValueError, RuntimeError)):
                self.parse(text)

    def test_copy_new(self):
        with tempfile.TemporaryDirectory() as folder:
            a, b = Path(folder)/'a', Path(folder)/'b'
            a.write_bytes(b'original')
            runner.copy_new(a, b)
            self.assertEqual(runner.identity(a), runner.identity(b))
            with self.assertRaises(FileExistsError):
                runner.copy_new(a, b)

    def test_close_rejects_inexact_witness_before_window_api(self):
        with self.assertRaisesRegex(RuntimeError, 'witness mismatch'):
            runner.close_owned_window({}, None)

    def test_close_rejects_foreground_before_window_api(self):
        pin = dict(pid=1, creationEpochMs=1, canonicalExePath='test', available=True)
        owner = SimpleNamespace(snapshot=lambda: pin)
        with self.assertRaisesRegex(RuntimeError, 'noninteractive'):
            runner.close_owned_window(dict(nativeProcessWitness=pin,
                useIsolatedDesktop=True, desktop=dict(nonInteractiveOnly=True, name='Default')), owner)

    def test_close_rejects_unowned_state_before_window_api(self):
        pin = dict(pid=1, creationEpochMs=1, canonicalExePath='test', available=True)
        owner = SimpleNamespace(snapshot=lambda: pin)
        with self.assertRaisesRegex(RuntimeError, 'owner changed'):
            runner.close_owned_window(dict(nativeProcessWitness=pin,
                useIsolatedDesktop=True, desktop=dict(nonInteractiveOnly=True, name='offline')), owner)

    def test_safety_shape_not_runtime_proof(self):
        text = Path(runner.__file__).read_text(encoding='utf-8')
        for forbidden in ('sys.path.insert', 'SwitchDesktop', 'force=True', 'allowControlPlaneSemanticDrain',
                          'SendInput', '_run_war3_input_plan', 'subprocess.Popen'):
            self.assertNotIn(forbidden, text)
        self.assertIn("use_isolated_desktop=True", text)
        self.assertIn("baseline_width=2560, baseline_height=1440", text)
        self.assertIn("DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB", text)
        self.assertIn("frozen manifest cap must be 384", text)
        self.assertIn("mergeAuthorizedByResults=False", text)
        self.assertIn("imageRecoveryProven=False", text)
        self.assertNotIn("shutil.copyfile(out/'candidate.dll', paths['live'])", text)
        self.assertIn("move_new(live, parked)", text)
        self.assertIn("restore_live_from_backup", text)
        self.assertIn("backup_identity = identity(backup)", text)
        self.assertIn("require(backup_identity == before['live']", text)
        self.assertIn("require(current == before['candidate']", text)
        self.assertIn("auto_continue_loading", text)
        self.assertIn("continue_max_pulses", text)
        self.assertIn("'continue_key': 'SPACE'", text)
        self.assertIn("'timeout_sec': 100", text)
        self.assertIn("previous=previous, owner=owner", text)
        self.assertEqual(text.count("war3.wait_for_game_ready("), 1)
        self.assertIn("launch/noninteractive desktop", text)
        self.assertEqual(text.count("HEAVY_FORENSICS_OFF = ("), 1)
        self.assertIn("--frozen-manifest", text)
        self.assertIn("build_binding_manifest", text)
        self.assertIn("readiness_diagnostic", text)
        self.assertIn("_control_plane_request", text)
        self.assertNotIn("war3._read_runtime_status_best_effort", text)


class ReadinessPreflight(unittest.TestCase):
    @staticmethod
    def menu_ready_status():
        return {
            "module": {"state": "Running"},
            "runtime": {"jassReady": True, "gameStarted": False, "runtimeReady": False},
            "render": {"worldPtr": 0, "isInGame": False, "isLoading": False,
                       "inGameRenderReady": False},
            "frame": {"frameNumber": 1534},
            "frameIndex": 270842,
            "timestampMs": 1789818391131,
            "perf": {"enabled": True, "recording": True, "frameAnchorValid": True,
                      "businessFrameSerial": 4242, "perfFrameEpoch": 777,
                      "producerAccumulationEpoch": 7},
        }

    def test_menu_ready_not_started_is_classified(self):
        result = runner.classify_readiness(self.menu_ready_status())
        self.assertEqual(result["state"], "menu-ready-not-started")
        self.assertEqual(result["worldPtr"], 0)
        self.assertEqual(result["frameNumber"], 1534)
        self.assertTrue(result["statusComplete"])

    def test_missing_critical_runtime_fields_are_not_ready(self):
        status = self.menu_ready_status()
        del status["runtime"]
        result = runner.classify_readiness(status)
        self.assertEqual(result["state"], "status-incomplete")
        self.assertFalse(result["statusComplete"])
        self.assertIn("runtime", result["missingFields"])

    def test_in_game_ready_is_not_mislabeled_as_menu(self):
        status = self.menu_ready_status()
        status["runtime"] = {"jassReady": True, "gameStarted": True, "runtimeReady": True}
        status["render"] = {"worldPtr": 0x1234, "isInGame": True, "isLoading": False,
                            "inGameRenderReady": True}
        result = runner.classify_readiness(status)
        self.assertEqual(result["state"], "in-game-ready")

    def test_phase_marker_stores_nested_readiness_and_run_domain(self):
        with patch.object(runner.war3, "_control_plane_request",
                          return_value=_cp_result(self.menu_ready_status())):
            marker = runner.phase_marker(18904, "sample-start", "frozen-run")
        self.assertEqual(marker["phase"], "sample-start")
        self.assertEqual(marker["pid"], 18904)
        self.assertEqual(marker["runId"], "frozen-run")
        self.assertEqual(marker["readiness"]["state"], "menu-ready-not-started")
        self.assertEqual(marker["readiness"]["frameNumber"], 1534)
        self.assertNotIn("module", marker)
        self.assertEqual(marker["frameDomain"], "workload.businessFrameSerial")
        self.assertEqual(marker["businessFrameSerial"], 4242)
        self.assertEqual(marker["perfFrameEpoch"], 777)
        self.assertEqual(marker["producerAccumulationEpoch"], 7)

    def test_phase_marker_rejects_missing_or_false_perf(self):
        base = self.menu_ready_status()
        cases = []
        missing = json.loads(json.dumps(base)); missing.pop("perf"); cases.append(missing)
        false_enabled = json.loads(json.dumps(base)); false_enabled["perf"]["enabled"] = False; cases.append(false_enabled)
        false_recording = json.loads(json.dumps(base)); false_recording["perf"]["recording"] = False; cases.append(false_recording)
        false_valid = json.loads(json.dumps(base)); false_valid["perf"]["frameAnchorValid"] = False; cases.append(false_valid)
        bool_serial = json.loads(json.dumps(base)); bool_serial["perf"]["businessFrameSerial"] = True; cases.append(bool_serial)
        zero_epoch = json.loads(json.dumps(base)); zero_epoch["perf"]["producerAccumulationEpoch"] = 0; cases.append(zero_epoch)
        for status in cases:
            with patch.object(runner.war3, "_control_plane_request", return_value=_cp_result(status)):
                with self.assertRaises(RuntimeError):
                    runner.phase_marker(18904, "sample-start", "frozen-run")

    def test_ready_wait_kwargs_only_allows_pulse_on_owned_isolated_desktop(self):
        isolated = runner.ready_wait_kwargs({
            "useIsolatedDesktop": True,
            "desktop": {"nonInteractiveOnly": True},
        })
        self.assertTrue(isolated["auto_continue_loading"])
        self.assertFalse(isolated["allow_fallback"])
        self.assertEqual(isolated["timeout_sec"], 100)
        self.assertEqual(isolated["continue_max_pulses"], 1)
        self.assertTrue(isolated["require_control_plane_for_continue"])
        self.assertIn("require_control_plane_for_continue",
                      inspect.signature(runner.war3.wait_for_game_ready).parameters)
        for launch in (
            {"useIsolatedDesktop": False, "desktop": {"nonInteractiveOnly": True}},
            {"useIsolatedDesktop": True, "desktop": {"nonInteractiveOnly": False}},
            {"useIsolatedDesktop": True},
        ):
            kwargs = runner.ready_wait_kwargs(launch)
            self.assertFalse(kwargs["auto_continue_loading"])

    def test_continue_pulse_failure_is_visible(self):
        summary = runner.continue_pulse_summary({
            "continuePulses": [
                {"ok": True},
                {"ok": False, "error": "post failed"},
            ]
        })
        self.assertEqual(summary["count"], 2)
        self.assertEqual(summary["failed"], 1)
        self.assertTrue(summary["failures"])
        missing = runner.continue_pulse_summary({})
        self.assertEqual(missing["coverage"], "continuePulses missing")


class PhaseAcquisitionTests(unittest.TestCase):
    def previous(self, serial=10, epoch=20, accumulation=7):
        return {"businessFrameSerial": serial, "perfFrameEpoch": epoch,
                "producerAccumulationEpoch": accumulation}

    def status(self, serial, epoch, accumulation=7):
        return {"module": {"state": "Running"},
                "runtime": {"jassReady": True, "gameStarted": True, "runtimeReady": True},
                "render": {"worldPtr": 0x1000, "isInGame": True, "isLoading": False,
                           "inGameRenderReady": True},
                "frame": {"frameNumber": 999},
                "frameIndex": 100,
                "timestampMs": 1000,
                "perf": {"enabled": True, "recording": True, "frameAnchorValid": True,
                         "businessFrameSerial": serial, "perfFrameEpoch": epoch,
                         "producerAccumulationEpoch": accumulation}}

    def initial_status(self, serial=0, epoch=0, accumulation=7):
        status = self.status(serial, epoch, accumulation)
        status["perf"]["frameAnchorValid"] = False
        return status

    def test_unchanged_then_advance_succeeds_with_fake_clock(self):
        sequence = [self.status(10, 20), self.status(10, 20),
                    self.status(20, 21)]
        calls = {"n": 0}
        def cp(*args, **kwargs):
            value = sequence[min(calls["n"], len(sequence) - 1)]
            calls["n"] += 1
            return _cp_result(value)
        fake_time = _FakeTime()
        with patch.object(runner.war3, "_control_plane_request", side_effect=cp),              patch.object(runner, "time", fake_time):
            marker = runner.phase_marker(9876, "pressure-start", "run",
                                         previous=self.previous(), owner=_FakeOwner())
        self.assertEqual(marker["businessFrameSerial"], 20)
        self.assertEqual(marker["perfFrameEpoch"], 21)
        self.assertGreater(calls["n"], 2)

    def test_permanent_unchanged_times_out(self):
        fake_time = _FakeTime()
        with patch.object(runner.war3, "_control_plane_request",
                          return_value=_cp_result(self.status(10, 20))),              patch.object(runner, "time", fake_time):
            with self.assertRaisesRegex(RuntimeError, "timeout"):
                runner.phase_marker(9876, "pressure-start", "run",
                                    previous=self.previous(), owner=_FakeOwner())

    def test_backward_or_epoch_reset_fails_immediately(self):
        for status in (self.status(9, 20), self.status(10, 19),
                       self.status(11, 21, accumulation=8)):
            with patch.object(runner.war3, "_control_plane_request",
                              return_value=_cp_result(status)),                  patch.object(runner, "time", _FakeTime()):
                with self.assertRaisesRegex(RuntimeError, "backward|epoch"):
                    runner.phase_marker(9876, "pressure-start", "run",
                                        previous=self.previous(), owner=_FakeOwner())

    def test_initial_pending_then_completed_frame_succeeds(self):
        sequence = [self.initial_status(), self.initial_status(), self.status(10, 20)]
        calls = {"n": 0}

        def cp(*args, **kwargs):
            value = sequence[min(calls["n"], len(sequence) - 1)]
            calls["n"] += 1
            return _cp_result(value)

        fake_time = _FakeTime()
        with patch.object(runner.war3, "_control_plane_request", side_effect=cp), \
                patch.object(runner, "time", fake_time):
            marker = runner.phase_marker(9876, "sample-start", "run",
                                         previous=None, owner=_FakeOwner())
        self.assertEqual(marker["businessFrameSerial"], 10)
        self.assertEqual(marker["perfFrameEpoch"], 20)
        self.assertEqual(marker["initialAnchorWait"]["attempts"], 2)
        self.assertEqual(
            marker["initialAnchorWait"]["pinnedProducerAccumulationEpoch"], 7)

    def test_initial_pending_times_out_with_bounded_receipt(self):
        fake_time = _FakeTime()
        with patch.object(runner.war3, "_control_plane_request",
                          return_value=_cp_result(self.initial_status())), \
                patch.object(runner, "time", fake_time):
            with self.assertRaisesRegex(
                    RuntimeError,
                    "initial completed frame.*attempts=.*lastPerf"):
                runner.phase_marker(9876, "sample-start", "run",
                                    previous=None, owner=_FakeOwner())
        self.assertAlmostEqual(fake_time.now - 1000.0, 5.0, places=6)

    def test_initial_pending_changed_accumulation_epoch_fails(self):
        sequence = [self.initial_status(accumulation=7),
                    self.initial_status(accumulation=8)]
        calls = {"n": 0}

        def cp(*args, **kwargs):
            value = sequence[min(calls["n"], len(sequence) - 1)]
            calls["n"] += 1
            return _cp_result(value)

        with patch.object(runner.war3, "_control_plane_request", side_effect=cp), \
                patch.object(runner, "time", _FakeTime()):
            with self.assertRaisesRegex(RuntimeError,
                                        "initial accumulation epoch changed"):
                runner.phase_marker(9876, "sample-start", "run",
                                    previous=None, owner=_FakeOwner())

    def test_initial_malformed_or_disabled_fails_immediately(self):
        cases = []
        base = self.initial_status()
        cases.append(("missing_perf", json.loads(json.dumps(base))))
        cases[0][1].pop("perf")
        case = json.loads(json.dumps(base))
        case["perf"]["enabled"] = False
        cases.append(("disabled", case))
        case = json.loads(json.dumps(base))
        case["perf"]["recording"] = False
        cases.append(("recording_off", case))
        case = json.loads(json.dumps(base))
        case["perf"]["frameAnchorValid"] = "false"
        cases.append(("bad_type", case))
        case = json.loads(json.dumps(base))
        case["perf"]["businessFrameSerial"] = 5
        cases.append(("false_nonzero", case))
        case = json.loads(json.dumps(base))
        case["perf"]["businessFrameSerial"] = True
        cases.append(("bool_serial", case))
        case = json.loads(json.dumps(base))
        case["perf"]["producerAccumulationEpoch"] = 0
        cases.append(("zero_accumulation", case))
        for label, status in cases:
            fake_time = _FakeTime()
            with self.subTest(label=label), \
                    patch.object(runner.war3, "_control_plane_request",
                                 return_value=_cp_result(status)), \
                    patch.object(runner, "time", fake_time):
                with self.assertRaises(RuntimeError):
                    runner.phase_marker(9876, "sample-start", "run",
                                        previous=None, owner=_FakeOwner())
            self.assertEqual(fake_time.now, 1000.0)

    def test_initial_owner_exit_fails(self):
        owner = _FakeOwner()
        owner.exit = True
        with patch.object(runner.war3, "_control_plane_request",
                          return_value=_cp_result(self.initial_status())), \
                patch.object(runner, "time", _FakeTime()):
            with self.assertRaisesRegex(RuntimeError, "owner exited"):
                runner.phase_marker(9876, "sample-start", "run",
                                    previous=None, owner=owner)

    def test_initial_shape_is_not_accepted_for_subsequent_phase(self):
        status = self.initial_status()
        with patch.object(runner.war3, "_control_plane_request",
                          return_value=_cp_result(status)), \
                patch.object(runner, "time", _FakeTime()):
            with self.assertRaisesRegex(RuntimeError, "frameAnchorValid"):
                runner.phase_marker(9876, "pressure-start", "run",
                                    previous=self.previous(), owner=_FakeOwner())

    def test_transport_failure_never_uses_stale_file(self):
        stale = self.status(99, 99)
        def stale_reader(*args, **kwargs):
            calls["stale"] += 1
            return stale
        calls = {"stale": 0}
        with patch.object(runner.war3, "_control_plane_request",
                          return_value={"transportOk": False, "ok": False,
                                        "error": "pipe unavailable"}),              patch.object(runner.war3, "_read_runtime_status_best_effort",
                          side_effect=stale_reader):
            with self.assertRaisesRegex(RuntimeError, "transport"):
                runner.phase_marker(9876, "sample-start", "run", owner=_FakeOwner())
        self.assertEqual(calls["stale"], 0)


class InstallTransaction(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.src, self.live, self.stage, self.park = [root/x for x in ('source', 'live', 'stage', 'park')]
        self.src.write_bytes(b'candidate')
        self.live.write_bytes(b'original')
        self.old, self.new = runner.identity(self.live), runner.identity(self.src)

    def install(self):
        return runner.install_guarded(self.src, self.live, self.old, self.new, self.stage, self.park)

    def test_success_preserves_original_and_source(self):
        self.install()
        self.assertEqual(runner.identity(self.live), self.new)
        self.assertEqual(runner.identity(self.src), self.new)
        self.assertEqual(runner.identity(self.park), self.old)
        self.assertFalse(self.stage.exists())

    def test_partial_stage_does_not_touch_live(self):
        def partial(src, dst):
            dst.write_bytes(b'part')
            raise OSError('injected disk-full')
        with patch.object(runner, 'copy_new', side_effect=partial):
            with self.assertRaises(OSError):
                self.install()
        self.assertEqual(runner.identity(self.live), self.old)
        self.assertFalse(self.park.exists())
        self.assertEqual(self.stage.read_bytes(), b'part')

    def test_live_changed_during_stage_is_not_overwritten(self):
        real_copy = runner.copy_new
        def changed(src, dst):
            real_copy(src, dst)
            self.live.write_bytes(b'external')
        with patch.object(runner, 'copy_new', side_effect=changed):
            with self.assertRaises(RuntimeError):
                self.install()
        self.assertEqual(self.live.read_bytes(), b'external')
        self.assertFalse(self.park.exists())

    def test_failed_publish_rolls_original_back(self):
        real_move = runner.move_new
        def fail(src, dst):
            if src == self.stage:
                raise OSError('injected rename failure')
            real_move(src, dst)
        with patch.object(runner, 'move_new', side_effect=fail):
            with self.assertRaises(OSError):
                self.install()
        self.assertEqual(runner.identity(self.live), self.old)
        self.assertEqual(runner.identity(self.stage), self.new)
        self.assertFalse(self.park.exists())

    def test_external_file_after_park_is_not_overwritten(self):
        real_move = runner.move_new
        def conflict(src, dst):
            if src == self.stage:
                self.live.write_bytes(b'external')
            real_move(src, dst)
        with patch.object(runner, 'move_new', side_effect=conflict):
            with self.assertRaises(RuntimeError):
                self.install()
        self.assertEqual(self.live.read_bytes(), b'external')
        self.assertEqual(runner.identity(self.park), self.old)
        self.assertEqual(runner.identity(self.stage), self.new)

    def test_existing_transaction_target_stops_before_mutation(self):
        self.park.write_bytes(b'existing')
        with self.assertRaises(RuntimeError):
            self.install()
        self.assertEqual(runner.identity(self.live), self.old)
        self.assertEqual(self.park.read_bytes(), b'existing')
        self.assertFalse(self.stage.exists())


def _run_env():
    env = {key: "0" for key in runner.HEAVY_FORENSICS_OFF}
    env.update({
        "DXVK_WAR3_INTERNAL_TEST_API": "1",
        "DXVK_WAR3_INTERNAL_EXIT_TEST": "1",
        "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE": "1",
        "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE": "1",
        "DXVK_WAR3_PERF_MONITOR": "1",
        "DXVK_WAR3_PERF_LEVEL": "1",
        "DXVK_WAR3_ASYNC_SCREENSHOT": "1",
        "DXVK_WAR3_PERF_AUTO_EXPORT_SEC": "20",
        "DXVK_WAR3_PERF_HISTORY_FRAMES": "4000",
        "DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB": "384",
        "DXVK_WAR3_PROFILE": "full_default",
        "DISABLE_VULKAN_OBS_CAPTURE": "1",
        "DXVK_WAR3_STAGE11_BUDGET_CENSUS": "1",
        "DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE": "0",
    })
    return env


class _FakeOwner:
    def __init__(self, pid=9876):
        self.pid = pid
        self.exit = False

    def poll(self):
        return 0 if self.exit else None

    def snapshot_modules(self):
        return [{"name": "war3.exe", "path": "war3.exe"}]

    def snapshot(self):
        return {"available": True, "pid": self.pid, "creationEpochMs": 1,
                "canonicalExePath": "war3.exe"}


class _FakeTime:
    def __init__(self):
        self.now = 1000.0

    def time(self):
        return self.now

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += max(0.0, float(seconds))


class _FakeWar3:
    def __init__(self, owner, site, candidate_sha, candidate_size, run_id):
        self.__file__ = str(site / "war3_autotest_mcp_fake.py")
        self.STATE = SimpleNamespace(retained_native_process=owner, war3_pid=owner.pid)
        self.owner = owner
        self.site = site
        self.candidate_sha = candidate_sha
        self.candidate_size = candidate_size
        self.run_id = run_id
        self.wait_ok = True
        self.graceful_exit = True
        self.fail_camera_apply_at = 0
        self.camera_apply_calls = 0
        self.current_env = {}
        self.calls = []
        self.perf_serial = 0
        self.perf_frame_epoch = 1000
        self.producer_accumulation_epoch = 7
        self.pre_ready_without_perf = False
        self.freeze_after = None
        self.control_plane_calls = 0
        self.last_cp_status = None
        self.desktop_noninteractive = True
        self.wait_raises = False
        self.ready_returned = False
        self.initial_anchor_pending_after_ready = 0
        self.phase_initial_pending_served = 0

    def _prefer_inplace_relative_loadfile_arg(self, site, map_path):
        return "Maps\\test.w3x"

    def _read_runtime_status_best_effort(self, pid):
        self.perf_serial += 100
        self.perf_frame_epoch += 1
        return {
            "module": {"state": "Running"},
            "runtime": {"jassReady": True, "gameStarted": True, "runtimeReady": True},
            "render": {"worldPtr": 0x1000, "isInGame": True, "isLoading": False,
                       "inGameRenderReady": True},
            "frame": {"frameNumber": 42},
            "frameIndex": 100,
            "timestampMs": 1000,
            "perf": {"enabled": True, "recording": True, "frameAnchorValid": True,
                     "businessFrameSerial": self.perf_serial,
                     "perfFrameEpoch": self.perf_frame_epoch,
                     "producerAccumulationEpoch": self.producer_accumulation_epoch},
        }

    def launch_war3_test(self, **kwargs):
        self.current_env = json.loads(kwargs["env_overrides_json"])
        self.calls.append(("launch", dict(kwargs)))
        return {
            "ok": True,
            "useIsolatedDesktop": True,
            "desktop": {"name": "offline-desktop",
                        "nonInteractiveOnly": self.desktop_noninteractive},
            "pid": self.owner.pid,
            "envOverrides": dict(self.current_env),
            "effectiveWar3Environment": {
                key: value for key, value in self.current_env.items()
                if key.startswith("DXVK_WAR3_")
            },
        }

    def _write_report(self):
        logs = self.site / "WarVK/Log"
        logs.mkdir(parents=True, exist_ok=True)
        report = logs / ("war3_perf_report_" + self.run_id + ".html")
        data = {
            "meta": {"dllSha256": self.candidate_sha,
                     "dllFileSize": self.candidate_size,
                     "runtimeProfile": "full_default",
                     "env": dict(self.current_env)},
            "frameCount": 1,
            "shadowBudgetSummary": {},
        }
        report.write_text("const data = " + json.dumps(data) + ";", encoding="utf-8")

    def _control_plane_request(self, pid, command, payload=None, timeout_sec=6.0):
        self.control_plane_calls += 1
        if command != "get_runtime_status":
            return {"transportOk": True, "ok": True, "result": {}}
        if self.control_plane_calls == 1 and self.pre_ready_without_perf:
            status = self._read_runtime_status_best_effort(pid)
            status.pop("perf", None)
            self.last_cp_status = status
            return {"transportOk": True, "ok": True, "result": status}
        if (command == "get_runtime_status"
                and self.initial_anchor_pending_after_ready
                and self.ready_returned
                and self.phase_initial_pending_served
                    < self.initial_anchor_pending_after_ready):
            self.phase_initial_pending_served += 1
            status = self._read_runtime_status_best_effort(pid)
            status["perf"]["frameAnchorValid"] = False
            status["perf"]["businessFrameSerial"] = 0
            status["perf"]["perfFrameEpoch"] = 0
            self.last_cp_status = status
            return {"transportOk": True, "ok": True, "result": status}
        if self.freeze_after is not None and self.control_plane_calls > self.freeze_after:
            return {"transportOk": True, "ok": True, "result": self.last_cp_status}
        status = self._read_runtime_status_best_effort(pid)
        self.last_cp_status = status
        return {"transportOk": True, "ok": True, "result": status}

    def wait_for_game_ready(self, pid, **kwargs):
        self.calls.append(("wait_for_game_ready", dict(kwargs)))
        if self.wait_raises:
            raise RuntimeError('ready helper failed')
        if self.wait_ok:
            self._write_report()
            self.ready_returned = True
            return {"ok": True, "runtimeStatus": self._read_runtime_status_best_effort(pid),
                    "continuePulses": []}
        return {"ok": False, "runtimeStatus": self._read_runtime_status_best_effort(pid),
                "continuePulses": [{"ok": False, "error": "pulse failed"}],
                "error": "timeout"}

    def _invoke_internal_test_request(self, pid, site, cmd, payload, timeout_sec=6):
        self.calls.append(("command", cmd, dict(payload)))
        if cmd == "camera.snapshot":
            return {"ok": True, "result": {"targetX": 0, "targetY": 0, "rotation": 0,
                                            "angleOfAttack": 335, "targetDistance": 100}}
        if cmd == "camera.apply":
            self.camera_apply_calls += 1
            if self.fail_camera_apply_at and self.camera_apply_calls == self.fail_camera_apply_at:
                return {"ok": False, "error": "camera apply failed"}
            return {"ok": True, "result": {}}
        if cmd == "game.end_for_exit_test":
            if self.graceful_exit:
                self.owner.exit = True
            return {"ok": True, "result": {"stockEndGameInvoked": True,
                                            "processExitProven": False}}
        return {"ok": True, "result": {}}

    def _request_internal_frame_capture(self, pid, path, site, timeout_sec=10):
        self.calls.append(("capture", str(path)))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"BM" + b"\x00" * 16 + struct.pack("<i", 2560)
                         + struct.pack("<i", 1440) + b"\x00" * 28)
        return {"ok": True}

    def stop_war3(self, pid, force=False, avoid_foreground_switch=True):
        self.calls.append(("stop_war3", pid, force, avoid_foreground_switch))
        return {"ok": True}


class MainOrchestrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.site = self.root / "site"
        self.map_path = self.site / "Maps/test.w3x"
        self.protected = self.root / "protected.dll"
        self.candidate = self.root / "build32/src/d3d9/d3d9.dll"
        self.live = self.site / "d3d9.dll"
        self.game = self.site / "Game.dll"
        self.exe = self.site / "war3.exe"
        self.logs = self.site / "WarVK/Log"
        self.logs.mkdir(parents=True, exist_ok=True)
        (self.root / "AutoTest/artifacts").mkdir(parents=True, exist_ok=True)
        self.candidate.parent.mkdir(parents=True, exist_ok=True)
        self.map_path.parent.mkdir(parents=True, exist_ok=True)
        self.candidate.write_bytes(b"candidate")
        self.live.write_bytes(b"live")
        self.game.write_bytes(b"game")
        self.exe.write_bytes(b"\x00" * 0x100)
        self.map_path.write_bytes(b"map")
        self.protected.write_bytes(b"protected")
        (self.site / "war3_autotest_mcp_fake.py").write_bytes(b"fake module")
        self.identities = {
            "candidate": runner.identity(self.candidate),
            "liveRecovery": runner.identity(self.live),
            "map": runner.identity(self.map_path),
            "game": runner.identity(self.game),
            "exe": runner.identity(self.exe),
        }
        self.run_id = "b04-main-offline"
        env = _run_env()
        env["DXVK_WAR3_SCENARIO"] = self.run_id
        self.manifest = {
            "schema": 2,
            "runId": self.run_id,
            "scenario": self.run_id,
            "profile": "full_default",
            "matrix": "night-pressure-off-on-v2",
            "resolution": {"width": 2560, "height": 1440},
            "route": runner.FROZEN_ROUTE,
            "isolatedDesktop": True,
            "globalInputUsed": False,
            "candidate": self.identities["candidate"],
            "liveRecovery": self.identities["liveRecovery"],
            "map": self.identities["map"],
            "game": self.identities["game"],
            "exe": self.identities["exe"],
            "env": env,
        }
        self.manifest_path = self.root / "source-freeze-0338.json"
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")
        self.owner = _FakeOwner()
        self.fake = _FakeWar3(self.owner, self.site,
                              self.identities["candidate"]["sha256"],
                              self.identities["candidate"]["size"], self.run_id)

    def _run(self, install_guarded=None):
        argv = ["runner", "--apply", "--frozen-manifest", str(self.manifest_path)]
        patches = dict(
            war3=self.fake,
            ROOT=self.root,
            SITE=self.site,
            MAP=self.map_path,
            PROTECTED=self.protected,
            zero_processes=lambda: [],
            gpu_events_since=lambda start: [],
            time=_FakeTime(),
            close_owned_window=lambda launch, owner: {"posted": []},
            exact_resolution=lambda pid: {"ok": True, "info": {"clientRect": {"width": 2560,
                                                                             "height": 1440}}},
        )
        if install_guarded is not None:
            patches["install_guarded"] = install_guarded
        with patch.object(sys, "argv", argv), patch.multiple(runner, **patches):
            return runner.main()

    def test_successful_mocked_main_and_identity_mapping(self):
        self.assertEqual(self._run(), 0)
        out = self.root / "AutoTest/artifacts" / self.run_id
        self.assertTrue(out.is_dir())
        self.assertEqual(runner.identity(self.live), self.identities["liveRecovery"])
        self.assertEqual(runner.identity(self.candidate), self.identities["candidate"])
        receipt = json.loads((out / "receipt.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt["runCount"], 1)
        self.assertEqual(len(receipt["phaseMarkers"]), 5)
        for phase in ("sample-start", "pressure-start", "pressure-end",
                      "relief-start", "relief-end"):
            self.assertTrue((out / ("phaseMarkers." + phase + ".json")).is_file(), phase)
        self.assertTrue((out / "phaseMarkers.json").is_file())
        binding = json.loads((out / "binding.json").read_text(encoding="utf-8"))
        self.assertEqual(binding["frozenManifestSha256"], runner.identity(self.manifest_path)["sha256"])
        self.assertTrue((out / "frozen-manifest.json").is_file())
        for marker in receipt["phaseMarkers"]:
            self.assertEqual(marker["frameDomain"], "workload.businessFrameSerial")
            self.assertEqual(marker["producerAccumulationEpoch"], 7)
            self.assertGreater(marker["businessFrameSerial"], 0)
            self.assertGreater(marker["perfFrameEpoch"], 0)
        serials = [marker["businessFrameSerial"] for marker in receipt["phaseMarkers"]]
        epochs = [marker["perfFrameEpoch"] for marker in receipt["phaseMarkers"]]
        self.assertEqual(serials, sorted(serials))
        self.assertEqual(epochs, sorted(epochs))

    def test_initial_anchor_pending_then_completed_reaches_sample_start(self):
        self.fake.initial_anchor_pending_after_ready = 2
        self.assertEqual(self._run(), 0)
        out = self.root / "AutoTest/artifacts" / self.run_id
        marker = json.loads(
            (out / "phaseMarkers.sample-start.json").read_text(encoding="utf-8"))
        self.assertEqual(marker["frameDomain"], "workload.businessFrameSerial")
        self.assertEqual(marker["initialAnchorWait"]["attempts"], 2)
        self.assertEqual(
            marker["initialAnchorWait"]["pinnedProducerAccumulationEpoch"], 7)

    def test_initial_anchor_pending_main_timeout_restores(self):
        self.fake.initial_anchor_pending_after_ready = 1000
        self.assertEqual(self._run(), 1)
        out = self.root / "AutoTest/artifacts" / self.run_id
        self.assertEqual(runner.identity(self.live), self.identities["liveRecovery"])
        receipt = json.loads((out / "receipt.json").read_text(encoding="utf-8"))
        self.assertIn("initial completed frame", receipt["error"])
        self.assertIn("attempts=", receipt["error"])
        self.assertIn("lastPerf", receipt["error"])

    def test_preexisting_output_stops_before_deploy(self):
        out = self.root / "AutoTest/artifacts" / self.run_id
        out.mkdir(parents=True)
        with self.assertRaises(RuntimeError):
            self._run()
        self.assertEqual(runner.identity(self.live), self.identities["liveRecovery"])
        self.assertEqual(runner.identity(self.candidate), self.identities["candidate"])
        self.assertFalse((out / "receipt.json").exists())

    def test_wrong_expected_live_fails_before_output(self):
        bad = dict(self.manifest)
        bad["liveRecovery"] = {"sha256": "F" * 64, "size": self.identities["liveRecovery"]["size"]}
        self.manifest_path.write_text(json.dumps(bad), encoding="utf-8")
        with self.assertRaises(RuntimeError):
            self._run()
        self.assertFalse((self.root / "AutoTest/artifacts" / self.run_id).exists())

    def test_wrong_candidate_identity_fails_before_output(self):
        bad = dict(self.manifest)
        bad["candidate"] = {"sha256": "F" * 64, "size": self.identities["candidate"]["size"]}
        self.manifest_path.write_text(json.dumps(bad), encoding="utf-8")
        with self.assertRaises(RuntimeError):
            self._run()
        self.assertFalse((self.root / "AutoTest/artifacts" / self.run_id).exists())

    def test_wrong_manifest_env_fails_before_output(self):
        bad = dict(self.manifest)
        bad["env"] = dict(self.manifest["env"])
        bad["env"]["DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB"] = "512"
        self.manifest_path.write_text(json.dumps(bad), encoding="utf-8")
        with self.assertRaises(RuntimeError):
            self._run()
        self.assertFalse((self.root / "AutoTest/artifacts" / self.run_id).exists())

    def test_unknown_identity_key_fails(self):
        before = {"candidate": self.identities["candidate"],
                  "live": self.identities["liveRecovery"],
                  "map": self.identities["map"], "game": self.identities["game"],
                  "exe": self.identities["exe"]}
        identities = dict(self.identities)
        identities["extra"] = {"sha256": "A" * 64, "size": 1}
        with self.assertRaises(RuntimeError):
            runner.check_frozen_identities(before, identities)

    def test_ready_timeout_restores_and_writes_partial_receipt(self):
        self.fake.wait_ok = False
        self.assertEqual(self._run(), 1)
        out = self.root / "AutoTest/artifacts" / self.run_id
        self.assertEqual(runner.identity(self.live), self.identities["liveRecovery"])
        self.assertTrue((out / "ready-failure.json").is_file())
        self.assertEqual(json.loads((out / "phaseMarkers.json").read_text(encoding="utf-8")), [])
        receipt = json.loads((out / "receipt.json").read_text(encoding="utf-8"))
        self.assertIn("map ready", receipt["error"])

    def test_second_phase_failure_restores_and_keeps_markers(self):
        self.fake.fail_camera_apply_at = 2
        self.assertEqual(self._run(), 1)
        out = self.root / "AutoTest/artifacts" / self.run_id
        self.assertEqual(runner.identity(self.live), self.identities["liveRecovery"])
        for phase in ("sample-start", "pressure-start"):
            self.assertTrue((out / ("phaseMarkers." + phase + ".json")).is_file())
        markers = json.loads((out / "phaseMarkers.json").read_text(encoding="utf-8"))
        self.assertEqual([marker["phase"] for marker in markers], ["sample-start", "pressure-start"])

    def test_graceful_exit_failure_records_residual_pid(self):
        self.fake.graceful_exit = False
        self.assertEqual(self._run(), 1)
        out = self.root / "AutoTest/artifacts" / self.run_id
        self.assertEqual(runner.identity(self.live), self.identities["liveRecovery"])
        receipt = json.loads((out / "receipt.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt.get("residualPid"), self.owner.pid)

    def test_restore_identity_conflict_does_not_overwrite(self):
        real_install = runner.install_guarded
        calls = {"count": 0}
        def conflict(*args, **kwargs):
            calls["count"] += 1
            if calls["count"] == 2:
                Path(args[1]).write_bytes(b"external-restore-conflict")
            return real_install(*args, **kwargs)
        self.assertEqual(self._run(install_guarded=conflict), 1)
        self.assertEqual(self.live.read_bytes(), b"external-restore-conflict")
        out = self.root / "AutoTest/artifacts" / self.run_id
        receipt = json.loads((out / "receipt.json").read_text(encoding="utf-8"))
        self.assertIn("restoreOrResidualError", receipt)


    def test_auto_export_env_value_is_pinned(self):
        bad = json.loads(json.dumps(self.manifest))
        bad['env']['DXVK_WAR3_PERF_AUTO_EXPORT_SEC'] = '19'
        self.manifest_path.write_text(json.dumps(bad), encoding='utf-8')
        with self.assertRaises(RuntimeError):
            self._run()
        self.assertFalse((self.root/'AutoTest/artifacts'/self.run_id).exists())

    def test_run_id_must_be_path_component(self):
        bad = json.loads(json.dumps(self.manifest))
        bad['runId'] = '../escape'
        bad['scenario'] = '../escape'
        bad['env']['DXVK_WAR3_SCENARIO'] = '../escape'
        self.manifest_path.write_text(json.dumps(bad), encoding='utf-8')
        with self.assertRaises(RuntimeError):
            self._run()
        self.assertFalse((self.root/'AutoTest/artifacts'/'escape').exists())

    def test_noninteractive_desktop_required_before_route(self):
        self.fake.desktop_noninteractive = False
        self.assertEqual(self._run(), 1)
        out = self.root/'AutoTest/artifacts'/self.run_id
        self.assertEqual(runner.identity(self.live), self.identities['liveRecovery'])
        receipt = json.loads((out/'receipt.json').read_text(encoding='utf-8'))
        self.assertIn('noninteractive', receipt['error'])

    def test_ready_exception_writes_failure_and_restores(self):
        self.fake.wait_raises = True
        self.assertEqual(self._run(), 1)
        out = self.root/'AutoTest/artifacts'/self.run_id
        self.assertEqual(runner.identity(self.live), self.identities['liveRecovery'])
        self.assertTrue((out/'ready-failure.json').is_file())
        receipt = json.loads((out/'receipt.json').read_text(encoding='utf-8'))
        self.assertIn('wait_for_game_ready exception', receipt['error'])

    def test_missing_live_restored_from_frozen_backup(self):
        real_install = runner.install_guarded
        def delete_live_after_install(*args, **kwargs):
            result = real_install(*args, **kwargs)
            Path(args[1]).unlink()
            return result
        self.assertEqual(self._run(install_guarded=delete_live_after_install), 0)
        out = self.root/'AutoTest/artifacts'/self.run_id
        self.assertEqual(runner.identity(self.live), self.identities['liveRecovery'])
        receipt = json.loads((out/'receipt.json').read_text(encoding='utf-8'))
        self.assertIn('restoration', receipt)
        self.assertTrue(receipt['restoreOk'])

    def test_missing_live_corrupt_backup_fails_closed(self):
        real_install = runner.install_guarded
        def corrupt_backup_after_install(*args, **kwargs):
            result = real_install(*args, **kwargs)
            Path(args[1]).unlink()
            backup = self.root/'AutoTest/artifacts'/self.run_id/'live.dll'
            backup.write_bytes(b'corrupt')
            return result
        self.assertEqual(self._run(install_guarded=corrupt_backup_after_install), 1)
        self.assertFalse(self.live.exists())
        out = self.root/'AutoTest/artifacts'/self.run_id
        receipt = json.loads((out/'receipt.json').read_text(encoding='utf-8'))
        self.assertIn('restoreOrResidualError', receipt)

    def test_partial_deploy_restores_live_from_backup(self):
        def partial_install(*args, **kwargs):
            live = Path(args[1])
            parked = Path(args[4])
            runner.move_new(live, parked)
            raise RuntimeError('partial deploy')
        self.assertEqual(self._run(install_guarded=partial_install), 1)
        out = self.root/'AutoTest/artifacts'/self.run_id
        self.assertEqual(runner.identity(self.live), self.identities['liveRecovery'])
        receipt = json.loads((out/'receipt.json').read_text(encoding='utf-8'))
        self.assertIn('partial deploy', receipt['error'])

    def test_install_preflight_failure_keeps_stable_live(self):
        def fail_install(*args, **kwargs):
            raise RuntimeError('install preflight failed')
        self.assertEqual(self._run(install_guarded=fail_install), 1)
        self.assertEqual(runner.identity(self.live), self.identities['liveRecovery'])


class PhaseMainIntegrationTests(unittest.TestCase):
    def test_pre_ready_missing_perf_still_reaches_mocked_ready_route(self):
        setup = MainOrchestrationTests("test_successful_mocked_main_and_identity_mapping")
        setup.setUp()
        self.addCleanup(setup.temp.cleanup)
        setup.fake.pre_ready_without_perf = True
        self.assertEqual(setup._run(), 0)
        out = setup.root / "AutoTest/artifacts" / setup.run_id
        ready = json.loads((out / "ready-preflight.json").read_text(encoding="utf-8"))
        self.assertTrue(ready["controlPlaneOk"])
        self.assertNotIn("businessFrameSerial", ready)
        self.assertEqual(ready["frameDomain"], "diagnostic-not-phase-anchor")

    def test_phase_timeout_restores_and_writes_partial_receipt(self):
        setup = MainOrchestrationTests("test_successful_mocked_main_and_identity_mapping")
        setup.setUp()
        self.addCleanup(setup.temp.cleanup)
        setup.fake.pre_ready_without_perf = True
        setup.fake.freeze_after = 2
        self.assertEqual(setup._run(), 1)
        out = setup.root / "AutoTest/artifacts" / setup.run_id
        self.assertEqual(runner.identity(setup.live), setup.identities["liveRecovery"])
        markers = json.loads((out / "phaseMarkers.json").read_text(encoding="utf-8"))
        self.assertEqual([item["phase"] for item in markers], ["sample-start"])
        receipt = json.loads((out / "receipt.json").read_text(encoding="utf-8"))
        self.assertIn("phase marker timeout", receipt["error"])


class B10ContractTests(unittest.TestCase):
    def test_zero_processes_filters_extended_toolchain_and_project_python(self):
        captured = {}
        def fake_run(cmd, **kwargs):
            captured['script'] = cmd[-1]
            return SimpleNamespace(stdout='[]')
        with patch.object(runner.subprocess, 'run', side_effect=fake_run):
            self.assertEqual(runner.zero_processes(), [])
        script = captured['script']
        self.assertIn('cl|link|ld|clang', script)
        self.assertIn('gcc', script)
        self.assertIn(str(os.getpid()), script)
        self.assertIn('run_snapshot_pressure_canary', script)
        self.assertIn('war3_autotest_mcp', script)

    def test_restore_live_from_backup_stable_and_corrupt(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            live, backup, staged = root/'live.dll', root/'backup.dll', root/'staged.dll'
            backup.write_bytes(b'original')
            expected = runner.identity(backup)
            result = runner.restore_live_from_backup(live, backup, expected, staged)
            self.assertEqual(runner.identity(live), expected)
            self.assertFalse(staged.exists())
            self.assertEqual(result['identity'], expected)
            live.unlink()
            corrupt = root/'corrupt.dll'
            corrupt.write_bytes(b'corrupt')
            with self.assertRaises(RuntimeError):
                runner.restore_live_from_backup(live, corrupt, expected, staged)
            self.assertFalse(live.exists())

    def test_restore_live_from_backup_rejects_competing_file(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            live, backup, staged = root/'live.dll', root/'backup.dll', root/'staged.dll'
            backup.write_bytes(b'original')
            expected = runner.identity(backup)
            real_move = runner.move_new
            def competing_move(src, dst):
                dst.write_bytes(b'thirdparty')
                real_move(src, dst)
            with patch.object(runner, 'move_new', side_effect=competing_move):
                with self.assertRaises(RuntimeError):
                    runner.restore_live_from_backup(live, backup, expected, staged)
            self.assertEqual(live.read_bytes(), b'thirdparty')
            self.assertTrue(staged.exists())
            self.assertEqual(runner.identity(backup), expected)


class WaitPulseTests(unittest.TestCase):
    def _run_wait(self, pipe_ok, max_pulses):
        fake = _FakeTime()
        pulses = []
        def control_plane(**kwargs):
            if pipe_ok:
                return {'transportOk': True, 'ok': False,
                        'result': {'runtimeStatus': {'runtime': {
                            'jassReady': True, 'gameStarted': False}}}}
            return {'transportOk': False}
        def post_key(*args, **kwargs):
            pulses.append(dict(kwargs))
            return {'ok': True, 'key': kwargs.get('key', args[1] if len(args) > 1 else '')}
        with patch.multiple(runner.war3,
                time=SimpleNamespace(time=fake.time, sleep=fake.sleep),
                _start_debug_monitor=lambda *a, **k: None,
                _session_by_selector=lambda **k: None,
                _pid_alive=lambda pid: True,
                _control_plane_request=control_plane,
                _post_war3_key_pulse=post_key):
            result = runner.war3.wait_for_game_ready(
                pid=123, timeout_sec=21, allow_fallback=False,
                auto_continue_loading=True, continue_key='SPACE',
                continue_interval_sec=5, continue_max_pulses=max_pulses)
        return result, pulses

    def test_max_one_pulse_on_pipe_and_no_pipe_paths(self):
        for pipe_ok in (True, False):
            result, pulses = self._run_wait(pipe_ok, 1)
            self.assertEqual(len(pulses), 1, (pipe_ok, result))
            self.assertFalse(result['ok'])
            if 'continuePulses' in result:
                self.assertEqual(len(result['continuePulses']), 1)

    def test_default_caller_can_still_repeat_pulses(self):
        for pipe_ok in (True, False):
            result, pulses = self._run_wait(pipe_ok, None)
            self.assertGreaterEqual(len(pulses), 2, (pipe_ok, result))

    def _run_wait_with_control(self, control_plane, max_pulses=1,
                               timeout_sec=21,
                               require_control_plane_for_continue=True):
        fake = _FakeTime()
        pulses = []

        def post_key(*args, **kwargs):
            pulses.append(dict(kwargs))
            return {"ok": True, "key": kwargs.get("key", "")}

        with patch.multiple(runner.war3,
                time=SimpleNamespace(time=fake.time, sleep=fake.sleep),
                _start_debug_monitor=lambda *a, **k: None,
                _session_by_selector=lambda **k: None,
                _pid_alive=lambda pid: True,
                _control_plane_request=control_plane,
                _post_war3_key_pulse=post_key):
            result = runner.war3.wait_for_game_ready(
                pid=123, timeout_sec=timeout_sec, allow_fallback=False,
                auto_continue_loading=True, continue_key="SPACE",
                continue_interval_sec=5, continue_max_pulses=max_pulses,
                require_control_plane_for_continue=require_control_plane_for_continue)
        return result, pulses, fake

    def test_require_control_plane_waits_past_10s_then_pulse_once(self):
        calls = {"n": 0}

        def control_plane(**kwargs):
            calls["n"] += 1
            if calls["n"] <= 55:
                return {"transportOk": False}
            return {"transportOk": True, "ok": False,
                    "result": {"runtimeStatus": {"runtime": {
                        "jassReady": True, "gameStarted": False}}}}

        result, pulses, fake = self._run_wait_with_control(control_plane)
        self.assertEqual(len(pulses), 1)
        self.assertNotIn("controlPlaneReady", pulses[0])
        self.assertFalse(result["ok"])
        self.assertGreaterEqual(fake.now - 1000.0, 10.0)

    def test_require_control_plane_never_pipe_timeout_zero_posts(self):
        result, pulses, _ = self._run_wait_with_control(
            lambda **kwargs: {"transportOk": False})
        self.assertEqual(pulses, [])
        self.assertFalse(result["ok"])

    def test_require_control_plane_rejects_bad_or_false_jass(self):
        cases = (
            {"jassReady": "false", "gameStarted": False},
            {"jassReady": 1, "gameStarted": False},
            {"gameStarted": False},
            {"jassReady": True},
            {"jassReady": False, "gameStarted": False},
            {"jassReady": None, "gameStarted": False},
            {"jassReady": True, "gameStarted": True},
            {"jassReady": True, "gameStarted": "false"},
            {"jassReady": True, "gameStarted": None},
            {"jassReady": True, "gameStarted": 0},
        )
        for runtime in cases:
            with self.subTest(runtime=runtime):
                def control_plane(**kwargs):
                    return {"transportOk": True, "ok": False,
                            "result": {"runtimeStatus": {"runtime": runtime}}}
                result, pulses, _ = self._run_wait_with_control(control_plane)
                self.assertEqual(pulses, [])
                self.assertFalse(result["ok"])

    def test_require_control_plane_rejects_malformed_transport_or_status(self):
        cases = (
            {"transportOk": "true", "ok": False,
             "result": {"runtimeStatus": {"runtime": {
                 "jassReady": True, "gameStarted": False}}}},
            {"transportOk": "false", "ok": True,
             "result": {"runtimeStatus": {"runtime": {
                 "jassReady": True, "gameStarted": False}}}},
            {"transportOk": True, "ok": "false",
             "result": {"runtimeStatus": {"runtime": {
                 "jassReady": True, "gameStarted": False}}}},
            {"transportOk": True,
             "result": {"runtimeStatus": {"runtime": {
                 "jassReady": True, "gameStarted": False}}}},
        )
        for response in cases:
            with self.subTest(response=response):
                result, pulses, _ = self._run_wait_with_control(
                    lambda **kwargs: response)
                self.assertEqual(pulses, [])
                self.assertFalse(result["ok"])

    def test_require_control_plane_cap_one_even_repeated_ready_fail(self):
        def control_plane(**kwargs):
            return {"transportOk": True, "ok": False,
                    "result": {"runtimeStatus": {"runtime": {
                        "jassReady": True, "gameStarted": False}}}}

        result, pulses, _ = self._run_wait_with_control(control_plane, max_pulses=1)
        self.assertEqual(len(pulses), 1)

    def test_legacy_default_still_allows_no_pipe_pulse(self):
        result, pulses, _ = self._run_wait_with_control(
            lambda **kwargs: {"transportOk": False}, max_pulses=1,
            require_control_plane_for_continue=False)
        self.assertEqual(len(pulses), 1)


if __name__ == '__main__':
    unittest.main()
