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

    def test_fixed_camera_paused_simulation_and_sun_steps(self):
        self.assertIn('"game.pause", {"paused": True}', RUNNER)
        self.assertIn('"targetX": 0.0', RUNNER)
        self.assertIn('"angleOfAttack": 328.0', RUNNER)
        self.assertIn('"shadow.lock_sun"', RUNNER)
        self.assertIn('"shadow.debug_mode", {"mode": 2}', RUNNER)

    def test_objective_gate_cannot_promote_without_later_gates(self):
        self.assertIn("improvement >= 0.40", RUNNER)
        self.assertIn("coverage_ratio >= 0.99", RUNNER)
        self.assertIn("0.99 <= edge_ratio <= 1.15", RUNNER)
        self.assertIn('"promotionAllowed": False', RUNNER)

    def test_first_device_loss_stops_remaining_rounds(self):
        self.assertIn('if bool(row.get("deviceLost")):', RUNNER)
        self.assertIn('result["stoppedAfterFirstDeviceLost"] = True', RUNNER)
        self.assertIn("break", RUNNER)


if __name__ == "__main__":
    unittest.main()
