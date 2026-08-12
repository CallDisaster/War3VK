#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"

source = DEVICE.read_text(encoding="utf-8")
body = source.split(
    "War3SemanticScene/Direct/ExplicitBlendResolve", 1
)[1].split(
    "War3SemanticScene/Direct/MaterialSignature", 1
)[0]

# The synchronous resolver only consumes matrices. It must borrow the packet's
# current authoritative palette rather than deep-copying it into a temporary
# ShadowPoseRecord.
assert "ShadowPoseRecord explicitPose" not in body
assert "explicitPose.matrixPalette" not in body
assert "ShadowMatrixPaletteView explicitPalette" in body
assert "out.runtimeGroupPalette.data()" in body
assert "uint32_t(out.runtimeGroupPalette.size())" in body
assert "TryResolveExplicitBlendSkinningForRenderable(" in body
assert "maxExpectedGroupSize, explicitPalette, explicitBlend, nullptr" in body

core_header = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.h"
).read_text(encoding="utf-8")
core = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp"
).read_text(encoding="utf-8")
assert "struct ShadowMatrixPaletteView" in core_header
assert "const Matrix4* data = nullptr;" in core_header
assert "uint32_t size = 0u;" in core_header
assert "ShadowMatrixPaletteView posePalette" in core_header
resolver = core.split("bool TryResolveMeshDynamicExplicitBlendSkinning(", 1)[1].split(
    "bool TryBuildDirectPosePaletteForDynamicGroups(", 1
)[0]
assert "const ShadowPoseRecord& pose" not in resolver
assert "pose.matrixPalette" not in resolver
assert "maxGroupSlot >= posePalette.size" in resolver
assert "posePalette.data + size_t(maxGroupSlot + 1u)" in resolver
assert "maxExpectedGroupSize, explicitPosePalette, &layerContract" in core

print("direct explicit-blend palette view static checks passed")
