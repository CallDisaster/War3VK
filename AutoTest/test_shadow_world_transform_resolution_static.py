#!/usr/bin/env python3
"""Static contract for the canonical world-transform resolution fast path."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)
CANONICAL = (ROOT / "src/d3d9/war3/render/war3_canonical_draw.cpp").read_text(
    encoding="utf-8", errors="replace"
)


def between(text: str, begin: str, end: str) -> str:
    start = text.index(begin)
    finish = text.index(end, start)
    return text[start:finish]


class ShadowWorldTransformResolutionStaticTest(unittest.TestCase):
    def test_current_draw_world_palette_skips_unused_resolution(self) -> None:
        block = between(
            DEVICE,
            "const bool currentDrawPaletteOwnsWorldTransform =",
            "fallbackAppendTiming.enter(War3FallbackAppendPhase::LivePalette)",
        )
        self.assertIn("CurrentDrawResolveStatus::Ready", block)
        self.assertIn("authoritativeGroupSlotsReady", block)
        self.assertIn("drawTimeCapturedPaletteReady", block)
        self.assertIn("drawTimeCapturedPaletteHash != 0u", block)
        self.assertGreaterEqual(
            block.count("if (currentDrawPaletteOwnsWorldTransform"), 1
        )

    def test_scene_node_precedes_pose_registry_fallback(self) -> None:
        block = between(
            DEVICE,
            "const bool currentDrawPaletteOwnsWorldTransform =",
            "fallbackAppendTiming.enter(War3FallbackAppendPhase::LivePalette)",
        )
        scene = block.index("const bool hasSceneNodeWorldMatrix")
        pose = block.index("const bool hasLivePoseWorldTransform")
        self.assertLess(scene, pose)
        pose_block = block[pose:]
        self.assertIn(
            "if (currentDrawPaletteOwnsWorldTransform || hasSceneNodeWorldMatrix)",
            pose_block,
        )
        self.assertIn("findByRuntimeModelAugment", pose_block)
        self.assertIn("findBySceneNodeAugment", pose_block)
        self.assertIn("findByUnitPtrAugment", pose_block)

    def test_canonical_priority_remains_world_palette_scene_pose_packet(self) -> None:
        block = between(
            CANONICAL,
            "auto& world = instance.worldTransform;",
            "const auto meshReady =",
        )
        ordered = [
            "CanonicalPaletteSource::CurrentDrawCapturedPalette",
            "inputs.hasSceneNodeWorldMatrix",
            "inputs.liveWorldTransform != nullptr",
            "packet.pose.hasWorldTransform",
            "!packet.pose.matrixPalette.empty()",
        ]
        positions = [block.index(token) for token in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("CanonicalWorldTransformSource::CurrentDrawPaletteWorld", block)
        self.assertIn("CanonicalWorldTransformSource::SceneNode", block)
        self.assertIn("CanonicalWorldTransformSource::LivePoseRegistry", block)


if __name__ == "__main__":
    unittest.main()
