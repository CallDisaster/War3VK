#!/usr/bin/env python3
"""Contracts for the non-mutating D3D9 indexed-range parity observer."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class TerrainIndexHintObserverStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.policy = (ROOT / "src/d3d9/war3/render/war3_terrain_bounds_provenance.h").read_text(
            encoding="utf-8"
        )
        cls.device = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
        cls.scene = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")

    def test_policy_distinguishes_exact_superset_and_undercoverage(self) -> None:
        for token in (
            "War3TerrainIndexedHintRelation::Exact",
            "War3TerrainIndexedHintRelation::ConservativeSuperset",
            "War3TerrainIndexedHintRelation::UnderCoversExactDomain",
            "hintFirst < 0",
            "hintEnd > int64_t(evidence.vertexCapacity)",
            "exactEnd > uint64_t(evidence.vertexCapacity)",
        ):
            self.assertIn(token, self.policy)

    def test_hint_comparison_requires_a_scanned_exact_domain(self) -> None:
        self.assertIn("exactDomainKnown", self.policy)
        self.assertRegex(
            self.policy,
            r"!evidence\.exactDomainKnown\s*\|\|\s*evidence\.exactVertexCount == 0u",
        )
        call = self.device.index("War3EvaluateTerrainIndexedHintAgainstExactDomain")
        scan = self.device.rfind("ComputeWar3ExactIndexVertexDomainPrepared", 0, call)
        self.assertGreater(scan, 0)

    def test_observer_does_not_authorize_bounds(self) -> None:
        self.assertEqual(
            self.device.count("War3EvaluateTerrainIndexedHintAgainstExactDomain"),
            1,
        )
        resolver = re.search(
            r"War3ResolveTerrainBoundsVertexRange\((?P<body>.*?)\);",
            self.device,
            re.S,
        )
        self.assertIsNotNone(resolver)
        self.assertIn("exactIndexedDomainKnown", resolver.group("body"))
        self.assertNotIn("Hint", resolver.group("body"))

    def test_all_relations_are_structurally_accounted(self) -> None:
        for suffix in (
            "HintComparableCount",
            "HintExactCount",
            "HintSupersetCount",
            "HintUnderCoverageCount",
            "HintInvalidCount",
            "HintRangeAcceptedCount",
            "HintRangeRejectedCount",
        ):
            field = "semanticSceneTerrainBoundsProducer" + suffix
            self.assertIn(field, self.scene)
            self.assertIn(field, self.device)


if __name__ == "__main__":
    unittest.main()
