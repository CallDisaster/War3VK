#include "war3_point_shadow_cpu_plan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace dxvk::war3::render {

namespace {

constexpr uint64_t kSignatureSeed = 0xcbf29ce484222325ull;
constexpr uint64_t kSignatureMixConstant = 0x9e3779b97f4a7c15ull;
constexpr float kSqrt2 = 1.41421356237f;
constexpr float kPointShadowFov = 1.5707963f;

struct FaceParameters {
  War3PointShadowCpuFloat4 direction;
  War3PointShadowCpuFloat4 up;
  War3PointShadowCpuFloat4 right;
};

constexpr std::array<FaceParameters, kWar3PointShadowCpuPlanFaceCount>
    kFaceParameters = {{
        {{1.0f, 0.0f, 0.0f, 0.0f},
         {0.0f, -1.0f, 0.0f, 0.0f},
         {0.0f, 0.0f, 1.0f, 0.0f}},
        {{-1.0f, 0.0f, 0.0f, 0.0f},
         {0.0f, -1.0f, 0.0f, 0.0f},
         {0.0f, 0.0f, -1.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f, 0.0f},
         {0.0f, 0.0f, 1.0f, 0.0f},
         {-1.0f, 0.0f, 0.0f, 0.0f}},
        {{0.0f, -1.0f, 0.0f, 0.0f},
         {0.0f, 0.0f, -1.0f, 0.0f},
         {-1.0f, 0.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, 1.0f, 0.0f},
         {0.0f, -1.0f, 0.0f, 0.0f},
         {-1.0f, 0.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f, 0.0f},
         {0.0f, -1.0f, 0.0f, 0.0f},
         {1.0f, 0.0f, 0.0f, 0.0f}},
    }};

struct SignatureBuilder {
  uint64_t value = kSignatureSeed;

  void mixU64(uint64_t token) noexcept {
    value ^= token + kSignatureMixConstant + (value << 6u) + (value >> 2u);
  }

  void mixF32(float token) noexcept {
    uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(token));
    std::memcpy(&bits, &token, sizeof(bits));
    mixU64(bits);
  }

  void mixMatrix(const War3PointShadowCpuMatrix4& matrix) noexcept {
    for (uint32_t vector = 0u; vector < 4u; ++vector) {
      for (uint32_t component = 0u; component < 4u; ++component)
        mixF32(matrix.vectors[vector][component]);
    }
  }
};

bool IsFinite(float value) noexcept {
  return std::isfinite(value);
}

bool IsFiniteLight(const War3PointShadowCpuLight& light) noexcept {
  return IsFinite(light.position.x) && IsFinite(light.position.y) &&
      IsFinite(light.position.z) && IsFinite(light.position.w) &&
      IsFinite(light.shadowIntensity);
}

bool HasKnownBounds(const War3PointShadowCpuCaster& caster) noexcept {
  // Hardening delta from the legacy async path: a positive radius did not
  // prove that its center was finite, allowing NaN into range comparisons and
  // the nearest-N comparator. Any non-finite component is now unknown and
  // therefore conservatively pinned rather than culled. This is intentionally
  // not claimed to be bit-equivalent for malformed inputs.
  return caster.boundsRadius > 0.0f && IsFinite(caster.boundsRadius) &&
      IsFinite(caster.boundsCenter.x) && IsFinite(caster.boundsCenter.y) &&
      IsFinite(caster.boundsCenter.z);
}

bool HasNonFiniteBounds(const War3PointShadowCpuCaster& caster) noexcept {
  return !IsFinite(caster.boundsRadius) ||
      !IsFinite(caster.boundsCenter.x) ||
      !IsFinite(caster.boundsCenter.y) ||
      !IsFinite(caster.boundsCenter.z);
}

War3PointShadowCpuFloat4 Cross3(const War3PointShadowCpuFloat4& lhs,
                                const War3PointShadowCpuFloat4& rhs) noexcept {
  return {lhs.y * rhs.z - lhs.z * rhs.y,
          lhs.z * rhs.x - lhs.x * rhs.z,
          lhs.x * rhs.y - lhs.y * rhs.x,
          0.0f};
}

War3PointShadowCpuFloat4 Normalize3(
    War3PointShadowCpuFloat4 value) noexcept {
  const float lengthSquared =
      value.x * value.x + value.y * value.y + value.z * value.z;
  if (lengthSquared <= std::numeric_limits<float>::min())
    return value;
  const float inverseLength = 1.0f / std::sqrt(lengthSquared);
  value.x *= inverseLength;
  value.y *= inverseLength;
  value.z *= inverseLength;
  value.w = 0.0f;
  return value;
}

War3PointShadowCpuMatrix4 MakeLookAtLeftHanded(
    const War3PointShadowCpuFloat4& eye,
    const War3PointShadowCpuFloat4& target,
    const War3PointShadowCpuFloat4& up) noexcept {
  const War3PointShadowCpuFloat4 forward = Normalize3(
      {target.x - eye.x, target.y - eye.y, target.z - eye.z, 0.0f});
  const War3PointShadowCpuFloat4 right = Normalize3(Cross3(up, forward));
  const War3PointShadowCpuFloat4 adjustedUp = Cross3(forward, right);

  War3PointShadowCpuMatrix4 result = {};
  result.vectors[0][0] = right.x;
  result.vectors[0][1] = adjustedUp.x;
  result.vectors[0][2] = forward.x;
  result.vectors[1][0] = right.y;
  result.vectors[1][1] = adjustedUp.y;
  result.vectors[1][2] = forward.y;
  result.vectors[2][0] = right.z;
  result.vectors[2][1] = adjustedUp.z;
  result.vectors[2][2] = forward.z;
  result.vectors[3][0] =
      -(eye.x * right.x + eye.y * right.y + eye.z * right.z);
  result.vectors[3][1] = -(eye.x * adjustedUp.x +
                           eye.y * adjustedUp.y +
                           eye.z * adjustedUp.z);
  result.vectors[3][2] =
      -(eye.x * forward.x + eye.y * forward.y + eye.z * forward.z);
  result.vectors[3][3] = 1.0f;
  return result;
}

War3PointShadowCpuMatrix4 MultiplyLikeDxvkMatrix4(
    const War3PointShadowCpuMatrix4& lhs,
    const War3PointShadowCpuMatrix4& rhs) noexcept {
  War3PointShadowCpuMatrix4 result = {};
  for (uint32_t vector = 0u; vector < 4u; ++vector) {
    for (uint32_t component = 0u; component < 4u; ++component) {
      result.vectors[vector][component] =
          lhs.vectors[0][component] * rhs.vectors[vector][0] +
          lhs.vectors[1][component] * rhs.vectors[vector][1] +
          lhs.vectors[2][component] * rhs.vectors[vector][2] +
          lhs.vectors[3][component] * rhs.vectors[vector][3];
    }
  }
  return result;
}

War3PointShadowCpuMatrix4 MakePointShadowProjection(float range) noexcept {
  const float nearPlane = 1.0f;
  const float farPlane = std::max(range, nearPlane + 1.0f);
  const float tangent = std::tan(kPointShadowFov * 0.5f);
  War3PointShadowCpuMatrix4 result = {};
  result.vectors[0][0] = -1.0f / tangent;
  result.vectors[1][1] = 1.0f / tangent;
  result.vectors[2][2] = farPlane / (farPlane - nearPlane);
  result.vectors[2][3] = 1.0f;
  result.vectors[3][2] =
      -(nearPlane * farPlane) / (farPlane - nearPlane);
  return result;
}

void MixCasterSignature(SignatureBuilder& signature,
                        const War3PointShadowCpuCaster& caster) noexcept {
  signature.mixF32(caster.boundsCenter.x);
  signature.mixF32(caster.boundsCenter.y);
  signature.mixF32(caster.boundsCenter.z);
  signature.mixF32(caster.boundsRadius);
  signature.mixU64(caster.vertexCount);
  signature.mixU64(caster.indexCount);
  signature.mixU64(caster.indexed ? 1u : 0u);
  signature.mixU64(caster.positionFormat);
  signature.mixU64(caster.positionStride);
  signature.mixU64(caster.positionOffset);
  signature.mixU64(caster.indexType);
  signature.mixU64(caster.vertexBlendEnabled ? 1u : 0u);
  signature.mixU64(caster.vertexBlendIndexed ? 1u : 0u);
  signature.mixU64(caster.vertexBlendCount);
  signature.mixU64(caster.paletteIndex);
  signature.mixU64(caster.blendWeightFormat);
  signature.mixU64(caster.blendWeightOffset);
  signature.mixU64(caster.blendIndexFormat);
  signature.mixU64(caster.blendIndexOffset);
  signature.mixU64(caster.blendBinding);
  signature.mixU64(caster.blendStride);
  signature.mixU64(caster.topology);
  signature.mixU64(caster.firstIndex);
  signature.mixU64(static_cast<uint32_t>(caster.vertexOffset));
  signature.mixU64(caster.firstVertex);
  signature.mixU64(caster.minVertexIndex);
  signature.mixU64(caster.numVertices);
  signature.mixU64(caster.positionStorageIdentity);
  signature.mixU64(caster.positionBufferIdentity);
  signature.mixU64(caster.positionBufferOffset);
  signature.mixU64(caster.positionBufferSize);
  signature.mixU64(caster.indexStorageIdentity);
  signature.mixU64(caster.indexBufferIdentity);
  signature.mixU64(caster.indexBufferOffset);
  signature.mixU64(caster.indexBufferSize);
  signature.mixU64(caster.blendStorageIdentity);
  signature.mixU64(caster.blendBufferIdentity);
  signature.mixU64(caster.blendBufferOffset);
  signature.mixU64(caster.blendBufferSize);
  signature.mixU64(caster.alphaTestEnabled ? 1u : 0u);
  signature.mixU64(caster.alphaBlendEnabled ? 1u : 0u);
  signature.mixF32(caster.alphaRef);
  signature.mixU64(caster.uvFormat);
  signature.mixU64(caster.uvStride);
  signature.mixU64(caster.uvOffset);
  signature.mixU64(caster.uvBinding);
  signature.mixU64(caster.uvStorageIdentity);
  signature.mixU64(caster.uvBufferIdentity);
  signature.mixU64(caster.uvBufferOffset);
  signature.mixU64(caster.uvBufferSize);
  signature.mixU64(caster.diffuseTextureIdentity);
  signature.mixU64(caster.diffuseSamplerIdentity);
  signature.mixU64(caster.diffuseSamplerIndex);
  signature.mixMatrix(caster.worldMatrix);
}

bool CasterInLightRange(const War3PointShadowCpuCaster& caster,
                        const War3PointShadowCpuFloat4& lightPositionRange,
                        float cullPadding) noexcept {
  if (!HasKnownBounds(caster))
    return true;
  const float range = std::max(lightPositionRange.w, 1.0f) * cullPadding;
  const float deltaX = caster.boundsCenter.x - lightPositionRange.x;
  const float deltaY = caster.boundsCenter.y - lightPositionRange.y;
  const float deltaZ = caster.boundsCenter.z - lightPositionRange.z;
  const float reach = range + caster.boundsRadius;
  return deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ <=
      reach * reach;
}

bool CasterOnFace(const War3PointShadowCpuCaster& caster,
                  const War3PointShadowCpuFloat4& lightPositionRange,
                  const FaceParameters& face,
                  bool faceCulling) noexcept {
  if (!faceCulling || !HasKnownBounds(caster))
    return true;
  const float deltaX = caster.boundsCenter.x - lightPositionRange.x;
  const float deltaY = caster.boundsCenter.y - lightPositionRange.y;
  const float deltaZ = caster.boundsCenter.z - lightPositionRange.z;
  const float viewX = deltaX * face.right.x + deltaY * face.right.y +
      deltaZ * face.right.z;
  const float viewY = deltaX * face.up.x + deltaY * face.up.y +
      deltaZ * face.up.z;
  const float viewZ = deltaX * face.direction.x +
      deltaY * face.direction.y + deltaZ * face.direction.z;
  if (viewZ < -caster.boundsRadius)
    return false;
  const float sideMargin = caster.boundsRadius * kSqrt2;
  return std::abs(viewX) <= viewZ + sideMargin &&
      std::abs(viewY) <= viewZ + sideMargin;
}

uint32_t ClampCount(size_t size) noexcept {
  return static_cast<uint32_t>(std::min<size_t>(
      size, std::numeric_limits<uint32_t>::max()));
}

War3PointShadowCpuPlanResultPayload MakeRejected(
    War3PointShadowCpuPlanDisposition disposition,
    War3PointShadowCpuPlanRejectReason reason,
    const War3PointShadowCpuPlanSealTuple& seal,
    War3PointShadowCpuPlanOwnedStorage&& storage) {
  War3PointShadowCpuPlanResultPayload result = {};
  result.disposition = disposition;
  result.rejectReason = reason;
  result.ownerMustInvalidatePublication = true;
  result.seal = seal;
  result.storage = std::move(storage);
  return result;
}

} // namespace

uint32_t War3ResolvePointShadowCpuPlanCapacity(uint32_t resolution,
                                               uint32_t requestedLights) {
  resolution = std::clamp<uint32_t>(resolution, 128u, 2048u);
  requestedLights = std::clamp<uint32_t>(
      requestedLights, 1u, kWar3PointShadowCpuPlanMaxLights);
  const uint64_t bytesPerLight = uint64_t(resolution) * resolution * 4ull * 6ull;
  const uint64_t capacity = std::max<uint64_t>(
      1ull, kWar3PointShadowCpuPlanResourceBudgetBytes / bytesPerLight);
  return std::min<uint32_t>(
      requestedLights,
      static_cast<uint32_t>(std::min<uint64_t>(
          capacity, kWar3PointShadowCpuPlanMaxLights)));
}

War3PointShadowCpuPlanResultPayload War3BuildPointShadowCpuPlan(
    War3PointShadowCpuPlanRequest&& request) {
  War3PointShadowCpuPlanOwnedStorage storage =
      std::move(request.payload.storage);
  const War3PointShadowCpuPlanSealTuple seal = request.payload.seal;
  storage.rangeCandidateIndices.clear();
  storage.rankedCandidates.clear();
  for (std::vector<uint32_t>& faceCasters : storage.faceCasters)
    faceCasters.clear();

  if (!request.generation.valid()) {
    return MakeRejected(War3PointShadowCpuPlanDisposition::InvalidRequest,
                        War3PointShadowCpuPlanRejectReason::InvalidGeneration,
                        seal,
                        std::move(storage));
  }
  if (!seal.valid()) {
    return MakeRejected(War3PointShadowCpuPlanDisposition::InvalidRequest,
                        War3PointShadowCpuPlanRejectReason::InvalidSealTuple,
                        seal, std::move(storage));
  }

  const War3PointShadowCpuPlanSettings& settings = request.payload.settings;
  if (!settings.pointLightsEnabled || !settings.pointShadowEnabled ||
      settings.pointShadowMaxLights == 0u || !request.payload.hasAnyLight) {
    return MakeRejected(War3PointShadowCpuPlanDisposition::Disabled,
                        War3PointShadowCpuPlanRejectReason::Disabled,
                        seal,
                        std::move(storage));
  }
  if (storage.casters.empty()) {
    return MakeRejected(War3PointShadowCpuPlanDisposition::NoCasters,
                        War3PointShadowCpuPlanRejectReason::NoCasters,
                        seal,
                        std::move(storage));
  }
  if (storage.casters.size() >
      std::numeric_limits<uint32_t>::max()) {
    return MakeRejected(War3PointShadowCpuPlanDisposition::InvalidRequest,
                        War3PointShadowCpuPlanRejectReason::TooManyCasters,
                        seal,
                        std::move(storage));
  }
  const uint32_t resolution =
      std::clamp<uint32_t>(settings.pointShadowResolution, 128u, 2048u);
  const uint32_t requestedLights = std::clamp<uint32_t>(
      settings.pointShadowMaxLights, 1u,
      kWar3PointShadowCpuPlanMaxLights);
  const uint32_t capacityLights =
      War3ResolvePointShadowCpuPlanCapacity(resolution, requestedLights);
  const uint32_t shadowLightCount = std::min<uint32_t>(
      {kWar3PointShadowCpuPlanMaxLights, capacityLights,
       request.payload.shadowLightCount});
  if (shadowLightCount == 0u) {
    return MakeRejected(War3PointShadowCpuPlanDisposition::NoLights,
                        War3PointShadowCpuPlanRejectReason::NoLights,
                        seal,
                        std::move(storage));
  }
  for (uint32_t lightIndex = 0u; lightIndex < shadowLightCount; ++lightIndex) {
    if (!IsFiniteLight(request.payload.lights[lightIndex])) {
      return MakeRejected(
          War3PointShadowCpuPlanDisposition::InvalidRequest,
          War3PointShadowCpuPlanRejectReason::NonFiniteLight,
          seal,
          std::move(storage));
    }
  }
  for (const War3PointShadowCpuCaster& caster : storage.casters) {
    if (!caster.frozenComplete) {
      return MakeRejected(
          War3PointShadowCpuPlanDisposition::InvalidRequest,
          War3PointShadowCpuPlanRejectReason::IncompleteCasterFreeze,
          seal,
          std::move(storage));
    }
  }

  War3PointShadowCpuPlanResultPayload result = {};
  result.disposition = War3PointShadowCpuPlanDisposition::Render;
  result.rejectReason = War3PointShadowCpuPlanRejectReason::None;
  result.ownerMustInvalidatePublication = false;
  result.shadowLightCount = shadowLightCount;
  result.resourceCapacityLights = capacityLights;
  result.resolution = resolution;
  result.maxCastersPerFace = settings.pointShadowMaxCastersPerFace;
  result.seal = seal;
  for (const War3PointShadowCpuCaster& caster : storage.casters) {
    if (!HasKnownBounds(caster)) {
      ++result.unknownBoundsPinnedCount;
      if (HasNonFiniteBounds(caster))
        ++result.nonFiniteBoundsPinnedCount;
    }
  }

  const float cullPadding =
      std::max(1.0f, settings.pointShadowCasterCullPadding);
  const bool incrementalSignature =
      (settings.pointShadowTemporalReuse &&
       settings.pointShadowUpdatePeriod > 1u) ||
      (settings.pointShadowMaxFacesPerFrame > 0u &&
       settings.pointShadowMaxFacesPerFrame <
           kWar3PointShadowCpuPlanFaceCount);

  SignatureBuilder signature;
  signature.mixU64(shadowLightCount);
  signature.mixU64(request.generation.lightGeneration);
  signature.mixU64(resolution);
  signature.mixU64(capacityLights);
  signature.mixU64(settings.pointShadowFaceCulling ? 1u : 0u);
  signature.mixU64(settings.pointShadowMaxCastersPerFace);
  signature.mixF32(cullPadding);
  for (uint32_t lightIndex = 0u; lightIndex < shadowLightCount; ++lightIndex) {
    const War3PointShadowCpuLight& light = request.payload.lights[lightIndex];
    signature.mixU64(static_cast<uint64_t>(light.id));
    signature.mixF32(light.position.x);
    signature.mixF32(light.position.y);
    signature.mixF32(light.position.z);
    signature.mixF32(light.position.w);
    signature.mixF32(light.shadowIntensity);
  }

  bool hasDynamicCaster = false;
  if (incrementalSignature) {
    signature.mixU64(storage.casters.size());
    signature.mixU64(settings.alphaShadowHashed ? 1u : 0u);
    signature.mixU64(request.payload.dynamicPoseSignature);
    for (uint64_t paletteHash : storage.paletteHashes)
      signature.mixU64(paletteHash);
    hasDynamicCaster = request.payload.dynamicPoseCount > 0u ||
        request.payload.dynamicSkinnedOutputCount > 0u;
    for (const War3PointShadowCpuCaster& caster : storage.casters) {
      MixCasterSignature(signature, caster);
      hasDynamicCaster = hasDynamicCaster || caster.vertexBlendEnabled;
    }
  }
  result.contentSignature = signature.value;

  const War3PointShadowCpuHistory& history = request.payload.history;
  const bool signatureUnchanged = history.cubeAllocated &&
      history.readyLightCount > 0u &&
      history.publishedContentSignature == result.contentSignature;
  const bool temporalReuse = settings.pointShadowTemporalReuse &&
      settings.pointShadowUpdatePeriod > 1u && signatureUnchanged &&
      !hasDynamicCaster;
  if (temporalReuse) {
    const uint32_t period =
        std::max(1u, settings.pointShadowUpdatePeriod);
    if (uint64_t(history.temporalAge) + 1ull < uint64_t(period)) {
      result.disposition =
          War3PointShadowCpuPlanDisposition::ReusePublished;
      result.shouldRender = false;
      result.reusePublished = true;
      result.ownerMustInvalidatePublication = false;
      result.nextTemporalAge = history.temporalAge + 1u;
      result.storage = std::move(storage);
      return result;
    }
  }

  result.shouldRender = true;
  result.forceFullFaceUpdate = !signatureUnchanged || hasDynamicCaster;
  result.ownerMustClearFaceValidityBeforeRecord =
      result.forceFullFaceUpdate;
  result.nextTemporalAge = 0u;
  uint32_t maxFacesPerFrame = settings.pointShadowMaxFacesPerFrame;
  if (maxFacesPerFrame == 0u ||
      maxFacesPerFrame >= kWar3PointShadowCpuPlanFaceCount ||
      result.forceFullFaceUpdate) {
    maxFacesPerFrame = kWar3PointShadowCpuPlanFaceCount;
  }
  result.maxFacesPerFrame = maxFacesPerFrame;

  std::vector<uint32_t>& rangeCandidates = storage.rangeCandidateIndices;
  rangeCandidates.reserve(storage.casters.size());
  std::vector<War3PointShadowCpuRankedCandidate>& faceCandidates =
      storage.rankedCandidates;
  faceCandidates.reserve(storage.casters.size());

  for (uint32_t lightIndex = 0u; lightIndex < shadowLightCount; ++lightIndex) {
    const War3PointShadowCpuLight& light = request.payload.lights[lightIndex];
    War3PointShadowCpuLightPlan& lightPlan = result.lights[lightIndex];
    lightPlan.lightPositionRange =
        {light.position.x, light.position.y, light.position.z,
         std::max(light.position.w, 1.0f)};
    lightPlan.shadowIntensity =
        std::clamp(light.shadowIntensity, 0.0f, 1.0f);

    const War3PointShadowCpuMatrix4 projection =
        MakePointShadowProjection(lightPlan.lightPositionRange.w);
    const War3PointShadowCpuFloat4 eye =
        {lightPlan.lightPositionRange.x, lightPlan.lightPositionRange.y,
         lightPlan.lightPositionRange.z, 1.0f};
    for (uint32_t face = 0u; face < kWar3PointShadowCpuPlanFaceCount;
         ++face) {
      const War3PointShadowCpuFloat4 target = {
          eye.x + kFaceParameters[face].direction.x,
          eye.y + kFaceParameters[face].direction.y,
          eye.z + kFaceParameters[face].direction.z,
          1.0f};
      const War3PointShadowCpuMatrix4 view = MakeLookAtLeftHanded(
          eye, target, kFaceParameters[face].up);
      lightPlan.faceViewProjection[face] =
          MultiplyLikeDxvkMatrix4(projection, view);
    }

    rangeCandidates.clear();
    for (uint32_t casterIndex = 0u;
         casterIndex < storage.casters.size(); ++casterIndex) {
      const War3PointShadowCpuCaster& caster =
          storage.casters[casterIndex];
      if (caster.positionBufferIdentity == 0u ||
          caster.positionBufferSize == 0u)
        continue;
      if (caster.indexed &&
          (caster.indexBufferIdentity == 0u ||
           caster.indexBufferSize == 0u))
        continue;
      if (!CasterInLightRange(caster, lightPlan.lightPositionRange,
                              cullPadding)) {
        continue;
      }
      rangeCandidates.push_back(casterIndex);
    }

    uint8_t updateMask = 0u;
    if (maxFacesPerFrame >= kWar3PointShadowCpuPlanFaceCount) {
      updateMask = 0x3fu;
    } else {
      for (uint32_t pick = 0u; pick < maxFacesPerFrame; ++pick) {
        int32_t bestFace = -1;
        uint64_t bestAge = 0u;
        for (uint32_t face = 0u; face < kWar3PointShadowCpuPlanFaceCount;
             ++face) {
          if ((updateMask & uint8_t(1u << face)) != 0u)
            continue;
          const bool valid = !result.forceFullFaceUpdate &&
              (history.faceValidMask[lightIndex] & uint8_t(1u << face)) != 0u;
          const uint64_t age = uint64_t(std::min(
                                   history.faceAge[lightIndex][face],
                                   kWar3PointShadowCpuPlanMaxFaceAge)) +
              (valid ? 0ull : 1000ull);
          if (bestFace < 0 || age > bestAge) {
            bestFace = static_cast<int32_t>(face);
            bestAge = age;
          }
        }
        if (bestFace < 0)
          break;
        updateMask |= uint8_t(1u << uint32_t(bestFace));
      }
    }
    result.updateMask[lightIndex] = updateMask;

    for (uint32_t face = 0u; face < kWar3PointShadowCpuPlanFaceCount;
         ++face) {
      const size_t facePlanIndex =
          size_t(lightIndex) * kWar3PointShadowCpuPlanFaceCount + face;
      std::vector<uint32_t>& output = storage.faceCasters[facePlanIndex];
      if ((updateMask & uint8_t(1u << face)) == 0u)
        continue;

      if (settings.pointShadowMaxCastersPerFace == 0u) {
        output.reserve(rangeCandidates.size());
        for (uint32_t casterIndex : rangeCandidates) {
          const War3PointShadowCpuCaster& caster =
              storage.casters[casterIndex];
          if (CasterOnFace(caster, lightPlan.lightPositionRange,
                           kFaceParameters[face],
                           settings.pointShadowFaceCulling)) {
            output.push_back(casterIndex);
          }
        }
        const uint32_t keptCount = ClampCount(output.size());
        result.faceCandidateCount[facePlanIndex] = keptCount;
        result.faceKeptCount[facePlanIndex] = keptCount;
        continue;
      }

      faceCandidates.clear();
      for (uint32_t casterIndex : rangeCandidates) {
        const War3PointShadowCpuCaster& caster =
            storage.casters[casterIndex];
        if (!CasterOnFace(caster, lightPlan.lightPositionRange,
                          kFaceParameters[face],
                          settings.pointShadowFaceCulling)) {
          continue;
        }
        const bool unknownBounds = !HasKnownBounds(caster);
        float surfaceDistanceSquared = 0.0f;
        if (!unknownBounds) {
          const float deltaX =
              caster.boundsCenter.x - lightPlan.lightPositionRange.x;
          const float deltaY =
              caster.boundsCenter.y - lightPlan.lightPositionRange.y;
          const float deltaZ =
              caster.boundsCenter.z - lightPlan.lightPositionRange.z;
          const float centerDistance = std::sqrt(
              deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
          const float surfaceDistance =
              std::max(centerDistance - caster.boundsRadius, 0.0f);
          surfaceDistanceSquared = surfaceDistance * surfaceDistance;
        }
        faceCandidates.push_back(
            {casterIndex, surfaceDistanceSquared, unknownBounds});
      }

      const uint32_t candidateCount = ClampCount(faceCandidates.size());
      result.faceCandidateCount[facePlanIndex] = candidateCount;
      const auto knownBegin = std::partition(
          faceCandidates.begin(), faceCandidates.end(),
          [](const War3PointShadowCpuRankedCandidate& candidate) {
            return candidate.pinned;
          });
      const size_t pinnedCount =
          size_t(knownBegin - faceCandidates.begin());
      const size_t knownCount = faceCandidates.size() - pinnedCount;
      if (knownCount > settings.pointShadowMaxCastersPerFace) {
        const auto keepEnd =
            knownBegin + settings.pointShadowMaxCastersPerFace;
        std::nth_element(
            knownBegin, keepEnd, faceCandidates.end(),
            [](const War3PointShadowCpuRankedCandidate& lhs,
               const War3PointShadowCpuRankedCandidate& rhs) {
              if (lhs.surfaceDistanceSquared != rhs.surfaceDistanceSquared) {
                return lhs.surfaceDistanceSquared < rhs.surfaceDistanceSquared;
              }
              return lhs.index < rhs.index;
            });
        faceCandidates.resize(
            pinnedCount + settings.pointShadowMaxCastersPerFace);
      }
      std::sort(faceCandidates.begin(), faceCandidates.end(),
                [](const War3PointShadowCpuRankedCandidate& lhs,
                   const War3PointShadowCpuRankedCandidate& rhs) {
                  return lhs.index < rhs.index;
                });
      const uint32_t keptCount = ClampCount(faceCandidates.size());
      result.faceKeptCount[facePlanIndex] = keptCount;
      result.faceDroppedCount[facePlanIndex] =
          candidateCount - std::min(candidateCount, keptCount);
      output.reserve(faceCandidates.size());
      for (const War3PointShadowCpuRankedCandidate& candidate : faceCandidates)
        output.push_back(candidate.index);
    }
  }

  result.storage = std::move(storage);
  return result;
}

War3PointShadowCpuPlanResultPayload War3PointShadowCpuPlanner::operator()(
    War3PointShadowCpuPlanRequest&& request) const {
  return War3BuildPointShadowCpuPlan(std::move(request));
}

} // namespace dxvk::war3::render
