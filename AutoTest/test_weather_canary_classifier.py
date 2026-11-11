"""Counterexamples for the actual canary-b classifier, not game/visual proof.

The launcher is imported directly; no game-control module or process starts.
"""

from __future__ import annotations

import copy
import ctypes
import json
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import run_weather_canary_20261010 as canary


TIMES = (0, 0, 8, 16, 24, 32, 2, 2, 10, 10, 18, 0, 8, 0, 0)
NON_ACCEPTANCE = ("captureExactWeatherFrameCertified", "gpuCompletionCertified",
                  "pixelOrArtisticAcceptance")


class FakeWinCall:
    def __init__(self, function):
        self.function = function
        self.calls = []

    def __call__(self, *args):
        self.calls.append(args)
        return self.function(*args)


class FakeResizeUser32:
    def __init__(self, owner=7, set_error=0, get_error=0,
                 actual_desktop="War3AutoTest_unit", input_desktop="Default",
                 client_dimensions=(2560, 1440), name_error_handle=0,
                 open_input_ok=True, close_input_ok=True, get_client_ok=True,
                 enum_windows=(88,), enum_ok=True, member_owner=None, member_thread_id=77):
        owner_queries = 0
        def get_owner(_hwnd, output):
            nonlocal owner_queries
            owner_queries += 1
            output._obj.value = owner if owner_queries == 1 or member_owner is None else member_owner
            return 77 if owner_queries == 1 else member_thread_id
        def enum_desktop(_handle, callback, parameter):
            for window in enum_windows:
                callback(window, parameter)
            ctypes.set_last_error(0)
            return 1 if enum_ok else 0
        def set_style(*_args):
            ctypes.set_last_error(set_error)
            return 0  # Zero previous value is legal only with LastError=0.
        def get_style(*_args):
            ctypes.set_last_error(get_error)
            return 0
        def desktop_info(handle, _index, buffer, _size, output_needed):
            if handle == name_error_handle:
                ctypes.set_last_error(5)
                return 0
            name = actual_desktop if handle == 11 else input_desktop
            output_needed._obj.value = (len(name) + 1) * ctypes.sizeof(ctypes.c_wchar)
            if buffer is None:
                ctypes.set_last_error(122)
                return 0
            buffer.value = name
            return 1
        def client_rect(_hwnd, output):
            if not get_client_ok:
                ctypes.set_last_error(5)
                return 0
            output._obj.left = output._obj.top = 0
            output._obj.right, output._obj.bottom = client_dimensions
            return 1
        self.GetWindowThreadProcessId = FakeWinCall(get_owner)
        self.EnumDesktopWindows = FakeWinCall(enum_desktop)
        self.GetUserObjectInformationW = FakeWinCall(desktop_info)
        self.OpenInputDesktop = FakeWinCall(lambda *_args: 12 if open_input_ok else 0)
        self.CloseDesktop = FakeWinCall(lambda *_args: 1 if close_input_ok else 0)
        self.GetWindowLongW = FakeWinCall(get_style)
        self.SetWindowLongW = FakeWinCall(set_style)
        self.SetWindowPos = FakeWinCall(lambda *_args: 1)
        self.GetClientRect = FakeWinCall(client_rect)


def fake_resize_runtime():
    return mock.Mock(**{
        "_wait_for_main_window_hwnd.return_value": 88,
        "_resize_window_client_native.return_value": {"ok": True,
            "pid": 7, "hwnd": 88, "info": {"ok": True,
                "clientRect": {"width": 2560, "height": 1440}}},
    })


def resize_launch():
    return {"ok": True, "pid": 7,
        "instanceRoot": str(canary.SANDBOX / "_AutoTestInstances/unit-only/weather"),
        "desktop": {"ok": True, "name": "War3AutoTest_unit", "handle": 11,
                    "nonInteractiveOnly": True,
                    "inputDesktopBefore": "Default", "inputDesktopAfter": "Default"}}


def observation(stage: int, seconds: float, serial: int, off_frame: int = 0) -> dict:
    enabled = stage not in (0, 8, 10, 13, 14)
    weather_id = 0 if stage in (13, 14) else 7
    runtime = {"observationKind": "owner-control-live-state-not-render-input",
        "id": weather_id, "enabled": enabled, "paused": stage in (5, 6),
        "timeSeconds": seconds, "frameSerial": serial + 5,
        "mapEpoch": 31, "deviceEpoch": 41}
    params = canary.expected_params(stage, seconds)
    frozen = {"observationKind": "render-consumed-value-snapshot",
        "id": weather_id if enabled else 0, "enabled": enabled,
        "paused": stage in (5, 6) if enabled else False,
        "timeSeconds": seconds if enabled else 0,
        "frameSerial": serial if enabled else 0,
        "mapEpoch": 31 if enabled else 0, "deviceEpoch": 41 if enabled else 0,
        "revision": 10 + stage if enabled else 0,
        "historyRevision": 1 + stage if enabled else 0,
        "params": params if enabled else dict.fromkeys(canary.PARAM_FIELDS, 0)}
    snowfall = params.get("snowfall", 0)
    draws = enabled and snowfall > 0
    medium = enabled and (params.get("haze", 0) > 0 or params.get("lowSnow", 0) > 0)
    return {"runtime": runtime, "frozenInput": frozen,
        "render": {"id": weather_id if enabled else 0,
            "timeSeconds": seconds if enabled else 0,
            "frameSerial": serial if enabled else off_frame,
            "mapEpoch": 31 if stage else 0, "deviceEpoch": 41 if stage else 0,
            "commandsRecorded": draws, "particleCount": int(16384 * snowfall) if draws else 0,
            "capacity": 16384, "resourceBytes": 1024 if enabled else 0,
            "stage": "commands-recorded" if draws else "no-snowfall" if enabled else "disabled"},
        "volumetric": {"frameSerial": serial if stage >= 2 else 0,
            "mapEpoch": 31 if stage >= 2 else 0, "deviceEpoch": 41 if stage >= 2 else 0,
            "compositeSubmitted": medium,
            "stage": "composite-submitted" if medium else "disabled",
            "resourceBytes": 1024 if stage >= 2 else 0, "admittedShaderWork": 1000 if medium else 0}}


def baseline() -> dict:
    receipt = {"stageReceipts": [{"stage": i, "id": 7, "timeSeconds": TIMES[i],
        "checks": canary.EXPECTED_CHECKS[i], "errors": 0} for i in range(canary.STAGE_COUNT)],
        "snapshots": [], "captures": []}
    receipt["stageReceipts"][11]["queryDiagnostic"] = {
        "stockS2RScientific": 9.73, "stockS2RDecimal": 0.0, "playRaw": "0.0000973"}
    receipt["stageReceipts"][6]["tinyQueryRaw"] = "0.0000973"
    serial = 100
    off_frame = 0
    for stage in range(canary.STAGE_COUNT):
        if stage in (8, 10, 13):
            off_frame = serial + 1
        offset = 0 if stage in (0, 5, 6, 8, 10, 13, 14) else 0.5
        values = []
        for seconds in (TIMES[stage] + offset, TIMES[stage] + 2 * offset):
            serial += 1
            value = observation(stage, seconds, serial, off_frame)
            receipt["snapshots"].append({"jassStage": stage, "stageAtStatusCompletion": stage,
                "observation": value, "alignment": canary.frame_alignment(value)})
            values.append(value)
        if stage in canary.CAPTURES:
            receipt["captures"].append({"stage": stage, "stageAtRequest": stage,
                "stageAtCompletion": stage, "label": canary.CAPTURES[stage], "ok": True,
                "dimensions": [2560, 1440], "sha256": f"{stage + 1:064x}", "size": 2560 * 1440 * 4 + 54,
                "beforeCaptureObservation": copy.deepcopy(values[0]),
                "afterCaptureObservation": copy.deepcopy(values[1])})
    return receipt


def mutate_stage(receipt: dict, stage: int, mutate) -> None:
    for row in receipt["snapshots"]:
        if row["jassStage"] == stage:
            mutate(row["observation"])
            row["alignment"] = canary.frame_alignment(row["observation"])


class WeatherCanaryClassifierTests(unittest.TestCase):
    def test_tiny_paused_raw_query_cannot_hide_exponent_bug(self):
        for raw in ("9.73000003e-05", "9.73", "0", "", "nan"):
            receipt = baseline()
            receipt["stageReceipts"][6]["tinyQueryRaw"] = raw
            self.assertFalse(canary.classify(receipt)["tinyPausedJassTimeQueryConsistent"], raw)

    def test_query_scientific_or_missing_reply_is_not_accepted(self):
        for raw in ("9.73e-05", "9.73", "", "nan", "inf", "-0.1"):
            receipt = baseline()
            receipt["stageReceipts"][11]["queryDiagnostic"]["playRaw"] = raw
            self.assertFalse(canary.classify(receipt)["immediateJassTimeQueryConsistent"], raw)
        receipt = baseline()
        del receipt["stageReceipts"][11]["queryDiagnostic"]
        self.assertFalse(canary.classify(receipt)["immediateJassTimeQueryConsistent"])

    def test_query_vm_probe_and_stage_receipt_must_agree(self):
        for field, value in (("stockS2RScientific", 0.0), ("stockS2RDecimal", 9.73)):
            receipt = baseline()
            receipt["stageReceipts"][11]["queryDiagnostic"][field] = value
            self.assertFalse(canary.classify(receipt)["immediateJassTimeQueryConsistent"])
        receipt = baseline()
        receipt["stageReceipts"][11]["timeSeconds"] = 9.73
        self.assertFalse(canary.classify(receipt)["immediateJassTimeQueryConsistent"])

    def test_resize_missing_isolated_receipt_refuses_before_winapi(self):
        for changed in ({"ok": False}, {"desktop": {}}, {"desktop": {"ok": False}},
                        {"desktop": {"ok": True}},
                        {"desktop": {"ok": True, "name": "War3AutoTest_unit"}},
                        {"desktop": {"ok": True, "name": "War3AutoTest_unit", "handle": 0}}):
            launch = resize_launch()
            launch.update(changed)
            with mock.patch.object(ctypes, "WinDLL") as native:
                with self.assertRaises(RuntimeError):
                    canary.resize_owned_isolated_client(fake_resize_runtime(), launch)
                native.assert_not_called()

    def test_resize_escaped_instance_refuses_before_winapi(self):
        launch = resize_launch()
        launch["instanceRoot"] = str(canary.SANDBOX.parent / "unowned-player-tree")
        with mock.patch.object(ctypes, "WinDLL") as native:
            with self.assertRaises(RuntimeError):
                canary.resize_owned_isolated_client(fake_resize_runtime(), launch)
            native.assert_not_called()

    def test_resize_wrong_hwnd_owner_refuses_before_style_write(self):
        api = FakeResizeUser32(owner=99)
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaises(RuntimeError):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
        self.assertFalse(api.GetWindowLongW.calls)
        self.assertFalse(api.SetWindowLongW.calls)
        self.assertFalse(api.SetWindowPos.calls)

    def test_resize_zero_previous_style_with_last_error_is_failure(self):
        api = FakeResizeUser32(set_error=5)
        runtime = fake_resize_runtime()
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaises(OSError):
                canary.resize_owned_isolated_client(runtime, resize_launch())
        runtime._resize_window_client_native.assert_not_called()
        self.assertFalse(api.SetWindowPos.calls)

    def test_resize_zero_previous_style_without_error_is_valid_return(self):
        api = FakeResizeUser32()
        runtime = fake_resize_runtime()
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            result = canary.resize_owned_isolated_client(runtime, resize_launch())
        self.assertTrue(result["ok"])
        self.assertFalse(result["globalInputUsed"])
        self.assertFalse(result["registryWritePerformed"])
        self.assertEqual("War3AutoTest_unit", result["actualLaunchDesktop"])
        self.assertEqual(11, result["ownedLaunchDesktopHandle"])
        self.assertEqual({"proof": "EnumDesktopWindows", "enumSucceeded": True,
                          "targetMatches": [{"hwnd": 88, "pid": 7, "threadId": 77}]},
                         result["desktopMembership"])
        self.assertEqual(11, api.EnumDesktopWindows.calls[0][0])
        self.assertEqual("Default", result["inputDesktopAtResize"])
        self.assertEqual([2560, 1440], result["actualClientDimensions"])
        self.assertEqual([(12,)], api.CloseDesktop.calls)  # Not borrowed handle 11.
        flags = api.SetWindowPos.calls[0][-1]
        self.assertTrue(flags & 0x10)  # NOACTIVATE
        self.assertTrue(flags & 0x04)  # NOZORDER
        self.assertTrue(flags & 0x20)  # FRAMECHANGED

    def test_resize_retained_desktop_name_mismatch_refuses_before_style(self):
        api = FakeResizeUser32(actual_desktop="SomeOtherDesktop")
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaises(RuntimeError):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
        self.assertFalse(api.GetWindowLongW.calls)
        self.assertFalse(api.SetWindowLongW.calls)
        self.assertFalse(api.OpenInputDesktop.calls)

    def test_resize_target_absent_from_owned_desktop_refuses_before_style(self):
        for windows in ((), (99,)):
            api = FakeResizeUser32(enum_windows=windows)
            with mock.patch.object(ctypes, "WinDLL", return_value=api):
                with self.assertRaisesRegex(RuntimeError, "membership not proven"):
                    canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
            self.assertFalse(api.GetWindowLongW.calls)
            self.assertFalse(api.SetWindowLongW.calls)
            self.assertFalse(api.SetWindowPos.calls)

    def test_resize_enumerated_target_wrong_pid_refuses_before_style(self):
        api = FakeResizeUser32(member_owner=99)
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaisesRegex(RuntimeError, "membership not proven"):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
        self.assertFalse(api.GetWindowLongW.calls)
        self.assertFalse(api.SetWindowLongW.calls)
        self.assertFalse(api.SetWindowPos.calls)

    def test_resize_enumerated_target_wrong_tid_refuses_before_style(self):
        api = FakeResizeUser32(member_thread_id=78)
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaisesRegex(RuntimeError, "membership not proven"):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
        self.assertFalse(api.SetWindowLongW.calls)

    def test_resize_desktop_enumeration_failure_refuses_even_with_target(self):
        api = FakeResizeUser32(enum_ok=False)
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaisesRegex(RuntimeError, "enumeration failed"):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
        self.assertFalse(api.SetWindowLongW.calls)
        self.assertFalse(api.SetWindowPos.calls)

    def test_resize_duplicate_enumerated_target_refuses_before_style(self):
        api = FakeResizeUser32(enum_windows=(88, 88))
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaisesRegex(RuntimeError, "membership not proven"):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
        self.assertEqual(3, len(api.GetWindowThreadProcessId.calls))
        self.assertFalse(api.SetWindowLongW.calls)

    def test_resize_input_desktop_equal_refuses_and_closes_owned_handle(self):
        api = FakeResizeUser32(input_desktop="war3autotest_UNIT")
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaises(RuntimeError):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
        self.assertFalse(api.GetWindowLongW.calls)
        self.assertFalse(api.SetWindowLongW.calls)
        self.assertEqual([(12,)], api.CloseDesktop.calls)

    def test_resize_input_name_read_failure_still_closes_owned_handle(self):
        api = FakeResizeUser32(name_error_handle=12)
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaises(OSError):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
        self.assertEqual([(12,)], api.CloseDesktop.calls)
        self.assertFalse(api.SetWindowLongW.calls)

    def test_resize_open_or_close_desktop_failure_never_changes_style(self):
        for changed in ({"open_input_ok": False}, {"close_input_ok": False}):
            api = FakeResizeUser32(**changed)
            with mock.patch.object(ctypes, "WinDLL", return_value=api):
                with self.assertRaises(OSError):
                    canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
            self.assertFalse(api.SetWindowLongW.calls)

    def test_resize_getwindowlong_zero_with_last_error_refuses_before_style(self):
        api = FakeResizeUser32(get_error=5)
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaises(OSError):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())
        self.assertFalse(api.SetWindowLongW.calls)
        self.assertFalse(api.SetWindowPos.calls)

    def test_resize_real_client_size_must_equal_target_despite_helper_ok(self):
        api = FakeResizeUser32(client_dimensions=(2560, 1415))
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaisesRegex(RuntimeError, "resize mismatch"):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())

    def test_resize_getclientrect_failure_cannot_certify_size(self):
        api = FakeResizeUser32(get_client_ok=False)
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaises(OSError):
                canary.resize_owned_isolated_client(fake_resize_runtime(), resize_launch())

    def test_resize_helper_different_hwnd_cannot_certify_owned_window(self):
        api = FakeResizeUser32()
        runtime = fake_resize_runtime()
        runtime._resize_window_client_native.return_value["hwnd"] = 99
        with mock.patch.object(ctypes, "WinDLL", return_value=api):
            with self.assertRaises(RuntimeError):
                canary.resize_owned_isolated_client(runtime, resize_launch())

    def test_final_registry_read_failure_saves_cleanup_and_unknown_verdict(self):
        registry = mock.MagicMock()
        registry.OpenKey.side_effect = PermissionError("unit-only access denied")
        receipt = {"runtimeAcceptance": True, "stop": {"ok": True, "pid": 7},
                   "videoRegistryBefore": [1280, 720]}
        with tempfile.TemporaryDirectory(prefix="weather-receipt-test-") as temporary:
            with self.assertRaisesRegex(RuntimeError, "verification unavailable"):
                canary.save_final_run_receipt(Path(temporary), receipt, {}, [1280, 720],
                                              registry, "unit-only-read-key")
            saved = json.loads((Path(temporary) / "run.json").read_text(encoding="utf-8"))
        self.assertEqual({"ok": True, "pid": 7}, saved["stop"])
        self.assertTrue(saved["protectedInputsUnchanged"])
        self.assertFalse(saved["runtimeAcceptance"])
        self.assertFalse(saved["videoRegistryReadAfter"]["ok"])
        self.assertIsNone(saved["videoRegistryChanged"])
        self.assertIsNone(saved["videoRegistryAfter"])
        registry.SetValueEx.assert_not_called()

    def test_final_cleanup_failure_and_registry_failure_are_both_saved(self):
        registry = mock.MagicMock()
        registry.OpenKey.side_effect = OSError("unit-only registry error")
        receipt = {"runtimeAcceptance": True, "stop": {"ok": False, "error": "unit-only stop error"}}
        with tempfile.TemporaryDirectory(prefix="weather-receipt-test-") as temporary:
            with self.assertRaisesRegex(RuntimeError, "cleanup failed"):
                canary.save_final_run_receipt(Path(temporary), receipt, {}, [1280, 720],
                                              registry, "unit-only-read-key")
            saved = json.loads((Path(temporary) / "run.json").read_text(encoding="utf-8"))
        self.assertEqual("unit-only stop error", saved["stop"]["error"])
        self.assertEqual("unit-only registry error", saved["videoRegistryReadAfter"]["error"])
        self.assertFalse(saved["runtimeAcceptance"])

    def test_final_registry_drift_is_saved_without_restoration(self):
        registry = mock.MagicMock()
        registry.QueryValueEx.side_effect = [(2560, 4), (1440, 4)]
        receipt = {"runtimeAcceptance": True, "stop": {"ok": True}}
        with tempfile.TemporaryDirectory(prefix="weather-receipt-test-") as temporary:
            with self.assertRaisesRegex(RuntimeError, "registry drifted"):
                canary.save_final_run_receipt(Path(temporary), receipt, {}, [1280, 720],
                                              registry, "unit-only-read-key")
            saved = json.loads((Path(temporary) / "run.json").read_text(encoding="utf-8"))
        self.assertTrue(saved["videoRegistryChanged"])
        self.assertFalse(saved["runtimeAcceptance"])
        registry.SetValueEx.assert_not_called()

    def test_expected_check_counts_match_actual_jass_harness(self):
        source = canary.HARNESS.read_text(encoding="utf-8")
        blocks = re.findall(r"(?:if|elseif) wcStage == (\d+) then(.*?)"
                            r"(?=\n    elseif|\n    endif)", source, re.S)
        total = 0
        expected = []
        for stage, body in blocks:
            self.assertEqual(len(expected), int(stage))
            total += len(re.findall(r"call WeatherCanary(?:Check|Error)\(", body))
            expected.append(total)
        self.assertEqual(tuple(expected), canary.EXPECTED_CHECKS)

    def test_complete_synthetic_observation_preserves_evidence_levels(self):
        result = canary.classify(baseline())
        for key, passed in result.items():
            self.assertFalse(passed, key) if key in NON_ACCEPTANCE else self.assertTrue(passed, key)

    def test_owner_live_frame_can_lead_actual_frozen_render(self):
        self.assertTrue(canary.classify(baseline())["fullWeatherAtFrozenInput"])
        row = next(row for row in baseline()["snapshots"] if row["jassStage"] == 4)
        self.assertEqual(row["alignment"]["render"]["liveQueryLagFrames"], 5)

    def test_each_of_six_wrong_or_missing_full_params_refuses_certification(self):
        for field in canary.PARAM_FIELDS:
            for missing in (False, True):
                with self.subTest(field=field, missing=missing):
                    receipt = baseline()
                    def mutate(obs):
                        params = obs["frozenInput"]["params"]
                        params.pop(field) if missing else params.update({field: 0.01})
                    mutate_stage(receipt, 4, mutate)
                    self.assertFalse(canary.classify(receipt)["fullWeatherAtFrozenInput"])

    def test_missing_or_nonfinite_param_refuses(self):
        for value in (float("nan"), float("inf")):
            receipt = baseline()
            mutate_stage(receipt, 4, lambda obs: obs["frozenInput"]["params"].update(haze=value))
            self.assertFalse(canary.classify(receipt)["fullWeatherAtFrozenInput"])

    def test_snow_only_cannot_secretly_enable_haze_or_low_snow(self):
        for field in ("haze", "lowSnow"):
            receipt = baseline()
            mutate_stage(receipt, 1, lambda obs: obs["frozenInput"]["params"].update({field: 0.01}))
            self.assertFalse(canary.classify(receipt)["snowOnlyCommandsRecordedAtFrozenInput"])

    def test_haze_only_requires_zero_snow_and_no_snow_draw(self):
        for mutate in (lambda obs: obs["frozenInput"]["params"].update(snowfall=0.01),
                       lambda obs: obs["render"].update(commandsRecorded=True),
                       lambda obs: obs["render"].update(stage="commands-recorded")):
            receipt = baseline()
            mutate_stage(receipt, 2, mutate)
            self.assertFalse(canary.classify(receipt)["hazeOnlyVolumeAtFrozenInput"])

    def test_low_snow_only_requires_zero_haze(self):
        receipt = baseline()
        mutate_stage(receipt, 3, lambda obs: obs["frozenInput"]["params"].update(haze=0.2))
        self.assertFalse(canary.classify(receipt)["lowSnowOnlyVolumeAtFrozenInput"])

    def test_wrong_stage_name_cannot_override_recorded_flag(self):
        receipt = baseline()
        mutate_stage(receipt, 4, lambda obs: obs["render"].update(stage="pipeline-unavailable"))
        self.assertFalse(canary.classify(receipt)["fullWeatherAtFrozenInput"])

    def test_wrong_particle_count_or_capacity_cannot_certify(self):
        for field, value in (("particleCount", 0), ("particleCount", 100), ("capacity", 65536)):
            receipt = baseline()
            mutate_stage(receipt, 4, lambda obs: obs["render"].update({field: value}))
            self.assertFalse(canary.classify(receipt)["fullWeatherAtFrozenInput"])

    def test_wrong_frame_epoch_id_and_time_cannot_certify(self):
        for part in ("render", "volumetric"):
            changes = (("frameSerial", 1), ("mapEpoch", 0), ("mapEpoch", 99),
                       ("deviceEpoch", 0), ("deviceEpoch", 99))
            if part == "render":
                changes += (("id", 8), ("timeSeconds", 0))
            for field, value in changes:
                with self.subTest(part=part, field=field):
                    receipt = baseline()
                    mutate_stage(receipt, 4, lambda obs: obs[part].update({field: value}))
                    self.assertFalse(canary.classify(receipt)["fullWeatherAtFrozenInput"])

    def test_frozen_phase_time_outside_window_is_not_stage_evidence(self):
        receipt = baseline()
        def mutate(obs):
            obs["frozenInput"]["timeSeconds"] = 1
            obs["render"]["timeSeconds"] = 1
        mutate_stage(receipt, 4, mutate)
        self.assertFalse(canary.classify(receipt)["fullWeatherAtFrozenInput"])

    def test_saved_alignment_bool_cannot_override_raw_mismatch(self):
        receipt = baseline()
        for row in receipt["snapshots"]:
            if row["jassStage"] == 4:
                row["observation"]["render"]["frameSerial"] = 1
        self.assertFalse(canary.classify(receipt)["fullWeatherAtFrozenInput"])

    def test_live_owner_kind_cannot_be_relabelled_as_frozen(self):
        receipt = baseline()
        mutate_stage(receipt, 4, lambda obs: obs["frozenInput"].update(
            observationKind="owner-control-live-state-not-render-input"))
        self.assertFalse(canary.classify(receipt)["fullWeatherAtFrozenInput"])

    def test_pause_requires_paused_frozen_input_not_only_live_clock(self):
        receipt = baseline()
        mutate_stage(receipt, 5, lambda obs: obs["frozenInput"].update(paused=False))
        result = canary.classify(receipt)
        self.assertTrue(result["pausedClockStable"])
        self.assertFalse(result["pausedRenderInputObserved"])

    def test_seek_requires_actual_frozen_time_not_only_jass_return(self):
        receipt = baseline()
        mutate_stage(receipt, 6, lambda obs: obs["frozenInput"].update(timeSeconds=32))
        self.assertFalse(canary.classify(receipt)["seekPausedRenderInputObserved"])

    def test_resumed_and_reenabled_clock_must_progress_in_distinct_frozen_frames(self):
        for stage, field in ((7, "resumedClockProgressAtFrozenInput"),
                             (9, "reenabledClockProgressAtFrozenInput")):
            receipt = baseline()
            def stop_time(obs):
                obs["frozenInput"]["timeSeconds"] = TIMES[stage] + 0.5
                obs["render"]["timeSeconds"] = TIMES[stage] + 0.5
            mutate_stage(receipt, stage, stop_time)
            self.assertFalse(canary.classify(receipt)[field])

    def test_disable_cannot_leave_render_or_volume_active(self):
        for part, field in (("render", "commandsRecorded"), ("volumetric", "compositeSubmitted")):
            receipt = baseline()
            mutate_stage(receipt, 8, lambda obs: obs[part].update({field: True}))
            result = canary.classify(receipt)
            self.assertTrue(result["disabledClockStable"])
            self.assertFalse(result["disabledRenderAndVolumeObserved"])

    def test_disabled_old_draw_frame_cannot_certify_off_transition(self):
        receipt = baseline()
        mutate_stage(receipt, 8, lambda obs: obs["render"].update(frameSerial=1))
        self.assertFalse(canary.classify(receipt)["disabledRenderAndVolumeObserved"])

    def test_destroy_cannot_leave_render_or_frozen_state_active(self):
        for part, field, value in (("render", "commandsRecorded", True),
                                  ("frozenInput", "enabled", True),
                                  ("frozenInput", "id", 7),
                                  ("volumetric", "compositeSubmitted", True)):
            receipt = baseline()
            for stage in (13, 14):
                mutate_stage(receipt, stage, lambda obs: obs[part].update({field: value}))
            result = canary.classify(receipt)
            self.assertTrue(result["destroyedRuntimeObserved"])
            self.assertFalse(result["destroyedRenderAndVolumeObserved"])

    def test_timeline_checks_actual_shortest_angle_and_interpolated_params(self):
        receipt = baseline()
        mutate_stage(receipt, 11, lambda obs: obs["frozenInput"]["params"].update(windAngle=300))
        self.assertFalse(canary.classify(receipt)["timelineInterpolationAtFrozenInput"])
        self.assertAlmostEqual(0, canary.expected_params(11, 3)["windAngle"])

    def test_status_crossing_stage_boundary_is_not_phase_evidence(self):
        receipt = baseline()
        for row in receipt["snapshots"]:
            if row["jassStage"] == 4:
                row["stageAtStatusCompletion"] = 5
        self.assertFalse(canary.classify(receipt)["fullWeatherAtFrozenInput"])

    def test_empty_sample_does_not_pass_by_empty_all(self):
        self.assertFalse(any(canary.classify({}).values()))

    def test_jass_missing_duplicate_errors_zero_checks_and_changed_id_refuse(self):
        for mode in ("missing", "duplicate", "error", "zero-checks", "changed-id"):
            receipt = baseline()
            if mode == "missing":
                receipt["stageReceipts"].pop()
            elif mode == "duplicate":
                receipt["stageReceipts"].append(dict(receipt["stageReceipts"][0]))
            elif mode == "error":
                receipt["stageReceipts"][0]["errors"] = 1
            elif mode == "zero-checks":
                receipt["stageReceipts"][14]["checks"] = 0
            else:
                receipt["stageReceipts"][14]["id"] = 8
            self.assertFalse(canary.classify(receipt)["jassChecksPassed"], mode)

    def test_unpaused_or_other_identity_cannot_certify_stable_live_clock(self):
        for stage, changed, field in ((5, {"paused": False}, "pausedClockStable"),
                                      (8, {"enabled": True}, "disabledClockStable"),
                                      (8, {"id": 8}, "disabledClockStable")):
            receipt = baseline()
            mutate_stage(receipt, stage, lambda obs: obs["runtime"].update(changed))
            self.assertFalse(canary.classify(receipt)[field])

    def test_capture_missing_duplicate_label_failed_dimensions_size_and_hash_refuse(self):
        for changed in ({"stage": 99}, {"label": "wrong"}, {"ok": False},
                        {"dimensions": [1280, 720]}, {"size": 54}, {"sha256": ""}):
            receipt = baseline()
            receipt["captures"][0].update(changed)
            self.assertFalse(canary.classify(receipt)["allCaptures2560x1440"])
        receipt = baseline()
        receipt["captures"] = [copy.deepcopy(receipt["captures"][0]) for _ in canary.CAPTURES]
        self.assertFalse(canary.classify(receipt)["allCaptures2560x1440"])

    def test_capture_phase_crossing_or_wrong_actual_params_cannot_keep_label(self):
        for mode in ("before-stage", "after-stage", "params", "frame", "epoch", "volume-frame"):
            receipt = baseline()
            row = next(row for row in receipt["captures"] if row["stage"] == 4)
            if mode == "before-stage":
                row["stageAtRequest"] = 3
            elif mode == "after-stage":
                row["stageAtCompletion"] = 5
            elif mode == "params":
                row["afterCaptureObservation"]["frozenInput"]["params"]["haze"] = 0
            elif mode == "frame":
                row["afterCaptureObservation"]["render"]["frameSerial"] = 1
            elif mode == "epoch":
                row["afterCaptureObservation"]["frozenInput"]["mapEpoch"] = 99
            else:
                row["afterCaptureObservation"]["volumetric"]["frameSerial"] = 1
            self.assertFalse(canary.classify(receipt)["capturePhasesBracketed"], mode)

    def test_identical_baseline_and_effect_files_are_not_visual_change_evidence(self):
        receipt = baseline()
        for row in receipt["captures"]:
            row["sha256"] = receipt["captures"][0]["sha256"]
        self.assertFalse(canary.classify(receipt)["weatherImagesDifferFromBaseline"])

    def test_health_collector_filters_other_pid_and_preserves_evidence_limit(self):
        class FakeRuntime:
            @staticmethod
            def get_runtime_events(**_kwargs):
                return {"ok": True, "latestId": 3, "events": [
                    {"id": 1, "pid": 99, "msg": "VK_ERROR_DEVICE_LOST"},
                    {"id": 2, "pid": 7, "msg": "weather submitted"},
                    {"id": 3, "pid": 7, "msg": "VK_ERROR_DEVICE_LOST"}]}
        with tempfile.TemporaryDirectory(prefix="weather-health-test-") as temporary:
            result = canary.collect_health(FakeRuntime, "synthetic", Path(temporary), 7)
        self.assertEqual(2, result["eventCount"])
        self.assertEqual(1, result["otherPidEventsIgnored"])
        self.assertEqual(1, len(result["gpuFailureKeywordEvents"]))
        self.assertFalse(result["noRecordedGpuFailureInCollectedEvidence"])
        self.assertFalse(result["gpuCorrectnessCertified"])
        self.assertFalse(result["earlierEvictedDebugHistoryCovered"])

    def test_health_collector_unavailable_does_not_fabricate_clean_gpu(self):
        class FakeRuntime:
            @staticmethod
            def get_runtime_events(**_kwargs):
                return {"ok": False, "error": "unavailable"}
        result = canary.collect_health(FakeRuntime, "synthetic", Path("not-used"), 7)
        self.assertFalse(result["covered"])
        self.assertNotIn("noRecordedGpuFailureInCollectedEvidence", result)
        self.assertFalse(result["gpuCorrectnessCertified"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
