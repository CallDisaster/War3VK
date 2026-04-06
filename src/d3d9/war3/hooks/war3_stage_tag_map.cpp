#include "war3_stage_tag_map.h"

namespace dxvk::war3::hooks {

static War3BatchTag MapStageToTagProfileMeasured(int stage) {
  switch (stage) {
  case 0:
    return War3BatchTag::Skybox;
  case 1:
    return War3BatchTag::Terrain;
  case 2:
    return War3BatchTag::Unknown;
  case 6:
    return War3BatchTag::Weather;
  case 7:
    return War3BatchTag::SelectionOverlay;
  case 9:
    return War3BatchTag::GroundEffect;
  case 10:
    return War3BatchTag::Decorations;
  case 11:
    return War3BatchTag::WorldObjects;
  case 12:
    return War3BatchTag::RangeIndicatorTarget;
  case 14:
    return War3BatchTag::Water;
  case 18:
    return War3BatchTag::RangeIndicatorTarget;
  case 19:
    return War3BatchTag::BuildingFloorDecal;
  case 20:
    return War3BatchTag::Decorations;
  case 21:
    return War3BatchTag::RangeIndicator;
  default:
    return War3BatchTag::Unknown;
  }
}

static War3BatchTag MapStageToTagProfileLegacy(int stage) {
  switch (stage) {
  case 0:
    return War3BatchTag::Skybox;
  case 1:
    return War3BatchTag::Terrain;
  case 2:
    return War3BatchTag::Unknown;
  case 6:
    return War3BatchTag::Weather;
  case 9:
    return War3BatchTag::GroundEffect;
  case 11:
    return War3BatchTag::WorldObjects;
  case 12:
    return War3BatchTag::SelectionOverlay;
  case 13:
    return War3BatchTag::Decorations;
  case 14:
    return War3BatchTag::Water;
  case 18:
    return War3BatchTag::RangeIndicatorTarget;
  case 19:
    return War3BatchTag::BuildingFloorDecal;
  case 20:
    return War3BatchTag::Decorations;
  case 21:
    return War3BatchTag::RangeIndicator;
  default:
    return War3BatchTag::Unknown;
  }
}

War3BatchTag MapStageToTag(int stage, uint32_t profile) {
  if (profile == 1u) {
    return MapStageToTagProfileLegacy(stage);
  }
  return MapStageToTagProfileMeasured(stage);
}

bool ShouldSuppressStageTagByGroupMode(int stage, uint32_t profile,
                                       bool tagWorldByGroupIdx) {
  return tagWorldByGroupIdx && profile == 1u &&
         (stage == 11 || stage == 12 || stage == 13);
}

bool IsWorldBridgeTag(War3BatchTag tag) {
  return tag == War3BatchTag::WorldObjects ||
         tag == War3BatchTag::SelectionOverlay ||
         tag == War3BatchTag::Decorations;
}

} // namespace dxvk::war3::hooks
