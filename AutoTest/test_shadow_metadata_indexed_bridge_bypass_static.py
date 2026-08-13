#!/usr/bin/env python3
"""Contracts for bypassing the metadata bridge on indexed Stage11 draws."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class ShadowMetadataIndexedBridgeBypassStaticTests(unittest.TestCase):
    def test_indexed_draws_use_the_later_exact_domain_path(self) -> None:
        start = DEVICE.index("const bool runShadowMetadataBridge = !indexed")
        end = DEVICE.index("if (metadataRejectedBlocker) {", start)
        block = DEVICE[start:end]
        self.assertIn("if (runShadowMetadataBridge) {", block)
        call = block.index("War3CaptureShadowDrawMetadata(")
        guard = block.index("if (runShadowMetadataBridge) {")
        self.assertLess(guard, call)

        metadata = DEVICE[
            DEVICE.index("bool D3D9DeviceEx::War3CaptureShadowDrawMetadata(") :
            DEVICE.index("void D3D9DeviceEx::War3TryCaptureShadowCaster(")
        ]
        self.assertIn("if (indexed) {", metadata)
        self.assertIn(
            "Stage11 exact\n  // captures indexed cutout UV after scanning the immutable IB",
            metadata,
        )
        self.assertIn("const bool readable = !indexed", metadata)

    def test_metadata_timing_is_inactive_outside_recording(self) -> None:
        start = DEVICE.index("const bool runShadowMetadataBridge = !indexed")
        end = DEVICE.index("if (metadataRejectedBlocker) {", start)
        block = DEVICE[start:end]
        self.assertIn(
            "runShadowMetadataBridge && trackShadowCaptureCpu &&", block
        )
        self.assertIn(
            "if (trackShadowCaptureCpu)\n"
            "          ++g_war3CaptureCpuTls.shadowMetadataCaptureCalls;",
            block,
        )
        self.assertNotIn(
            "++g_war3CaptureCpuTls.shadowMetadataCaptureCalls;\n"
            "      const bool sampleShadowMetadata",
            block,
        )

    def test_blocker_rejection_and_nonindexed_call_order_remain(self) -> None:
        start = DEVICE.index("const bool runShadowMetadataBridge = !indexed")
        call = DEVICE.index("War3CaptureShadowDrawMetadata(", start)
        reject = DEVICE.index("if (metadataRejectedBlocker) {", call)
        semantic_candidate = DEVICE.index(
            "const bool earlySemanticSceneUnitLikeCandidate", reject
        )
        self.assertLess(call, reject)
        self.assertLess(reject, semantic_candidate)
        reject_block = DEVICE[reject:semantic_candidate]
        self.assertIn("markCurrentStage11ExactRejected", reject_block)
        self.assertIn("return;", reject_block)


if __name__ == "__main__":
    unittest.main()
