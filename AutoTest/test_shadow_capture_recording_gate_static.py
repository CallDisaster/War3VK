#!/usr/bin/env python3
"""Release contract for the per-draw ShadowCapture performance observer."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = DEVICE.index(signature)
    brace = DEVICE.index("{", start)
    depth = 0
    for index in range(brace, len(DEVICE)):
        if DEVICE[index] == "{":
            depth += 1
        elif DEVICE[index] == "}":
            depth -= 1
            if depth == 0:
                return DEVICE[brace : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class ShadowCaptureRecordingGateStaticTest(unittest.TestCase):
    def test_disabled_sample_never_reads_qpc(self) -> None:
        sample = DEVICE.split("struct War3CaptureCpuSample", 1)[1].split(
            "static void War3FlushCaptureCpuTlsToPerf", 1
        )[0]
        self.assertIn("explicit War3CaptureCpuSample(bool active = true)", sample)
        self.assertIn(
            "start(active ? dxvk::high_resolution_clock::get_counter() : 0)",
            sample,
        )
        self.assertIn("if (start == 0 || bucketTicks == nullptr", sample)

    def test_recording_state_is_read_before_capture_timers(self) -> None:
        body = function_body("void D3D9DeviceEx::War3TryCaptureShadowCaster(")
        gate = body.index("const bool trackShadowCaptureCpu =")
        first_sample = body.index(
            "War3CaptureCpuSample shadowCaptureGatesSample(trackShadowCaptureCpu)"
        )
        self.assertLess(gate, first_sample)
        self.assertIn("shadowPerfMonitor.isEnabled()", body[gate:first_sample])
        self.assertIn("shadowPerfMonitor.isRecording()", body[gate:first_sample])

    def test_every_per_draw_capture_timer_uses_the_same_gate(self) -> None:
        body = function_body("void D3D9DeviceEx::War3TryCaptureShadowCaster(")
        declarations = re.findall(r"War3CaptureCpuSample\s+\w+\(([^)]*)\);", body)
        self.assertGreaterEqual(len(declarations), 5)
        self.assertEqual(
            ["trackShadowCaptureCpu"] * len(declarations), declarations
        )

    def test_recursive_observers_cannot_run_without_recording(self) -> None:
        body = function_body("void D3D9DeviceEx::War3TryCaptureShadowCaster(")
        self.assertIn(
            "trackShadowCaptureCpu && War3ShadowCaptureGateBreakdownRuntime()",
            body,
        )
        self.assertIn(
            "trackShadowCaptureCpu && War3ShadowDrawTimeCaptureBreakdownRuntime()",
            body,
        )
        self.assertIn(
            "trackShadowCaptureCpu && War3ShadowCapturePostBreakdownRuntime()",
            body,
        )


if __name__ == "__main__":
    unittest.main()
