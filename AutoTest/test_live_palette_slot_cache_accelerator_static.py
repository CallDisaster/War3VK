from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# M2-2：resolvePaletteSlotIndex lambda 已迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.cpp（逐字节）；
# 本门禁的函数体断言跟随符号改锚到模块，断言内容一个字未改。
MODULE = (ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp").read_text(
    encoding="utf-8"
)

start = MODULE.index("auto resolvePaletteSlotIndex =")
end = MODULE.index("// 首选：从 Hook_RuntimeMatrixWrite", start)
body = MODULE[start:end]

assert "kMaxPaletteSlotCacheEntries = 4096u" in body
assert "kPaletteSlotCacheLookupEntries = 8192u" in body
assert "s_paletteSlotCacheLookup" in body

# Fast hits must revalidate both the accelerator and the authoritative cache
# entry. A lookup collision falls through to the original complete scan.
fast = body.index("if (lookup.renderablePart == partPtr")
fallback = body.index("for (size_t cacheIndex = 0u", fast)
insert = body.index("s_paletteSlotCacheCursor++", fallback)
assert fast < fallback < insert
assert "cached.renderablePart == partPtr && cached.mapEpoch == mapEpoch" in body[fast:fallback]
assert "entry.renderablePart != partPtr || entry.mapEpoch != mapEpoch" in body[fallback:insert]

# The slot itself is still refreshed from the current field or producer
# binding; the accelerator never supplies an unchecked palette slot value.
use = body[body.index("auto useCachedEntry ="):fast]
assert "currentSlotIndex != 0xFFFFFFFFu" in use
assert "queryProducerBindingSlot(" in use
assert "return entry.paletteSlotIndex" in use

print("live palette slot cache accelerator static checks passed")
