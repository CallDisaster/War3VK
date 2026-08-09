#pragma once

#include <cstdint>

namespace dxvk::war3::render {

/** Renderer-owned current layout for the two outline mask images. */
enum class War3OutlineMaskLayoutState : uint8_t {
  Undefined = 0u,
  ShaderReadOnly,
  ColorAttachment,
};

struct War3OutlineMaskLayoutTransition {
  bool valid = false;
  bool discardContents = false;
  War3OutlineMaskLayoutState oldState =
      War3OutlineMaskLayoutState::Undefined;
  War3OutlineMaskLayoutState newState =
      War3OutlineMaskLayoutState::Undefined;

  explicit constexpr operator bool() const noexcept {
    return valid;
  }
};

constexpr War3OutlineMaskLayoutTransition
PlanWar3OutlineMaskBegin(War3OutlineMaskLayoutState current) noexcept {
  switch (current) {
  case War3OutlineMaskLayoutState::Undefined:
    return {true, true, current,
            War3OutlineMaskLayoutState::ColorAttachment};
  case War3OutlineMaskLayoutState::ShaderReadOnly:
    return {true, false, current,
            War3OutlineMaskLayoutState::ColorAttachment};
  case War3OutlineMaskLayoutState::ColorAttachment:
    break;
  }
  return {};
}

constexpr War3OutlineMaskLayoutTransition
PlanWar3OutlineMaskEnd(War3OutlineMaskLayoutState current) noexcept {
  if (current == War3OutlineMaskLayoutState::ColorAttachment) {
    return {true, false, current,
            War3OutlineMaskLayoutState::ShaderReadOnly};
  }
  return {};
}

} // namespace dxvk::war3::render
