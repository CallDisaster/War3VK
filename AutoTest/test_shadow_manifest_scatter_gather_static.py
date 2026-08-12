#!/usr/bin/env python3
"""Static contract for allocation-free two-range manifest publication."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/war3/render/war3_visible_renderables.h").read_text(
    encoding="utf-8"
)
SOURCE = (ROOT / "src/d3d9/war3/render/war3_visible_renderables.cpp").read_text(
    encoding="utf-8"
)
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class ShadowManifestScatterGatherStaticTests(unittest.TestCase):
    def test_registry_exposes_compatibility_and_two_range_overloads(self) -> None:
        signature = "void refreshShadowManifestFromCurrentDraw("
        self.assertGreaterEqual(HEADER.count(signature), 2)
        self.assertGreaterEqual(
            SOURCE.count(
                "VisibleRenderableRegistry::refreshShadowManifestFromCurrentDraw("
            ),
            2,
        )
        wrapper = SOURCE.index("const std::vector<CurrentDrawContractRecord>& records,")
        two_range = SOURCE.index(
            "const std::vector<CurrentDrawContractRecord>& firstRecords,", wrapper
        )
        wrapper_body = SOURCE[wrapper:two_range]
        self.assertIn(
            "refreshShadowManifestFromCurrentDraw(records, s_emptyRecords, frameNumber)",
            wrapper_body,
        )

    def test_two_ranges_are_visited_in_concatenation_order(self) -> None:
        start = SOURCE.index(
            "const std::vector<CurrentDrawContractRecord>& firstRecords,"
        )
        end = SOURCE.index(
            "void VisibleRenderableRegistry::noteShadowManifestPartGoodPacket", start
        )
        body = SOURCE[start:end]
        first = body.index("for (const auto& record : firstRecords)")
        second = body.index("for (const auto& record : secondRecords)")
        self.assertLess(first, second)
        self.assertIn("forEachRecord([&](const CurrentDrawContractRecord& record)", body)
        self.assertIn("firstSliceByPartAnchor.reserve(recordCount)", body)
        self.assertIn("poseFreshVerifierObjects->reserve(recordCount)", body)

    def test_hot_path_does_not_copy_exact_records_into_direct_scratch(self) -> None:
        start = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped("
        )
        end = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", start
        )
        body = DEVICE[start:end]
        self.assertNotIn("shadowEligibleManifestRecords.insert(", body)
        self.assertIn(
            "publishShadowManifestSummary(exactSubmittedManifestRecords,",
            body,
        )
        self.assertIn("shadowEligibleManifestRecords.reserve(recordIndicesForBuild.size())", body)


if __name__ == "__main__":
    unittest.main()
