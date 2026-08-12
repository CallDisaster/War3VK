"""Static contracts for LT/YT DirectGrouped fail-closed ownership."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class Stage11StaticWorldGenericFailClosedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        grouped = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped("
        )
        grouped_end = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(",
            grouped,
        )
        cls.grouped = DEVICE[grouped:grouped_end]

    def test_preselect_rejects_known_static_world_before_policy_and_budget(self) -> None:
        preselect = self.grouped.index("War3SemanticScene/Direct/Preselect")
        loop = self.grouped.index(
            "for (uint32_t recordIndex = 0u;", preselect
        )
        policy = self.grouped.index(
            "ShadowProducerPolicyContext producerContext", loop
        )
        block = self.grouped[loop:policy]
        owner = block.index("currentFrameDrawTimeProducerOwnsRecord(record)")
        reject = block.index(
            "War3SemanticRawcodeLooksStaticWorldCaster(record.rawcode)"
        )
        self.assertLess(owner, reject)
        self.assertIn("continue;", block[reject:])

    def test_record_loop_rejects_known_static_world_before_alpha_and_build(self) -> None:
        loop = self.grouped.index('enterBuildEligiblePhase("RecordLoop")')
        owner = self.grouped.index(
            "currentFrameDrawTimeProducerOwnsRecord(record)", loop
        )
        reject = self.grouped.index(
            "War3SemanticRawcodeLooksStaticWorldCaster(record.rawcode)", owner
        )
        alpha = self.grouped.index(
            "War3RejectCurrentDrawRecordByUnsafeAlphaVisualPolicy(", reject
        )
        packet_build = self.grouped.index(
            "War3TryBuildShadowPacketFromCurrentDrawRecord(", reject
        )
        self.assertLess(owner, reject)
        self.assertLess(reject, alpha)
        self.assertLess(reject, packet_build)

    def test_backfilled_static_world_rawcode_rejects_every_packet_path(self) -> None:
        comment = self.grouped.index(
            "LT/YT world objects are known to masquerade as Unit"
        )
        reject = self.grouped.index(
            "War3SemanticRawcodeLooksStaticWorldCaster(", comment
        )
        rejection_end = self.grouped.index("continue;", reject)
        block = self.grouped[comment:rejection_end]
        self.assertIn("eligible.packet.renderable.rawcode", block)
        self.assertNotIn("eligible.packet.path", block)


if __name__ == "__main__":
    unittest.main()
