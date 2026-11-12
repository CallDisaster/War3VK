#pragma once

#include <cstdint>

namespace dxvk::war3::render {

// Where the device currently is relative to the main 3D world.
//
// WarVK rewrites three fixed-function inputs for the day-night sun and the
// ambient fill: light 0 (direction/colour/ambient), D3DRS_AMBIENT and the
// material's ambient colour. Those rewrites are meant for the main world
// only. Applied to UI-owned 3D models (the unit portrait, the cursor, menu
// scenes) they made the portrait follow the world's time of day: too dark at
// night, washed out at noon.
//
// The device keeps the game's own ("pure") values and switches the effective
// state when this scope changes, so the result does not depend on whether the
// game re-sends a state it believes is unchanged.
enum class War3WorldLightingScope : uint8_t {
  // The world RenderScene boundary hook has not fired on this device yet
  // (hook unavailable, or still in menus). Keep the pre-scope behaviour:
  // always rewrite. A build without the hook therefore loses nothing.
  Unscoped = 0,
  // Inside the main world RenderScene: rewrite.
  World = 1,
  // After the world RenderScene returned: the game's own values.
  Outside = 2,
};

constexpr bool War3WorldLightingActive(War3WorldLightingScope scope) noexcept {
  return scope != War3WorldLightingScope::Outside;
}

constexpr War3WorldLightingScope War3WorldLightingScopeFor(bool inWorld) noexcept {
  return inWorld ? War3WorldLightingScope::World
                 : War3WorldLightingScope::Outside;
}

// True when a scope change flips whether the rewrites apply, i.e. when the
// effective light/material/ambient must be recomputed from the pure values.
constexpr bool War3WorldLightingScopeNeedsReapply(
    War3WorldLightingScope from, War3WorldLightingScope to) noexcept {
  return War3WorldLightingActive(from) != War3WorldLightingActive(to);
}

} // namespace dxvk::war3::render
