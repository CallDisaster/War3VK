"""Adversarial pure tests of the actual stress classifier, never a launcher."""
from __future__ import annotations

import copy
from pathlib import Path
import re
import tempfile
import unittest

import weather_stress_contract_20261010 as suite


def make_receipt() -> dict:
    """Synthetic typed evidence, not a claimed game/driver/pixel receipt."""
    stages = []
    samples = []
    next_frame = 100
    public_id = 1
    history = 1
    for stage in range(suite.FINAL_STAGE + 1):
        expected = suite.expected_stage(stage)
        old_id = public_id if expected["rebuilt"] or stage == 68 else 0
        if expected["rebuilt"]:
            public_id += 1
            history += 2
        if expected["played"]:
            history += 1
        if stage == 68:
            public_id = 0
            history += 1
        seconds = (30.0 if stage == 2 else 60.0 if stage == 3 else
                   4.0 if 4 <= stage <= 67 and stage % 2 else 0.0)
        stages.append({
            "stage": stage, "checks": suite.expected_checks(stage), "errors": 0,
            "postfxRefusalCode": suite.POSTFX_UNSUPPORTED_FEATURE,
            "id": public_id, "timeSeconds": seconds,
            "querySeconds": 4.0 if 4 <= stage <= 67 and stage % 2 else 0.0,
            "cycle": expected["cycle"], "weatherEnabled": expected["weatherEnabled"],
            "sunEnabled": expected["sunEnabled"], "ordinaryEnabled": expected["ordinaryEnabled"],
            "played": expected["played"], "oldId": old_id, "oldRejected": expected["staleWriteRefused"],
            "recordsInFile": 1})
        if stage == 69:
            continue
        for index in range(4 if expected["weatherEnabled"] or stage == 0 else 2):
            next_frame += 10
            observed_seconds = seconds + (0.5 + index * 0.2 if expected["weatherEnabled"] else 0.0)
            domain = dict(frameSerial=next_frame, mapEpoch=2, deviceEpoch=3)
            runtime = {
                **domain, "observationKind": "owner-control-live-state-not-render-input",
                "enabled": expected["weatherEnabled"], "paused": False,
                "id": public_id, "historyRevision": history, "timeSeconds": observed_seconds,
                "params": dict(suite.PARAMS)}
            frozen = {
                **domain, "observationKind": "render-consumed-value-snapshot",
                "enabled": expected["weatherEnabled"], "paused": False,
                "id": public_id if expected["weatherEnabled"] else 0,
                "historyRevision": history, "timeSeconds": observed_seconds if expected["weatherEnabled"] else 0.0,
                "params": dict(suite.PARAMS)}
            render = {
                **domain, "id": frozen["id"], "timeSeconds": frozen["timeSeconds"],
                "commandsRecorded": expected["weatherEnabled"],
                "stage": "commands-recorded" if expected["weatherEnabled"] else "disabled",
                "resourceBytes": 28 * 1024 * 1024 if expected["weatherEnabled"] else 0,
                "capacity": 16384, "particleCount": int(16384 * suite.PARAMS["snowfall"])}
            volume = {
                **domain, "compositeSubmitted": expected["weatherEnabled"],
                "stage": "composite-submitted" if expected["weatherEnabled"] else "disabled",
                "resourceBytes": 84 * 1024 * 1024 if stage else 0,
                "width": 2560 if stage else 0, "height": 1440 if stage else 0}
            ordinary = {
                **domain, "compositeSubmitted": expected["ordinaryEnabled"],
                "stage": "composite-submitted" if expected["ordinaryEnabled"] else "disabled",
                "resourceBytes": None, "currentResourceAccountingCovered": False,
                "width": 2560 if expected["ordinaryEnabled"] else 0,
                "height": 1440 if expected["ordinaryEnabled"] else 0}
            settings = {
                "observationKind": "owner-settings-live-state-not-render-input",
                "sunEnabled": expected["sunEnabled"],
                "ordinaryVolumetricEnabled": expected["ordinaryEnabled"],
                "globalMediumEnabled": True, "volumetricDensity": 0.12, "volumetricBackend": 2}
            samples.append({"jassStage": stage, "stageAtStatusCompletion": stage,
                            "observation": {"runtime": runtime, "frozenInput": frozen,
                                            "render": render, "volumetric": volume,
                                            "ordinaryVolumetric": ordinary,
                                            "authorSettings": settings}})
    captures = []
    for stage, label in suite.CAPTURES.items():
        observations = [sample["observation"] for sample in samples if sample["jassStage"] == stage]
        captures.append({
            "stage": stage, "label": label, "ok": True, "dimensions": [2560, 1440],
            "stageAtRequest": stage, "stageAtCompletion": stage,
            "beforeCaptureObservation": copy.deepcopy(observations[0]),
            "afterCaptureObservation": copy.deepcopy(observations[-1])})
    return {"stageReceipts": stages, "snapshots": samples, "captures": captures}


class StressContractTests(unittest.TestCase):
    def setUp(self):
        self.receipt = make_receipt()

    def verdict(self):
        return suite.classify(self.receipt)

    def stage_samples(self, stage):
        return [sample["observation"] for sample in self.receipt["snapshots"]
                if sample["jassStage"] == stage]

    def test_complete_synthetic_receipt_passes_only_supported_claims(self):
        verdict = self.verdict()
        self.assertEqual(len(verdict), 15)
        for name, result in verdict.items():
            with self.subTest(name=name):
                self.assertIs(result, name not in suite.NON_ACCEPTANCE)

    def test_empty_evidence_is_not_vacuous_success(self):
        verdict = suite.classify({})
        for name, result in verdict.items():
            with self.subTest(name=name):
                self.assertFalse(result)

    def test_schedule_is_bounded_and_has_eight_rebuilds(self):
        schedule = [suite.expected_stage(stage) for stage in range(69)]
        self.assertEqual(sum(row["rebuilt"] for row in schedule), 8)
        self.assertEqual(sum(row["weatherEnabled"] for row in schedule[4:68]), 32)
        self.assertEqual(25 + sum(row["duration"] for row in schedule), 397)
        self.assertLess(397, 480)
        self.assertEqual(suite.MAX_RUN_SECONDS, 600)
        self.assertEqual(suite.expected_checks(69), 1083)

    def test_invalid_schedule_stage_is_refused(self):
        for value in (-1, 70):
            with self.subTest(value=value), self.assertRaises(ValueError):
                suite.expected_stage(value)

    def test_harness_paths_use_jass_escaped_backslashes(self):
        source = suite.HARNESS.read_text(encoding="utf-8")
        self.assertIn(r'PreloadGenEnd("WarVK\\Temp\\weather_stress_stage_', source)
        self.assertIn(r'"Environment\\DNC\\DNCLordaeron\\DNCLordaeronTerrain\\', source)
        self.assertNotRegex(source, r'(?<!\\)\\(?!\\)')

    def test_harness_uses_real_play_query_and_valid_different_stale_write(self):
        source = suite.HARNESS.read_text(encoding="utf-8")
        self.assertIn("set wsStartQuery = WarVKGetWeatherTime(wsId)", source)
        self.assertIn("wsStartQuery >= 0.0 and wsStartQuery < 0.08", source)
        self.assertIn("wsStartQuery >= 3.5 and wsStartQuery < 6.0", source)
        self.assertIn("WarVKSetSnowWeatherTarget(wsOldId, 0.2, 200.0, 20.0", source)
        self.assertIn("set wsOldRejected = WarVKGetLastErrorCode() != 0", source)
        self.assertNotIn("ReconstructGpu", source)

    def test_each_setting_and_configuration_call_checks_error_immediately(self):
        source = suite.HARNESS.read_text(encoding="utf-8")
        for function, expected_calls in (("WeatherStressOrdinary", 9),
                                          ("WeatherStressCreate", 6)):
            with self.subTest(function=function):
                body = source.split("function " + function + " takes")[1].split("endfunction")[0]
                lines = [line.strip() for line in body.splitlines()
                         if line.strip() and not line.strip().startswith("//")]
                calls = [index for index, line in enumerate(lines) if line.startswith("call WarVK")
                         and line != "call WarVKClearError()"]
                self.assertEqual(len(calls), expected_calls)
                for index in calls:
                    if lines[index].startswith("call WarVKSetPostfxEnabled("):
                        self.assertEqual(lines[index + 1], "set wsPostfxRefusalCode = WarVKGetLastErrorCode()")
                        self.assertIn("wsPostfxRefusalCode == 18", lines[index + 2])
                        self.assertEqual(lines[index + 3], "call WarVKClearError()")
                    else:
                        self.assertTrue(lines[index + 1].startswith("call WeatherStressError("))
        self.assertIn('set wsId = WarVKCreateSnowWeather(23463)\n'
                      '    call WeatherStressError("create")', source)
        self.assertIn('set wsStartQuery = WarVKGetWeatherTime(wsId)\n'
                      '    call WeatherStressError("play-query")', source)
        self.assertIn('set alive = WarVKIsWeatherAlive(wsId)\n'
                      '        call WeatherStressError("receipt-alive-query")', source)
        for label in ("create-alive-query", "rebuild-alive-query", "tail-alive-query"):
            self.assertIn('call WeatherStressError("' + label + '")', source)

    def test_missing_stage_and_duplicate_stage_fail_checks(self):
        self.receipt["stageReceipts"].pop(37)
        self.assertFalse(self.verdict()["allStressJassStagesAndChecks"])
        self.receipt = make_receipt()
        self.receipt["stageReceipts"][37] = copy.deepcopy(self.receipt["stageReceipts"][36])
        self.assertFalse(self.verdict()["allStressJassStagesAndChecks"])

    def test_fake_checks_and_actual_error_fail(self):
        for key, value in (("checks", 9999), ("errors", 1), ("recordsInFile", 2)):
            with self.subTest(key=key):
                self.receipt = make_receipt()
                self.receipt["stageReceipts"][16][key] = value
                self.assertFalse(self.verdict()["allStressJassStagesAndChecks"])

    def test_unsupported_postfx_refusal_must_have_exact_actual_code(self):
        for value in (0, 17, 19, None, True, 18.0):
            with self.subTest(value=value):
                self.receipt = make_receipt()
                self.receipt["stageReceipts"][34]["postfxRefusalCode"] = value
                self.assertFalse(self.verdict()["allStressJassStagesAndChecks"])
                self.assertFalse(self.verdict()["newInstanceParamsSurviveOldIdRefusals"])
        self.receipt = make_receipt()
        del self.receipt["stageReceipts"][34]["postfxRefusalCode"]
        self.assertFalse(self.verdict()["allStressJassStagesAndChecks"])
        header = Path(__file__).parents[1] / "src/d3d9/war3/japi/war3_japi_v1.h"
        self.assertRegex(header.read_text(encoding="utf-8"), r"UnsupportedFeature\s*=\s*18")

    def test_bad_immediate_play_query_fails(self):
        self.receipt["stageReceipts"][10]["querySeconds"] = 9.73
        self.assertFalse(self.verdict()["allStressJassStagesAndChecks"])
        self.assertFalse(self.verdict()["newInstanceParamsSurviveOldIdRefusals"])

    def test_wrong_id_or_unrejected_stale_id_fails(self):
        for key, value in (("oldRejected", False), ("oldId", 9999), ("id", 1)):
            with self.subTest(key=key):
                self.receipt = make_receipt()
                self.receipt["stageReceipts"][10][key] = value
                self.assertFalse(self.verdict()["allStressJassStagesAndChecks"])

    def test_old_write_mutates_new_owner_even_three_good_frames_fail(self):
        self.stage_samples(10)[3]["runtime"]["params"]["snowfall"] = 0.2
        self.assertFalse(self.verdict()["newInstanceParamsSurviveOldIdRefusals"])
        self.assertFalse(self.verdict()["weatherClocksAndHistoriesConsistent"])

    def test_three_good_frames_cannot_hide_bad_frozen_current_params(self):
        self.stage_samples(34)[-1]["frozenInput"]["params"]["snowfall"] = 0.2
        self.assertFalse(self.verdict()["newInstanceParamsSurviveOldIdRefusals"])

    def test_missing_and_nan_params_cannot_pass(self):
        for location in ("runtime", "frozenInput"):
            for value in (None, float("nan"), float("inf")):
                with self.subTest(location=location, value=value):
                    self.receipt = make_receipt()
                    for observation in self.stage_samples(34):
                        observation[location]["params"]["haze"] = value
                    self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])
            self.receipt = make_receipt()
            for observation in self.stage_samples(34):
                del observation[location]["params"]["haze"]
            self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])

    def test_wrong_draw_frame_refused_despite_stored_alignment_true(self):
        for observation in self.stage_samples(34):
            observation["render"]["frameSerial"] -= 1
            observation["frameAlignment"] = {"render": {"sameFrozenFrame": True},
                                              "volumetric": {"sameFrozenFrame": True}}
        self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])
        self.assertFalse(self.verdict()["all32EnabledRecoveriesObserved"])

    def test_wrong_volume_epoch_refused(self):
        for observation in self.stage_samples(34):
            observation["volumetric"]["deviceEpoch"] += 1
        self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])

    def test_wrong_frozen_id_and_history_refused(self):
        for key in ("id", "historyRevision"):
            with self.subTest(key=key):
                self.receipt = make_receipt()
                for observation in self.stage_samples(34):
                    observation["frozenInput"][key] += 1
                self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])

    def test_wrong_draw_time_refused(self):
        for observation in self.stage_samples(34):
            observation["render"]["timeSeconds"] += 0.25
        self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])

    def test_wrong_ordinary_same_frame_or_enabled_refused(self):
        for key, value in (("frameSerial", 1), ("mapEpoch", 0),
                           ("compositeSubmitted", False), ("width", 1280)):
            with self.subTest(key=key):
                self.receipt = make_receipt()
                for observation in self.stage_samples(1):
                    observation["ordinaryVolumetric"][key] = value
                self.assertFalse(self.verdict()["sunOrdinaryWeatherCoexistence"])

    def test_baseline_requires_actual_ordinary_frame_domain_and_dimensions(self):
        stages = {row["stage"]: row for row in self.receipt["stageReceipts"]}
        for key, value in (("frameSerial", 0), ("deviceEpoch", 0),
                           ("width", 0), ("stage", "not-submitted")):
            with self.subTest(key=key):
                observation = copy.deepcopy(self.stage_samples(0)[0])
                observation["ordinaryVolumetric"][key] = value
                self.assertFalse(suite.capture_ready(observation, 0, stages))

    def test_duplicate_actual_frame_cannot_count_three_samples(self):
        for observation in self.stage_samples(34):
            for key in ("runtime", "frozenInput", "render", "volumetric", "ordinaryVolumetric"):
                observation[key]["frameSerial"] = 20000
        self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])
        self.assertFalse(self.verdict()["all32EnabledRecoveriesObserved"])

    def test_stage_bracket_mismatch_cannot_supply_coverage(self):
        for sample in self.receipt["snapshots"]:
            if sample["jassStage"] == 34:
                sample["stageAtStatusCompletion"] = 35
        self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])

    def test_off_drain_rejects_sticky_old_draw_or_old_frame_or_bytes(self):
        for fault in ("sticky", "old-frame", "bytes"):
            with self.subTest(fault=fault):
                self.receipt = make_receipt()
                for observation in self.stage_samples(35):
                    if fault == "sticky":
                        observation["render"]["commandsRecorded"] = True
                        observation["render"]["stage"] = "commands-recorded"
                    elif fault == "old-frame":
                        for key in ("frozenInput", "render", "volumetric"):
                            observation[key]["frameSerial"] = 1
                    else:
                        observation["render"]["resourceBytes"] = 32
                self.assertFalse(self.verdict()["all32OffDrainsObserved"])

    def test_missing_off_drain_is_uncovered(self):
        self.receipt["snapshots"] = [sample for sample in self.receipt["snapshots"]
                                     if sample["jassStage"] != 35]
        self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])
        self.assertFalse(self.verdict()["all32OffDrainsObserved"])

    def test_tail_preserves_existing_last_off_consumption_but_cannot_replace_it(self):
        last_off = self.stage_samples(67)[-1]
        for observation in self.stage_samples(68):
            for owner in ("frozenInput", "render", "volumetric"):
                observation[owner] = copy.deepcopy(last_off[owner])
        self.assertTrue(self.verdict()["all32OffDrainsObserved"])
        self.receipt["snapshots"] = [sample for sample in self.receipt["snapshots"]
                                     if sample["jassStage"] != 67]
        self.assertFalse(self.verdict()["all32OffDrainsObserved"])

    def test_backing_growth_same_known_key_fails(self):
        for owner in ("volumetric", "render"):
            with self.subTest(owner=owner):
                self.receipt = make_receipt()
                self.stage_samples(34)[-1][owner]["resourceBytes"] += 1024
                self.assertFalse(self.verdict()["sameKeyCurrentWeatherBackingStable"])

    def test_lagging_frame_cannot_hide_current_same_owner_backing_growth(self):
        observation = self.stage_samples(34)[-1]
        observation["render"]["resourceBytes"] += 1024
        observation["render"]["frameSerial"] -= 1
        self.assertFalse(self.verdict()["sameKeyCurrentWeatherBackingStable"])

    def test_unknown_resource_bytes_are_not_known_zero(self):
        for value in (None, False, 0, -1, "1024", 1024.0):
            with self.subTest(value=value):
                self.receipt = make_receipt()
                for sample in self.receipt["snapshots"]:
                    if suite.expected_stage(sample["jassStage"])["weatherEnabled"]:
                        sample["observation"]["render"]["resourceBytes"] = value
                self.assertFalse(self.verdict()["sameKeyCurrentWeatherBackingStable"])
                self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])

    def test_unknown_ordinary_or_small_private_delta_is_not_certification(self):
        self.receipt["memory"] = {"privateBytesBefore": 1234, "privateBytesAfter": 1234}
        verdict = self.verdict()
        self.assertFalse(verdict["ordinaryCurrentResourceAccountingCertified"])
        self.assertFalse(verdict["processLeakCertified"])
        self.assertFalse(verdict["gpuCompletionCertified"])

    def test_resource_creation_failure_cannot_hide_behind_good_frames(self):
        self.stage_samples(34)[-1]["render"]["stage"] = "resource-create-failed"
        self.assertFalse(self.verdict()["noUnexpectedWeatherPreparationFailures"])

    def test_clock_regression_in_owner_fails(self):
        self.stage_samples(34)[-1]["runtime"]["timeSeconds"] = 0.1
        self.assertFalse(self.verdict()["weatherClocksAndHistoriesConsistent"])

    def test_play_must_increment_history(self):
        old_history = self.stage_samples(33)[0]["runtime"]["historyRevision"]
        for observation in self.stage_samples(34):
            observation["runtime"]["historyRevision"] = old_history
            observation["frozenInput"]["historyRevision"] = old_history
        self.assertFalse(self.verdict()["weatherClocksAndHistoriesConsistent"])

    def test_wrong_owner_kind_and_author_flags_fail(self):
        for location, key, value in (("runtime", "observationKind", "render-input"),
                                     ("authorSettings", "sunEnabled", False),
                                     ("authorSettings", "volumetricDensity", 0.0)):
            with self.subTest(location=location, key=key):
                self.receipt = make_receipt()
                for observation in self.stage_samples(1):
                    observation[location][key] = value
                self.assertFalse(self.verdict()["sunOrdinaryWeatherCoexistence"])

    def test_bad_numeric_frame_is_fail_visible_not_exception(self):
        for observation in self.stage_samples(34):
            observation["render"]["frameSerial"] = "not-a-frame"
        self.assertFalse(self.verdict()["allStagesHaveActualConsumption"])

    def test_capture_count_label_size_and_phase_brackets_checked(self):
        for key, value in (("label", "wrong"), ("dimensions", [1280, 720]),
                           ("stageAtCompletion", 99), ("ok", False)):
            with self.subTest(key=key):
                self.receipt = make_receipt()
                self.receipt["captures"][1][key] = value
                self.assertFalse(self.verdict()["allSevenCapturesPhaseBracketed"])
        self.receipt = make_receipt()
        self.receipt["captures"].pop()
        self.assertFalse(self.verdict()["allSevenCapturesPhaseBracketed"])

    def test_tail_must_be_destroyed_and_final_query_zero(self):
        self.receipt["stageReceipts"][68]["oldId"] = 9999
        self.assertFalse(self.verdict()["allStressJassStagesAndChecks"])
        self.receipt = make_receipt()
        self.receipt["stageReceipts"][69]["timeSeconds"] = 1.0
        self.assertFalse(self.verdict()["allStressJassStagesAndChecks"])

    def test_receipt_reader_parses_hashes_and_duplicate_markers(self):
        with tempfile.TemporaryDirectory(prefix="warvk-stress-pure-") as directory:
            destination = Path(directory) / "WarVK/Temp"
            destination.mkdir(parents=True)
            marker = ("WEATHER_STRESS_STAGE=0;checks=23;errors=0;id=1;time=0.000;"
                      "cycle=-1;weather=0;sun=1;ordinary=1;played=0;query=0.000;"
                      "oldId=0;oldRejected=0;postfxRefusalCode=18")
            path = destination / "weather_stress_stage_0.txt"
            path.write_text(marker + "\n" + marker, encoding="utf-8")
            rows = suite.stage_receipts(Path(directory))
            self.assertEqual(len(rows), 2)
            self.assertTrue(all(row["recordsInFile"] == 2 for row in rows))
            self.assertTrue(all(re.fullmatch(r"[0-9a-fA-F]{64}", row["sha256"]) for row in rows))
            self.assertIs(rows[0]["weatherEnabled"], False)
            self.assertEqual(rows[0]["postfxRefusalCode"], 18)

    def test_old_marker_remains_readable_without_certifying_new_refusal(self):
        with tempfile.TemporaryDirectory(prefix="warvk-stress-old-pure-") as directory:
            destination = Path(directory) / "WarVK/Temp"
            destination.mkdir(parents=True)
            marker = ("WEATHER_STRESS_STAGE=0;checks=23;errors=1;id=1;time=0.000;"
                      "cycle=-1;weather=0;sun=1;ordinary=1;played=0;query=0.000;"
                      "oldId=0;oldRejected=0")
            (destination / "weather_stress_stage_0.txt").write_text(marker, encoding="utf-8")
            rows = suite.stage_receipts(Path(directory))
            self.assertEqual(len(rows), 1)
            self.assertIsNone(rows[0]["postfxRefusalCode"])
            self.assertFalse(suite.classify({"stageReceipts": rows})["allStressJassStagesAndChecks"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
