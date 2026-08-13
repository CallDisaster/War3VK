from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)


def function_region() -> str:
    start = SOURCE.index("uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped")
    end = SOURCE.index(
        "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene", start
    )
    return SOURCE[start:end]


body = function_region()

# The scratch table keeps only CPU vector capacity. Frame and map stamps make
# stale entries unreachable; no geometry, palette, packet, or GPU owner is
# retained by this table.
assert "struct LiveSubmittedCorePartsEntry" in body
assert "uint64_t frameSerial = 0u;" in body
assert "std::vector<uint64_t> partKeys;" in body
assert "s_liveSubmittedCorePartsMapEpoch != m_war3GpuSkinMapEpoch" in body
assert "s_liveSubmittedCorePartsByObject.clear();" in body
assert "s_liveSubmittedCorePartsFrameSerial !=" in body
assert "directPartPacketLeaseFrame" in body
assert "entry.frameSerial != directPartPacketLeaseFrame" in body
assert "entry.partKeys.clear();" in body
assert "entry.partKeys.push_back(partKey);" in body

# A stale retained node must not participate in this frame's completeness
# lookup or update traversal.
assert "it->second.frameSerial == directPartPacketLeaseFrame" in body
assert "for (uint64_t objectKey : liveSubmittedCorePartObjectKeys)" in body
assert "liveIt->second.frameSerial != directPartPacketLeaseFrame" in body

# Do not regress to destroying every unordered_map node and every nested
# vector allocation once per frame.
old_reset = """for (auto& kv : s_liveSubmittedCorePartsByObject)
    kv.second.clear();
  s_liveSubmittedCorePartsByObject.clear();"""
assert old_reset not in body

# Transient objects are retired with an amortized, bounded frame-age policy;
# the table is not allowed to grow for the full lifetime of a long map.
assert "kCorePartScratchRetireFrames = 256u" in body
assert "(directPartPacketLeaseFrame & 0xFFu) == 0u" in body
assert "s_liveSubmittedCorePartsByObject.erase(it)" in body

# Both live DirectGrouped submissions and exact Stage11 submissions feed the
# same current-frame helper.
assert body.count("appendLiveSubmittedCorePart(") == 2

print("direct core-part scratch reuse static checks passed")
