#include "war3_native_shadow_hint.h"

#include "../game/war3_agent.h"
#include "../game/war3_unit.h"
#include "../handle/war3_handle_resolver.h"
#include "../hooks/war3_shadow_filter_policy.h"

#include <algorithm>

namespace dxvk::war3::native {

namespace {

constexpr uint64_t kHintMaxAgeFrames = 240u;
constexpr uint64_t kHintPrunePeriodFrames = 60u;

float EstimateRadiusHint(render::ObjectKind kind) {
  switch (kind) {
  case render::ObjectKind::Unit:
    return 260.0f;
  case render::ObjectKind::Building:
    return 900.0f;
  case render::ObjectKind::Destructible:
    return 750.0f;
  case render::ObjectKind::Item:
    return 220.0f;
  case render::ObjectKind::Effect:
    return 900.0f;
  default:
    return 0.0f;
  }
}

War3NativeShadowHint ResolveShadowHint(void* objectPtr, uint32_t flags,
                                       uint64_t frameNo) {
  War3NativeShadowHint hint = {};
  hint.objectPtr = objectPtr;
  hint.flags = flags;
  hint.lastSeenFrame = frameNo;

  if (objectPtr == nullptr)
    return hint;

  void* unitPtr = nullptr;

  game::UnitWrapper directUnit(objectPtr);
  if (directUnit.IsValid()) {
    unitPtr = objectPtr;
    hint.jHandle = directUnit.GetJassHandle();
    hint.rawcode = directUnit.GetRawcode();
    hint.objectKind = directUnit.GetKind();
  }

  if (hint.objectKind == render::ObjectKind::Unknown || hint.rawcode == 0u ||
      hint.jHandle == 0u || unitPtr == nullptr) {
    game::AgentWrapper agent(objectPtr);
    if (agent.IsValid()) {
      if (unitPtr == nullptr)
        unitPtr = agent.GetUnitPtr();

      if (hint.objectKind == render::ObjectKind::Unknown) {
        if (agent.IsUnit())
          hint.objectKind = render::ObjectKind::Unit;
        else if (agent.IsDestructible())
          hint.objectKind = render::ObjectKind::Destructible;
        else if (agent.IsItem())
          hint.objectKind = render::ObjectKind::Item;
      }

      if (unitPtr != nullptr) {
        game::UnitWrapper unit(unitPtr);
        if (unit.IsValid()) {
          if (hint.jHandle == 0u)
            hint.jHandle = unit.GetJassHandle();
          if (hint.rawcode == 0u)
            hint.rawcode = unit.GetRawcode();
          if (hint.objectKind == render::ObjectKind::Unknown)
            hint.objectKind = unit.GetKind();
        }
      }
    }
  }

  if (hint.rawcode == 0u) {
    uint32_t fourcc = 0u;
    if (hooks::shadowfilter::TryExtractShadowObjectFourCC(objectPtr, fourcc))
      hint.rawcode = fourcc;
  }

  if (hint.jHandle == 0u && unitPtr != nullptr) {
    uint32_t handleId = 0u;
    if (HandleResolver::instance().findHandleByUnitPtr(unitPtr, &handleId,
                                                       nullptr)) {
      hint.jHandle = 0x100000u | handleId;
    }
  }

  hint.unitPtr = unitPtr;
  hint.radiusHint = EstimateRadiusHint(hint.objectKind);
  if (hint.radiusHint > 0.0f)
    hint.flags |= War3NativeShadowHint_HasRadius;

  return hint;
}

} // namespace

War3NativeShadowHintRegistry& War3NativeShadowHintRegistry::instance() {
  static War3NativeShadowHintRegistry s_registry;
  return s_registry;
}

void War3NativeShadowHintRegistry::beginFrame(uint64_t frameNo) {
  std::lock_guard lock(m_mutex);
  if (frameNo > m_currentFrame)
    m_currentFrame = frameNo;
  if (m_currentFrame >= m_lastPruneFrame + kHintPrunePeriodFrames)
    pruneLocked(m_currentFrame);
}

void War3NativeShadowHintRegistry::recordFromObject(void* objectPtr,
                                                    bool fromRuntimePath,
                                                    bool fromAltPath,
                                                    bool blocked) {
  uint32_t flags = 0u;
  if (fromRuntimePath)
    flags |= War3NativeShadowHint_FromRuntimePath;
  if (fromAltPath)
    flags |= War3NativeShadowHint_FromAltPath;
  if (blocked)
    flags |= War3NativeShadowHint_Blocked;

  uint64_t frameNo = 0;
  {
    std::lock_guard lock(m_mutex);
    frameNo = m_currentFrame;
  }

  const auto hint = ResolveShadowHint(objectPtr, flags, frameNo);
  if (!hint.hasSemanticIdentity())
    return;

  std::lock_guard lock(m_mutex);
  storeHintLocked(hint);
}

void War3NativeShadowHintRegistry::recordSimple(void* objectPtr,
                                                bool fromSimplePath,
                                                bool blocked) {
  uint32_t flags = 0u;
  if (fromSimplePath)
    flags |= War3NativeShadowHint_FromSimplePath;
  if (blocked)
    flags |= War3NativeShadowHint_Blocked;

  uint64_t frameNo = 0;
  {
    std::lock_guard lock(m_mutex);
    frameNo = m_currentFrame;
  }

  const auto hint = ResolveShadowHint(objectPtr, flags, frameNo);
  if (!hint.hasSemanticIdentity())
    return;

  std::lock_guard lock(m_mutex);
  storeHintLocked(hint);
}

bool War3NativeShadowHintRegistry::findByObjectPtr(
    void* objectPtr, War3NativeShadowHint& out) const {
  if (objectPtr == nullptr)
    return false;

  std::lock_guard lock(m_mutex);
  const auto it = m_byObjectPtr.find(objectPtr);
  if (it == m_byObjectPtr.end())
    return false;
  out = it->second;
  return true;
}

bool War3NativeShadowHintRegistry::findByHandle(
    uint32_t jHandle, War3NativeShadowHint& out) const {
  if (jHandle == 0u)
    return false;

  std::lock_guard lock(m_mutex);
  const auto it = m_byHandle.find(jHandle);
  if (it == m_byHandle.end())
    return false;
  out = it->second;
  return true;
}

void War3NativeShadowHintRegistry::storeHintLocked(
    const War3NativeShadowHint& hint) {
  auto mergeInto = [&](War3NativeShadowHint& dst) {
    dst.flags |= hint.flags;
    dst.lastSeenFrame = (std::max)(dst.lastSeenFrame, hint.lastSeenFrame);
    if (dst.objectPtr == nullptr)
      dst.objectPtr = hint.objectPtr;
    if (dst.unitPtr == nullptr)
      dst.unitPtr = hint.unitPtr;
    if (dst.jHandle == 0u)
      dst.jHandle = hint.jHandle;
    if (dst.rawcode == 0u)
      dst.rawcode = hint.rawcode;
    if (dst.objectKind == render::ObjectKind::Unknown)
      dst.objectKind = hint.objectKind;
    if (hint.radiusHint > dst.radiusHint) {
      dst.radiusHint = hint.radiusHint;
      dst.flags |= War3NativeShadowHint_HasRadius;
    }
    if (hint.heightHint > dst.heightHint)
      dst.heightHint = hint.heightHint;
  };

  if (hint.objectPtr != nullptr) {
    auto& slot = m_byObjectPtr[hint.objectPtr];
    mergeInto(slot);
  }
  if (hint.unitPtr != nullptr) {
    auto& slot = m_byObjectPtr[hint.unitPtr];
    mergeInto(slot);
  }
  if (hint.jHandle != 0u) {
    auto& slot = m_byHandle[hint.jHandle];
    mergeInto(slot);
  }
}

void War3NativeShadowHintRegistry::pruneLocked(uint64_t frameNo) {
  m_lastPruneFrame = frameNo;

  const auto isExpired = [frameNo](const War3NativeShadowHint& hint) {
    return frameNo > hint.lastSeenFrame &&
           (frameNo - hint.lastSeenFrame) > kHintMaxAgeFrames;
  };

  for (auto it = m_byObjectPtr.begin(); it != m_byObjectPtr.end();) {
    if (isExpired(it->second))
      it = m_byObjectPtr.erase(it);
    else
      ++it;
  }

  for (auto it = m_byHandle.begin(); it != m_byHandle.end();) {
    if (isExpired(it->second))
      it = m_byHandle.erase(it);
    else
      ++it;
  }
}

} // namespace dxvk::war3::native
