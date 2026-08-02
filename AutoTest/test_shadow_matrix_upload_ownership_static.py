"""Static ownership contracts for the shadow matrix upload buffers."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/d3d9_war3_shadow.h"
SOURCE = ROOT / "src/d3d9/d3d9_war3_shadow_resources.cpp"


class ShadowMatrixUploadOwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_fixed_modulo_ring_is_retired(self) -> None:
        self.assertNotIn("kPaletteRingCount", self.header)
        self.assertNotIn("% kPaletteRingCount", self.source)
        self.assertNotIn("ringIndex", self.source)

    def test_pool_reuses_only_gpu_completed_buffers(self) -> None:
        self.assertIn("struct ShadowMatrixUploadSlot", self.header)
        self.assertIn("m_shadowMatrixUploadSlots", self.header)
        self.assertIn("kShadowMatrixUploadPoolLimit", self.header)
        self.assertIn("slot.buffer->isInUse(DxvkAccess::Read)", self.source)
        self.assertIn("m_shadowMatrixUploadSlots.size() >=", self.source)

    def test_each_upload_uses_offset_zero_and_is_tracked(self) -> None:
        start = self.source.index(
            "DxvkResourceBufferInfo War3ShadowReceiverPass::ensureShadowMatrixBuffer")
        block = self.source[start:]
        self.assertIn("const VkDeviceSize baseOffset = 0u;", block)
        self.assertIn("m_paletteBaseMatrixIndex = 0u;", block)
        self.assertIn(
            "ctx->track(m_vertexBlendPaletteBuffer, DxvkAccess::Read);", block
        )


if __name__ == "__main__":
    unittest.main()
