from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)
CONTRACT_HEADER = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.h"
).read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
POLICY = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_winner_filter_policy.h"
).read_text(encoding="utf-8")


class CurrentDrawCanonicalWinnerFilterStaticTests(unittest.TestCase):
    def test_filter_is_deferred_until_after_canonical_order(self) -> None:
        snapshot = CONTRACT.split(
            "void SnapshotPublishedCurrentDrawContracts(\n"
            "    const CurrentDrawContractSnapshotOptions& options,\n"
            "    std::vector<CurrentDrawContractRecord>& out) {",
            1,
        )[1].split("CurrentDrawResolveStatus ResolveCurrentDrawAuthoritativeSample", 1)[0]
        scan = snapshot.index('enterSnapshotPhase("SnapshotActiveSlotScan")')
        order = snapshot.index('enterSnapshotPhase("SnapshotBatchedOrder")')
        materialize = snapshot.index(
            "MaterializeCurrentDrawCanonicalWinnerPrefix(", order
        )
        self.assertLess(scan, order)
        self.assertLess(order, materialize)
        self.assertNotIn(
            "canonicalWinnerFilter.accepts",
            snapshot[:order],
        )
        self.assertIn("std::sort(s_sortKeys.begin(), s_sortKeys.end()", snapshot[order:materialize])
        self.assertIn("out.push_back(SnapshotRecordWithGrace(record))", snapshot[materialize:])

    def test_fast_route_is_local_bounded_and_trace_safe(self) -> None:
        gate = CONTRACT.split(
            "const bool deferredCanonicalWinnerFilter =", 1
        )[1].split(";", 1)[0]
        for token in (
            "batchedBoundedSnapshot",
            "activeSlotSnapshot",
            "!traceSnapshot",
            "options.canonicalWinnerFilter.valid()",
        ):
            self.assertIn(token, gate)
        grouped = DEVICE.split(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
        )[1].split(
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1
        )[0]
        self.assertIn(
            "if constexpr (!dxvk::war3::render::kDevelopmentShadowObserversEnabled)",
            grouped,
        )
        self.assertIn("ShadowPoseFullTraceFastEnabledForRenderThread()", grouped)
        self.assertIn("snapshotOptions.maxRecords != 0u", grouped)
        self.assertIn("const void* context = nullptr", CONTRACT_HEADER)
        trace_call = grouped.index("NoteCurrentDrawSnapshotFrame(")
        trace_gate = grouped.rfind(
            "if (!snapshotSummary.canonicalWinnerFilterApplied)",
            0,
            trace_call,
        )
        self.assertGreaterEqual(trace_gate, 0)

    def test_grace_materialization_cannot_change_exact_owner_key(self) -> None:
        grace = CONTRACT.split(
            "CurrentDrawContractRecord SnapshotRecordWithGrace(", 1
        )[1].split("std::atomic<uint64_t> g_publishAttemptCount", 1)[0]
        for allowed_write in (
            "snapshot.fromGrace =",
            "snapshot.producerFreshThisFrame =",
            "snapshot.graceAge =",
        ):
            self.assertIn(allowed_write, grace)
        for exact_key_field in (
            "renderablePart",
            "layerIndex",
            "meshPayloadPtr",
            "payloadWord108",
            "payloadWord11C",
            "jHandle",
            "instanceIdentity",
        ):
            self.assertNotIn(f"snapshot.{exact_key_field} =", grace)

    def test_exact_filter_uses_full_existing_owner_contract(self) -> None:
        grouped = DEVICE.split(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
        )[1].split(
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1
        )[0]
        owner = grouped.split(
            "const auto currentFrameDrawTimeProducerOwnsRecord =", 1
        )[1].split(
            "m_war3Scene.shadowStats.semanticSceneDirectSelectionLeaseActiveKeyCount", 1
        )[0]
        for token in (
            "War3CurrentDrawContractNamesExactSlice(",
            "War3MakeDrawTimeVBCacheKey(",
            "m_war3GpuSkinMapEpoch",
            "War3DrawTimeExactRejectedCurrentFrame(cacheKey)",
            "vbIt->second.MatchesKey(cacheKey)",
            "vbIt->second.frameSerial == m_war3ShadowPersistentFrameSerial",
            "vbIt->second.exactOwnerFrameSerial ==",
        ):
            self.assertIn(token, owner)
        self.assertLess(
            grouped.index("const auto currentFrameDrawTimeProducerOwnsRecord ="),
            grouped.index("SnapshotPublishedCurrentDrawContracts("),
        )

    def test_diagnostics_retain_prefilter_cardinality(self) -> None:
        grouped = DEVICE.split(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
        )[1].split(
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1
        )[0]
        self.assertIn("snapshotSummary.canonicalWinnerCount", grouped)
        self.assertIn("snapshotSummary.filteredCanonicalWinnerCount", grouped)
        self.assertIn("snapshotSummary.canonicalLayerNonZeroCount", grouped)
        self.assertIn(
            ".drawTimeSemanticProducerOwnedDirectGroupedSkipCount +=",
            grouped,
        )

    def test_policy_filters_only_fixed_prefix(self) -> None:
        self.assertIn("const size_t canonicalCount", POLICY)
        self.assertIn("std::min(orderedWinners.size(), maxWinners)", POLICY)
        self.assertIn("for (size_t i = 0u; i < canonicalCount; ++i)", POLICY)
        self.assertNotIn("backfill", POLICY.lower().split("//", 1)[0])


if __name__ == "__main__":
    unittest.main()
