#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
REGISTRY_H = ROOT / "src/d3d9/war3/model/war3_model_registry.h"
REGISTRY_CPP = ROOT / "src/d3d9/war3/model/war3_model_registry.cpp"

source = DEVICE.read_text(encoding="utf-8")
builder = source.split(
    "bool War3TryBuildShadowPacketFromCurrentDrawRecord", 1
)[1].split(
    "dxvk::war3::render::ObjectKind War3ResolveSemanticPacketObjectKindFast", 1
)[0]

palette_install = builder.index("out.hasRuntimeGroupPalette = true;")
fallback_gate = builder.index("const bool needPoseMatrixPayload =", palette_install)
pose_lookup = builder.index('War3SemanticScene/Direct/PoseLookup', fallback_gate)
pose_install = builder.index("War3PacketBuildPhase::PoseInstall", pose_lookup)
assert palette_install < fallback_gate < pose_lookup < pose_install
assert (
    "!out.hasRuntimeGroupPalette && !authoritativeSkinnedRequired" in
    builder[fallback_gate:pose_lookup]
)

lookup_body = builder[pose_lookup:pose_install]
assert "if (needPoseMatrixPayload)" in lookup_body
assert lookup_body.count("findFirstForDirectPacket(") == 1
assert lookup_body.count("findFirstForDirectPacketAugment(") == 1
for legacy in (
    "findByRuntimeModel(",
    "findByRuntimeModelAugment(",
    "findBySceneNode(",
    "findBySceneNodeAugment(",
    "findByUnitPtr(",
    "findByUnitPtrAugment(",
):
    assert legacy not in lookup_body

# The common authoritative path must not copy the matrix payload into pose.
install_body = builder[pose_install:builder.index("ExplicitBlendResolve", pose_install)]
assert "if (poseHit && needPoseMatrixPayload)" in install_body
assert "pose.matrixPalette = std::move(poseRecord.matrixPalette);" in install_body
augment_branch = install_body.split("} else if (poseHit) {", 1)[1]
assert "poseAugment.matrixCount" in augment_branch
assert "poseAugment.matrixHash" in augment_branch
assert "matrixPalette" not in augment_branch

header = REGISTRY_H.read_text(encoding="utf-8")
augment = header.split("struct PoseAugmentView", 1)[1].split("};", 1)[0]
for field in ("runtimeModelPtr", "sceneNode", "unitPtr"):
    assert field in augment

registry = REGISTRY_CPP.read_text(encoding="utf-8")
projection = registry.split("static inline void ProjectPoseAugment", 1)[1].split(
    "bool PoseRegistry::findByRuntimeModelAugment", 1
)[0]
for field in ("runtimeModelPtr", "sceneNode", "unitPtr"):
    assert f"out.{field} = rec.{field};" in projection
assert "matrixPalette" not in projection

full_lookup = registry.split(
    "bool PoseRegistry::findFirstForDirectPacket", 1
)[1].split("// Project the fields", 1)[0]
assert full_lookup.count("std::shared_lock<std::shared_mutex> lock(m_mutex)") == 1
augment_lookup = registry.split(
    "bool PoseRegistry::findFirstForDirectPacketAugment", 1
)[1].split("std::vector<PoseRecord> PoseRegistry::snapshot", 1)[0]
assert augment_lookup.count(
    "std::shared_lock<std::shared_mutex> lock(m_mutex)"
) == 1
for lookup in (full_lookup, augment_lookup):
    priority = [
        "m_byRuntimeModel, runtimeModelPtr",
        "m_bySceneNode, sceneNode",
        "m_byUnitPtr, unitPtr",
    ]
    assert [lookup.index(token) for token in priority] == sorted(
        lookup.index(token) for token in priority
    )

print("direct pose payload on-demand static checks passed")
