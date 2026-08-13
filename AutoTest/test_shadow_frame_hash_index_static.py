from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/war3/render/war3_frame_hash_index.h").read_text(
    encoding="utf-8"
)
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

assert "class War3FrameHashIndex final" in HEADER
assert "std::vector<Bucket> m_buckets" in HEADER
assert "std::vector<Node> m_nodes" in HEADER
assert "if (++m_generation == 0u)" in HEADER
assert "m_nodes.clear();" in HEADER
assert "m_nodes =" not in HEADER
assert "#include <unordered_" not in HEADER
assert "std::unordered_" not in HEADER

assert "War3FrameHashIndex m_war3ShadowPaletteHashIndex" in DEVICE_H
assert "War3FrameHashIndex\n      m_war3SemanticPaletteCacheHashIndex" in DEVICE_H
assert "std::unordered_multimap<uint64_t, uint32_t> m_war3ShadowPaletteHashIndex" not in DEVICE_H

for index_name in (
    "m_war3ShadowPaletteHashIndex",
    "m_war3SemanticPaletteCacheHashIndex",
):
    assert f"{index_name}.insert(" in DEVICE
    assert f"{index_name}.forEach(" in DEVICE
    assert f"{index_name}.reserve(m_shadowPaletteReserveHint)" in DEVICE
    assert f"{index_name}.emplace(" not in DEVICE

print("shadow frame hash index static checks passed")
