#include "../war3_point_shadow_cpu_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace {

using namespace dxvk::war3::render;

int g_failures = 0;

#define CHECK(expression)                                                       \
  do {                                                                          \
    if (!(expression)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,  \
                   #expression);                                                \
      ++g_failures;                                                             \
    }                                                                           \
  } while (false)

War3PointShadowPrepareGenerationTuple Generation(
    uint64_t job = 1u, uint64_t renderer = 2u, uint64_t frame = 3u,
    uint64_t light = 4u) {
  return {job, renderer, frame, light};
}

War3PointShadowCpuMatrix4 IdentityMatrix() {
  War3PointShadowCpuMatrix4 result = {};
  for (uint32_t lane = 0u; lane < 4u; ++lane)
    result.vectors[lane][lane] = 1.0f;
  return result;
}

War3PointShadowCpuCaster Caster(float x, float y, float z, float radius) {
  War3PointShadowCpuCaster result = {};
  result.boundsCenter = {x, y, z, 0.0f};
  result.boundsRadius = radius;
  result.worldMatrix = IdentityMatrix();
  result.vertexCount = 3u;
  result.indexCount = 3u;
  result.indexed = true;
  result.positionBufferIdentity = 0x100u;
  result.positionBufferSize = 96u;
  result.indexBufferIdentity = 0x200u;
  result.indexBufferSize = 6u;
  result.frozenComplete = true;
  return result;
}

War3PointShadowCpuPlanRequest BaseRequest(uint64_t job = 1u) {
  War3PointShadowCpuPlanRequest request = {};
  request.generation = Generation(job, 2u, job + 10u, 4u);
  request.payload.seal = {job + 100u, job + 200u, job + 300u};
  request.payload.settings.pointLightsEnabled = true;
  request.payload.settings.pointShadowEnabled = true;
  request.payload.settings.pointShadowFaceCulling = true;
  request.payload.settings.pointShadowResolution = 1024u;
  request.payload.settings.pointShadowMaxLights = 4u;
  request.payload.settings.pointShadowMaxFacesPerFrame = 6u;
  request.payload.settings.pointShadowCasterCullPadding = 1.2f;
  request.payload.hasAnyLight = true;
  request.payload.shadowLightCount = 1u;
  request.payload.lights[0].id = 7;
  request.payload.lights[0].position = {10.0f, -20.0f, 30.0f, 512.0f};
  request.payload.lights[0].shadowIntensity = 0.75f;
  request.payload.storage.casters.push_back(
      Caster(10.0f, -20.0f, 30.0f, 1.0f));
  return request;
}

War3PointShadowCpuPlanResultPayload Build(
    War3PointShadowCpuPlanRequest request) {
  return War3BuildPointShadowCpuPlan(std::move(request));
}

bool MatrixIsZero(const War3PointShadowCpuMatrix4& matrix) {
  for (const auto& vector : matrix.vectors) {
    for (float value : vector) {
      if (value != 0.0f)
        return false;
    }
  }
  return true;
}

void TestRuntimeBoundaryAndCapacity() {
  static_assert(!kWar3PointShadowCpuPlanRuntimeIntegrated);
  static_assert(
      kWar3PointShadowCpuPlanRejectedSubmitStorageRecoveryIntegrated);
  static_assert(!kWar3PointShadowCpuPlanFailedJobStorageRecoveryIntegrated);
  static_assert(!kWar3PointShadowCpuPlanMayPublishRendererState);
  static_assert(!kWar3PointShadowCpuPlanMayOwnGpuResources);
  CHECK(War3ResolvePointShadowCpuPlanCapacity(127u, 4u) == 4u);
  CHECK(War3ResolvePointShadowCpuPlanCapacity(128u, 99u) == 4u);
  CHECK(War3ResolvePointShadowCpuPlanCapacity(1024u, 4u) == 4u);
  CHECK(War3ResolvePointShadowCpuPlanCapacity(1025u, 4u) == 3u);
  CHECK(War3ResolvePointShadowCpuPlanCapacity(2048u, 4u) == 1u);

  auto fourLights = BaseRequest();
  fourLights.payload.shadowLightCount = 4u;
  for (uint32_t index = 1u; index < 4u; ++index) {
    fourLights.payload.lights[index] = fourLights.payload.lights[0];
    fourLights.payload.lights[index].id += static_cast<int32_t>(index);
  }
  auto result = Build(fourLights);
  CHECK(result.shadowLightCount == 4u);
  for (uint32_t index = 0u; index < 4u; ++index)
    CHECK(result.updateMask[index] == 0x3fu);

  fourLights.payload.settings.pointShadowResolution = 2048u;
  result = Build(fourLights);
  CHECK(result.resourceCapacityLights == 1u);
  CHECK(result.shadowLightCount == 1u);
  CHECK(result.updateMask[0] == 0x3fu);
  CHECK(result.updateMask[1] == 0u);
}

void TestNoWorkAndMalformedInputsFailClosed() {
  auto request = BaseRequest();
  request.generation.jobSerial = 0u;
  auto result = Build(request);
  CHECK(result.disposition ==
        War3PointShadowCpuPlanDisposition::InvalidRequest);
  CHECK(result.ownerMustInvalidatePublication);

  request = BaseRequest();
  request.payload.settings.pointLightsEnabled = false;
  result = Build(request);
  CHECK(result.disposition == War3PointShadowCpuPlanDisposition::Disabled);
  CHECK(result.ownerMustInvalidatePublication);
  CHECK(!result.reusePublished);

  request = BaseRequest();
  request.payload.seal.policyRevision = 0u;
  result = Build(request);
  CHECK(result.rejectReason ==
        War3PointShadowCpuPlanRejectReason::InvalidSealTuple);
  CHECK(result.ownerMustInvalidatePublication);

  request = BaseRequest();
  request.payload.storage.casters.clear();
  result = Build(request);
  CHECK(result.disposition == War3PointShadowCpuPlanDisposition::NoCasters);
  CHECK(result.ownerMustInvalidatePublication);

  request = BaseRequest();
  request.payload.shadowLightCount = 0u;
  result = Build(request);
  CHECK(result.disposition == War3PointShadowCpuPlanDisposition::NoLights);
  CHECK(result.ownerMustInvalidatePublication);

  request = BaseRequest();
  request.payload.lights[0].position.x =
      std::numeric_limits<float>::quiet_NaN();
  result = Build(request);
  CHECK(result.rejectReason ==
        War3PointShadowCpuPlanRejectReason::NonFiniteLight);
  CHECK(result.ownerMustInvalidatePublication);

  request = BaseRequest();
  request.payload.storage.casters[0].frozenComplete = false;
  result = Build(request);
  CHECK(result.rejectReason ==
        War3PointShadowCpuPlanRejectReason::IncompleteCasterFreeze);
  CHECK(result.ownerMustInvalidatePublication);
  CHECK(result.storage.casters.size() == 1u);
}

void TestLegacySignatureOracles() {
  auto request = BaseRequest();
  request.generation.lightGeneration = 0x0102030405060708ull;
  auto result = Build(request);
  CHECK(result.contentSignature == 0xb5d6942cfc00ffaeull);

  request = BaseRequest();
  request.generation.lightGeneration = 0x1234u;
  request.payload.settings.pointShadowMaxFacesPerFrame = 2u;
  request.payload.settings.pointShadowMaxCastersPerFace = 2u;
  request.payload.settings.pointShadowCasterCullPadding = 1.25f;
  request.payload.settings.alphaShadowHashed = true;
  request.payload.lights[0].id = -7;
  request.payload.lights[0].position = {1.0f, -2.0f, 3.0f, 64.0f};
  request.payload.lights[0].shadowIntensity = 0.5f;
  request.payload.dynamicPoseSignature = 0x99u;
  request.payload.storage.paletteHashes = {0x1111u, 0x2222u};
  War3PointShadowCpuCaster& caster = request.payload.storage.casters[0];
  caster.boundsCenter = {4.0f, 5.0f, 6.0f, 0.0f};
  caster.boundsRadius = 2.0f;
  caster.vertexCount = 11u;
  caster.indexCount = 12u;
  caster.indexed = true;
  caster.positionFormat = 106u;
  caster.positionStride = 32u;
  caster.positionOffset = 4u;
  caster.indexType = 0u;
  caster.topology = 3u;
  caster.firstIndex = 2u;
  caster.vertexOffset = -3;
  caster.firstVertex = 7u;
  caster.minVertexIndex = 8u;
  caster.numVertices = 9u;
  caster.positionStorageIdentity = 0x1000u;
  caster.positionBufferIdentity = 0x2000u;
  caster.positionBufferOffset = 16u;
  caster.positionBufferSize = 320u;
  caster.indexStorageIdentity = 0x3000u;
  caster.indexBufferIdentity = 0x4000u;
  caster.indexBufferOffset = 8u;
  caster.indexBufferSize = 64u;
  caster.blendStorageIdentity = 0x5000u;
  caster.blendBufferIdentity = 0x6000u;
  caster.blendBufferOffset = 4u;
  caster.blendBufferSize = 128u;
  caster.alphaTestEnabled = true;
  caster.alphaBlendEnabled = false;
  caster.alphaRef = 0.4f;
  caster.uvFormat = 103u;
  caster.uvStride = 32u;
  caster.uvOffset = 24u;
  caster.uvBinding = 0u;
  caster.uvStorageIdentity = 0x7000u;
  caster.uvBufferIdentity = 0x8000u;
  caster.uvBufferOffset = 12u;
  caster.uvBufferSize = 256u;
  caster.diffuseTextureIdentity = 0x9000u;
  caster.diffuseSamplerIdentity = 0xa000u;
  caster.diffuseSamplerIndex = 5u;
  caster.worldMatrix = IdentityMatrix();
  caster.worldMatrix.vectors[3][0] = 10.0f;
  caster.worldMatrix.vectors[3][1] = 20.0f;
  caster.worldMatrix.vectors[3][2] = 30.0f;
  result = Build(request);
  CHECK(result.contentSignature == 0xff6737aa6748ed15ull);

  const uint64_t incrementalSignature = result.contentSignature;
  auto allFields = request;
  War3PointShadowCpuCaster& allFieldsCaster =
      allFields.payload.storage.casters[0];
  allFieldsCaster.indexType = 1u;
  allFieldsCaster.vertexBlendEnabled = true;
  allFieldsCaster.vertexBlendIndexed = true;
  allFieldsCaster.vertexBlendCount = 3u;
  allFieldsCaster.paletteIndex = 13u;
  allFieldsCaster.blendWeightFormat = 102u;
  allFieldsCaster.blendWeightOffset = 20u;
  allFieldsCaster.blendIndexFormat = 98u;
  allFieldsCaster.blendIndexOffset = 24u;
  allFieldsCaster.blendBinding = 1u;
  allFieldsCaster.blendStride = 32u;
  allFieldsCaster.alphaBlendEnabled = true;
  allFieldsCaster.uvBinding = 2u;
  float matrixLane = 1.0f;
  for (auto& vector : allFieldsCaster.worldMatrix.vectors) {
    for (float& value : vector)
      value = matrixLane++;
  }
  CHECK(Build(allFields).contentSignature == 0xd3199c952ea5844cull);

  request.payload.storage.casters[0].uvBufferOffset += 1u;
  CHECK(Build(request).contentSignature != incrementalSignature);

  auto fullQuality = BaseRequest();
  const uint64_t fullSignature = Build(fullQuality).contentSignature;
  fullQuality.payload.storage.casters[0].boundsCenter.x += 123.0f;
  fullQuality.payload.storage.paletteHashes.push_back(0xdeadbeefu);
  fullQuality.payload.dynamicPoseSignature = 77u;
  fullQuality.payload.settings.alphaShadowHashed = true;
  CHECK(Build(fullQuality).contentSignature == fullSignature);
  fullQuality.payload.settings.pointShadowMaxFacesPerFrame = 5u;
  CHECK(Build(fullQuality).contentSignature != fullSignature);
}

War3PointShadowCpuPlanRequest StableRequest(uint32_t maxFaces = 3u) {
  auto request = BaseRequest();
  request.payload.settings.pointShadowMaxFacesPerFrame = maxFaces;
  auto first = Build(request);
  request.payload.history.cubeAllocated = true;
  request.payload.history.readyLightCount = 1u;
  request.payload.history.publishedContentSignature = first.contentSignature;
  request.payload.history.faceValidMask[0] = 0x3fu;
  return request;
}

void TestTemporalCadenceAndDynamicForcesFullUpdate() {
  auto request = BaseRequest();
  request.payload.settings.pointShadowTemporalReuse = true;
  request.payload.settings.pointShadowUpdatePeriod = 3u;
  auto initial = Build(request);
  CHECK(initial.forceFullFaceUpdate);
  CHECK(initial.ownerMustClearFaceValidityBeforeRecord);

  request.payload.history.cubeAllocated = true;
  request.payload.history.readyLightCount = 1u;
  request.payload.history.publishedContentSignature = initial.contentSignature;
  request.payload.history.temporalAge = 0u;
  auto result = Build(request);
  CHECK(result.disposition ==
        War3PointShadowCpuPlanDisposition::ReusePublished);
  CHECK(result.nextTemporalAge == 1u);
  CHECK(MatrixIsZero(result.lights[0].faceViewProjection[0]));
  CHECK(result.storage.faceCasters[0].empty());

  request.payload.history.temporalAge = 1u;
  result = Build(request);
  CHECK(result.reusePublished);
  CHECK(result.nextTemporalAge == 2u);

  request.payload.history.temporalAge = 2u;
  result = Build(request);
  CHECK(result.shouldRender);
  CHECK(!result.reusePublished);
  CHECK(result.nextTemporalAge == 0u);

  auto periodOne = request;
  periodOne.payload.settings.pointShadowUpdatePeriod = 1u;
  const auto periodOneNamed = Build(periodOne);
  periodOne.payload.history.publishedContentSignature =
      periodOneNamed.contentSignature;
  periodOne.payload.history.temporalAge = 0u;
  result = Build(periodOne);
  CHECK(result.shouldRender);
  CHECK(!result.reusePublished);

  for (uint32_t dynamicCase = 0u; dynamicCase < 3u; ++dynamicCase) {
    auto dynamicRequest = request;
    dynamicRequest.payload.history.temporalAge = 0u;
    if (dynamicCase == 0u)
      dynamicRequest.payload.dynamicPoseCount = 1u;
    if (dynamicCase == 1u)
      dynamicRequest.payload.dynamicSkinnedOutputCount = 1u;
    if (dynamicCase == 2u)
      dynamicRequest.payload.storage.casters[0].vertexBlendEnabled = true;
    const auto named = Build(dynamicRequest);
    dynamicRequest.payload.history.publishedContentSignature =
        named.contentSignature;
    const auto dynamic = Build(dynamicRequest);
    CHECK(dynamic.shouldRender);
    CHECK(dynamic.forceFullFaceUpdate);
    CHECK(dynamic.maxFacesPerFrame == 6u);
    CHECK(dynamic.updateMask[0] == 0x3fu);
  }
}

void TestFaceAgeBudgetIsPerLightAndMatchesLegacyScoring() {
  auto request = StableRequest(3u);
  request.payload.history.faceAge[0] = {4u, 9u, 9u, 0u, 2u, 8u};
  auto result = Build(request);
  CHECK(!result.forceFullFaceUpdate);
  CHECK(result.updateMask[0] == 0x26u);

  request.payload.history.faceValidMask[0] = 0u;
  request.payload.history.faceAge[0].fill(0u);
  request.payload.settings.pointShadowMaxFacesPerFrame = 2u;
  result = Build(request);
  CHECK(result.updateMask[0] == 0x03u);

  request.payload.settings.pointShadowMaxFacesPerFrame = 1u;
  request.payload.history.faceValidMask[0] = 0x01u;
  request.payload.history.faceAge[0] = {1001u, 0u, 0u, 0u, 0u, 0u};
  result = Build(request);
  CHECK(result.updateMask[0] == 0x01u);

  auto fourLights = BaseRequest();
  fourLights.payload.shadowLightCount = 4u;
  fourLights.payload.settings.pointShadowMaxFacesPerFrame = 1u;
  for (uint32_t index = 1u; index < 4u; ++index) {
    fourLights.payload.lights[index] = fourLights.payload.lights[0];
    fourLights.payload.lights[index].id += static_cast<int32_t>(index);
  }
  auto first = Build(fourLights);
  fourLights.payload.history.cubeAllocated = true;
  fourLights.payload.history.readyLightCount = 4u;
  fourLights.payload.history.publishedContentSignature = first.contentSignature;
  fourLights.payload.history.faceValidMask.fill(0x3fu);
  result = Build(fourLights);
  CHECK(!result.forceFullFaceUpdate);
  for (uint32_t index = 0u; index < 4u; ++index)
    CHECK(result.updateMask[index] == 0x01u);
}

void TestRangeFaceCullingBufferGateAndNearestSurfaceCap() {
  auto request = BaseRequest();
  request.payload.settings.pointShadowMaxCastersPerFace = 1u;
  request.payload.lights[0].position = {0.0f, 0.0f, 0.0f, 10.0f};
  request.payload.storage.casters = {
      Caster(10.0f, 0.0f, 0.0f, 9.0f),
      Caster(3.0f, 0.0f, 0.0f, 1.0f),
      Caster(0.0f, 0.0f, 0.0f, 0.0f),
      Caster(-5.0f, 0.0f, 0.0f, 1.0f),
      Caster(30.0f, 0.0f, 0.0f, 1.0f),
  };
  auto result = Build(request);
  CHECK(result.faceCandidateCount[0] == 3u);
  CHECK(result.faceKeptCount[0] == 2u);
  CHECK(result.faceDroppedCount[0] == 1u);
  CHECK((result.storage.faceCasters[0] ==
         std::vector<uint32_t>{0u, 2u}));
  CHECK((result.storage.faceCasters[1] ==
         std::vector<uint32_t>{2u, 3u}));

  request.payload.settings.pointShadowMaxCastersPerFace = 0u;
  request.payload.settings.pointShadowFaceCulling = false;
  request.payload.settings.pointShadowCasterCullPadding = 1.0f;
  request.payload.storage.casters = {
      Caster(11.0f, 0.0f, 0.0f, 1.0f),
      Caster(std::nextafter(11.0f, 12.0f), 0.0f, 0.0f, 1.0f),
      Caster(0.0f, 0.0f, 0.0f, 1.0f),
      Caster(0.0f, 0.0f, 0.0f, 1.0f),
  };
  request.payload.storage.casters[2].positionBufferIdentity = 0u;
  request.payload.storage.casters[3].indexed = false;
  request.payload.storage.casters[3].indexBufferIdentity = 0u;
  request.payload.storage.casters[3].indexBufferSize = 0u;
  result = Build(request);
  for (uint32_t face = 0u; face < 6u; ++face)
    CHECK((result.storage.faceCasters[face] ==
           std::vector<uint32_t>{0u, 3u}));
}

void TestNonFiniteBoundsArePinnedHardeningDelta() {
  auto request = BaseRequest();
  request.payload.settings.pointShadowMaxCastersPerFace = 1u;
  request.payload.lights[0].position = {0.0f, 0.0f, 0.0f, 10.0f};
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  request.payload.storage.casters = {
      Caster(nan, 0.0f, 0.0f, 1.0f),
      Caster(0.0f, infinity, 0.0f, 1.0f),
      Caster(0.0f, 0.0f, 0.0f, infinity),
      Caster(2.0f, 0.0f, 0.0f, 1.0f),
      Caster(4.0f, 0.0f, 0.0f, 1.0f),
  };
  const auto result = Build(request);
  CHECK(result.unknownBoundsPinnedCount == 3u);
  CHECK(result.nonFiniteBoundsPinnedCount == 3u);
  CHECK(result.faceCandidateCount[0] == 5u);
  CHECK(result.faceKeptCount[0] == 4u);
  CHECK(result.faceDroppedCount[0] == 1u);
  CHECK((result.storage.faceCasters[0] ==
         std::vector<uint32_t>{0u, 1u, 2u, 3u}));
}

void TestLargeCappedCohortKeepsOnlyNearestKnownSubset() {
  auto request = BaseRequest();
  request.payload.settings.pointShadowFaceCulling = false;
  request.payload.settings.pointShadowCasterCullPadding = 1.0f;
  request.payload.settings.pointShadowMaxCastersPerFace = 16u;
  request.payload.lights[0].position = {0.0f, 0.0f, 0.0f, 10000.0f};
  request.payload.storage.casters.clear();
  request.payload.storage.casters.reserve(4096u);
  for (uint32_t index = 0u; index < 4096u; ++index) {
    request.payload.storage.casters.push_back(
        Caster(float(index + 1u), 0.0f, 0.0f, 0.25f));
  }
  const auto result = Build(request);
  for (uint32_t face = 0u; face < 6u; ++face) {
    CHECK(result.faceCandidateCount[face] == 4096u);
    CHECK(result.faceKeptCount[face] == 16u);
    CHECK(result.faceDroppedCount[face] == 4080u);
    CHECK(result.storage.faceCasters[face].size() == 16u);
    for (uint32_t kept = 0u; kept < 16u; ++kept)
      CHECK(result.storage.faceCasters[face][kept] == kept);
  }
}

void TransformPoint(const War3PointShadowCpuMatrix4& matrix,
                    float x, float y, float z, float (&clip)[4]) {
  const float point[4] = {x, y, z, 1.0f};
  for (uint32_t component = 0u; component < 4u; ++component) {
    clip[component] = 0.0f;
    for (uint32_t vector = 0u; vector < 4u; ++vector)
      clip[component] += point[vector] * matrix.vectors[vector][component];
  }
}

void TestCubeMatricesUseTranslatedViewAndClipXFlip() {
  auto request = BaseRequest();
  request.payload.lights[0].position = {17.0f, -9.0f, 31.0f, 128.0f};
  const auto result = Build(request);
  CHECK(result.shouldRender);
  constexpr float directions[6][3] = {
      {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
      {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
  };
  constexpr float rights[6][3] = {
      {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
      {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
      {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
  };
  constexpr float ups[6][3] = {
      {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
      {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
      {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
  };
  constexpr float eye[3] = {17.0f, -9.0f, 31.0f};
  for (uint32_t face = 0u; face < 6u; ++face) {
    const auto& matrix = result.lights[0].faceViewProjection[face];
    for (const auto& vector : matrix.vectors) {
      for (float value : vector)
        CHECK(std::isfinite(value));
    }
    float centerClip[4] = {};
    TransformPoint(matrix,
                   eye[0] + directions[face][0] * 10.0f,
                   eye[1] + directions[face][1] * 10.0f,
                   eye[2] + directions[face][2] * 10.0f,
                   centerClip);
    CHECK(std::abs(centerClip[0]) < 1.0e-4f);
    CHECK(std::abs(centerClip[1]) < 1.0e-4f);
    CHECK(centerClip[3] > 0.0f);
    CHECK(centerClip[2] / centerClip[3] > 0.0f);
    CHECK(centerClip[2] / centerClip[3] < 1.0f);

    float rightClip[4] = {};
    TransformPoint(matrix,
                   eye[0] + directions[face][0] * 10.0f + rights[face][0],
                   eye[1] + directions[face][1] * 10.0f + rights[face][1],
                   eye[2] + directions[face][2] * 10.0f + rights[face][2],
                   rightClip);
    CHECK(rightClip[0] / rightClip[3] < 0.0f);

    float upClip[4] = {};
    TransformPoint(matrix,
                   eye[0] + directions[face][0] * 10.0f + ups[face][0],
                   eye[1] + directions[face][1] * 10.0f + ups[face][1],
                   eye[2] + directions[face][2] * 10.0f + ups[face][2],
                   upClip);
    CHECK(upClip[1] / upClip[3] > 0.0f);
  }
}

void TestPlannerRunsInsidePersistentOwnedValueWorker() {
  using Worker = War3PointShadowPersistentPrepareWorker<
      War3PointShadowCpuPlanRequestPayload,
      War3PointShadowCpuPlanResultPayload,
      War3PointShadowCpuPlanner>;
  static_assert(std::is_empty_v<War3PointShadowCpuPlanner>);
  Worker worker;
  auto request = BaseRequest(41u);
  const auto expected = request.generation;
  const auto expectedSeal = request.payload.seal;
  CHECK(worker.submit(request) ==
        War3PointShadowPrepareSubmitStatus::Accepted);
  const auto result = worker.waitAndCollectExact(expected);
  CHECK(result.state == War3PointShadowPrepareResultState::Ready);
  CHECK(!result.failClosed());
  CHECK(result.generation == expected);
  CHECK(result.payload.has_value());
  if (result.payload)
    CHECK(result.payload->seal == expectedSeal);
  if (result.payload)
    CHECK(result.payload->shouldRender);
}

void TestOwnedStorageRecyclesAcrossTwoHundredWorkerJobs() {
  using Worker = War3PointShadowPersistentPrepareWorker<
      War3PointShadowCpuPlanRequestPayload,
      War3PointShadowCpuPlanResultPayload,
      War3PointShadowCpuPlanner>;

  auto request = BaseRequest(100u);
  request.payload.settings.pointShadowMaxCastersPerFace = 1u;
  request.payload.storage.casters.reserve(32u);
  request.payload.storage.paletteHashes.reserve(16u);
  request.payload.storage.paletteHashes.push_back(0x1234u);
  request.payload.storage.rangeCandidateIndices.reserve(32u);
  request.payload.storage.rankedCandidates.reserve(32u);
  for (auto& face : request.payload.storage.faceCasters)
    face.reserve(32u);

  const auto settings = request.payload.settings;
  const auto lights = request.payload.lights;
  War3PointShadowCpuPlanOwnedStorage recycled =
      std::move(request.payload.storage);
  const void* casterAllocation = recycled.casters.data();
  const void* paletteAllocation = recycled.paletteHashes.data();
  const void* rangeAllocation = recycled.rangeCandidateIndices.data();
  const void* rankedAllocation = recycled.rankedCandidates.data();
  std::array<const void*,
             kWar3PointShadowCpuPlanMaxLights *
                 kWar3PointShadowCpuPlanFaceCount>
      faceAllocations = {};
  for (size_t face = 0u; face < faceAllocations.size(); ++face)
    faceAllocations[face] = recycled.faceCasters[face].data();

  Worker worker;
  for (uint64_t iteration = 1u; iteration <= 200u; ++iteration) {
    War3PointShadowCpuPlanRequest job = {};
    job.generation =
        Generation(100u + iteration, 2u, 1000u + iteration,
                   2000u + iteration);
    job.payload.settings = settings;
    job.payload.seal = {3000u + iteration, 4000u + iteration,
                        5000u + iteration};
    job.payload.hasAnyLight = true;
    job.payload.shadowLightCount = 1u;
    job.payload.lights = lights;
    job.payload.storage = std::move(recycled);
    const auto expected = job.generation;
    CHECK(worker.submit(job) ==
          War3PointShadowPrepareSubmitStatus::Accepted);
    auto completed = worker.waitAndCollectExact(expected);
    CHECK(completed.state == War3PointShadowPrepareResultState::Ready);
    CHECK(completed.payload.has_value());
    if (!completed.payload)
      return;
    recycled = std::move(completed.payload->storage);
    CHECK(recycled.casters.data() == casterAllocation);
    CHECK(recycled.paletteHashes.data() == paletteAllocation);
    CHECK(recycled.rangeCandidateIndices.data() == rangeAllocation);
    CHECK(recycled.rankedCandidates.data() == rankedAllocation);
    for (size_t face = 0u; face < faceAllocations.size(); ++face)
      CHECK(recycled.faceCasters[face].data() == faceAllocations[face]);
  }
  const auto diagnostics = worker.diagnostics();
  CHECK(diagnostics.threadStarts == 1u);
  CHECK(diagnostics.submittedJobs == 200u);
  CHECK(diagnostics.readyJobs == 200u);
  CHECK(diagnostics.exactCollections == 200u);
}

} // namespace

int main() {
  TestRuntimeBoundaryAndCapacity();
  TestNoWorkAndMalformedInputsFailClosed();
  TestLegacySignatureOracles();
  TestTemporalCadenceAndDynamicForcesFullUpdate();
  TestFaceAgeBudgetIsPerLightAndMatchesLegacyScoring();
  TestRangeFaceCullingBufferGateAndNearestSurfaceCap();
  TestNonFiniteBoundsArePinnedHardeningDelta();
  TestLargeCappedCohortKeepsOnlyNearestKnownSubset();
  TestCubeMatricesUseTranslatedViewAndClipXFlip();
  TestPlannerRunsInsidePersistentOwnedValueWorker();
  TestOwnedStorageRecyclesAcrossTwoHundredWorkerJobs();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d point-shadow CPU plan checks failed\n", g_failures);
    return 1;
  }
  std::puts("war3_point_shadow_cpu_plan_test: PASS");
  return 0;
}
