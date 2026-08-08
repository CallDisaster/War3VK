#pragma once

#include "../shadow/war3_shadow_backend.h"
#include "../shadow/war3_shadow_renderer_core.h"
#include "war3_current_draw_contract.h"
#include "war3_render_objects.h"

#include "../../util/util_matrix.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dxvk::war3::render {

enum class CanonicalGeometrySource : uint8_t {
  StaticResource = 0,
  DynamicPositionStream = 1,
};

enum class CanonicalIndexSource : uint8_t {
  None = 0,
  StaticResource = 1,
  DynamicVisibleSlice = 2,
  OwnedDynamicVisibleSlice = 3,
};

enum class CanonicalGroupSlotSource : uint8_t {
  None = 0,
  ResourceVertexGroups = 1,
  CurrentDrawStream = 2,
  ExplicitBlend = 3,
};

enum class CanonicalPaletteSource : uint8_t {
  None = 0,
  PacketRuntimePalette = 1,
  CurrentDrawCapturedPalette = 2,
  SubmitTimeLivePalette = 3,
};

enum class CanonicalWorldTransformSource : uint8_t {
  None = 0,
  SceneNode = 1,
  LivePoseRegistry = 2,
  PacketPoseWorld = 3,
  PacketPosePalette0 = 4,
  CurrentDrawPaletteWorld = 5,
};

enum class CanonicalShadowReadinessReason : uint8_t {
  Ready = 0,
  NoStableIdentity = 1,
  NoMesh = 2,
  NoWorldTransform = 3,
  NoPalette = 4,
  NoSlotContract = 5,
  StaleProducerSample = 6,
  InvalidVertexOrIndexSlice = 7,
  ExplicitBlendIncomplete = 8,
};

struct CanonicalDrawIdentity {
  void* worldObjectEntry = nullptr;
  void* sceneNode = nullptr;
  void* unitPtr = nullptr;
  void* renderablePart = nullptr;
  void* meshDataPtr = nullptr;
  void* runtimeModelPtr = nullptr;
  void* modelResourcePtr = nullptr;
  void* runtimeGeosetPtr = nullptr;
  void* runtimeGeosetDataPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  uint64_t modelKey = 0u;
  ObjectKind objectKind = ObjectKind::Unknown;
  uint64_t frameSerial = 0u;
  uint64_t visibleFrameSerial = 0u;
  uint32_t renderFrameIndex = 0u;
  uint32_t poseMatrixCount = 0u;
  uint64_t poseMatrixHash = 0u;

  bool hasStableIdentity() const {
    return worldObjectEntry != nullptr || sceneNode != nullptr ||
           unitPtr != nullptr || runtimeModelPtr != nullptr ||
           modelResourcePtr != nullptr || modelKey != 0u || jHandle != 0u ||
           rawcode != 0u;
  }
};

// Phase 2 first landing uses a hybrid canonical asset:
// stable/topology-facing fields borrow resource-side buffers,
// while frame-local dynamic data remains referenced by the caller.
struct CanonicalMeshAsset {
  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0u;
  void* runtimeGeosetPtr = nullptr;
  void* runtimeGeosetDataPtr = nullptr;
  uint32_t geosetIndex = shadow::kInvalidShadowContractGeosetIndex;
  uint32_t vertexCount = 0u;
  uint32_t primitiveRecordCount = 0u;
  uint64_t contentHash = 0u;
  uint64_t mapEpoch = 0u;
  uint64_t immutableModelGeneration = 0u;
  model::ShadowGeosetLocalBounds localBounds = {};
  shadow::ShadowPrimitiveTopology topology =
      shadow::ShadowPrimitiveTopology::TriangleList;
  CanonicalGeometrySource geometrySource =
      CanonicalGeometrySource::StaticResource;
  CanonicalIndexSource indexSource = CanonicalIndexSource::None;
  bool useIndices = false;
  uint32_t effectiveIndexCount = 0u;
  const std::vector<float>* staticPositions = nullptr;
  const std::vector<uint16_t>* staticIndices = nullptr;
  const void* dynamicPositionStream = nullptr;
  uint32_t dynamicPositionStride = 0u;
  const uint16_t* dynamicIndexStream = nullptr;
  uint32_t dynamicIndexCount = 0u;
  uint64_t dynamicIndexHash = 0u;
  uint32_t dynamicPrimitiveBaseIndex = 0u;
  std::vector<float> ownedDynamicPositions;
  std::vector<uint16_t> ownedDynamicIndices;

  const std::vector<float>& effectivePositionVec() const {
    static const std::vector<float> kEmpty;
    if (!ownedDynamicPositions.empty())
      return ownedDynamicPositions;
    if (staticPositions != nullptr)
      return *staticPositions;
    return kEmpty;
  }

  const uint16_t* effectiveIndexData() const {
    if (!ownedDynamicIndices.empty())
      return ownedDynamicIndices.data();
    if (dynamicIndexStream != nullptr && dynamicIndexCount != 0u)
      return dynamicIndexStream;
    if (staticIndices != nullptr && !staticIndices->empty())
      return staticIndices->data();
    return nullptr;
  }

  bool hasStaticPositions() const {
    return staticPositions != nullptr && !staticPositions->empty();
  }

  bool hasDynamicPositions() const {
    return dynamicPositionStream != nullptr && dynamicPositionStride >= 12u &&
           vertexCount != 0u;
  }

  bool hasVisibleIndexSlice() const {
    return indexSource == CanonicalIndexSource::DynamicVisibleSlice ||
           indexSource == CanonicalIndexSource::OwnedDynamicVisibleSlice;
  }
};

struct CanonicalMaterialContract {
  uint64_t signatureHash = 0u;
  shadow::ShadowAlphaMode alphaMode = shadow::ShadowAlphaMode::Opaque;
  float alphaCutoutRef = 0.5f;
  uint32_t blendOrDrawMode = 0u;
  uint32_t layerIndex = 0u;
  uint32_t queueKind = 0u;
  uint32_t transparentType = 0u;
  bool layerContractResolved = false;
  uint32_t layerContractMeshIndex = 0u;
  uint32_t layerContractLayerCount = 0u;

  bool valid() const {
    return signatureHash != 0u;
  }
};

struct CanonicalSkinContract {
  bool skinned = false;
  bool matrixGroupsUseAveraging = false;
  uint8_t explicitBlendCount = 0u;
  uint32_t maxVertexGroupSlot = 0u;
  CanonicalGroupSlotSource groupSlotSource = CanonicalGroupSlotSource::None;
  CanonicalPaletteSource paletteSource = CanonicalPaletteSource::None;
  std::vector<uint8_t> groupSlots;
  std::vector<std::array<float, 3>> explicitBlendWeights;
  std::vector<std::array<uint8_t, 4>> explicitBlendIndices;
  std::vector<Matrix4> palette;
  const std::vector<uint8_t>* groupSlotsRef = nullptr;
  const std::vector<Matrix4>* paletteRef = nullptr;
  uint32_t paletteCount = 0u;
  uint64_t paletteHash = 0u;

  const std::vector<uint8_t>& groupSlotsVec() const {
    static const std::vector<uint8_t> kEmpty;
    if (groupSlotsRef != nullptr)
      return *groupSlotsRef;
    return groupSlots.empty() ? kEmpty : groupSlots;
  }

  const std::vector<Matrix4>& paletteVec() const {
    static const std::vector<Matrix4> kEmpty;
    if (paletteRef != nullptr)
      return *paletteRef;
    return palette.empty() ? kEmpty : palette;
  }

  bool paletteReady() const {
    const auto& activePalette = paletteVec();
    return !activePalette.empty() && paletteCount != 0u &&
           activePalette.size() >= paletteCount;
  }

  bool usesExplicitBlendContract() const {
    return skinned &&
           groupSlotSource == CanonicalGroupSlotSource::ExplicitBlend;
  }

  bool usesCurrentDrawGroupSlots() const {
    return groupSlotSource == CanonicalGroupSlotSource::CurrentDrawStream;
  }

  bool hasUsableVertexContract(uint32_t vertexCount) const {
    if (!skinned)
      return true;
    if (usesExplicitBlendContract())
      return explicitBlendIndices.size() >= size_t(vertexCount) &&
             explicitBlendWeights.size() >= size_t(vertexCount);
    return groupSlotsVec().size() >= size_t(vertexCount);
  }
};

struct CanonicalWorldTransform {
  bool valid = false;
  Matrix4 matrix = Matrix4();
  CanonicalWorldTransformSource source = CanonicalWorldTransformSource::None;
};

struct CanonicalStaticIdentityContract {
  void* worldObjectEntry = nullptr;
  void* sceneNode = nullptr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  uint64_t modelKey = 0u;
  ObjectKind objectKind = ObjectKind::Unknown;

  bool valid() const {
    return worldObjectEntry != nullptr || sceneNode != nullptr ||
           unitPtr != nullptr || jHandle != 0u || rawcode != 0u ||
           modelKey != 0u;
  }
};

struct CanonicalRigidTransformContract {
  bool valid = false;
  Matrix4 matrix = Matrix4();
  CanonicalWorldTransformSource source = CanonicalWorldTransformSource::None;
};

struct CanonicalDrawInstance {
  CanonicalDrawIdentity identity = {};
  CanonicalMeshAsset mesh = {};
  CanonicalMaterialContract material = {};
  CanonicalSkinContract skin = {};
  CanonicalWorldTransform worldTransform = {};
};

struct CanonicalShadowDrawItem {
  CanonicalDrawInstance instance = {};
  shadow::ShadowDrawPath legacyPath = shadow::ShadowDrawPath::Rigid;
  CurrentDrawResolveStatus currentDrawResolveStatus =
      CurrentDrawResolveStatus::MissingContract;
  bool authoritativeCurrentDrawResolved = false;
  bool liveRuntimePaletteRefreshUsed = false;
  CanonicalShadowReadinessReason readinessReason =
      CanonicalShadowReadinessReason::Ready;

  bool readyForShadowConsumer() const {
    return readinessReason == CanonicalShadowReadinessReason::Ready;
  }
};

struct CanonicalRigidDrawItem {
  CanonicalStaticIdentityContract identity = {};
  CanonicalMeshAsset mesh = {};
  CanonicalMaterialContract material = {};
  CanonicalRigidTransformContract transform = {};

  bool readyForShadowConsumer() const {
    return identity.valid() && transform.valid &&
           mesh.vertexCount != 0u &&
           mesh.effectivePositionVec().size() >= size_t(mesh.vertexCount) * 3u;
  }
};

struct CanonicalShadowBuildInputs {
  const shadow::ShadowDrawPacket* packet = nullptr;
  ObjectKind resolvedObjectKind = ObjectKind::Unknown;
  uint32_t vertexCount = 0u;
  bool useIndices = false;
  uint32_t effectiveIndexCount = 0u;
  const CurrentDrawAuthoritativeSample* currentDrawSample = nullptr;
  const std::vector<Matrix4>* effectiveRuntimeGroupPalette = nullptr;
  uint64_t effectiveRuntimeGroupPaletteHash = 0u;
  uint32_t effectiveMaxVertexGroupSlot = 0u;
  bool liveRuntimeGroupPaletteReady = false;
  const std::vector<uint8_t>* authoritativeGroupSlots = nullptr;
  bool authoritativeGroupSlotsReady = false;
  const std::vector<float>* packetPositions = nullptr;
  const std::vector<uint16_t>* packetIndices = nullptr;
  const Matrix4* liveWorldTransform = nullptr;
  const Matrix4* sceneNodeWorldMatrix = nullptr;
  bool hasSceneNodeWorldMatrix = false;
  bool ownSkinContracts = true;
};

bool BuildCanonicalShadowDrawItem(const CanonicalShadowBuildInputs& inputs,
                                  CanonicalShadowDrawItem& out);

const char* CanonicalShadowReadinessReasonName(
    CanonicalShadowReadinessReason reason);

} // namespace dxvk::war3::render
