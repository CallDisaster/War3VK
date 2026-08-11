#!/usr/bin/env python3
"""Contracts for exact same-producer Stage11 stream sharing."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
PROOF = (
    ROOT / "src/d3d9/war3/render/war3_shadow_generation_backed_stream.h"
).read_text(encoding="utf-8")


class GenerationSharedBackingStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        begin = DEVICE.index("using GenerationBackedStreamProof")
        end = DEVICE.index("entry.lastCaptureFingerprint = captureFingerprint", begin)
        cls.capture = DEVICE[begin:end]

    def test_full_value_identity_is_the_map_key(self) -> None:
        self.assertIn("War3ShadowGenerationBackedStreamProofHash", PROOF)
        self.assertIn("constexpr bool operator==", PROOF)
        for field in (
            "ownerIdentity",
            "identityGeneration",
            "allocationGeneration",
            "contentGeneration",
            "sourceOffset",
            "sourceLength",
            "elementStride",
            "elementSize",
            "mapEpoch",
            "deviceEpoch",
            "streamKind",
        ):
            self.assertIn(field, PROOF)
        self.assertIn("m_war3DrawTimeSharedStreamBackings", DEVICE_H)
        self.assertIn("War3ShadowGenerationBackedStreamProofHash", DEVICE_H)

    def test_shared_lookup_precedes_allocation_budget(self) -> None:
        lookup = self.capture.index("tryBindSharedStreamBacking")
        position_budget = self.capture.index("const bool needsNewPositionBuffer")
        uv_budget = self.capture.index("kShadowDrawTimeVBCacheAllocBudgetEnabled", position_budget)
        index_budget = self.capture.index("IndexBacking", uv_budget)
        self.assertLess(lookup, position_budget)
        self.assertIn("generationBackedPositionSharedReuse", self.capture)
        self.assertIn("generationBackedUvSharedReuse", self.capture[:index_budget])
        self.assertIn("generationBackedIndexSharedReuse", self.capture)

    def test_sharing_is_static_exact_and_immutable(self) -> None:
        shared = self.capture[
            self.capture.index("const auto tryBindSharedStreamBacking") :
            self.capture.index("War3DrawTimeCacheIteratorReuseVerifyRuntime")
        ]
        self.assertIn("generationBackedStaticCandidate", shared)
        self.assertIn("proof.valid()", shared)
        self.assertIn("m_war3ShadowPersistentFrameSerial", shared)
        self.assertNotIn("lastCaptureFingerprint", shared)
        self.assertNotIn("sourceFingerprint", shared)
        for stream in ("position", "uv", "index"):
            self.assertIn(f"{stream}GenerationShared", DEVICE_H)
            self.assertIn(f"entry.{stream}GenerationShared", self.capture)
        self.assertIn("!generationBackedPositionEntryReuse", self.capture)
        self.assertIn("!generationBackedIndexEntryReuse", self.capture)
        self.assertIn("!generationBackedUvEntryReuse", self.capture)

    def test_frame_and_map_reset_drop_only_lookup_aliases(self) -> None:
        self.assertIn(
            "m_war3DrawTimeSharedStreamBackingFrameSerial !=",
            self.capture,
        )
        self.assertIn("m_war3DrawTimeSharedStreamBackings.clear()", self.capture)
        reset_begin = DEVICE.index("void D3D9DeviceEx::War3ResetShadowSessionState")
        reset_end = DEVICE.index("void D3D9DeviceEx::", reset_begin + 10)
        reset = DEVICE[reset_begin:reset_end]
        self.assertIn("m_war3DrawTimeSharedStreamBackings.clear()", reset)
        self.assertIn("m_war3DrawTimeSharedStreamBackingFrameSerial = 0u", reset)


if __name__ == "__main__":
    unittest.main()
