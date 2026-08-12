#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp").read_text(
    encoding="utf-8"
)


def function(signature: str, start_at: int = 0) -> str:
    start = SOURCE.index(signature, start_at)
    brace = SOURCE.index("{", start)
    depth = 0
    for index in range(brace, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


assert "enum class MeshLayerBindingResolveExtent : uint8_t" in SOURCE
assert "MaterialSignature," in SOURCE
assert "MeshLayerBindingResolveExtent::Full" in SOURCE

resolver = function(
    "bool TryResolveMeshLayerBindingContract(",
    SOURCE.index("DynamicAuxStreamCandidates CollectDynamicAuxStreamCandidates("),
)
material_guard = resolver.index(
    "extent == MeshLayerBindingResolveExtent::MaterialSignature"
)
aux_table = resolver.index("void* auxTable = nullptr;")
assert material_guard < aux_table

for signature in (
    "ShadowMaterialSignature BuildShadowMaterialSignatureForRenderable(",
    "bool InspectShadowMaterialBindingForRenderable(",
    "bool TryConvertUpperLayerResolvedItem(",
):
    body = function(signature)
    assert "MeshLayerBindingResolveExtent::MaterialSignature" in body

# Full geometry/explicit-blend consumers must retain the default Full resolver,
# since they consume the auxiliary stream fields skipped by the material-only path.
for signature in (
    "bool TryResolveExplicitBlendSkinningForRenderable(",
    "bool ShadowRendererCore::resolveRecord(",
):
    body = function(signature)
    assert "TryResolveMeshLayerBindingContract(" in body
    assert "MeshLayerBindingResolveExtent::MaterialSignature" not in body

signature_builder = function("ShadowMaterialSignature BuildShadowMaterialSignature(")
for forbidden in (
    "auxStreamPtr0",
    "auxStreamPtr1",
    "auxEntry0Word0",
    "auxEntry1Word0",
    "auxEntry0Word8",
    "auxEntry1Word8",
):
    assert forbidden not in signature_builder

print("shadow material signature resolve extent static checks passed")
