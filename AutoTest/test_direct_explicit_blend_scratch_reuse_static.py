#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
CORE = (ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp").read_text(
    encoding="utf-8")
HEADER = (ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.h").read_text(
    encoding="utf-8")


def function(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise AssertionError(signature)


packet = function(DEVICE, "bool War3TryBuildShadowPacketFromCurrentDrawRecord(")
wrapper = function(CORE, "bool TryResolveExplicitBlendSkinningForRenderable(")
resolver = function(CORE, "bool TryResolveMeshDynamicExplicitBlendSkinning(")

assert "ResetShadowExplicitBlendSkinningResultPreserveScratch" in HEADER
assert "weights.capacity() > 200000u" in HEADER
assert "indices.capacity() > 200000u" in HEADER
assert "runtimeGroupPalette.capacity() > 256u" in HEADER
assert "std::move(resource.ownedVertexBlendWeights)" in packet
assert "std::move(resource.ownedVertexBlendIndices)" in packet
assert "resource.ownedVertexBlendWeights =\n            std::move(explicitBlend.weights)" in packet
assert "resource.ownedVertexBlendIndices =\n            std::move(explicitBlend.indices)" in packet

assert "internal.weights = std::move(outResult.weights)" in wrapper
assert "internal.indices = std::move(outResult.indices)" in wrapper
assert "const auto restoreScratch" in wrapper
assert wrapper.count("restoreScratch();") == 2
assert wrapper.index("restoreScratch();") < wrapper.index(
    "TryResolveMeshDynamicExplicitBlendSkinning(")

assert "auto weightsScratch = std::move(outResult.weights)" in resolver
assert "auto indicesScratch = std::move(outResult.indices)" in resolver
assert resolver.index("outResult.weights = std::move(weightsScratch)") < resolver.index(
    "outResult.weights.resize(vertexCount)")
assert "outResult.weights[i] = weights" in resolver
assert "outResult.indices[i] = indices" in resolver

for text in (packet, wrapper, resolver):
    assert "static thread_local" not in text
    assert "static ShadowExplicitBlendSkinningResult" not in text

print("test_direct_explicit_blend_scratch_reuse_static: 15/15 passed")
