#!/usr/bin/env python3
"""Static contract for allocation-free common explicit-blend scratch."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp"
).read_text(encoding="utf-8")

assert '#include "../../../util/util_small_vector.h"' in SOURCE
for declaration in (
    "small_vector<DynamicAuxStreamCandidate, 16u>",
    "small_vector<CompactRemapSpanTable, 32u>",
    "small_vector<PendingCompactRemapNode, 32u>",
    "small_vector<uint32_t, 4u>",
):
    assert declaration in SOURCE

collect_aux = SOURCE.split(
    "DynamicAuxStreamCandidates CollectDynamicAuxStreamCandidates(", 1
)[1].split("bool TryResolveMeshLayerBindingContract(", 1)[0]
assert "std::vector<DynamicAuxStreamCandidate>" not in collect_aux
assert "candidates.reserve" not in collect_aux

collect_remap = SOURCE.split(
    "CompactRemapSpanTables CollectCompactRemapSpanTables(", 1
)[1].split("bool TryBuildPackedTupleKeyWithSpanRemap(", 1)[0]
assert "std::vector<CompactRemapSpanTable>" not in collect_remap
assert "std::vector<PendingCompactRemapNode>" not in collect_remap
assert "kMaxCompactRemapCandidates = 32u" in SOURCE
assert "pending.size() >= kMaxCompactRemapCandidates" in collect_remap
assert "out.size() < kMaxCompactRemapCandidates" in collect_remap

explicit = SOURCE.split(
    "bool TryResolveMeshDynamicExplicitBlendSkinning(", 1
)[1].split("bool ShouldBuildAttachmentSupplementalForChunk(", 1)[0]
assert "std::vector<DynamicAuxStreamCandidate>" not in explicit
assert "std::vector<uint32_t> stagePresetBaseBiases" not in explicit
assert "StagePresetBaseBiasCandidates stagePresetBaseBiases = {0u}" in explicit
for token in (
    "std::stable_sort(",
    "TryBuildOrderedTupleSlots(",
    "TryBuildOrderedTupleSlotsWithAnySpanRemap(",
    "TryBuildOrderedTupleSlotsWithLocalRemap(",
    "outResult.weights.resize(vertexCount)",
    "outResult.indices.resize(vertexCount)",
):
    assert token in SOURCE

print("explicit blend small scratch static tests passed")
