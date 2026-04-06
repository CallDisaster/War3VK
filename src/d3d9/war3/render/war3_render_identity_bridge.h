#pragma once

#include <cstdint>

namespace dxvk::war3::render {

enum class ObjectKind : uint8_t;
struct RenderObjectInfo;

struct RenderObjectIdentitySnapshot {
  void *worldObjectEntry = nullptr;
  void *sceneNode = nullptr;
  void *unitPtr = nullptr;
  void *agentPtr = nullptr;

  uint32_t handleId = 0;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  uint32_t agentType = 0;
  uint32_t flags5C = 0;

  ObjectKind kind;
  int8_t groupIdx = -1;
  uint8_t reserved[3] = {};

  RenderObjectIdentitySnapshot();

  bool HasContext() const;
  bool HasStableIdentity() const;
};

RenderObjectIdentitySnapshot
MakeRenderObjectIdentitySnapshot(const RenderObjectInfo &info);

bool TryResolveRenderObjectIdentity(void *worldObjectEntry, void *sceneNodeHint,
                                    RenderObjectIdentitySnapshot &out);

bool TryResolveCurrentRenderObjectIdentity(void *sceneNodeHint,
                                           RenderObjectIdentitySnapshot &out);

} // namespace dxvk::war3::render
