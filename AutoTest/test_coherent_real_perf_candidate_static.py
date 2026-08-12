#!/usr/bin/env python3
"""Contracts for the isolated coherent-REAL performance candidate build."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
OPTIONS = (ROOT / "meson_options.txt").read_text(encoding="utf-8")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")
CONTRACT = (ROOT / "src/d3d9/war3/memory/war3_coherent_real_index_trim_contract.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class CoherentRealPerfCandidateStaticTests(unittest.TestCase):
    def test_release_default_remains_disabled(self):
        self.assertIn(
            "option('warvk_coherent_real_perf_candidate_dev', type : 'boolean', value : false",
            OPTIONS,
        )
        self.assertIn("kCoherentRealPerformanceCandidateEnabled = false", CONTRACT)

    def test_candidate_enables_only_the_proven_coherent_real_route(self):
        block = MESON[MESON.index("if get_option('warvk_coherent_real_perf_candidate_dev')"):]
        block = block[:block.index("endif")]
        self.assertIn("WARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV=1", block)
        self.assertIn("WARVK_ENABLE_COHERENT_REAL_PERF_CANDIDATE_DEV=1", block)
        for forbidden in (
            "WARVK_ENABLE_SHADOW_OBSERVERS_DEV",
            "WARVK_ENABLE_CURRENT_UP_SHADOW_REPLAY_DEV",
            "WARVK_ENABLE_COHERENT_UP_INDEX_TRIM_DEV",
            "WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV",
        ):
            self.assertNotIn(forbidden, block)

    def test_candidate_compiles_route_but_runtime_defaults_off(self):
        self.assertIn("DefaultWar3CoherentRealIndexTrimConfiguredMode", CONTRACT)
        self.assertIn("return 0u;", CONTRACT)
        self.assertIn("DefaultWar3CoherentRealHintDomainEnabled", CONTRACT)
        self.assertIn("return false;", CONTRACT)
        mode = DEVICE[DEVICE.index("War3CoherentRealIndexTrimModeRuntime()") :]
        mode = mode[:mode.index("inline bool War3CoherentRealDomainCacheRuntime")]
        self.assertIn("DefaultWar3CoherentRealIndexTrimConfiguredMode()", mode)
        hint = DEVICE[DEVICE.index("inline bool War3CoherentRealHintDomainRuntime()") :]
        hint = hint[:hint.index("inline bool War3Stage11DirectUploadSourceRuntime")]
        self.assertIn("DefaultWar3CoherentRealHintDomainEnabled()", hint)

    def test_runtime_ab_overrides_remain_available(self):
        self.assertIn("DXVK_WAR3_COHERENT_REAL_INDEX_TRIM_MODE", DEVICE)
        self.assertIn("DXVK_WAR3_COHERENT_REAL_DOMAIN_CACHE", DEVICE)
        self.assertIn("DXVK_WAR3_COHERENT_REAL_HINT_DOMAIN", DEVICE)
        self.assertIn("configuredMode == 1u", CONTRACT)
        self.assertIn("configuredMode == 2u", CONTRACT)


if __name__ == "__main__":
    unittest.main()
