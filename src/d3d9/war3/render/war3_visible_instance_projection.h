#pragma once

#include <cstdint>

namespace dxvk::war3::render {

// Scalar identity proof used to project model ownership from the exact
// current-frame VisibleRenderable part/layer record. A shared model part is
// not instance identity: CurrentDraw must carry at least one strong alias,
// and every alias it does carry must agree with the visible instance.
struct War3VisibleInstanceProjectionFacts {
  const void* recordWorldObjectEntry = nullptr;
  const void* recordSceneNode = nullptr;
  const void* recordUnitPtr = nullptr;
  uint32_t recordJHandle = 0u;
  uint32_t recordRawcode = 0u;

  const void* visibleWorldObjectEntry = nullptr;
  const void* visibleSceneNode = nullptr;
  const void* visibleIdentitySceneNode = nullptr;
  const void* visibleUnitPtr = nullptr;
  uint32_t visibleJHandle = 0u;
  uint32_t visibleRawcode = 0u;
  const void* visibleRuntimeModelPtr = nullptr;
  const void* visibleModelResourcePtr = nullptr;
  uint64_t visibleModelKey = 0u;
};

inline bool War3CanProjectVisibleInstance(
    const War3VisibleInstanceProjectionFacts& facts) noexcept {
  if (facts.visibleRuntimeModelPtr == nullptr ||
      facts.visibleModelResourcePtr == nullptr ||
      facts.visibleModelKey == 0u)
    return false;

  // The record's scene node names an instance only when the visible record's
  // public and identity projections agree on that same node.
  if (facts.recordSceneNode != nullptr &&
      (facts.visibleSceneNode != facts.recordSceneNode ||
       facts.visibleIdentitySceneNode != facts.recordSceneNode))
    return false;
  if (facts.visibleSceneNode != nullptr &&
      facts.visibleIdentitySceneNode != nullptr &&
      facts.visibleSceneNode != facts.visibleIdentitySceneNode)
    return false;

  if (facts.recordWorldObjectEntry != nullptr &&
      facts.visibleWorldObjectEntry != facts.recordWorldObjectEntry)
    return false;
  if (facts.recordUnitPtr != nullptr &&
      facts.visibleUnitPtr != facts.recordUnitPtr)
    return false;
  if (facts.recordJHandle != 0u &&
      facts.visibleJHandle != facts.recordJHandle)
    return false;
  if (facts.recordRawcode != 0u &&
      facts.visibleRawcode != facts.recordRawcode)
    return false;

  return facts.recordWorldObjectEntry != nullptr ||
      facts.recordSceneNode != nullptr || facts.recordUnitPtr != nullptr ||
      facts.recordJHandle != 0u;
}

} // namespace dxvk::war3::render
