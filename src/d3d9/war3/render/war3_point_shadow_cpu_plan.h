#pragma once

#include "war3_point_shadow_prepare_worker.h"

#include <array>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace dxvk::war3::render {

// This module is an isolated value-algorithm foundation. The live shadow pass
// neither includes nor instantiates it, and the result is only a suggestion:
// a future renderer owner must revalidate the complete generation tuple before
// publishing resources or recording any draw.
inline constexpr bool kWar3PointShadowCpuPlanRuntimeIntegrated = false;
inline constexpr bool kWar3PointShadowCpuPlanOwnerBuilderIntegrated = false;
// Rejected submissions retain caller ownership. Processor exceptions and
// cancellation can still destroy an accepted request after ownership crossed
// the worker boundary, so that separate recovery contract remains a blocker.
inline constexpr bool
    kWar3PointShadowCpuPlanRejectedSubmitStorageRecoveryIntegrated = true;
inline constexpr bool
    kWar3PointShadowCpuPlanFailedJobStorageRecoveryIntegrated = false;
inline constexpr bool kWar3PointShadowCpuPlanMayPublishRendererState = false;
inline constexpr bool kWar3PointShadowCpuPlanMayOwnGpuResources = false;
inline constexpr uint32_t kWar3PointShadowCpuPlanMaxLights = 4u;
inline constexpr uint32_t kWar3PointShadowCpuPlanFaceCount = 6u;
inline constexpr uint32_t kWar3PointShadowCpuPlanMaxFaceAge = 100000u;
inline constexpr uint64_t kWar3PointShadowCpuPlanResourceBudgetBytes =
    96ull * 1024ull * 1024ull;

struct War3PointShadowCpuFloat4 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 0.0f;
};

// Four vectors in the same storage convention as dxvk::Matrix4. Keeping a
// private value representation prevents the worker request/result from owning
// or reaching a renderer-side matrix object.
struct War3PointShadowCpuMatrix4 {
  float vectors[4][4] = {};
};

struct War3PointShadowCpuPlanSettings {
  bool pointLightsEnabled = false;
  bool pointShadowEnabled = false;
  bool pointShadowFaceCulling = true;
  bool pointShadowTemporalReuse = false;
  bool alphaShadowHashed = false;
  uint32_t pointShadowResolution = 1024u;
  uint32_t pointShadowMaxLights = 4u;
  uint32_t pointShadowMaxFacesPerFrame = 6u;
  uint32_t pointShadowMaxCastersPerFace = 0u;
  uint32_t pointShadowUpdatePeriod = 1u;
  float pointShadowCasterCullPadding = 1.2f;
};

struct War3PointShadowCpuLight {
  War3PointShadowCpuFloat4 position = {};
  float shadowIntensity = 0.0f;
  int32_t id = 0;
};

/**
 * \brief Frozen scalar description of one replay draw
 *
 * Identity fields are opaque integer tokens minted by the future owner. They
 * may name a backing/allocation for signature parity, but are never converted
 * back to pointers and never grant ownership. Every field used by the current
 * incremental-content signature is represented here as a plain value.
 */
struct War3PointShadowCpuCaster {
  War3PointShadowCpuFloat4 boundsCenter = {};
  float boundsRadius = 0.0f;
  War3PointShadowCpuMatrix4 worldMatrix = {};

  uint32_t vertexCount = 0u;
  uint32_t indexCount = 0u;
  uint32_t positionFormat = 0u;
  uint32_t positionStride = 0u;
  uint32_t positionOffset = 0u;
  uint32_t indexType = 0u;
  uint32_t vertexBlendCount = 0u;
  uint32_t paletteIndex = 0u;
  uint32_t blendWeightFormat = 0u;
  uint32_t blendWeightOffset = 0u;
  uint32_t blendIndexFormat = 0u;
  uint32_t blendIndexOffset = 0u;
  uint32_t blendBinding = 0u;
  uint32_t blendStride = 0u;
  uint32_t topology = 0u;
  uint32_t firstIndex = 0u;
  int32_t vertexOffset = 0;
  uint32_t firstVertex = 0u;
  uint32_t minVertexIndex = 0u;
  uint32_t numVertices = 0u;

  uint64_t positionStorageIdentity = 0u;
  uint64_t positionBufferIdentity = 0u;
  uint64_t positionBufferOffset = 0u;
  uint64_t positionBufferSize = 0u;
  uint64_t indexStorageIdentity = 0u;
  uint64_t indexBufferIdentity = 0u;
  uint64_t indexBufferOffset = 0u;
  uint64_t indexBufferSize = 0u;
  uint64_t blendStorageIdentity = 0u;
  uint64_t blendBufferIdentity = 0u;
  uint64_t blendBufferOffset = 0u;
  uint64_t blendBufferSize = 0u;

  float alphaRef = 0.5f;
  uint32_t uvFormat = 0u;
  uint32_t uvStride = 0u;
  uint32_t uvOffset = 0u;
  uint32_t uvBinding = 0u;
  uint64_t uvStorageIdentity = 0u;
  uint64_t uvBufferIdentity = 0u;
  uint64_t uvBufferOffset = 0u;
  uint64_t uvBufferSize = 0u;
  uint64_t diffuseTextureIdentity = 0u;
  uint64_t diffuseSamplerIdentity = 0u;
  uint32_t diffuseSamplerIndex = 0u;

  bool indexed = true;
  bool vertexBlendEnabled = false;
  bool vertexBlendIndexed = false;
  bool alphaTestEnabled = false;
  bool alphaBlendEnabled = false;
  // Owner-builder completeness declaration. As a public POD bool this is not
  // authority by itself: future runtime admission must allow only the single
  // renderer owner to mint it from a sealed replay row and must revalidate the
  // exact generation tuple before publication. It covers every scalar
  // identity, draw parameter, bounds value and all 16 world-matrix lanes above,
  // but is deliberately not mixed into the legacy content signature.
  bool frozenComplete = false;
};

struct War3PointShadowCpuHistory {
  bool cubeAllocated = false;
  uint32_t readyLightCount = 0u;
  uint64_t publishedContentSignature = 0u;
  uint32_t temporalAge = 0u;
  std::array<uint8_t, kWar3PointShadowCpuPlanMaxLights> faceValidMask = {};
  std::array<std::array<uint32_t, kWar3PointShadowCpuPlanFaceCount>,
             kWar3PointShadowCpuPlanMaxLights>
      faceAge = {};
};

// Exact non-worker portion of the owner validation tuple. The worker envelope
// already binds job/renderer/frame/light generation; these values additionally
// bind the sealed replay, point-shadow policy and lifecycle snapshot consumed
// to build the owned request.
struct War3PointShadowCpuPlanSealTuple {
  uint64_t replayGeneration = 0u;
  uint64_t policyRevision = 0u;
  uint64_t lifecycleGeneration = 0u;

  constexpr bool valid() const noexcept {
    return replayGeneration != 0u && policyRevision != 0u &&
        lifecycleGeneration != 0u;
  }
};

constexpr bool operator==(const War3PointShadowCpuPlanSealTuple& lhs,
                          const War3PointShadowCpuPlanSealTuple& rhs) noexcept {
  return lhs.replayGeneration == rhs.replayGeneration &&
      lhs.policyRevision == rhs.policyRevision &&
      lhs.lifecycleGeneration == rhs.lifecycleGeneration;
}

constexpr bool operator!=(const War3PointShadowCpuPlanSealTuple& lhs,
                          const War3PointShadowCpuPlanSealTuple& rhs) noexcept {
  return !(lhs == rhs);
}

struct War3PointShadowCpuRankedCandidate {
  uint32_t index = 0u;
  float surfaceDistanceSquared = 0.0f;
  bool pinned = false;
};

/**
 * \brief Owned buffers transferred request -> planner -> result
 *
 * The owner may move this bundle back into the next request after consuming
 * the plan. clear() is used instead of replacement, so caster/palette input,
 * range/ranking scratch, and all 24 face lists retain their allocations across
 * persistent-worker jobs without sharing mutable storage between threads.
 */
struct War3PointShadowCpuPlanOwnedStorage {
  std::vector<uint64_t> paletteHashes;
  std::vector<War3PointShadowCpuCaster> casters;
  std::vector<uint32_t> rangeCandidateIndices;
  std::vector<War3PointShadowCpuRankedCandidate> rankedCandidates;
  std::array<std::vector<uint32_t>,
             kWar3PointShadowCpuPlanMaxLights *
                 kWar3PointShadowCpuPlanFaceCount>
      faceCasters = {};
};

struct War3PointShadowCpuPlanRequestPayload {
  War3PointShadowCpuPlanSettings settings = {};
  War3PointShadowCpuPlanSealTuple seal = {};
  bool hasAnyLight = false;
  uint32_t shadowLightCount = 0u;
  uint64_t dynamicPoseSignature = 0u;
  uint32_t dynamicPoseCount = 0u;
  uint32_t dynamicSkinnedOutputCount = 0u;
  std::array<War3PointShadowCpuLight,
             kWar3PointShadowCpuPlanMaxLights>
      lights = {};
  War3PointShadowCpuHistory history = {};
  War3PointShadowCpuPlanOwnedStorage storage = {};
};

enum class War3PointShadowCpuPlanDisposition : uint8_t {
  InvalidRequest = 0u,
  Disabled,
  NoLights,
  NoCasters,
  ReusePublished,
  Render,
};

enum class War3PointShadowCpuPlanRejectReason : uint8_t {
  None = 0u,
  InvalidGeneration,
  InvalidSealTuple,
  NonFiniteLight,
  IncompleteCasterFreeze,
  TooManyCasters,
  Disabled,
  NoLights,
  NoCasters,
};

struct War3PointShadowCpuLightPlan {
  War3PointShadowCpuFloat4 lightPositionRange = {};
  float shadowIntensity = 0.0f;
  std::array<War3PointShadowCpuMatrix4,
             kWar3PointShadowCpuPlanFaceCount>
      faceViewProjection = {};
};

struct War3PointShadowCpuPlanResultPayload {
  War3PointShadowCpuPlanDisposition disposition =
      War3PointShadowCpuPlanDisposition::InvalidRequest;
  War3PointShadowCpuPlanRejectReason rejectReason =
      War3PointShadowCpuPlanRejectReason::InvalidGeneration;
  bool shouldRender = false;
  bool reusePublished = false;
  bool forceFullFaceUpdate = false;
  // A proposal only: when true, the renderer owner must clear the active
  // lights' valid masks in its publication transaction before recording. The
  // planner never mutates history and never claims a successful face commit.
  bool ownerMustClearFaceValidityBeforeRecord = false;
  bool ownerMustInvalidatePublication = true;
  uint32_t shadowLightCount = 0u;
  uint32_t resourceCapacityLights = 1u;
  uint32_t maxFacesPerFrame = 6u;
  uint32_t resolution = 1024u;
  uint32_t maxCastersPerFace = 0u;
  uint32_t nextTemporalAge = 0u;
  uint32_t unknownBoundsPinnedCount = 0u;
  uint32_t nonFiniteBoundsPinnedCount = 0u;
  uint64_t contentSignature = 0u;
  War3PointShadowCpuPlanSealTuple seal = {};
  std::array<uint8_t, kWar3PointShadowCpuPlanMaxLights> updateMask = {};
  std::array<uint32_t,
             kWar3PointShadowCpuPlanMaxLights *
                 kWar3PointShadowCpuPlanFaceCount>
      faceCandidateCount = {};
  std::array<uint32_t,
             kWar3PointShadowCpuPlanMaxLights *
                 kWar3PointShadowCpuPlanFaceCount>
      faceKeptCount = {};
  std::array<uint32_t,
             kWar3PointShadowCpuPlanMaxLights *
                 kWar3PointShadowCpuPlanFaceCount>
      faceDroppedCount = {};
  std::array<War3PointShadowCpuLightPlan,
             kWar3PointShadowCpuPlanMaxLights>
      lights = {};
  War3PointShadowCpuPlanOwnedStorage storage = {};
};

using War3PointShadowCpuPlanRequest =
    War3PointShadowPrepareRequest<War3PointShadowCpuPlanRequestPayload>;

uint32_t War3ResolvePointShadowCpuPlanCapacity(uint32_t resolution,
                                               uint32_t requestedLights);

War3PointShadowCpuPlanResultPayload War3BuildPointShadowCpuPlan(
    War3PointShadowCpuPlanRequest&& request);

struct War3PointShadowCpuPlanner final {
  War3PointShadowCpuPlanResultPayload operator()(
      War3PointShadowCpuPlanRequest&& request) const;
};

static_assert(std::is_standard_layout_v<War3PointShadowCpuFloat4>);
static_assert(std::is_trivially_copyable_v<War3PointShadowCpuFloat4>);
static_assert(std::is_standard_layout_v<War3PointShadowCpuMatrix4>);
static_assert(std::is_trivially_copyable_v<War3PointShadowCpuMatrix4>);
static_assert(std::is_standard_layout_v<War3PointShadowCpuPlanSettings>);
static_assert(std::is_trivially_copyable_v<War3PointShadowCpuPlanSettings>);
static_assert(std::is_standard_layout_v<War3PointShadowCpuLight>);
static_assert(std::is_trivially_copyable_v<War3PointShadowCpuLight>);
static_assert(std::is_standard_layout_v<War3PointShadowCpuCaster>);
static_assert(std::is_trivially_copyable_v<War3PointShadowCpuCaster>);
static_assert(std::is_standard_layout_v<War3PointShadowCpuHistory>);
static_assert(std::is_trivially_copyable_v<War3PointShadowCpuHistory>);
static_assert(std::is_standard_layout_v<War3PointShadowCpuPlanSealTuple>);
static_assert(std::is_trivially_copyable_v<War3PointShadowCpuPlanSealTuple>);
static_assert(std::is_standard_layout_v<War3PointShadowCpuRankedCandidate>);
static_assert(std::is_trivially_copyable_v<
              War3PointShadowCpuRankedCandidate>);
static_assert(std::is_empty_v<War3PointShadowCpuPlanner>);
static_assert(std::is_nothrow_move_constructible_v<
              War3PointShadowCpuPlanRequestPayload>);
static_assert(std::is_nothrow_move_constructible_v<
              War3PointShadowCpuPlanResultPayload>);

} // namespace dxvk::war3::render
