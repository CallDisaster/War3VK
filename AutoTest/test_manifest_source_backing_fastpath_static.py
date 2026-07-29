import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CONTRACT_CPP = (
    ROOT
    / "src"
    / "d3d9"
    / "war3"
    / "shadow"
    / "war3_shadow_runtime_contract.cpp"
).read_text(encoding="utf-8")
CONTRACT_H = (
    ROOT
    / "src"
    / "d3d9"
    / "war3"
    / "shadow"
    / "war3_shadow_runtime_contract.h"
).read_text(encoding="utf-8")
BRIDGE_CPP = (
    ROOT
    / "src"
    / "d3d9"
    / "war3"
    / "render"
    / "war3_shadow_runtime_bridge.cpp"
).read_text(encoding="utf-8")
REPORT_CPP = (
    ROOT
    / "src"
    / "d3d9"
    / "war3"
    / "tools"
    / "war3_perf_monitor.cpp"
).read_text(encoding="utf-8")


class ManifestSourceBackingFastPathStaticTest(unittest.TestCase):
    def test_fast_path_requires_complete_consistent_source_backing(self):
        self.assertIn(
            "record.runtimeGeosetPtr != nullptr &&",
            CONTRACT_CPP,
        )
        self.assertIn(
            "record.meshIndex != kInvalidShadowContractGeosetIndex &&",
            CONTRACT_CPP,
        )
        self.assertIn(
            "record.geosetIndex != kInvalidShadowContractGeosetIndex &&",
            CONTRACT_CPP,
        )
        self.assertIn("record.meshIndex == record.geosetIndex", CONTRACT_CPP)

    def test_default_path_has_exact_rollback_and_verifier(self):
        self.assertIn(
            'std::getenv("DXVK_WAR3_MANIFEST_SOURCE_BACKING_FAST_PATH")',
            CONTRACT_CPP,
        )
        self.assertIn(
            "config.enabled = enabled != nullptr && enabled[0] == '1';",
            CONTRACT_CPP,
        )
        self.assertIn(
            '"DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY"',
            CONTRACT_CPP,
        )
        self.assertIn(
            '"DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY_ASSERT"',
            CONTRACT_CPP,
        )
        self.assertIn(
            "ResolveCurrentRuntimeGeosetFromDataLegacy(legacy,",
            CONTRACT_CPP,
        )
        self.assertIn("std::abort();", CONTRACT_CPP)

    def test_raw_scan_cost_and_mismatch_are_reported(self):
        for symbol in (
            "manifestResolveSourceCompleteSkipCount",
            "manifestResolveRawScanCount",
            "manifestResolveRawScanEntryVisitCount",
            "manifestResolveRawScanMissCount",
            "manifestResolveVerifierAttemptCount",
            "manifestResolveVerifierMismatchCount",
            "manifestResolveMaxRuntimeGeosetCount",
        ):
            self.assertIn(symbol, CONTRACT_H)
            self.assertIn(symbol, BRIDGE_CPP)
        self.assertIn(
            "semanticManifestResolveRawScanEntryVisitCount",
            REPORT_CPP,
        )
        self.assertIn(
            "semanticManifestResolveVerifierMismatchCount",
            REPORT_CPP,
        )

    def test_model_resource_negative_cache_has_revision_and_verifier(self):
        self.assertIn(
            '"DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE"',
            CONTRACT_CPP,
        )
        self.assertIn("entry.resourceRevision == resourceRevision", CONTRACT_CPP)
        self.assertIn(
            "entry.ownedModelDataHandle == ownedModelDataHandle",
            CONTRACT_CPP,
        )
        self.assertIn(
            '"DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY_ASSERT"',
            CONTRACT_CPP,
        )
        self.assertIn(
            "semanticManifestModelResourceCacheHitCount",
            REPORT_CPP,
        )
        self.assertIn(
            "semanticManifestModelResourceVerifierMismatchCount",
            REPORT_CPP,
        )


if __name__ == "__main__":
    unittest.main()
