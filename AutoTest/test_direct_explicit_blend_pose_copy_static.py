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

# The temporary must not deep-copy the fallback pose palette before replacing
# it with the authoritative current-draw palette.
assert "ShadowPoseRecord explicitPose = pose" not in body
assert "ShadowPoseRecord explicitPose = {};" in body
assert "explicitPose.matrixPalette = out.runtimeGroupPalette;" in body

# Keep every scalar field carried by ShadowPoseRecord and preserve the existing
# authoritative matrix identity and frame metadata overrides.
for field in (
    "runtimeModelPtr",
    "sceneNode",
    "unitPtr",
    "hasWorldTransform",
    "worldTransform",
    "frameSerial",
):
    assert f"explicitPose.{field} = pose.{field};" in body

assert "explicitPose.matrixCount = uint32_t(out.runtimeGroupPalette.size());" in body
assert "explicitPose.matrixHash = out.runtimeGroupPaletteHash;" in body
assert "TryResolveExplicitBlendSkinningForRenderable(" in body

print("direct explicit-blend pose copy static checks passed")
