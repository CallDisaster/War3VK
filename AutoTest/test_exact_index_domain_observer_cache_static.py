#!/usr/bin/env python3
"""Static contracts for the generation-backed exact index-domain observer cache."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class ExactIndexDomainObserverCacheStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = read(
            "src/d3d9/war3/memory/war3_exact_index_domain_observer_cache.h"
        )
        cls.device = read("src/d3d9/d3d9_device.cpp")
        cls.scene = read("src/d3d9/d3d9_war3_scene.h")
        cls.monitor_h = read("src/d3d9/war3/tools/war3_perf_monitor.h")
        cls.monitor_cpp = read("src/d3d9/war3/tools/war3_perf_monitor.cpp")
        cls.analyzer = read("AutoTest/war3_autotest_mcp.py")
        cls.release = read("src/d3d9/war3/core/war3_internal_test_config.h")

    def test_key_closes_epoch_generation_and_range_identity(self) -> None:
        for token in (
            "mapEpoch",
            "deviceEpoch",
            "ownerIdentity",
            "spanDataIdentity",
            "identityGeneration",
            "allocationGeneration",
            "contentGeneration",
            "spanLength",
            "indexElementBytes",
            "indexCount",
            "baseVertex",
            "vertexCapacity",
        ):
            self.assertIn(token, self.header)
        self.assertIn("entry.key == key", self.header)

    def test_cache_is_bounded_value_only_and_deterministic(self) -> None:
        self.assertIn("std::array<std::array<Entry, WayCount>, SetCount>", self.header)
        self.assertIn("least", self.header.lower())
        self.assertIn("set[way].lastUseSerial < set[victim].lastUseSerial", self.header)
        for forbidden in (
            "Rc<",
            "DxvkDevice",
            "VkBuffer",
            "vkGet",
            "vkWait",
            "std::vector",
            "std::unordered_map",
            "mutex",
        ):
            self.assertNotIn(forbidden, self.header)

    def test_observer_cache_cannot_authorize_freeze_or_consume(self) -> None:
        observer_gate = re.search(
            r"const bool useBoundsObserverDomainCache\s*=\s*"
            r"exactIndexedTerrainBoundsAuditSample\s*&&\s*"
            r"!exactIndexedFreezeTrimCandidate\s*&&\s*"
            r"!exactDomainFromDeclaredHint;",
            self.device,
        )
        self.assertIsNotNone(observer_gate)
        self.assertIn(
            "const bool needsRebasedIndex = exactIndexedFreezeTrimCandidate",
            self.device,
        )
        self.assertIn(
            "if ((exactIndexedFreezeTrimCandidate || consumeCoherentRealTrim)",
            self.device,
        )
        self.assertIn("kReleaseFreezeExperimentalShadowRoutes = true", self.release)

    def test_production_key_uses_current_span_and_epochs(self) -> None:
        block = re.search(
            r"War3ExactIndexDomainObserverKey cacheKey = \{(?P<body>.*?)\n\s*\};",
            self.device,
            re.S,
        )
        self.assertIsNotNone(block)
        body = block.group("body")
        for token in (
            "ShadowModelResourceCache::instance().mapEpoch()",
            "m_war3GpuSkinDeviceEpoch",
            "exactIndexSpan.ownerIdentity",
            "exactIndexSpan.data",
            "exactIndexSpan.identityGeneration",
            "exactIndexSpan.allocationGeneration",
            "exactIndexSpan.contentGeneration",
            "exactIndexSpan.length",
            "indexElementBytes",
            "CountVal",
            "BaseVertexIndex",
            "positionCapacity64",
        ):
            self.assertIn(token, body)

    def test_miss_scans_and_hit_does_not(self) -> None:
        block = re.search(
            r"if \(useGenerationBackedDomainCache\) \{(?P<body>.*?)\n\s*\}"
            r"\n\s*if \(!exactDomainFromDeclaredHint",
            self.device,
            re.S,
        )
        self.assertIsNotNone(block)
        body = block.group("body")
        self.assertIn("observerDomainCacheHit = lookup == Lookup::Hit", body)
        miss_branch = body.find("} else {")
        scan = body.find("ComputeWar3ExactIndexVertexDomainPrepared", miss_branch)
        store = body.find("s_observerDomainCache.store", scan)
        self.assertGreaterEqual(miss_branch, 0)
        self.assertGreater(scan, miss_branch)
        self.assertGreater(store, scan)

    def test_cache_diagnostics_reach_report_and_analyzer(self) -> None:
        names = (
            "DomainCacheLookupCount",
            "DomainCacheHitCount",
            "DomainCacheMissCount",
            "DomainCacheCollisionMissCount",
            "DomainCacheStoreCount",
            "DomainCacheEvictionCount",
            "HintComparableCount",
            "HintExactCount",
            "HintSupersetCount",
            "HintUnderCoverageCount",
            "HintInvalidCount",
            "HintRangeAcceptedCount",
            "HintRangeRejectedCount",
        )
        for suffix in names:
            field = "semanticSceneTerrainBoundsProducer" + suffix
            self.assertIn(field, self.scene)
            self.assertIn(field, self.monitor_h)
            self.assertGreaterEqual(self.monitor_cpp.count(field), 3)
        for name in (
            "domainCacheLookups",
            "domainCacheHits",
            "domainCacheMisses",
            "domainCacheCollisionMisses",
            "domainCacheStores",
            "domainCacheEvictions",
            "hintComparable",
            "hintExact",
            "hintConservativeSuperset",
            "hintUnderCoverage",
            "hintInvalid",
            "hintRangeAccepted",
            "hintRangeRejected",
        ):
            self.assertIn(name, self.analyzer)

    def test_no_content_fingerprint_or_gpu_lifetime_shortcut(self) -> None:
        self.assertNotIn("fingerprint", self.header.lower())
        self.assertNotIn("contentHash", self.header)
        self.assertNotIn("fence", self.header.lower())
        self.assertNotIn("last-use", self.header.lower())


if __name__ == "__main__":
    unittest.main()
