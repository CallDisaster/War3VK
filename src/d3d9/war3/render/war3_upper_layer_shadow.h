#pragma once

#include "../../d3d9_war3_scene.h"
#include "../model/war3_model_resource_cache.h"
#include "../model/war3_model_registry.h"
#include "war3_visible_renderables.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace dxvk::war3::render {

struct UpperLayerShadowResolvedItem {
  VisibleRenderableRecord visible = {};
  model::ShadowGeosetResourceRecord geoset = {};
  model::PoseRecord pose = {};
  bool hasPosePalette = false;
  bool hasRuntimeGroupPalette = false;
  bool skinned = false;
  bool matrixGroupsUseAveraging = false;
  uint32_t maxVertexGroupSlot = 0;
  std::vector<Matrix4> runtimeGroupPalette;

  bool HasAuthoritativeRigidPath() const {
    return visible.HasResolvedGeoset() && geoset.readyForShadowConsumer() &&
           !skinned;
  }

  bool HasAuthoritativeSkinnedPath() const {
    return visible.HasResolvedGeoset() && geoset.readyForShadowConsumer() &&
           skinned && hasPosePalette && hasRuntimeGroupPalette;
  }
};

struct UpperLayerShadowResolveStats {
  uint64_t resolveAttempts = 0;
  uint64_t resolveVisibleMiss = 0;
  uint64_t resolveVisibleUnresolvedGeoset = 0;
  uint64_t resolveGeosetMiss = 0;
  uint64_t resolvePoseMiss = 0;
  uint64_t resolveRuntimeGroupPaletteMiss = 0;
  uint64_t resolveAuthoritativeRigid = 0;
  uint64_t resolveAuthoritativeSkinned = 0;
  uint64_t resolvedAuthoritativeItems = 0;
  uint64_t emitted = 0;
  uint64_t duplicateOrSuppressed = 0;
};

class UpperLayerShadowRegistry {
public:
  static UpperLayerShadowRegistry &instance();

  void beginFrame();
  void endFrame();

  bool resolve(const dxvk::War3ShadowSemanticContext &semantic,
               UpperLayerShadowResolvedItem &out) const;
  bool tryMarkEmitted(const UpperLayerShadowResolvedItem &item);
  UpperLayerShadowResolveStats snapshotStats() const;
  std::vector<UpperLayerShadowResolvedItem> snapshotResolvedItems() const;

private:
  UpperLayerShadowRegistry() = default;

  struct EmissionKey {
    void *primaryPtr = nullptr;
    void *runtimeModelPtr = nullptr;
    uint32_t geosetIndex = kInvalidVisibleMeshIndex;

    bool operator==(const EmissionKey &other) const {
      return primaryPtr == other.primaryPtr &&
             runtimeModelPtr == other.runtimeModelPtr &&
             geosetIndex == other.geosetIndex;
    }
  };

  struct EmissionKeyHash {
    size_t operator()(const EmissionKey &key) const {
      const size_t h1 = std::hash<void *>()(key.primaryPtr);
      const size_t h2 = std::hash<void *>()(key.runtimeModelPtr);
      const size_t h3 = std::hash<uint32_t>()(key.geosetIndex);
      return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2)) ^
             (h3 + 0x9e3779b9u + (h2 << 6) + (h2 >> 2));
    }
  };

  static EmissionKey MakeEmissionKey(const UpperLayerShadowResolvedItem &item);
  void rememberResolvedItem(const UpperLayerShadowResolvedItem& item) const;

  std::unordered_set<EmissionKey, EmissionKeyHash> m_emitted;
  mutable std::unordered_set<EmissionKey, EmissionKeyHash> m_resolvedKeys;
  mutable std::vector<UpperLayerShadowResolvedItem> m_resolvedItems;
  mutable UpperLayerShadowResolveStats m_frameStats;
  mutable UpperLayerShadowResolveStats m_totalStats;
};

} // namespace dxvk::war3::render
