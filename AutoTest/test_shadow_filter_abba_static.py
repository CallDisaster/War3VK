import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "AutoTest/run_shadow_filter_abba.py").read_text(encoding="utf-8")
SHARED = (ROOT / "AutoTest/run_shadow_alpha_coverage_abba.py").read_text(
    encoding="utf-8"
)


class ShadowFilterAbbaStaticTests(unittest.TestCase):
    def test_visible_desktop_no_deploy_shared_contract(self):
        self.assertIn('"use_isolated_desktop": False', SHARED)
        self.assertIn('"deploy_d3d9_before_launch": False', SHARED)

    def test_fresh_abba_process_sequence(self):
        for token in (
            '("baseline-a", False)',
            '("candidate-b1", True)',
            '("candidate-b2", True)',
            '("baseline-a2", False)',
        ):
            self.assertIn(token, RUNNER)

    def test_only_existing_release_pcf_controls_are_varied(self):
        self.assertIn('"DXVK_WAR3_SHADOW_PCF_KERNEL": str(kernel)', RUNNER)
        self.assertIn('"DXVK_WAR3_SHADOW_PCF": f"{radius:.6f}"', RUNNER)
        self.assertIn('"DXVK_WAR3_SHADOW_ALPHA_HASH": "0"', RUNNER)

    def test_first_device_loss_stops_remaining_rounds(self):
        self.assertIn('if bool(row.get("deviceLost")):', RUNNER)
        self.assertIn('result["stoppedAfterFirstDeviceLost"] = True', RUNNER)

    def test_quality_gate_requires_live_shadow_publication(self):
        self.assertIn("pause_simulation: bool = False", SHARED)
        self.assertIn('row["shadowMapPublicationAdvanced"]', SHARED)
        self.assertIn("observed_render_serial > previous_render_serial", SHARED)
        self.assertIn('"fullMapVisibility": bool(args.full_map_visibility)', RUNNER)

    def test_aggregate_uses_explicit_candidate_axis(self):
        self.assertIn('"candidateEnabled"', SHARED)
        self.assertIn('row.get("candidateEnabled", row.get("hashed"))', SHARED)


if __name__ == "__main__":
    unittest.main()
