#!/usr/bin/env python3
"""Static contracts for the bridge/ramp CSM continuity diagnostic."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
SHADOW_CPP = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
RUNTIME_BRIDGE_CPP = (
    ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp"
)


class ShadowContinuityTraceContracts(unittest.TestCase):
    FIELDS = (
        ("semanticSceneReceiverCameraHash", "receiverCameraHash"),
        ("semanticSceneReceiverSunDirectionHash", "receiverSunDirectionHash"),
        ("semanticSceneReceiverCsmHash", "receiverCsmHash"),
        ("semanticSceneReceiverCameraDeltaNano", "receiverCameraDeltaNano"),
        ("semanticSceneReceiverCsmDeltaNano", "receiverCsmDeltaNano"),
        ("semanticSceneReplayBackingHash", "replayBackingHash"),
        (
            "semanticSceneStage13ReplayContentHash",
            "stage13ReplayContentHash",
        ),
        (
            "semanticSceneStage13ReplayBackingHash",
            "stage13ReplayBackingHash",
        ),
        ("semanticSceneStage13ReplayDrawCount", "stage13ReplayDrawCount"),
    )

    def test_both_device_publish_paths_copy_every_field(self) -> None:
        text = DEVICE_CPP.read_text(encoding="utf-8")
        for destination, source in self.FIELDS:
            assignment = (
                rf"{re.escape(destination)}\s*=\s*"
                rf"rec\.{re.escape(source)};"
            )
            self.assertEqual(
                len(re.findall(assignment, text)),
                2,
                f"{destination} must be copied in both device publish paths",
            )

    def test_shadow_run_populates_every_reconciliation_field(self) -> None:
        text = SHADOW_CPP.read_text(encoding="utf-8")
        for _, field in self.FIELDS:
            self.assertIn(f"reconciliation.{field}", text)

    def test_cadence_bridge_consumes_every_scene_field(self) -> None:
        text = RUNTIME_BRIDGE_CPP.read_text(encoding="utf-8")
        for field, _ in self.FIELDS:
            self.assertIn(f"stats.{field}", text)

    def test_bridge_diagnostics_are_opt_in_at_final_csm_boundary(self) -> None:
        text = SHADOW_CPP.read_text(encoding="utf-8")
        self.assertIn(
            'EnvIntOverride("DXVK_WAR3_SHADOW_DEBUG_CASTER_STAGE", 0, 31)',
            text,
        )
        self.assertIn(
            "s_debugCasterStage >= 0 && draw.stage != s_debugCasterStage",
            text,
        )
        self.assertIn(
            'EnvFloatDefault("DXVK_WAR3_SHADOW_FAR_CASTER_DEPTH_EXTENSION", '
            "-1.0f)",
            text,
        )
        self.assertIn(
            "std::clamp(s_farCasterDepthExtensionOverride, 0.0f, 4096.0f)",
            text,
        )


if __name__ == "__main__":
    unittest.main()
