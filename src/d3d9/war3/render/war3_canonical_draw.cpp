#include "war3_canonical_draw.h"

#include <cstring>

namespace dxvk::war3::render {

const char* CanonicalShadowReadinessReasonName(
    CanonicalShadowReadinessReason reason) {
  switch (reason) {
  case CanonicalShadowReadinessReason::Ready:
    return "ready";
  case CanonicalShadowReadinessReason::NoStableIdentity:
    return "no_stable_identity";
  case CanonicalShadowReadinessReason::NoMesh:
    return "no_mesh";
  case CanonicalShadowReadinessReason::NoWorldTransform:
    return "no_world_transform";
  case CanonicalShadowReadinessReason::NoPalette:
    return "no_palette";
  case CanonicalShadowReadinessReason::NoSlotContract:
    return "no_slot_contract";
  case CanonicalShadowReadinessReason::StaleProducerSample:
    return "stale_producer_sample";
  case CanonicalShadowReadinessReason::InvalidVertexOrIndexSlice:
    return "invalid_vertex_or_index_slice";
  case CanonicalShadowReadinessReason::ExplicitBlendIncomplete:
    return "explicit_blend_incomplete";
  }
  return "unknown";
}

bool BuildCanonicalShadowDrawItem(const CanonicalShadowBuildInputs& inputs,
                                  CanonicalShadowDrawItem& out) {
  out = {};
  if (inputs.packet == nullptr)
    return false;

  const auto& packet = *inputs.packet;
  out.legacyPath = packet.path;
  out.currentDrawResolveStatus =
      inputs.currentDrawSample != nullptr
          ? inputs.currentDrawSample->status
          : CurrentDrawResolveStatus::MissingContract;
  out.authoritativeCurrentDrawResolved =
      inputs.currentDrawSample != nullptr &&
      inputs.currentDrawSample->status == CurrentDrawResolveStatus::Ready;
  out.liveRuntimePaletteRefreshUsed = inputs.liveRuntimeGroupPaletteReady;

  auto& instance = out.instance;
  instance.identity.worldObjectEntry = packet.renderable.worldObjectEntry;
  instance.identity.sceneNode = packet.renderable.sceneNode;
  instance.identity.unitPtr = packet.renderable.unitPtr;
  instance.identity.renderablePart = packet.renderable.renderablePart;
  instance.identity.meshDataPtr = packet.renderable.meshData;
  instance.identity.runtimeModelPtr = packet.renderable.runtimeModelPtr;
  instance.identity.modelResourcePtr =
      packet.renderable.modelResourcePtr != nullptr
          ? packet.renderable.modelResourcePtr
          : packet.resource.modelResourcePtr;
  instance.identity.runtimeGeosetPtr = packet.renderable.runtimeGeosetPtr;
  instance.identity.runtimeGeosetDataPtr =
      packet.renderable.runtimeGeosetDataPtr;
  instance.identity.jHandle = packet.renderable.jHandle;
  instance.identity.rawcode = packet.renderable.rawcode;
  instance.identity.modelKey =
      packet.renderable.modelKey != 0u ? packet.renderable.modelKey
                                       : packet.resource.modelKey;
  instance.identity.objectKind = inputs.resolvedObjectKind;
  instance.identity.frameSerial = packet.renderable.frameSerial;
  instance.identity.poseMatrixCount = packet.pose.matrixCount;
  instance.identity.poseMatrixHash = packet.pose.matrixHash;
  if (inputs.currentDrawSample != nullptr &&
      inputs.currentDrawSample->contract.visibleFrameSerial != 0u) {
    instance.identity.visibleFrameSerial =
        inputs.currentDrawSample->contract.visibleFrameSerial;
    instance.identity.renderFrameIndex =
        inputs.currentDrawSample->contract.renderFrameIndex;
  }

  auto& mesh = instance.mesh;
  mesh.modelResourcePtr = instance.identity.modelResourcePtr;
  mesh.modelKey = instance.identity.modelKey;
  mesh.runtimeGeosetPtr = packet.renderable.runtimeGeosetPtr;
  mesh.runtimeGeosetDataPtr = packet.renderable.runtimeGeosetDataPtr;
  mesh.geosetIndex = packet.resource.geosetIndex;
  mesh.vertexCount = inputs.vertexCount;
  mesh.primitiveRecordCount = packet.resource.primitiveRecordCount;
  mesh.contentHash = packet.resource.contentHash;
  mesh.topology = packet.resource.topology;
  mesh.useIndices = inputs.useIndices;
  mesh.effectiveIndexCount = inputs.effectiveIndexCount;
  mesh.geometrySource = packet.usesDynamicMeshPositions
                            ? CanonicalGeometrySource::DynamicPositionStream
                            : CanonicalGeometrySource::StaticResource;
  mesh.staticPositions = packet.resource.positions;
  mesh.staticIndices = packet.resource.indices;
  mesh.dynamicPositionStream = packet.resource.dynamicPositionStream;
  mesh.dynamicPositionStride = packet.resource.dynamicPositionStride;
  mesh.dynamicIndexStream = packet.resource.dynamicIndexStream;
  mesh.dynamicIndexCount = packet.resource.dynamicIndexCount;
  mesh.dynamicIndexHash = packet.resource.dynamicIndexHash;
  mesh.dynamicPrimitiveBaseIndex = packet.resource.dynamicPrimitiveBaseIndex;
  if (packet.resource.ownedDynamicIndices != nullptr &&
      !packet.resource.ownedDynamicIndices->empty()) {
    mesh.indexSource = CanonicalIndexSource::OwnedDynamicVisibleSlice;
    mesh.dynamicIndexStream = packet.resource.ownedDynamicIndices->data();
    mesh.dynamicIndexCount = uint32_t(packet.resource.ownedDynamicIndices->size());
  } else if (packet.resource.dynamicIndexStream != nullptr &&
             packet.resource.dynamicIndexCount != 0u) {
    mesh.indexSource = CanonicalIndexSource::DynamicVisibleSlice;
  } else if (packet.resource.indices != nullptr &&
             !packet.resource.indices->empty()) {
    mesh.indexSource = CanonicalIndexSource::StaticResource;
  }
  if (packet.usesDynamicMeshPositions &&
      packet.resource.dynamicPositionStream != nullptr &&
      packet.resource.dynamicPositionStride >= 12u &&
      inputs.vertexCount != 0u) {
    const auto* srcBase =
        reinterpret_cast<const uint8_t*>(packet.resource.dynamicPositionStream);
    const size_t srcStride = size_t(packet.resource.dynamicPositionStride);
    mesh.ownedDynamicPositions.resize(size_t(inputs.vertexCount) * 3u);
    for (uint32_t i = 0u; i < inputs.vertexCount; ++i) {
      std::memcpy(mesh.ownedDynamicPositions.data() + size_t(i) * 3u,
                  srcBase + size_t(i) * srcStride, sizeof(float) * 3u);
    }
  }
  if (packet.resource.ownedDynamicIndices != nullptr &&
      !packet.resource.ownedDynamicIndices->empty()) {
    mesh.ownedDynamicIndices = *packet.resource.ownedDynamicIndices;
  }

  auto& material = instance.material;
  material.signatureHash = packet.material.signatureHash;
  material.alphaMode = packet.material.alphaMode;
  material.alphaCutoutRef = packet.material.alphaCutoutRef;
  material.blendOrDrawMode = packet.material.blendOrDrawMode;
  material.layerIndex = packet.material.layerIndex;
  material.queueKind = packet.material.queueKind;
  material.transparentType = packet.material.transparentType;
  material.layerContractResolved = packet.material.layerContractResolved;
  material.layerContractMeshIndex = packet.material.layerContractMeshIndex;
  material.layerContractLayerCount = packet.material.layerContractLayerCount;

  auto& skin = instance.skin;
  skin.skinned = packet.path == shadow::ShadowDrawPath::Skinned;
  skin.matrixGroupsUseAveraging = packet.matrixGroupsUseAveraging;
  skin.explicitBlendCount = packet.resource.explicitBlendCount;
  skin.maxVertexGroupSlot = inputs.effectiveMaxVertexGroupSlot;

  if (skin.skinned && inputs.effectiveRuntimeGroupPalette != nullptr &&
      !inputs.effectiveRuntimeGroupPalette->empty()) {
    if (inputs.ownSkinContracts)
      skin.palette = *inputs.effectiveRuntimeGroupPalette;
    else
      skin.paletteRef = inputs.effectiveRuntimeGroupPalette;
    skin.paletteCount = uint32_t(inputs.effectiveRuntimeGroupPalette->size());
    skin.paletteHash = inputs.effectiveRuntimeGroupPaletteHash;
    if (inputs.currentDrawSample != nullptr &&
        inputs.currentDrawSample->status == CurrentDrawResolveStatus::Ready &&
        inputs.authoritativeGroupSlotsReady &&
        inputs.effectiveRuntimeGroupPaletteHash != 0u) {
      skin.paletteSource = CanonicalPaletteSource::CurrentDrawCapturedPalette;
    } else if (inputs.liveRuntimeGroupPaletteReady) {
      skin.paletteSource = CanonicalPaletteSource::SubmitTimeLivePalette;
    } else {
      skin.paletteSource = CanonicalPaletteSource::PacketRuntimePalette;
    }
  }

  if (skin.skinned) {
    if (inputs.authoritativeGroupSlotsReady &&
        inputs.authoritativeGroupSlots != nullptr &&
        inputs.authoritativeGroupSlots->size() >= size_t(inputs.vertexCount)) {
      skin.groupSlotSource = CanonicalGroupSlotSource::CurrentDrawStream;
      if (inputs.ownSkinContracts) {
        skin.groupSlots.assign(inputs.authoritativeGroupSlots->begin(),
                               inputs.authoritativeGroupSlots->begin() +
                                   inputs.vertexCount);
      } else {
        skin.groupSlotsRef = inputs.authoritativeGroupSlots;
      }
    } else if (packet.resource.explicitBlendCount != 0u &&
        packet.resource.vertexBlendWeights != nullptr &&
        packet.resource.vertexBlendIndices != nullptr &&
        packet.resource.vertexBlendWeights->size() >= size_t(inputs.vertexCount) &&
        packet.resource.vertexBlendIndices->size() >= size_t(inputs.vertexCount)) {
      skin.groupSlotSource = CanonicalGroupSlotSource::ExplicitBlend;
      skin.explicitBlendWeights.assign(
          packet.resource.vertexBlendWeights->begin(),
          packet.resource.vertexBlendWeights->begin() + inputs.vertexCount);
      skin.explicitBlendIndices.assign(
          packet.resource.vertexBlendIndices->begin(),
          packet.resource.vertexBlendIndices->begin() + inputs.vertexCount);
    } else if (packet.resource.vertexGroupIndices != nullptr &&
               packet.resource.vertexGroupIndices->size() >=
                   size_t(inputs.vertexCount)) {
      skin.groupSlotSource = CanonicalGroupSlotSource::ResourceVertexGroups;
      skin.groupSlots.assign(packet.resource.vertexGroupIndices->begin(),
                             packet.resource.vertexGroupIndices->begin() +
                                 inputs.vertexCount);
    }
  }

  auto& world = instance.worldTransform;
  if (skin.skinned &&
      skin.paletteSource == CanonicalPaletteSource::CurrentDrawCapturedPalette) {
    world.valid = true;
    world.matrix = Matrix4();
    world.source = CanonicalWorldTransformSource::CurrentDrawPaletteWorld;
  } else if (inputs.hasSceneNodeWorldMatrix && inputs.sceneNodeWorldMatrix != nullptr) {
    world.valid = true;
    world.matrix = *inputs.sceneNodeWorldMatrix;
    world.source = CanonicalWorldTransformSource::SceneNode;
  } else if (inputs.liveWorldTransform != nullptr) {
    world.valid = true;
    world.matrix = *inputs.liveWorldTransform;
    world.source = CanonicalWorldTransformSource::LivePoseRegistry;
  } else if (packet.pose.hasWorldTransform) {
    world.valid = true;
    world.matrix = packet.pose.worldTransform;
    world.source = CanonicalWorldTransformSource::PacketPoseWorld;
  } else if (!packet.pose.matrixPalette.empty()) {
    world.valid = true;
    world.matrix = packet.pose.matrixPalette[0];
    world.source = CanonicalWorldTransformSource::PacketPosePalette0;
  }

  const auto meshReady =
      inputs.vertexCount != 0u &&
      instance.mesh.effectivePositionVec().size() >= size_t(inputs.vertexCount) * 3u;
  const auto indexReady =
      !instance.mesh.useIndices ||
      (instance.mesh.effectiveIndexCount != 0u &&
       instance.mesh.effectiveIndexData() != nullptr);
  if (!instance.identity.hasStableIdentity()) {
    out.readinessReason = CanonicalShadowReadinessReason::NoStableIdentity;
  } else if (!meshReady) {
    out.readinessReason = CanonicalShadowReadinessReason::NoMesh;
  } else if (!indexReady) {
    out.readinessReason =
        CanonicalShadowReadinessReason::InvalidVertexOrIndexSlice;
  } else if (out.currentDrawResolveStatus ==
             CurrentDrawResolveStatus::StaleVisibleFrame) {
    out.readinessReason = CanonicalShadowReadinessReason::StaleProducerSample;
  } else if (!world.valid) {
    out.readinessReason = CanonicalShadowReadinessReason::NoWorldTransform;
  } else if (out.legacyPath == shadow::ShadowDrawPath::Skinned) {
    if (!skin.paletteReady() ||
        skin.maxVertexGroupSlot >= skin.paletteVec().size()) {
      out.readinessReason = CanonicalShadowReadinessReason::NoPalette;
    } else if (skin.usesExplicitBlendContract()) {
      if (skin.explicitBlendIndices.size() < size_t(inputs.vertexCount) ||
          skin.explicitBlendWeights.size() < size_t(inputs.vertexCount)) {
        out.readinessReason =
            CanonicalShadowReadinessReason::ExplicitBlendIncomplete;
      }
    } else if (!skin.hasUsableVertexContract(inputs.vertexCount)) {
      out.readinessReason = CanonicalShadowReadinessReason::NoSlotContract;
    }
  }

  return true;
}

} // namespace dxvk::war3::render
