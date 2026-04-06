#pragma once

#include "../render/war3_render_objects.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace dxvk::war3::native {

enum War3NativeShadowHintFlags : uint32_t {
  War3NativeShadowHint_FromRuntimePath = 1u << 0,
  War3NativeShadowHint_FromAltPath = 1u << 1,
  War3NativeShadowHint_FromSimplePath = 1u << 2,
  War3NativeShadowHint_Blocked = 1u << 3,
  War3NativeShadowHint_HasRadius = 1u << 4,
};

struct War3NativeShadowHint {
  void* objectPtr = nullptr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  render::ObjectKind objectKind = render::ObjectKind::Unknown;
  uint32_t flags = 0;
  float radiusHint = 0.0f;
  float heightHint = 0.0f;
  uint64_t lastSeenFrame = 0;

  bool hasSemanticIdentity() const {
    return objectPtr != nullptr || unitPtr != nullptr || jHandle != 0 ||
           rawcode != 0 || objectKind != render::ObjectKind::Unknown;
  }
};

class War3NativeShadowHintRegistry {
public:
  static War3NativeShadowHintRegistry& instance();

  void beginFrame(uint64_t frameNo);
  void recordFromObject(void* objectPtr, bool fromRuntimePath, bool fromAltPath,
                        bool blocked);
  void recordSimple(void* objectPtr, bool fromSimplePath, bool blocked);

  bool findByObjectPtr(void* objectPtr, War3NativeShadowHint& out) const;
  bool findByHandle(uint32_t jHandle, War3NativeShadowHint& out) const;

private:
  War3NativeShadowHintRegistry() = default;

  void storeHintLocked(const War3NativeShadowHint& hint);
  void pruneLocked(uint64_t frameNo);

  mutable std::mutex m_mutex;
  std::unordered_map<void*, War3NativeShadowHint> m_byObjectPtr;
  std::unordered_map<uint32_t, War3NativeShadowHint> m_byHandle;
  uint64_t m_currentFrame = 0;
  uint64_t m_lastPruneFrame = 0;
};

} // namespace dxvk::war3::native
