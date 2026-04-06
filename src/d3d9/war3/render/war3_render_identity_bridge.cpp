#include "war3_render_identity_bridge.h"

#include "../core/war3_memory.h"
#include "war3_render_objects.h"
#include "war3_renderer.h"

namespace dxvk::war3::render {

RenderObjectIdentitySnapshot::RenderObjectIdentitySnapshot()
    : kind(ObjectKind::Unknown) {
}

bool RenderObjectIdentitySnapshot::HasContext() const {
  return worldObjectEntry != nullptr || sceneNode != nullptr ||
         unitPtr != nullptr || jHandle != 0u;
}

bool RenderObjectIdentitySnapshot::HasStableIdentity() const {
  return HasContext() || rawcode != 0u || kind != ObjectKind::Unknown;
}

RenderObjectIdentitySnapshot
MakeRenderObjectIdentitySnapshot(const RenderObjectInfo &info) {
  RenderObjectIdentitySnapshot snapshot;
  snapshot.worldObjectEntry = info.worldObjectEntry;
  snapshot.sceneNode = info.sceneNode;
  snapshot.unitPtr = info.unitPtr;
  snapshot.agentPtr = info.agentPtr;
  snapshot.handleId = info.handleId;
  snapshot.jHandle = info.jHandle;
  snapshot.rawcode = info.rawcode;
  snapshot.agentType = info.agentType;
  snapshot.flags5C = info.flags5C;
  snapshot.kind = info.kind;
  snapshot.groupIdx = static_cast<int8_t>(info.groupIdx);
  return snapshot;
}

static void TryFillSceneNodeFromEntry(void *worldObjectEntry,
                                      void *&sceneNodeHint) {
  if (sceneNodeHint != nullptr || worldObjectEntry == nullptr)
    return;

  void *sceneNode = nullptr;
  if (dxvk::war3::SafeReadPtrFast(worldObjectEntry, 0x20, sceneNode) &&
      sceneNode != nullptr) {
    sceneNodeHint = sceneNode;
  }
}

bool TryResolveRenderObjectIdentity(void *worldObjectEntry, void *sceneNodeHint,
                                    RenderObjectIdentitySnapshot &out) {
  out = {};
  out.worldObjectEntry = worldObjectEntry;
  out.sceneNode = sceneNodeHint;

  TryFillSceneNodeFromEntry(worldObjectEntry, out.sceneNode);

  const auto &registry = RenderObjectRegistry::instance();
  const RenderObjectInfo *info = nullptr;

  if (worldObjectEntry != nullptr)
    info = registry.findByEntry(worldObjectEntry);

  if (info == nullptr && out.sceneNode != nullptr)
    info = registry.findBySceneNode(out.sceneNode);

  if (info != nullptr) {
    out = MakeRenderObjectIdentitySnapshot(*info);
    if (worldObjectEntry != nullptr)
      out.worldObjectEntry = worldObjectEntry;
    if (sceneNodeHint != nullptr)
      out.sceneNode = sceneNodeHint;
    return true;
  }

  return out.HasStableIdentity();
}

bool TryResolveCurrentRenderObjectIdentity(void *sceneNodeHint,
                                           RenderObjectIdentitySnapshot &out) {
  auto &renderer = War3Renderer::instance();
  void *worldObjectEntry = renderer.GetCurrentWorldObjectEntry();
  void *sceneNode = sceneNodeHint != nullptr ? sceneNodeHint
                                             : renderer.GetCurrentSceneNode();
  return TryResolveRenderObjectIdentity(worldObjectEntry, sceneNode, out);
}

} // namespace dxvk::war3::render
