#!/usr/bin/env python3
"""Contracts for reusing the grouped preselector's exact Visible value."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = DEVICE.index(signature)
    brace = DEVICE.index("{", start)
    depth = 0
    for pos in range(brace, len(DEVICE)):
        if DEVICE[pos] == "{":
            depth += 1
        elif DEVICE[pos] == "}":
            depth -= 1
            if depth == 0:
                return DEVICE[start : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


class DirectPreselectedVisibleHandoffStaticTests(unittest.TestCase):
    def test_selection_lookup_returns_a_value_copy_only_on_hit(self) -> None:
        selector = function_body("uint64_t War3SemanticDirectRecordSelectionKey(")
        self.assertIn("VisibleRenderableRecord** outVisibleHint", selector)
        self.assertIn("*outVisibleHint = nullptr;", selector)
        query = selector.index("visibleQueryCache->queryPtr(")
        handoff = selector.index("*outVisibleHint = visible", query)
        self.assertLess(query, handoff)

    def test_hint_storage_is_dense_current_call_value_scratch(self) -> None:
        start = DEVICE.index(
            "static thread_local std::vector<uint32_t> s_recordIndicesForBuild"
        )
        end = DEVICE.index("// --- Step 2: build eligible record list", start)
        block = DEVICE[start:end]
        for token in (
            "s_preselectedVisibleHints",
            "s_recordVisibleHintIndicesForBuild",
            "preselectedVisibleHints.clear()",
            "recordVisibleHintIndicesForBuild.clear()",
            "record, &visibleHint, &visiblePartLayerQueryCache",
            "preselectedVisibleHints.push_back(*visibleHint)",
            "preselectedRecords[i].visibleHintIndex",
        ):
            self.assertIn(token, block)
        self.assertNotIn(
            "recordVisibleHintsByIndex.resize(directRecords.size())", block
        )

    def test_builder_uses_matching_hint_and_keeps_canonical_fallback(self) -> None:
        builder = function_body(
            "bool War3TryBuildShadowPacketFromCurrentDrawRecord("
        )
        hint = builder.index("const bool preselectedVisibleMatches")
        borrow = builder.index(
            "visibleHit ? preselectedVisibleRecord : nullptr", hint)
        fallback = builder.index("queryFirstForDirectPacket(", borrow)
        self.assertLess(hint, borrow)
        self.assertLess(borrow, fallback)
        self.assertNotIn("visibleRecord = *preselectedVisibleRecord", builder)
        gate = builder[hint:borrow]
        self.assertIn("record.renderablePart != nullptr", gate)
        self.assertIn(
            "preselectedVisibleRecord->renderablePart == record.renderablePart",
            gate,
        )

    def test_only_grouped_selected_records_can_supply_the_hint(self) -> None:
        loop = DEVICE.index("for (size_t buildIndex = 0u;")
        packet_call = DEVICE.index(
            "War3TryBuildShadowPacketFromCurrentDrawRecord(", loop
        )
        call_block = DEVICE[packet_call : DEVICE.index("packetBuildTiming.finish()", packet_call)]
        self.assertIn("preselectedVisibleRecord", call_block)
        fallback = DEVICE.index('enterDirectDetailPhase("SnapshotFallbackCopy")')
        fallback_end = DEVICE.index("// --- Step 2: build eligible record list", fallback)
        self.assertIn(
            "recordVisibleHintIndicesForBuild.assign(directRecords.size(), UINT32_MAX)",
            DEVICE[fallback:fallback_end],
        )


if __name__ == "__main__":
    unittest.main()
