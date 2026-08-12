#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
MODEL = (ROOT / "src/d3d9/war3/model/war3_model_registry.cpp").read_text(
    encoding="utf-8"
)
SHADOW = (
    ROOT / "src/d3d9/war3/render/war3_shadow_object_registry.cpp"
).read_text(encoding="utf-8")
RENDER = (ROOT / "src/d3d9/war3/render/war3_render_objects.cpp").read_text(
    encoding="utf-8"
)


def body(source: str, signature: str, next_signature: str) -> str:
    return source.split(signature, 1)[1].split(next_signature, 1)[0]


model_direct = body(
    MODEL,
    "bool ModelInstanceRegistry::findFirstForDirectPacket",
    "bool ModelInstanceRegistry::findBySourceObject",
)
assert model_direct.count("std::shared_lock<std::shared_mutex> lock(m_mutex)") == 1
model_priority = [
    "findPointer(m_bySceneNode, sceneNode)",
    "findPointer(m_byUnitPtr, unitPtr)",
    "findPointer(m_byWorldObjectEntry, worldObjectEntry)",
    "findPointer(m_byRuntimeModel, runtimeModelPtr)",
]
assert [model_direct.index(token) for token in model_priority] == sorted(
    model_direct.index(token) for token in model_priority
)

shadow_direct = body(
    SHADOW,
    "bool ShadowObjectRegistry::findFirstForDirectPacket",
    "bool ShadowObjectRegistry::findFirstForDirectPacketView",
)
assert shadow_direct.count("std::shared_lock<std::shared_mutex> lock(m_mutex)") == 1
shadow_priority = [
    "findPointer(m_byWorldObjectEntry, worldObjectEntry)",
    "findPointer(m_bySceneNode, sceneNode)",
    "m_byHandle.find(jHandle)",
    "findPointer(m_byRuntimeModel, primaryRuntimeModelPtr)",
    "findPointer(m_byRuntimeModel, secondaryRuntimeModelPtr)",
]
assert [shadow_direct.index(token) for token in shadow_priority] == sorted(
    shadow_direct.index(token) for token in shadow_priority
)
assert "secondaryRuntimeModelPtr != primaryRuntimeModelPtr" in shadow_direct

shadow_view = body(
    SHADOW,
    "bool ShadowObjectRegistry::findFirstForDirectPacketView",
    "bool ShadowObjectRegistry::findFirstForAugment",
)
assert shadow_view.count("std::shared_lock<std::shared_mutex> lock(m_mutex)") == 1
assert "ProjectShadowObjectAugmentView(*record, out);" in shadow_view
assert "ShadowObjectRecord& out" not in shadow_view

render_direct = body(
    RENDER,
    "const RenderObjectInfo* RenderObjectRegistry::findFirstForDirectPacket",
    "std::vector<RenderObjectInfo> RenderObjectRegistry::getAllObjects",
)
assert render_direct.count("snapshotForThread()") == 1
render_priority = [
    "snap.byEntry.find(worldObjectEntry)",
    "snap.sceneToInfo.find(sceneNode)",
    "snap.handleToInfo.find(jHandle)",
]
assert [render_direct.index(token) for token in render_priority] == sorted(
    render_direct.index(token) for token in render_priority
)

builder = body(
    DEVICE,
    "bool War3TryBuildShadowPacketFromCurrentDrawRecord",
    "dxvk::war3::render::ObjectKind War3ResolveSemanticPacketObjectKindFast",
)
assert builder.count("instanceRegistry.findFirstForDirectPacket(") == 1
assert builder.count("renderRegistry.findFirstForDirectPacket(") == 1
assert builder.count("shadowRegistry.findFirstForDirectPacketView(") == 1
assert "ShadowObjectAugmentView shadowRecord" in builder

instance_lookup = builder.split("Direct/InstanceLookup", 1)[1].split(
    "War3PacketBuildPhase::GeosetFallbacks", 1
)[0]
for legacy in (
    "instanceRegistry.findBySceneNode",
    "instanceRegistry.findByUnitPtr",
    "instanceRegistry.findByWorldObjectEntry",
):
    assert legacy not in instance_lookup

shadow_lookup = builder.split("Direct/ShadowObjectLookup", 1)[1].split(
    "War3PacketBuildNestedPhase::VisibleLookup", 1
)[0]
for legacy in (
    "shadowRegistry.findByWorldObjectEntry",
    "shadowRegistry.findBySceneNode",
    "shadowRegistry.findByHandle",
    "shadowRegistry.findByRuntimeModel",
):
    assert legacy not in shadow_lookup

print("direct registry lookup batch static checks passed")
