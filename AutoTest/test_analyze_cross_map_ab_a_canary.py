import copy
import unittest

from analyze_cross_map_ab_a_canary import examine


def fixture():
    result = {
        "phases": [{"phase": name} for name in ("A-first", "B", "A-return")],
        "sameProcess": True, "processStillAliveAtEnd": True,
        "sourceUnchanged": True, "stop": {"ok": True},
        "globalInputUsed": False,
        "resolution": {"window": {"clientRect": {"width": 2560, "height": 1440}}},
        "newGpuEvents": [], "gpuIncidents": [],
        "screenshots": [{"phase": name, "ok": True, "size": 10,
                         "expectedEpoch": index + 1,
                         "observedEpoch": index + 1,
                         "sha256": "A" * 64}
                        for index, name in enumerate(("A-first", "B", "A-return"))],
    }
    rows = []
    for index, name in enumerate(("A-first", "B", "A-return")):
        for frame in (1, 2):
            rows.append({
                "phase": name, "frameIndex": 100 * (index + 1) + frame,
                "monitor": {
                    "shadowMapEpoch": index + 1,
                    "shadowMapResetRequestedSerial": index,
                    "shadowMapResetAppliedSerial": index,
                    "shadowRetiredSessionCount": 1 if index == 1 else 0,
                    "shadowRetiredSessionCollectedCount": 1 if index == 2 else 0,
                    "semanticSceneReceiverHasCompleteShadowMap": 1,
                    "shadowMapProducerReady": 1,
                    "shadowReplayDrawnCasterCount": 100,
                },
            })
    return result, rows


class CrossMapAnalysisTests(unittest.TestCase):
    def test_minimal_structural_path_but_never_release(self):
        result, rows = fixture()
        verdict = examine(result, rows)
        self.assertTrue(verdict["ok"])
        self.assertFalse(verdict["releaseAuthorized"])
        self.assertFalse(verdict["persistentChargeAccountVerified"])

    def test_frozen_map_epoch_must_fail_even_if_shadows_draw(self):
        result, rows = fixture()
        for row in rows:
            row["monitor"]["shadowMapEpoch"] = 1
        verdict = examine(result, rows)
        self.assertIn("map-epoch-did-not-advance", verdict["failedChecks"])

    def test_predictive_markers_do_not_make_next_epoch_a_false_failure(self):
        result, rows = fixture()
        old_tail = copy.deepcopy(rows[1])
        old_tail["monitor"]["shadowMapEpoch"] = 2
        old_tail["frameIndex"] = 103
        rows.insert(2, old_tail)
        return_head = copy.deepcopy(rows[-2])
        return_head["monitor"]["shadowMapEpoch"] = 2
        return_head["frameIndex"] = 300
        rows.insert(-2, return_head)
        self.assertTrue(examine(result, rows)["ok"])

    def test_wrong_epoch_stage_image_cannot_pass(self):
        result, rows = fixture()
        result["screenshots"][2]["observedEpoch"] = 2
        self.assertIn("stage-image-coverage", examine(result, rows)["failedChecks"])

    def test_missing_retirement_must_fail(self):
        result, rows = fixture()
        for row in rows:
            row["monitor"]["shadowRetiredSessionCount"] = 0
            row["monitor"]["shadowRetiredSessionCollectedCount"] = 0
        self.assertIn("retirement-path-uncovered", examine(result, rows)["failedChecks"])

    def test_repeated_one_frame_does_not_count_as_sustained(self):
        result, rows = fixture()
        for row in rows:
            if row["phase"] == "B":
                row["frameIndex"] = 201
        self.assertIn("B-sustained-shadow", examine(result, rows)["failedChecks"])

    def test_three_processes_cannot_impersonate_same_pid(self):
        result, rows = fixture()
        result["sameProcess"] = False
        self.assertIn("same-pid-liveness", examine(result, rows)["failedChecks"])

    def test_gpu_incident_and_missing_stage_images_fail(self):
        result, rows = fixture()
        result["newGpuEvents"] = ["4101"]
        result["screenshots"] = result["screenshots"][:2]
        verdict = examine(result, rows)
        self.assertIn("gpu-events-or-incidents", verdict["failedChecks"])
        self.assertIn("stage-image-coverage", verdict["failedChecks"])


if __name__ == "__main__":
    unittest.main()
