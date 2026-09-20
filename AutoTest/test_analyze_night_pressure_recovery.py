"""Counterexample tests for the validation-b02 strict offline comparator."""
from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

import analyze_night_pressure_recovery as analyzer
import run_snapshot_pressure_canary as runner
from analyze_stage11_budget_census import REASONS, TAG_DIMENSIONS


CAND = "A" * 64
MAP = "B" * 64
GAME = "C" * 64
EXE = "D" * 64
LIVE = "E" * 64


def frozen_manifest(run_id="night-off-run", upload="0"):
    env = {key: "0" for key in runner.HEAVY_FORENSICS_OFF}
    env.update({
        "DXVK_WAR3_INTERNAL_TEST_API": "1",
        "DXVK_WAR3_INTERNAL_EXIT_TEST": "1",
        "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE": "1",
        "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE": "1",
        "DXVK_WAR3_PERF_MONITOR": "1",
        "DXVK_WAR3_PERF_LEVEL": "1",
        "DXVK_WAR3_PERF_AUTO_EXPORT_SEC": "20",
        "DXVK_WAR3_ASYNC_SCREENSHOT": "1",
        "DXVK_WAR3_PERF_HISTORY_FRAMES": "4000",
        "DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB": "384",
        "DXVK_WAR3_PROFILE": "full_default",
        "DXVK_WAR3_SCENARIO": run_id,
        "DISABLE_VULKAN_OBS_CAPTURE": "1",
        "DXVK_WAR3_STAGE11_BUDGET_CENSUS": "1",
        "DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE": upload,
    })
    return {
        "schema": 2,
        "runId": run_id,
        "scenario": run_id,
        "profile": "full_default",
        "matrix": "night-pressure-off-on-v2",
        "resolution": {"width": 2560, "height": 1440},
        "route": runner.FROZEN_ROUTE,
        "isolatedDesktop": True,
        "globalInputUsed": False,
        "candidate": {"sha256": CAND, "size": 36386190},
        "liveRecovery": {"sha256": LIVE, "size": 123},
        "map": {"sha256": MAP, "size": 456},
        "game": {"sha256": GAME, "size": 789},
        "exe": {"sha256": EXE, "size": 1011},
        "env": env,
    }


def normalized_binding(run_id="night-off-run", upload="0", frozen_manifest_sha="A" * 64):
    normalized = runner.validate_frozen_manifest(frozen_manifest(run_id, upload))
    return runner.build_binding_manifest(
        runId=normalized["runId"], scenario=normalized["scenario"],
        candidate=normalized["identities"]["candidate"],
        liveRecovery=normalized["identities"]["liveRecovery"],
        game_map=normalized["identities"]["map"],
        game=normalized["identities"]["game"],
        exe=normalized["identities"]["exe"],
        profile=normalized["profile"], matrix=normalized["matrix"],
        resolution=normalized["resolution"], route=normalized["route"],
        env=normalized["env"], frozenManifestSha256=frozen_manifest_sha)


def make_phases(relief_start=11, relief_frames=100):
    return {"pressureStart": 1, "pressureEnd": 10,
            "reliefStart": relief_start, "reliefEnd": relief_start + relief_frames - 1}


def make_data(relief_frames=100, relief_serial=None, relief_reuse=None,
              relief_partial=None, caster=None, skip=None, failure_counts=None,
              include_census=False, meta_env=None, meta_sha=None,
              scope_frame_serial=0, scope_map_epoch=1, scope_device_epoch=1,
              accumulation_epoch=1):
    columns = list(analyzer.REQUIRED_WORKLOAD_COLUMNS)
    rows = []
    pressure_serial = 100
    for frame in range(1, 11):
        rows.append([frame, frame, pressure_serial, 50, 10, 0, 1, 1, 5, 0, 0])
    serial = pressure_serial
    for index in range(relief_frames):
        frame = 11 + index
        if relief_serial is not None:
            serial_value = relief_serial[index]
        elif index % 8 == 0:
            serial += 1
            serial_value = serial
        else:
            serial_value = serial
        if relief_reuse is not None:
            reuse = relief_reuse[index]
        else:
            reuse = 0 if (relief_serial is None and index % 8 == 0) else 1
        partial = relief_partial[index] if relief_partial is not None else 0
        caster_value = caster[index] if caster is not None else 50
        skip_value = skip[index] if skip is not None else 0
        rows.append([frame, frame, serial_value, caster_value, 10, skip_value, 1, 1, 5, reuse, partial])
    summary = {key: 0 for key in analyzer.CUMULATIVE_FAILURE_KEYS + analyzer.CUMULATIVE_OBSERVABILITY_KEYS + analyzer.LAST_VALUE_KEYS + analyzer.SCOPE_KEYS}
    summary.update({
        "producerSealFrameSerialLast": scope_frame_serial,
        "producerSealMapEpochLast": scope_map_epoch,
        "producerSealDeviceEpochLast": scope_device_epoch,
    })
    if accumulation_epoch is not None:
        summary["producerAccumulationEpoch"] = accumulation_epoch
    if failure_counts:
        summary.update(failure_counts)
    env = meta_env if meta_env is not None else dict(frozen_manifest()["env"])
    data = {
        "frameCount": len(rows),
        "meta": {
            "dllSha256": meta_sha or CAND,
            "dllFileSize": 36386190,
            "runtimeProfile": "full_default",
            "env": dict(env),
        },
        "workloadSeriesColumns": columns,
        "workloadSeries": rows,
        "shadowBudgetSummary": summary,
        "stage11BudgetCensus": {} if include_census else None,
    }
    return data


def phase_marker(name, frame, run_id="night-off-run", pid=1234,
                 perf_frame_epoch=None, producer_accumulation_epoch=7):
    return {"phase": name, "pid": pid, "runId": run_id, "wallUnix": 1000.25,
            "frameDomain": "workload.businessFrameSerial",
            "businessFrameSerial": frame,
            "perfFrameEpoch": (perf_frame_epoch if perf_frame_epoch is not None
                               else frame + 100000),
            "producerAccumulationEpoch": producer_accumulation_epoch,
            "readiness": {"state": "in-game-ready", "frameNumber": frame + 100000}}


def valid_markers():
    return [
        phase_marker("sample-start", 1),
        phase_marker("pressure-start", 2),
        phase_marker("pressure-end", 10),
        phase_marker("relief-start", 12),
        phase_marker("relief-end", 110),
    ]


class PhaseMarkerTests(unittest.TestCase):
    def test_valid_markers_keep_run_pid_and_producer_fields(self):
        phases = analyzer.validate_phase_markers(valid_markers())
        self.assertEqual(phases["runId"], "night-off-run")
        self.assertEqual(phases["pid"], 1234)
        self.assertEqual(phases["producerAccumulationEpoch"], 7)
        self.assertEqual(phases["reliefStart"], 13)
        self.assertEqual(phases["reliefEnd"], 110)
        epochs = list(phases["perfFrameEpochs"].values())
        self.assertEqual(epochs, sorted(epochs))
        self.assertEqual(len(set(epochs)), len(epochs))

    def test_exclusive_start_inclusive_end_boundary(self):
        phases = analyzer.validate_phase_markers(valid_markers())
        self.assertEqual(phases["pressureStart"], 3)
        self.assertEqual(phases["pressureEnd"], 10)
        self.assertEqual(phases["reliefStart"], 13)
        self.assertEqual(phases["reliefEnd"], 110)

    def test_scene_frame_number_alone_is_not_business_domain(self):
        markers = valid_markers()
        for marker in markers:
            marker.pop("businessFrameSerial", None)
            marker.pop("frameDomain", None)
            marker.pop("perfFrameEpoch", None)
            marker.pop("producerAccumulationEpoch", None)
            marker["readiness"]["frameNumber"] = 12345
        with self.assertRaises(analyzer.PhaseMarkerEvidenceError) as raised:
            analyzer.validate_phase_markers(markers)
        self.assertTrue(raised.exception.missing)

    def test_missing_or_false_marker_domain_is_missing(self):
        cases = []
        missing_domain = valid_markers()
        missing_domain[0].pop("frameDomain")
        cases.append(missing_domain)
        missing_serial = valid_markers()
        missing_serial[1].pop("businessFrameSerial")
        cases.append(missing_serial)
        missing_perf = valid_markers()
        missing_perf[2].pop("perfFrameEpoch")
        cases.append(missing_perf)
        missing_epoch = valid_markers()
        missing_epoch[3].pop("producerAccumulationEpoch")
        cases.append(missing_epoch)
        bool_serial = valid_markers()
        bool_serial[4]["businessFrameSerial"] = True
        cases.append(bool_serial)
        zero_epoch = valid_markers()
        zero_epoch[0]["producerAccumulationEpoch"] = 0
        cases.append(zero_epoch)
        for markers in cases:
            with self.assertRaises(analyzer.PhaseMarkerEvidenceError) as raised:
                analyzer.validate_phase_markers(markers)
            self.assertTrue(raised.exception.missing)

    def test_epoch_change_fails_even_all_zero_aggregate(self):
        markers = valid_markers()
        markers[3]["producerAccumulationEpoch"] = 8
        with self.assertRaises(analyzer.PhaseMarkerEvidenceError) as raised:
            analyzer.validate_phase_markers(markers)
        self.assertFalse(raised.exception.missing)

    def test_perf_frame_epoch_must_strictly_increase(self):
        markers = valid_markers()
        markers[3]["perfFrameEpoch"] = markers[2]["perfFrameEpoch"]
        with self.assertRaises(analyzer.PhaseMarkerEvidenceError) as raised:
            analyzer.validate_phase_markers(markers)
        self.assertFalse(raised.exception.missing)

    def test_scene_and_perf_disagree_uses_perf_serial_only(self):
        markers = valid_markers()
        for marker in markers:
            marker["readiness"]["frameNumber"] = 999999
        phases = analyzer.validate_phase_markers(markers)
        self.assertEqual(phases["reliefStart"], 13)
        self.assertEqual(phases["reliefEnd"], 110)

    def test_missing_duplicate_zero_pid_or_run_change_fail(self):
        cases = []
        cases.append(valid_markers()[:-1])
        cases.append(valid_markers() + [phase_marker("relief-end", 111)])
        zero = valid_markers()
        zero[0]["businessFrameSerial"] = 0
        cases.append(zero)
        pid_change = valid_markers()
        pid_change[2]["pid"] = 999
        cases.append(pid_change)
        run_change = valid_markers()
        run_change[3]["runId"] = "other"
        cases.append(run_change)
        for markers in cases:
            with self.assertRaises(analyzer.EvidenceError):
                analyzer.validate_phase_markers(markers)


class RecoveryWindowTests(unittest.TestCase):
    def test_sustained_complete_window_passes(self):
        data = make_data(relief_frames=100)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertEqual(result["verdict"], "pass", result)

    def test_partial_recovery_then_stale_end_fails(self):
        serial = [100]
        reuse = []
        for index in range(100):
            if index < 20 and index % 4 == 0:
                serial.append(serial[-1] + 1)
                reuse.append(0)
            else:
                serial.append(serial[-1])
                reuse.append(1 if index < 20 else 0)
        data = make_data(relief_frames=100, relief_serial=serial[1:], relief_reuse=reuse)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("relief_not_sustained_to_end", result["failures"])

    def test_sticky_reuse_without_fresh_render_fails(self):
        data = make_data(relief_frames=100, relief_serial=[100] * 100,
                         relief_reuse=[1] * 100)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("relief_no_fresh_complete_render", result["failures"])

    def test_zero_placeholder_does_not_reset_fresh_highwater(self):
        serial = [0, 100] * 50
        data = make_data(relief_frames=100, relief_serial=serial,
                         relief_reuse=[1] * 100)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertEqual(result["metrics"]["freshCompleteFrames"], 0)
        self.assertIn("render_serial_decreased", result["failures"])

    def test_true_new_highs_only(self):
        serial = [0, 100] + list(range(101, 199))
        data = make_data(relief_frames=100, relief_serial=serial,
                         relief_reuse=[1] * 100)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertEqual(result["metrics"]["pressureRenderSerialMax"], 100)
        self.assertEqual(result["metrics"]["reliefRenderSerialMax"], 198)
        self.assertEqual(result["metrics"]["freshCompleteFrames"], 98)

    def test_regression_99_then_100_not_new(self):
        serial = [99, 100] * 50
        data = make_data(relief_frames=100, relief_serial=serial,
                         relief_reuse=[1] * 100)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertEqual(result["metrics"]["freshCompleteFrames"], 0)
        self.assertIn("render_serial_decreased", result["failures"])

    def test_normal_monotonic_unchanged_after_highwater_fix(self):
        data = make_data(relief_frames=100)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["metrics"]["freshCompleteFrames"], 13)

    def test_domain_reset_and_missing_frame_still_fail(self):
        reset = make_data(relief_frames=100, relief_serial=[0, 100] * 50,
                          relief_reuse=[1] * 100)
        reset_result = analyzer.evaluate_recovery_window(analyzer.extract_series(reset), make_phases())
        self.assertIn("render_serial_decreased", reset_result["failures"])

        missing = make_data(relief_frames=20)
        missing["workloadSeries"] = [row for row in missing["workloadSeries"]
                                     if row[1] not in (16, 17, 18)]
        missing["frameCount"] = len(missing["workloadSeries"])
        series = analyzer.extract_series(missing)
        union = analyzer.merge_run_series([{
            "path": Path("a.html"), "series": series,
            "aggregate": analyzer.extract_aggregate(missing), "range": (1, 30)}])
        uncovered = analyzer.evaluate_union(union, make_phases(relief_frames=20))["uncovered"]
        self.assertIn("business_frame_coverage_gaps", uncovered)

    def test_partial_publication_is_not_a_pass(self):
        partial = [0] * 100
        partial[50] = 1
        data = make_data(relief_frames=100, relief_partial=partial)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("partial_shadow_publication", result["failures"])

    def test_zero_caster_or_skip_cap_is_not_recovery(self):
        data = make_data(relief_frames=100, caster=[0] * 100)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertIn("caster_zero", result["failures"])
        skip = [0] * 100
        skip[10] = 1
        data = make_data(relief_frames=100, skip=skip)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertIn("caster_cap_skip_nonzero", result["failures"])

    def test_insufficient_relief_window_fails(self):
        data = make_data(relief_frames=20)
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases(relief_frames=20))
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("insufficient_relief_frames", result["failures"])


class AggregateDeltaTests(unittest.TestCase):
    def parsed(self, name, rng, counts, map_epoch=1, accumulation_epoch=1):
        data = make_data(failure_counts=counts, scope_frame_serial=rng[1],
                         scope_map_epoch=map_epoch, accumulation_epoch=accumulation_epoch)
        return {"path": Path(name), "aggregate": analyzer.extract_aggregate(data), "range": rng}

    def test_all_zero_cumulative_failures_pass(self):
        reports = [self.parsed("a.html", (1, 10), {}), self.parsed("b.html", (11, 20), {})]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 20})
        self.assertEqual(result["verdict"], "pass", result)

    def test_all_zero_missing_accumulation_epoch_is_uncovered(self):
        reports = [self.parsed("a.html", (1, 10), {}, accumulation_epoch=None),
                   self.parsed("b.html", (11, 20), {}, accumulation_epoch=None)]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 20})
        self.assertEqual(result["verdict"], "uncovered")
        self.assertIn("cumulative_accumulation_epoch_missing", result["uncovered"])

    def test_all_zero_zero_accumulation_epoch_is_uncovered(self):
        reports = [self.parsed("a.html", (1, 10), {}, accumulation_epoch=0),
                   self.parsed("b.html", (11, 20), {}, accumulation_epoch=0)]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 20})
        self.assertNotEqual(result["verdict"], "pass")
        self.assertIn("cumulative_accumulation_epoch_invalid", result["uncovered"])

    def test_all_zero_epoch_reset_fails(self):
        reports = [self.parsed("a.html", (1, 10), {}, accumulation_epoch=1),
                   self.parsed("b.html", (11, 20), {}, accumulation_epoch=2)]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 20})
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("post_relief_accumulation_epoch_change", result["failures"])

    def test_all_zero_straddling_overlap_report_passes_when_epoch_valid(self):
        reports = [self.parsed("a.html", (1, 15), {}), self.parsed("b.html", (10, 20), {})]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 20})
        self.assertEqual(result["verdict"], "pass", result)

    def test_all_zero_without_report_after_relief_end_is_uncovered(self):
        reports = [self.parsed("a.html", (1, 10), {}), self.parsed("b.html", (11, 19), {})]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 20})
        self.assertNotEqual(result["verdict"], "pass")
        self.assertIn("post_relief_export_before_relief_end", result["uncovered"])

    def test_one_post_report_with_nonzero_failures_is_uncovered(self):
        reports = [self.parsed("a.html", (1, 10), {"framesProducerIncomplete": 1})]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 20})
        self.assertEqual(result["verdict"], "uncovered")
        self.assertIn("post_relief_export_before_relief_end", result["uncovered"])

    def test_clean_nonzero_reports_need_exact_producer_cut_proof(self):
        reports = [
            self.parsed("a.html", (11, 20), {"framesProducerIncomplete": 5}),
            self.parsed("b.html", (21, 30), {"framesProducerIncomplete": 6}),
        ]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 30})
        self.assertEqual(result["verdict"], "uncovered")
        self.assertIn("post_relief_cut_proof_missing", result["uncovered"])

    def test_overlapping_nonzero_reports_need_cut_proof_not_window_rejection(self):
        reports = [
            self.parsed("a.html", (11, 25), {"framesProducerIncomplete": 1}),
            self.parsed("b.html", (20, 30), {"framesProducerIncomplete": 1}),
        ]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 30})
        self.assertEqual(result["verdict"], "uncovered")
        self.assertIn("post_relief_cut_proof_missing", result["uncovered"])
        self.assertNotIn("post_relief_window_not_clean", result["uncovered"])

    def test_phase_report_accumulation_epoch_mismatch_fails(self):
        reports = [
            self.parsed("a.html", (11, 20), {}, accumulation_epoch=2),
            self.parsed("b.html", (21, 30), {}, accumulation_epoch=2),
        ]
        result = analyzer._post_relief_growth(
            reports, {"reliefStart": 11, "reliefEnd": 30, "producerAccumulationEpoch": 1})
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("phase_report_accumulation_epoch_mismatch", result["failures"])

    def test_phase_zero_epoch_is_uncovered(self):
        reports = [self.parsed("a.html", (11, 30), {})]
        result = analyzer._post_relief_growth(
            reports, {"reliefStart": 11, "reliefEnd": 30, "producerAccumulationEpoch": 0})
        self.assertNotEqual(result["verdict"], "pass")
        self.assertIn("phase_producer_accumulation_epoch_invalid", result["uncovered"])

    def test_last_gauge_is_never_subtracted(self):
        reports = [
            self.parsed("a.html", (11, 20), {"drawTimeSnapshotPageResidentBytesLast": 100}),
            self.parsed("b.html", (21, 30), {"drawTimeSnapshotPageResidentBytesLast": 200}),
        ]
        result = analyzer._post_relief_growth(reports, {"reliefStart": 11, "reliefEnd": 30})
        self.assertEqual(result["verdict"], "pass")

class CasterComparisonTests(unittest.TestCase):
    def test_equal_frame_count_but_lower_caster_fails(self):
        baseline = analyzer.extract_series(make_data(relief_frames=100, caster=[50] * 100))
        candidate = analyzer.extract_series(make_data(relief_frames=100, caster=[40] * 100))
        result = analyzer.compare_caster_coverage(baseline, candidate, make_phases(), make_phases())
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(any("caster_reduced" in item for item in result["failures"]))
        self.assertFalse(result["perObjectCompletenessProven"])

    def test_frame_count_mismatch_is_uncovered_not_pass(self):
        baseline = analyzer.extract_series(make_data(relief_frames=100))
        candidate = analyzer.extract_series(make_data(relief_frames=90))
        result = analyzer.compare_caster_coverage(baseline, candidate, make_phases(), make_phases(relief_frames=90))
        self.assertEqual(result["verdict"], "uncovered")
        self.assertTrue(any("uncomparable_frame_count" in item for item in result["uncovered"]))

    def test_equal_statistical_series_passes_only_as_reference(self):
        baseline = analyzer.extract_series(make_data(relief_frames=100))
        candidate = analyzer.extract_series(make_data(relief_frames=100))
        result = analyzer.compare_caster_coverage(baseline, candidate, make_phases(), make_phases())
        self.assertEqual(result["verdict"], "pass")
        self.assertFalse(result["perObjectCompletenessProven"])


class ReportGateTests(unittest.TestCase):
    def binding(self):
        return analyzer._manifest_binding(normalized_binding())

    def test_missing_report_env_is_uncovered_not_silent_pass(self):
        data = make_data(meta_env={"DXVK_WAR3_PROFILE": "full_default"})
        meta = analyzer._meta_binding(data, self.binding())
        self.assertFalse(meta["ok"])
        self.assertIn("DXVK_WAR3_SCENARIO", meta["missing"])
        result = analyzer.evaluate_report(data, self.binding(), make_phases())
        self.assertNotEqual(result["verdict"], "pass")
        self.assertTrue(any("report_meta_env_coverage_missing" in item for item in result["uncovered"]))

    def test_candidate_mismatch_fails(self):
        data = make_data(meta_sha="F" * 64)
        with self.assertRaises(analyzer.EvidenceError):
            analyzer._meta_binding(data, self.binding())

    def test_census_missing_or_invalid_fails_when_required(self):
        missing = analyzer.evaluate_report(make_data(), self.binding(), make_phases(), require_census=True)
        self.assertIn("census_missing", missing["failures"])
        invalid = analyzer.evaluate_report(make_data(include_census=True), self.binding(), make_phases(), require_census=True)
        self.assertTrue(any(item.startswith("census_invalid") for item in invalid["failures"]))



class RunUnionAndScopeTests(unittest.TestCase):
    def item(self, name, data, rng):
        return {"path": Path(name), "series": analyzer.extract_series(data),
                "aggregate": analyzer.extract_aggregate(data), "range": rng}

    def test_identical_duplicate_frames_dedup(self):
        data = make_data(relief_frames=20)
        one = self.item("a.html", data, (1, 30))
        two = self.item("b.html", make_data(relief_frames=20), (1, 30))
        union = analyzer.merge_run_series([one, two])
        self.assertEqual(union["rowCount"], 30)
        self.assertEqual(union["duplicateFramesDeduplicated"], 30)

    def test_conflicting_duplicate_frames_fail(self):
        one = self.item("a.html", make_data(relief_frames=20), (1, 30))
        data = make_data(relief_frames=20)
        data["workloadSeries"][5][data["workloadSeriesColumns"].index("replayCasterCount")] = 999
        two = self.item("b.html", data, (1, 30))
        with self.assertRaises(analyzer.EvidenceError):
            analyzer.merge_run_series([one, two])

    def test_coverage_gap_is_uncovered_not_pass(self):
        data = make_data(relief_frames=100)
        rows = data["workloadSeries"]
        data["workloadSeries"] = rows[:20] + rows[25:]  # remove business frames 21..25
        data["frameCount"] = len(data["workloadSeries"])
        series = analyzer.extract_series(data)
        union = analyzer.merge_run_series([{"path": Path("a.html"), "series": series,
                                            "aggregate": analyzer.extract_aggregate(data),
                                            "range": (1, 110)}])
        result = analyzer.evaluate_union(union, make_phases())
        self.assertNotEqual(result["verdict"], "pass")
        self.assertTrue(any("business_frame_coverage_gaps" in item for item in result["uncovered"]))

    def test_valid_tail_conjuncts_receiver_and_caster(self):
        data = make_data(relief_frames=100)
        columns = data["workloadSeriesColumns"]
        receiver = columns.index("shadowTaaReceiverExecuted")
        data["workloadSeries"][-1][receiver] = 0
        result = analyzer.evaluate_recovery_window(analyzer.extract_series(data), make_phases())
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(any("relief_not_sustained_to_end" in item or "relief_last_frame_invalid" in item
                            for item in result["failures"]))

    def test_scope_change_fails_even_when_counters_zero(self):
        data_a = make_data(failure_counts={})
        data_b = make_data(scope_map_epoch=2)
        parsed = [
            {"path": Path("a.html"), "aggregate": analyzer.extract_aggregate(data_a), "range": (11, 20)},
            {"path": Path("b.html"), "aggregate": analyzer.extract_aggregate(data_b), "range": (21, 30)},
        ]
        result = analyzer._post_relief_growth(parsed, {"reliefStart": 11, "reliefEnd": 30})
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(any("scope_change" in item for item in result["failures"]))

    def test_missing_accumulation_epoch_is_uncovered(self):
        data_a = make_data(failure_counts={"framesProducerIncomplete": 1},
                           accumulation_epoch=None)
        data_b = make_data(failure_counts={"framesProducerIncomplete": 1},
                           accumulation_epoch=None)
        parsed = [
            {"path": Path("a.html"), "aggregate": analyzer.extract_aggregate(data_a),
             "range": (11, 20)},
            {"path": Path("b.html"), "aggregate": analyzer.extract_aggregate(data_b),
             "range": (21, 30)},
        ]
        result = analyzer._post_relief_growth(parsed, {"reliefStart": 11, "reliefEnd": 30})
        self.assertEqual(result["verdict"], "uncovered")
        self.assertIn("cumulative_accumulation_epoch_missing", result["uncovered"])

    def test_counter_decrease_is_reset_not_pass(self):
        parsed = [
            {"path": Path("a.html"), "aggregate": analyzer.extract_aggregate(
                make_data(failure_counts={"framesProducerIncomplete": 5}, scope_frame_serial=20)),
             "range": (11, 20)},
            {"path": Path("b.html"), "aggregate": analyzer.extract_aggregate(
                make_data(failure_counts={"framesProducerIncomplete": 4}, scope_frame_serial=30)),
             "range": (21, 30)},
        ]
        result = analyzer._post_relief_growth(parsed, {"reliefStart": 11, "reliefEnd": 30})
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(any("counter_reset_or_scope_change" in item for item in result["failures"]))

def _v2_sample(device=1, map_epoch=1, device_epoch=1, frame=10, stage=0, hits=0):
    return {
        "deviceIdentity": device,
        "mapEpoch": map_epoch,
        "deviceEpoch": device_epoch,
        "frame": frame,
        "stage": stage,
        "errors": 0,
        "cap": 4096,
        "activeResident": 4096,
        "capacityRejects": 0,
        "uploadRangeHits": hits,
        "sampleCpuUs": 0,
        "entryCount": 1,
        "sliceCount": 2,
        "complete": True,
        "unknownCounts": [0] * len(REASONS),
        "unknownPositionBytes": [0] * len(REASONS),
        "pages": [{
            "id": 1,
            "active": True,
            "capacity": 4096,
            "used": 2048,
            "notReferencedByCacheBytes": 512,
            "tail": 2048,
            "references": 2,
            "staticReferences": 1,
            "tagUnknownReferences": 0,
            "staticTaggedUnionBytes": 1024,
            "notStaticTaggedUnionBytes": 1024,
            "staticTagOnlyBytes": 512,
            "notStaticTagOnlyBytes": 512,
            "mixedTagOverlapBytes": 512,
            "failedRetainedUnionBytes": 1024,
            "touchedUnionBytes": 1024,
            "touchedAndFailedUnionBytes": 512,
            "touchedOrFailedUnionBytes": 1536,
            "tagUnknownUnionBytes": 0,
            "pageReferences": 0,
            "knownAgeReferences": 0,
            "minAccessAge": 0,
            "maxAccessAge": 0,
            "owned": [1024, 0, 0, 512, 0],
        }],
    }


def _v2_census(hits_sequence):
    return {
        "schema": 2,
        "enabled": True,
        "scope": "retained-page-and-cache-slices",
        "physicalBackingComplete": False,
        "gpuCompletionKnown": False,
        "ownerCategories": ["touched", "failed", "recentCache", "coldCache", "retired"],
        "unknownReasons": REASONS,
        "tagDimensions": TAG_DIMENSIONS,
        "samples": [
            _v2_sample(frame=10 + index * 10, stage=index, hits=hits)
            for index, hits in enumerate(hits_sequence)
        ],
    }


def _p2a_data(hits_sequence):
    data = make_data()
    data["stage11BudgetCensus"] = _v2_census(hits_sequence)
    return data


def _p2a_aggregate(hits_sequence):
    return analyzer.extract_aggregate(_p2a_data(hits_sequence))


class P2aCoverageTests(unittest.TestCase):
    def test_repeated_prefix_and_reordered_reports_canonicalize(self):
        baseline = [_p2a_aggregate([0])]
        prefix = [0, 5]
        later = [0, 5, 9]
        candidate = [_p2a_aggregate(later), _p2a_aggregate(prefix)]
        result = analyzer.compare_p2a_coverage(baseline, candidate)
        self.assertNotIn("p2a_uploadRangeHits_decreased", result["failures"])
        self.assertFalse(result["benefitProven"])
        self.assertIn("p2a_benefit_not_derived_from_totals", result["uncovered"])

    def test_conflicted_same_cut_fails(self):
        candidate = [_p2a_aggregate([5]), _p2a_aggregate([6])]
        result = analyzer.compare_p2a_coverage([_p2a_aggregate([0])], candidate)
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("p2a_census_cut_conflict", result["failures"])

    def test_true_later_decrease_fails(self):
        result = analyzer.compare_p2a_coverage(
            [_p2a_aggregate([0])], [_p2a_aggregate([6, 4])])
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("p2a_uploadRangeHits_decreased", result["failures"])

    def test_empty_samples_is_uncovered(self):
        result = analyzer.compare_p2a_coverage(
            [_p2a_aggregate([0])], [_p2a_aggregate([])])
        self.assertEqual(result["verdict"], "uncovered")
        self.assertIn("p2a_census_empty_samples", result["uncovered"])

    def test_mixed_valid_and_missing_reports_visible(self):
        missing = analyzer.extract_aggregate(make_data())
        result = analyzer.compare_p2a_coverage(
            [_p2a_aggregate([0]), missing], [_p2a_aggregate([1])])
        self.assertIn("p2a_census_missing_reports", result["uncovered"])
        self.assertNotEqual(result["verdict"], "pass")

    def test_strict_reader_rejects_zero_identity(self):
        raw = _v2_census([1])
        raw["samples"][0]["deviceIdentity"] = 0
        data = make_data()
        data["stage11BudgetCensus"] = raw
        aggregate = analyzer.extract_aggregate(data)
        self.assertTrue(aggregate["censusError"])
        result = analyzer.compare_p2a_coverage([aggregate], [aggregate])
        self.assertIn("p2a_census_invalid_reports", result["uncovered"])

    def test_strict_reader_rejects_invalid_uint(self):
        raw = _v2_census([1])
        raw["samples"][0]["uploadRangeHits"] = -1
        data = make_data()
        data["stage11BudgetCensus"] = raw
        aggregate = analyzer.extract_aggregate(data)
        self.assertTrue(aggregate["censusError"])
        result = analyzer.compare_p2a_coverage([aggregate], [aggregate])
        self.assertIn("p2a_census_invalid_reports", result["uncovered"])

    def test_binding_roles_are_explicit(self):
        result = analyzer.compare_p2a_coverage(
            [_p2a_aggregate([0])], [_p2a_aggregate([1])],
            {"env": {"DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE": "1"}},
            {"env": {"DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE": "0"}})
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("p2a_baseline_not_off", result["failures"])
        self.assertIn("p2a_candidate_not_on", result["failures"])

    def test_positive_hit_is_not_geometry_or_page_proof(self):
        result = analyzer.compare_p2a_coverage(
            [_p2a_aggregate([0])], [_p2a_aggregate([1])])
        self.assertFalse(result["benefitProven"])
        self.assertFalse(result["visualRecoveryProven"])
        self.assertIn("p2a_benefit_not_derived_from_totals", result["uncovered"])

class CompareRunsPairUnionTests(unittest.TestCase):
    def _base_data(self, upload):
        return make_data(relief_frames=100,
                         meta_env=dict(frozen_manifest(upload=upload)["env"]))

    def _subset(self, data, first, last):
        out = copy.deepcopy(data)
        rows = [row for row in out["workloadSeries"] if first <= row[1] <= last]
        out["workloadSeries"] = rows
        out["frameCount"] = len(rows)
        return out

    def _write_run(self, root, name, upload, reports):
        run_dir = Path(root) / name
        run_dir.mkdir(parents=True)
        sidecar = run_dir / "frozen-manifest.json"
        sidecar.write_text('{"frozen": true}', encoding="utf-8")
        binding = normalized_binding(run_id="night-off-run", upload=upload,
                                     frozen_manifest_sha=analyzer.identity(sidecar)["sha256"])
        (run_dir / "binding.json").write_text(json.dumps(binding, sort_keys=True), encoding="utf-8")
        for index, data in enumerate(reports):
            (run_dir / ("report_%d.html" % index)).write_text(
                "const data = " + json.dumps(data, sort_keys=True) + ";", encoding="utf-8")
        return run_dir

    def _pair(self, root, baseline_reports, candidate_reports):
        phases = {"pressureStart": 1, "pressureEnd": 10,
                  "reliefStart": 11, "reliefEnd": 70}
        phases_path = Path(root) / "phases.json"
        phases_path.write_text(json.dumps(phases), encoding="utf-8")
        baseline = self._write_run(root, "baseline", "0", baseline_reports)
        candidate = self._write_run(root, "candidate", "1", candidate_reports)
        return analyzer.compare_runs(baseline, candidate, phases_path=phases_path)

    def test_overlapping_reports_plus_exit_tail_uses_union(self):
        with tempfile.TemporaryDirectory() as tmp:
            base_b = self._base_data("0")
            base_c = self._base_data("1")
            result = self._pair(tmp,
                                [self._subset(base_b, 1, 40), self._subset(base_b, 35, 90)],
                                [self._subset(base_c, 1, 40), self._subset(base_c, 35, 90)])
            self.assertEqual(result["casterCoverage"]["verdict"], "pass", result["casterCoverage"])
            self.assertNotIn("caster_coverage_unavailable", result["uncovered"])
            self.assertEqual(result["baseline"]["union"]["coverageGaps"], [])
            self.assertEqual(result["candidate"]["union"]["coverageGaps"], [])

    def test_conflicting_duplicate_keeps_root_fail_and_marks_coverage_unavailable(self):
        with tempfile.TemporaryDirectory() as tmp:
            base_b = self._base_data("0")
            base_c = self._base_data("1")
            conflict = self._subset(base_b, 25, 90)
            for row in conflict["workloadSeries"]:
                if row[1] == 30:
                    row[3] = 999
            result = self._pair(tmp,
                                [self._subset(base_b, 1, 40), conflict],
                                [self._subset(base_c, 1, 40), self._subset(base_c, 35, 90)])
            self.assertEqual(result["verdict"], "fail")
            self.assertTrue(any("run_union:" in item for item in result["failures"]))
            self.assertTrue(any("caster_coverage_unavailable" in item for item in result["uncovered"]))
            self.assertNotEqual(result["casterCoverage"]["verdict"], "pass")

    def test_genuine_phase_gap_is_not_filled(self):
        with tempfile.TemporaryDirectory() as tmp:
            base_b = self._base_data("0")
            base_c = self._base_data("1")
            result = self._pair(tmp,
                                [self._subset(base_b, 1, 30), self._subset(base_b, 35, 90)],
                                [self._subset(base_c, 1, 30), self._subset(base_c, 35, 90)])
            self.assertNotEqual(result["verdict"], "pass")
            self.assertTrue(any("business_frame_coverage_gaps" in item for item in result["uncovered"]))
            self.assertNotEqual(result["casterCoverage"]["verdict"], "fail")


if __name__ == "__main__":
    unittest.main()
