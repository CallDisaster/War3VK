from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

start = SOURCE.index("bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(")
end = SOURCE.index("void D3D9DeviceEx::War3MarkDrawTimeExactRejectedCurrentFrame(", start)
body = SOURCE[start:end]

assert "struct SemanticBlendVertex" in body
assert "std::vector<SemanticBlendVertex> blendVertices;" in body
assert "std::vector<std::array<uint8_t, 4>> blendIndices" not in body
assert "std::vector<std::array<float, 3>> blendWeights" not in body

# Both contracts validate and populate the upload vertex in their original
# source traversal; no second vertex loop copies intermediate arrays.
assert "const auto& sourceWeights = canonicalSkin.explicitBlendWeights[i];" in body
assert "const auto& sourceIndices = canonicalSkin.explicitBlendIndices[i];" in body
assert "blendVertices[i].weights[0] = sourceWeights[0];" in body
assert "blendVertices[i].indices[0] = sourceIndices[0];" in body
assert "blendVertices[i].indices[0] = uint8_t(groupSlot);" in body
assert body.count("blendVertices.resize(vertexCount);") == 2

# The final upload contract and layout stay unchanged.
assert 'uploads[2].debugName = "War3SemanticShadowBlend";' in body
assert "uploads[2].bytes =\n        VkDeviceSize(blendVertices.size() * sizeof(blendVertices[0]));" in body
assert "candidate.blendStride = (skinned) ? 16u : 0u;" in body
assert "candidate.blendIndexOffset = skinned ? 12u : 0u;" in body

print("semantic blend vertex single-pass contract: ok")
