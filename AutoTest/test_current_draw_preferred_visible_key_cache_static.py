#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)

block = SOURCE.split("enterSnapshotPhase(\"SnapshotScratchSetup\")", 1)[1].split(
    "const auto isPreferredWithIdentity", 1
)[0]

assert "PreferredVisibleKeyCacheEntry" in block
assert "kPreferredVisibleKeyCacheEntries = 512u" in block
assert "std::array<PreferredVisibleKeyCacheEntry" in block
assert "thread_local uint64_t s_preferredVisibleKeyCacheGeneration" in block
assert "preferredVisibleKeyCacheGeneration == 0u" in block
assert "s_preferredVisibleKeyCache = {};" in block
assert "cached.renderablePart == record.renderablePart" in block
assert "cached.layerIndex == record.layerIndex" in block
assert "cached.generation == preferredVisibleKeyCacheGeneration" in block
assert "queryByRenderablePartAndLayer(" in block
assert block.index("cached.generation") < block.index(
    "queryByRenderablePartAndLayer("
)
assert "preferredVisibleKeyCache.emplace" not in block
assert "unordered_map<uint64_t, uint64_t>" not in block

# A direct-map collision must never grant a hit from the folded hash alone.
# The complete pointer/layer key is checked before the cached value is used.
hit = block.split("if (cached.generation", 1)[1].split("uint64_t key", 1)[0]
assert "cached.renderablePart == record.renderablePart" in hit
assert "cached.layerIndex == record.layerIndex" in hit
assert "return cached.value;" in hit

print("current-draw preferred visible-key cache static checks passed")
