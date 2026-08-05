"""Contracts for the opt-in low-disk bridge/ramp visual probe mode."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
AUTOTEST = ROOT / "AutoTest"
if str(AUTOTEST) not in sys.path:
    sys.path.insert(0, str(AUTOTEST))

import run_bridge_ramp_visual_probe as probe


class BridgeRampLowDiskProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = (AUTOTEST / "run_bridge_ramp_visual_probe.py").read_text(
            encoding="utf-8"
        )

    def test_low_disk_mode_is_opt_in_and_trace_is_manually_owned(self) -> None:
        self.assertIn('"--capture-retain-count"', self.source)
        self.assertIn('"--trace-ring-segment-sec"', self.source)
        self.assertIn("default=0.0", self.source)
        self.assertIn(
            'env["DXVK_WAR3_SHADOW_POSE_FULL_TRACE"] = "0"',
            self.source,
        )
        self.assertIn('command="start_shadow_pose_full_trace"', self.source)
        self.assertIn('command="stop_shadow_pose_full_trace"', self.source)

    def test_trace_rotation_archives_before_starting_next_segment(self) -> None:
        archive = self.source.index("source_resolved.replace(destination)")
        stopped = self.source.index(
            'reason="temporal-dark-trigger"', archive
        )
        restarted = self.source.index(
            "start_trace_segment(pid, pin_on_close=True)", stopped
        )
        self.assertLess(archive, stopped)
        self.assertLess(stopped, restarted)
        self.assertIn("shadow_pose_full_trace_", self.source)
        self.assertIn("run freshness check failed", self.source)
        self.assertIn(
            'shadow_trace_witness["originalPath"] = original_path',
            self.source,
        )
        self.assertIn(
            'shadow_trace_witness["path"] = archived_path',
            self.source,
        )

    def test_safe_unlink_refuses_files_outside_exact_probe_dir(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            root = pathlib.Path(root_text)
            allowed = root / "allowed"
            allowed.mkdir()
            inside = allowed / "frame.bmp"
            inside.write_bytes(b"frame")
            self.assertEqual(
                probe._safe_unlink_probe_artifact(inside, allowed), "deleted"
            )
            self.assertFalse(inside.exists())

            outside = root / "outside.bmp"
            outside.write_bytes(b"do-not-delete")
            outcome = probe._safe_unlink_probe_artifact(outside, allowed)
            self.assertTrue(outcome.startswith("refused:"), outcome)
            self.assertEqual(outside.read_bytes(), b"do-not-delete")

    def test_online_trigger_finds_local_dark_triangle_not_global_step(self) -> None:
        try:
            import cv2
            import numpy as np
        except Exception as exc:  # pragma: no cover - dependency gate
            self.skipTest(str(exc))

        with tempfile.TemporaryDirectory() as root_text:
            root = pathlib.Path(root_text)
            previous = np.full((240, 320, 3), 180, dtype=np.uint8)
            global_step = np.full((240, 320, 3), 160, dtype=np.uint8)
            triangle = previous.copy()
            cv2.fillPoly(
                triangle,
                [np.array([[80, 30], [280, 40], [170, 180]], np.int32)],
                (50, 50, 50),
            )
            previous_path = root / "previous.bmp"
            global_path = root / "global.bmp"
            triangle_path = root / "triangle.bmp"
            self.assertTrue(cv2.imwrite(str(previous_path), previous))
            self.assertTrue(cv2.imwrite(str(global_path), global_step))
            self.assertTrue(cv2.imwrite(str(triangle_path), triangle))

            global_result = probe._analyze_pairwise_dark_trigger(
                previous_path,
                global_path,
                target_width=320,
                scene_ratio=0.80,
                luma_threshold=24.0,
            )
            triangle_result = probe._analyze_pairwise_dark_trigger(
                previous_path,
                triangle_path,
                target_width=320,
                scene_ratio=0.80,
                luma_threshold=24.0,
            )
            self.assertTrue(global_result.get("ok"), global_result)
            self.assertEqual(global_result["largestDarkComponent"], 0)
            self.assertTrue(triangle_result.get("ok"), triangle_result)
            self.assertGreater(
                triangle_result["largestDarkComponent"], 10000
            )


if __name__ == "__main__":
    unittest.main()
