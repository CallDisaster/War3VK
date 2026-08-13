"""Contracts for O(1) shadow-fallback append bookkeeping."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
POLICY = (
    ROOT / "src/d3d9/war3/render/war3_shadow_fallback_breakdown.h"
).read_text(encoding="utf-8")


class ShadowFallbackBreakdownStaticTests(unittest.TestCase):
    def test_mutation_inventory_is_closed(self) -> None:
        self.assertEqual(DEVICE.count("shadowFallbacks.push_back("), 2)
        self.assertEqual(DEVICE.count("shadowFallbacks.erase("), 1)
        for forbidden in (
            "shadowFallbacks.emplace_back(",
            "shadowFallbacks.insert(",
            "shadowFallbacks.pop_back(",
            "shadowFallbacks.resize(",
            "shadowFallbacks.swap(",
            "shadowFallbacks.assign(",
        ):
            self.assertNotIn(forbidden, DEVICE)

    def test_both_append_sites_use_one_value_helper(self) -> None:
        pushes = [m.start() for m in re.finditer("shadowFallbacks.push_back\\(", DEVICE)]
        self.assertEqual(len(pushes), 2)
        for push in pushes:
            tail = DEVICE[push : push + 1800]
            note = tail.index("War3NoteShadowFallbackAppended(")
            snapshot = tail.index("fallbackSnapshotCount++")
            self.assertGreater(note, 0)
            self.assertGreater(snapshot, note)
            self.assertNotIn("War3RecomputeFallbackBreakdown", tail[:snapshot])
            self.assertIn("shadowFallbacks.back().snapshot", tail[:snapshot])

    def test_only_prune_recomputes_live_vector(self) -> None:
        self.assertEqual(DEVICE.count("War3RecomputeFallbackBreakdown("), 2)
        definition = DEVICE.index("void War3RecomputeFallbackBreakdown(")
        call = DEVICE.index(
            "War3RecomputeFallbackBreakdown(",
            definition + len("void War3RecomputeFallbackBreakdown("),
        )
        erase = DEVICE.rindex("shadowFallbacks.erase(", 0, call)
        self.assertLess(erase, call)
        self.assertLess(call - erase, 300)

    def test_classification_remains_nonexclusive(self) -> None:
        accumulate = POLICY.index("War3AccumulateShadowFallbackClassification")
        append = POLICY.index("War3NoteShadowFallbackAppended", accumulate)
        body = POLICY[accumulate:append]
        self.assertIn("StageCategory::Terrain", body)
        self.assertIn("StageCategory::WorldObject", body)
        self.assertIn("StageCategory::Effect", body)
        self.assertIn("ObjectKind::Unit", body)
        self.assertNotIn("else if", body)
        self.assertNotIn("stats.fallbackSnapshotCount", POLICY)

    def test_full_scan_reuses_same_classification_helper(self) -> None:
        start = DEVICE.index("void War3RecomputeFallbackBreakdown(")
        end = DEVICE.index("uint64_t War3GetShadowFallbackBudgetCapBytes", start)
        body = DEVICE[start:end]
        self.assertIn("War3ResetShadowFallbackBreakdown", body)
        self.assertIn("War3AccumulateShadowFallbackClassification", body)
        for field in (
            "fallbackDrawCountTerrain++",
            "fallbackDrawCountWorldObject++",
            "fallbackDrawCountUnitObject++",
        ):
            self.assertNotIn(field, body)


if __name__ == "__main__":
    unittest.main()
