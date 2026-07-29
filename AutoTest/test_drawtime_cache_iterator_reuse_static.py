"""Static contracts for DrawTime cache iterator reuse."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
MONITOR_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"


class DrawTimeCacheIteratorReuseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.device = DEVICE_CPP.read_text(encoding="utf-8")
        cls.monitor = MONITOR_CPP.read_text(encoding="utf-8")

    def test_default_candidate_and_opt_in_verifier_are_reported(self) -> None:
        candidate = "DXVK_WAR3_DRAWTIME_CACHE_ITERATOR_REUSE"
        verifier = "DXVK_WAR3_DRAWTIME_CACHE_ITERATOR_REUSE_VERIFY"
        self.assertIn(f'"{candidate}", 1u', self.device)
        self.assertIn(f'"{verifier}", 0u', self.device)
        self.assertIn(f'"{candidate}"', self.monitor)
        self.assertIn(f'"{verifier}"', self.monitor)

    def test_existing_iterator_is_reused_before_legacy_operator_index(self) -> None:
        start = self.device.index(
            "auto drawTimeCacheIt = m_war3DrawTimeVBCache.end();"
        )
        end = self.device.index(
            "entry.renderablePart = vbCachePart;", start
        )
        block = self.device[start:end]
        self.assertIn(
            "drawTimeCacheIt = m_war3DrawTimeVBCache.find(vbCacheKey);",
            block,
        )
        reuse = block.index("return drawTimeCacheIt->second;")
        legacy = block.index("return m_war3DrawTimeVBCache[vbCacheKey];")
        self.assertLess(reuse, legacy)
        self.assertIn(
            "auto& legacyEntry = m_war3DrawTimeVBCache[vbCacheKey];",
            block,
        )
        self.assertIn("if (&legacyEntry != &entry)", block)


if __name__ == "__main__":
    unittest.main()
