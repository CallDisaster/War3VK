#!/usr/bin/env python3
"""Contracts for the Observe-only DirectGrouped producer claim predictor."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
PERF_H = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h").read_text(
    encoding="utf-8"
)
PERF_CPP = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(
    encoding="utf-8"
)


class ProducerClaimLedgerObserveContracts(unittest.TestCase):
    def test_release_default_is_off_and_consume_is_denied(self) -> None:
        mode = DEVICE[
            DEVICE.index("enum class War3ProducerClaimObserveMode") :
            DEVICE.index("inline bool War3LegacyPerDrawSemanticScopesRuntime")
        ]
        self.assertIn(
            '"DXVK_WAR3_SEMANTIC_PRODUCER_CLAIM_LEDGER", 0u', mode
        )
        self.assertIn("Consume = 2u", mode)
        self.assertIn("kDevelopmentShadowObserversEnabled", mode)
        self.assertIn("ParseShadowObserverBuildMode", mode)
        runtime = mode[
            mode.index("War3ProducerClaimObserveModeRuntime") :
        ]
        self.assertIn("War3ProducerClaimObserveMode::Observe", runtime)
        self.assertIn("War3ProducerClaimObserveMode::Off", runtime)
        self.assertNotIn("War3ProducerClaimObserveMode::Consume", runtime)
        direct = DEVICE[DEVICE.index("producerClaimObserveMode =") :]
        direct = direct[: direct.index("m_war3CompactWorkTable.reset")]
        self.assertIn("semanticSceneProducerClaimConsumeDeniedCount++", direct)
        self.assertNotIn("continue;", direct)

    def test_observe_key_is_same_frame_and_does_not_query_winner_registry(self) -> None:
        start = DEVICE.index("uint64_t War3ProducerClaimObserveKey(")
        key = DEVICE[start : DEVICE.index("bool War3VisibleRecordMatches", start)]
        for token in (
            "mapEpoch",
            "deviceEpoch",
            "frameSerial",
            "objectKey",
            "record.renderablePart",
            "record.meshPayloadPtr",
            "record.layerIndex",
            "record.payloadWord108",
            "record.payloadWord11C",
        ):
            self.assertIn(token, key)
        object_start = DEVICE.index("War3ProducerClaimObserveObjectKey(")
        object_key = DEVICE[object_start:start]
        self.assertNotIn("VisibleRenderableRegistry::instance", object_key)

    def test_prediction_is_compared_only_after_canonical_packet_build(self) -> None:
        build = DEVICE[DEVICE.index("enterBuildEligiblePhase(\"RecordLoop\")") :]
        build = build[: build.index("if (traceBuildEligible)")]
        packet_build = build.index("War3TryBuildShadowPacketFromCurrentDrawRecord")
        compare = build.index("builtPacketHasCanonicalExactOwner")
        self.assertGreater(compare, packet_build)
        self.assertIn("semanticSceneProducerClaimStrictFalsePositiveCount++", build)
        self.assertIn("semanticSceneProducerClaimStrictFalseNegativeCount++", build)
        self.assertIn("semanticSceneProducerClaimLogicalFalsePositiveCount++", build)
        self.assertIn("semanticSceneProducerClaimLogicalFalseNegativeCount++", build)

    def test_closed_counters_are_exported_in_both_perf_summaries(self) -> None:
        names = (
            "semanticSceneProducerClaimObserveMode",
            "semanticSceneProducerClaimExactKeyCount",
            "semanticSceneProducerClaimCandidateCount",
            "semanticSceneProducerClaimCanonicalOwnedCount",
            "semanticSceneProducerClaimMissingKeyCount",
            "semanticSceneProducerClaimUnresolvedCount",
            "semanticSceneProducerClaimStrictFalsePositiveCount",
            "semanticSceneProducerClaimStrictFalseNegativeCount",
            "semanticSceneProducerClaimLogicalFalsePositiveCount",
            "semanticSceneProducerClaimLogicalFalseNegativeCount",
            "semanticSceneProducerClaimConsumeDeniedCount",
        )
        for name in names:
            self.assertIn(name, SCENE)
            self.assertIn(f"uint64_t {name} = 0", PERF_H)
            self.assertIn(f"agg.{name}", PERF_CPP)
            self.assertEqual(PERF_CPP.count(f'\\"{name}\\"'), 2)


if __name__ == "__main__":
    unittest.main()
