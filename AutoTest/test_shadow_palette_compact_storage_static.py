"""Contracts for compact CPU shadow palettes and fixed GPU upload slots."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCENE = ROOT / "src/d3d9/d3d9_war3_scene.h"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
RESOURCES = ROOT / "src/d3d9/d3d9_war3_shadow_resources.cpp"
POLICY = ROOT / "src/d3d9/war3/render/war3_shadow_palette_storage.h"


class ShadowPaletteCompactStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.scene = SCENE.read_text(encoding="utf-8")
        cls.device = DEVICE.read_text(encoding="utf-8")
        cls.resources = RESOURCES.read_text(encoding="utf-8")
        cls.policy = POLICY.read_text(encoding="utf-8")

    def test_cpu_scene_owns_only_live_prefix(self) -> None:
        self.assertIn("small_vector<Matrix4, 64u> worldMatrices", self.scene)
        self.assertNotIn("std::array<Matrix4, 256> worldMatrices", self.scene)
        bounded = self.device.index("const uint32_t boundedCount")
        append = self.device.index("palette.worldMatrices.resize(boundedCount)", bounded)
        publish = self.device.index("m_war3ShadowPaletteHashIndex.insert", append)
        self.assertLess(bounded, append)
        self.assertLess(append, publish)

    def test_hash_lookup_compares_count_and_live_bytes(self) -> None:
        full_start = self.device.index(
            "uint32_t D3D9DeviceEx::War3GetOrCreateShadowMatrixPalette()")
        full_end = self.device.index(
            "uint32_t D3D9DeviceEx::War3GetOrCreateShadowMatrixPaletteFromData",
            full_start,
        )
        full_body = self.device[full_start:full_end]
        self.assertIn("p.worldMatrices.size() == 256u", full_body)

        start = self.device.index(
            "uint32_t D3D9DeviceEx::War3GetOrCreateShadowMatrixPaletteFromData")
        end = self.device.index(
            "uint32_t D3D9DeviceEx::War3GetOrCreateSemanticShadowPalette", start)
        body = self.device[start:end]
        self.assertIn("existing.worldMatrices.size() == boundedCount", body)
        self.assertIn("std::memcmp(existing.worldMatrices.data(), matrices, bytes)", body)

    def test_upload_restores_fixed_identity_filled_stride(self) -> None:
        self.assertIn("kWar3ShadowPaletteMatrixCapacity = 256u", self.policy)
        self.assertIn("kIdentityTail", self.policy)
        self.assertIn("std::memcpy(destination, kIdentityTail.data()", self.policy)
        self.assertIn("War3ExpandShadowPaletteForUpload", self.resources)
        self.assertIn("War3BoundShadowPaletteMatrixCount", self.resources)
        self.assertIn("palette.worldMatrices.size()", self.resources)


if __name__ == "__main__":
    unittest.main()
