import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "AutoTest/run_shadow_alpha_coverage_abba.py").read_text(
    encoding="utf-8"
)


class ShadowAlphaCoverageAbbaStaticTests(unittest.TestCase):
    def test_visible_desktop_and_no_deploy_contract(self):
        self.assertIn('"use_isolated_desktop": False', RUNNER)
        self.assertIn('"deploy_d3d9_before_launch": False', RUNNER)
        self.assertNotIn("SwitchDesktop", RUNNER)

    def test_abba_uses_fresh_process_modes(self):
        for token in ('("off-a", False)', '("on-b1", True)',
                      '("on-b2", True)', '("off-a2", False)'):
            self.assertIn(token, RUNNER)
        self.assertIn('"DXVK_WAR3_SHADOW_ALPHA_HASH": "1" if hashed else "0"', RUNNER)
        self.assertIn('"DXVK_WAR3_SHADOW_TAA_MODE": "0"', RUNNER)
        self.assertIn('"DXVK_WAR3_CSM_CONTINUITY_TRACE": "1"', RUNNER)

    def test_fixed_camera_live_producer_and_sun_steps(self):
        self.assertIn("pause_simulation: bool = False", RUNNER)
        self.assertIn('"targetDistance": 1200.0', RUNNER)
        self.assertIn('"farZ": 3000.0', RUNNER)
        self.assertIn('"angleOfAttack": 328.0', RUNNER)
        self.assertIn('"shadow.lock_sun"', RUNNER)
        self.assertIn('"shadow.debug_mode", {"mode": 2}', RUNNER)

    def test_each_step_requires_a_new_shadow_publication(self):
        self.assertIn('"preflightCapture"', RUNNER)
        self.assertIn("observed_render_serial > previous_render_serial", RUNNER)
        self.assertIn('"publicationPolls"', RUNNER)
        self.assertIn('row["shadowMapPublicationAdvanced"]', RUNNER)
        self.assertIn("render_serials[index] > render_serials[index - 1]", RUNNER)
        self.assertIn(
            'shadow map publication did not advance for every sun step', RUNNER
        )

    def test_full_map_visibility_is_opt_in(self):
        self.assertIn("full_map_visibility: bool = False", RUNNER)
        self.assertIn("if full_map_visibility:", RUNNER)
        self.assertIn('parser.add_argument("--full-map-visibility", action="store_true")', RUNNER)

    def test_objective_gate_cannot_promote_without_later_gates(self):
        self.assertIn("improvement >= 0.40", RUNNER)
        self.assertIn("coverage_ratio >= 0.99", RUNNER)
        self.assertIn("0.99 <= edge_ratio <= 1.15", RUNNER)
        self.assertIn('"promotionAllowed": False', RUNNER)

    def test_first_device_loss_stops_remaining_rounds(self):
        self.assertIn('if bool(row.get("deviceLost")):', RUNNER)
        self.assertIn('result["stoppedAfterFirstDeviceLost"] = True', RUNNER)
        self.assertIn("break", RUNNER)

    def test_single_round_diagnostic_does_not_change_default_abba(self):
        self.assertIn('"--single-round"', RUNNER)
        self.assertIn('if args.single_round', RUNNER)
        for token in ('("off-a", False)', '("on-b1", True)',
                      '("on-b2", True)', '("off-a2", False)'):
            self.assertIn(token, RUNNER)


if __name__ == "__main__":
    unittest.main()
