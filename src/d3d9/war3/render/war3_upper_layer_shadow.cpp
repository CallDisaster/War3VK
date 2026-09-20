#include "war3_upper_layer_shadow.h"

// 2026-09-17 S2（收窄版）：与 shadow-core 共享的纯计算内核（只共享计算，不合并来源）。
#include "war3_runtime_group_palette_kernel.h"

#include "../../d3d9_war3_debug.h"
#include "../core/war3_internal_test_config.h"

#include <algorithm>
#include <array>
#include <atomic>

namespace dxvk::war3::render {

namespace {

bool TryResolveVisibleRecord(const dxvk::War3ShadowSemanticContext &semantic,
                             VisibleRenderableRecord &out) {
  out = {};
  auto &registry = VisibleRenderableRegistry::instance();
  if (semantic.renderablePart != nullptr &&
      registry.queryByPayload(semantic.renderablePart, out)) {
    return true;
  }
  if (semantic.renderablePart != nullptr &&
      registry.queryByRenderablePart(semantic.renderablePart, out)) {
    return true;
  }
  if (semantic.worldObjectEntry != nullptr &&
      registry.queryByWorldObjectEntry(semantic.worldObjectEntry, out)) {
    return true;
  }
  if (semantic.jHandle != 0u && registry.queryByHandle(semantic.jHandle, out))
    return true;
  if (semantic.sceneNode != nullptr && registry.queryBySceneNode(semantic.sceneNode, out))
    return true;
  if (semantic.runtimeModelPtr != nullptr &&
      registry.queryByRuntimeModel(semantic.runtimeModelPtr, out)) {
    return true;
  }
  return false;
}

bool TryResolveGeosetRecord(const VisibleRenderableRecord &visible,
                            const dxvk::War3ShadowSemanticContext& semantic,
                            model::ShadowGeosetResourceRecord &out) {
  out = {};
  auto &cache = model::ShadowModelResourceCache::instance();
  if (visible.runtimeGeosetPtr != nullptr &&
      cache.findGeosetByPtr(visible.runtimeGeosetPtr, out)) {
    return true;
  }
  if (visible.runtimeGeosetDataPtr != nullptr &&
      cache.findGeosetByData(visible.runtimeGeosetDataPtr, out)) {
    return true;
  }

  const uint32_t geosetIndex =
      visible.geosetIndex != kInvalidVisibleMeshIndex
          ? visible.geosetIndex
          : visible.meshIndex;
  const void* runtimeModelPtr =
      visible.runtimeModelPtr != nullptr ? visible.runtimeModelPtr
                                         : semantic.runtimeModelPtr;
  if (runtimeModelPtr != nullptr && geosetIndex != kInvalidVisibleMeshIndex &&
      cache.findRuntimeModelGeoset(const_cast<void*>(runtimeModelPtr),
                                   geosetIndex, out)) {
    return true;
  }

  const void* modelResourcePtr =
      visible.modelResourcePtr != nullptr ? visible.modelResourcePtr
                                          : semantic.modelResourcePtr;
  if (modelResourcePtr != nullptr && geosetIndex != kInvalidVisibleMeshIndex &&
      cache.findModelGeoset(const_cast<void*>(modelResourcePtr), geosetIndex,
                            out)) {
    return true;
  }
  return false;
}

bool TryResolvePoseRecord(const VisibleRenderableRecord &visible,
                          const dxvk::War3ShadowSemanticContext &semantic,
                          model::PoseRecord &out) {
  out = {};
  auto &poseRegistry = model::PoseRegistry::instance();
  if (visible.runtimeModelPtr != nullptr &&
      poseRegistry.findByRuntimeModel(visible.runtimeModelPtr, out)) {
    return true;
  }
  if (semantic.runtimeModelPtr != nullptr &&
      semantic.runtimeModelPtr != visible.runtimeModelPtr &&
      poseRegistry.findByRuntimeModel(semantic.runtimeModelPtr, out)) {
    return true;
  }
  if (visible.sceneNode != nullptr && poseRegistry.findBySceneNode(visible.sceneNode, out))
    return true;
  return false;
}

bool TryBuildRuntimeGroupPalette(const model::ShadowGeosetResourceRecord &geoset,
                                 const model::PoseRecord &pose,
                                 std::vector<Matrix4> &outPalette,
                                 uint32_t &outMaxVertexGroupSlot,
                                 bool &outUsesAveraging) {
  outPalette.clear();
  outMaxVertexGroupSlot = 0u;
  outUsesAveraging = false;

  if (!geoset.hasSkinningData())
    return false;
  if (pose.matrixPalette.empty() || pose.matrixCount == 0)
    return false;
  const uint32_t vertexGroupCount = std::min<uint32_t>(
      geoset.vertexGroupCount, uint32_t(geoset.vertexGroupIndices.size()));
  if (vertexGroupCount == 0u)
    return false;

  // 2026-09-20 S2 成本收口：计算仍由共享纯计算内核唯一完成。本适配层只保留
  // D4/D5 计数派生与记录视图填充；不合并来源链，也不引入 detail / 日志面。
  //
  // 成本口径：内核内部单趟有界扫描（maxSlot + unique）直接写入调用方的
  // outPalette，复用其容量，不再经过局部 RuntimeGroupPaletteOutput + move。
  RuntimeGroupPaletteInput kernelInput = {};
  kernelInput.vertexGroupIndices   = geoset.vertexGroupIndices.data();
  kernelInput.vertexGroupSlotCount = size_t(vertexGroupCount);
  kernelInput.matrixGroupSizes     = geoset.matrixGroupSizes.data();
  kernelInput.groupCount           = std::min<uint32_t>(
      geoset.matrixGroupCount, uint32_t(geoset.matrixGroupSizes.size()));
  kernelInput.matrixIndices        = geoset.matrixIndices.data();
  kernelInput.matrixIndexCount     = geoset.matrixIndices.size();
  kernelInput.posePalette          = pose.matrixPalette.data();
  kernelInput.posePaletteSize      = pose.matrixPalette.size();
  kernelInput.poseMatrixCount      = pose.matrixCount;

  return TryBuildRuntimeGroupPaletteKernel(
      kernelInput, RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap,
      outPalette, outMaxVertexGroupSlot, outUsesAveraging, nullptr);
}

} // namespace

UpperLayerShadowRegistry &UpperLayerShadowRegistry::instance() {
  static UpperLayerShadowRegistry *s_instance = new UpperLayerShadowRegistry();
  return *s_instance;
}

void UpperLayerShadowRegistry::beginFrame() {
  m_emitted.clear();
  m_resolvedKeys.clear();
  m_resolvedItems.clear();
  m_frameStats = {};
}

void UpperLayerShadowRegistry::endFrame() {
}

bool UpperLayerShadowRegistry::resolve(
    const dxvk::War3ShadowSemanticContext &semantic,
    UpperLayerShadowResolvedItem &out) const {
  out = {};
  m_frameStats.resolveAttempts++;
  m_totalStats.resolveAttempts++;
  if (!dxvk::war3::internal::kUpperLayerShadowConsumerEnabled)
    return false;

  if (!TryResolveVisibleRecord(semantic, out.visible)) {
    m_frameStats.resolveVisibleMiss++;
    m_totalStats.resolveVisibleMiss++;
    static std::atomic<uint32_t> s_visibleMissLogCount{0};
    const uint32_t visibleMissLogCount =
        s_visibleMissLogCount.fetch_add(1, std::memory_order_relaxed);
    if (visibleMissLogCount < 20u || (visibleMissLogCount % 4096u) == 0u) {
      VisibleRenderableRecord probe = {};
      auto &registry = VisibleRenderableRegistry::instance();
      const bool byPayload =
          semantic.renderablePart != nullptr &&
          registry.queryByPayload(semantic.renderablePart, probe);
      const bool byRenderablePart =
          semantic.renderablePart != nullptr &&
          registry.queryByRenderablePart(semantic.renderablePart, probe);
      const bool byWorldObject =
          semantic.worldObjectEntry != nullptr &&
          registry.queryByWorldObjectEntry(semantic.worldObjectEntry, probe);
      const bool byHandle =
          semantic.jHandle != 0u && registry.queryByHandle(semantic.jHandle, probe);
      const bool byScene =
          semantic.sceneNode != nullptr &&
          registry.queryBySceneNode(semantic.sceneNode, probe);
      const bool byRuntime =
          semantic.runtimeModelPtr != nullptr &&
          registry.queryByRuntimeModel(semantic.runtimeModelPtr, probe);
      dxvk::war3dbg::Print(
          "DXVK UpperShadow: visible miss part=%p scene=%p entry=%p "
          "runtime=%p handle=0x%08X raw=0x%08X kind=%u tag=%d stage=%d "
          "keys[payload=%d part=%d entry=%d handle=%d scene=%d runtime=%d] "
          "manifest[total=%zu main=%zu transparent=%zu]\n",
          semantic.renderablePart, semantic.sceneNode, semantic.worldObjectEntry,
          semantic.runtimeModelPtr, semantic.jHandle, semantic.rawcode,
          uint32_t(semantic.objectKind), int(semantic.tag), semantic.stage,
          byPayload ? 1 : 0, byRenderablePart ? 1 : 0, byWorldObject ? 1 : 0,
          byHandle ? 1 : 0, byScene ? 1 : 0, byRuntime ? 1 : 0,
          registry.getVisibleCount(), registry.getMainQueueCount(),
          registry.getTransparentCount());
    }
    return false;
  }
  if (out.visible.runtimeModelPtr == nullptr)
    out.visible.runtimeModelPtr = semantic.runtimeModelPtr;
  if (out.visible.modelResourcePtr == nullptr)
    out.visible.modelResourcePtr = semantic.modelResourcePtr;
  if (out.visible.sceneNode == nullptr)
    out.visible.sceneNode = semantic.sceneNode;
  if (out.visible.identity.worldObjectEntry == nullptr)
    out.visible.identity.worldObjectEntry = semantic.worldObjectEntry;
  if (out.visible.identity.jHandle == 0u)
    out.visible.identity.jHandle = semantic.jHandle;
  if (out.visible.identity.rawcode == 0u)
    out.visible.identity.rawcode = semantic.rawcode;
  out.visible.pathBlocker =
      out.visible.pathBlocker || semantic.pathBlocker ||
      dxvk::war3::internal::IsPathBlockerFourCc(out.visible.identity.rawcode);
  if (out.visible.modelKey == 0u)
    out.visible.modelKey = semantic.modelKey;
  if (!out.visible.HasResolvedGeoset()) {
    m_frameStats.resolveVisibleUnresolvedGeoset++;
    m_totalStats.resolveVisibleUnresolvedGeoset++;
    return false;
  }
  if (!TryResolveGeosetRecord(out.visible, semantic, out.geoset)) {
    m_frameStats.resolveGeosetMiss++;
    m_totalStats.resolveGeosetMiss++;
    static std::atomic<uint32_t> s_geosetMissLogCount{0};
    const uint32_t geosetMissLogCount =
        s_geosetMissLogCount.fetch_add(1, std::memory_order_relaxed);
    if (geosetMissLogCount < 24u || (geosetMissLogCount % 4096u) == 0u) {
      model::ShadowGeosetResourceRecord probe = {};
      auto &cache = model::ShadowModelResourceCache::instance();
      const bool byGeosetPtr =
          out.visible.runtimeGeosetPtr != nullptr &&
          cache.findGeosetByPtr(out.visible.runtimeGeosetPtr, probe);
      const bool byGeosetData =
          out.visible.runtimeGeosetDataPtr != nullptr &&
          cache.findGeosetByData(out.visible.runtimeGeosetDataPtr, probe);
      const bool byModelIndex =
          out.visible.modelResourcePtr != nullptr &&
          out.visible.geosetIndex != kInvalidVisibleMeshIndex &&
          cache.findModelGeoset(out.visible.modelResourcePtr, out.visible.geosetIndex,
                                probe);
      dxvk::war3dbg::Print(
          "DXVK UpperShadow: geoset miss idx=%u mesh=%u runtime=%p model=%p "
          "rGeo=%p rGeoData=%p keys[geo=%d geoData=%d model=%d]\n",
          out.visible.geosetIndex, out.visible.meshIndex,
          out.visible.runtimeModelPtr, out.visible.modelResourcePtr,
          out.visible.runtimeGeosetPtr, out.visible.runtimeGeosetDataPtr,
          byGeosetPtr ? 1 : 0, byGeosetData ? 1 : 0, byModelIndex ? 1 : 0);
    }
    return false;
  }

  out.skinned = out.geoset.hasSkinningData();
  out.hasPosePalette = TryResolvePoseRecord(out.visible, semantic, out.pose) &&
                       out.pose.matrixCount != 0 &&
                       !out.pose.matrixPalette.empty();
  if (out.skinned && !out.hasPosePalette) {
    m_frameStats.resolvePoseMiss++;
    m_totalStats.resolvePoseMiss++;
  }
  if (out.skinned && out.hasPosePalette) {
    out.hasRuntimeGroupPalette = TryBuildRuntimeGroupPalette(
        out.geoset, out.pose, out.runtimeGroupPalette,
        out.maxVertexGroupSlot, out.matrixGroupsUseAveraging);
    if (!out.hasRuntimeGroupPalette) {
      m_frameStats.resolveRuntimeGroupPaletteMiss++;
      m_totalStats.resolveRuntimeGroupPaletteMiss++;
    }
  }
  if (out.HasAuthoritativeRigidPath()) {
    m_frameStats.resolveAuthoritativeRigid++;
    m_totalStats.resolveAuthoritativeRigid++;
    rememberResolvedItem(out);
  }
  if (out.HasAuthoritativeSkinnedPath()) {
    m_frameStats.resolveAuthoritativeSkinned++;
    m_totalStats.resolveAuthoritativeSkinned++;
    rememberResolvedItem(out);
  }
  return true;
}

bool UpperLayerShadowRegistry::tryMarkEmitted(
    const UpperLayerShadowResolvedItem &item) {
  const EmissionKey key = MakeEmissionKey(item);
  if (key.primaryPtr == nullptr && key.runtimeModelPtr == nullptr &&
      key.geosetIndex == kInvalidVisibleMeshIndex) {
    m_frameStats.duplicateOrSuppressed++;
    m_totalStats.duplicateOrSuppressed++;
    return false;
  }

  const bool inserted = m_emitted.insert(key).second;
  if (inserted) {
    m_frameStats.emitted++;
    m_totalStats.emitted++;
  } else {
    m_frameStats.duplicateOrSuppressed++;
    m_totalStats.duplicateOrSuppressed++;
  }
  return inserted;
}

UpperLayerShadowResolveStats UpperLayerShadowRegistry::snapshotStats() const {
  return m_totalStats;
}

std::vector<UpperLayerShadowResolvedItem>
UpperLayerShadowRegistry::snapshotResolvedItems() const {
  return m_resolvedItems;
}

UpperLayerShadowRegistry::EmissionKey UpperLayerShadowRegistry::MakeEmissionKey(
    const UpperLayerShadowResolvedItem &item) {
  EmissionKey key = {};
  key.primaryPtr = item.visible.renderablePart != nullptr
                       ? item.visible.renderablePart
                       : item.visible.runtimeGeosetPtr != nullptr
                             ? item.visible.runtimeGeosetPtr
                             : item.visible.meshData != nullptr
                                   ? item.visible.meshData
                                   : item.visible.sceneNode;
  key.runtimeModelPtr = item.visible.runtimeModelPtr;
  key.geosetIndex = item.visible.geosetIndex;
  return key;
}

void UpperLayerShadowRegistry::rememberResolvedItem(
    const UpperLayerShadowResolvedItem& item) const {
  const EmissionKey key = MakeEmissionKey(item);
  if (key.primaryPtr == nullptr && key.runtimeModelPtr == nullptr &&
      key.geosetIndex == kInvalidVisibleMeshIndex) {
    return;
  }

  if (!m_resolvedKeys.insert(key).second)
    return;

  m_resolvedItems.push_back(item);
  m_frameStats.resolvedAuthoritativeItems++;
  m_totalStats.resolvedAuthoritativeItems++;
}

} // namespace dxvk::war3::render
