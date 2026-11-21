#pragma once
// What the host's shadow pass did with one frame (shadow_before_ui.h). The
// numbers are the pass's own account of its Run and the executor's account of
// the frame's ShadowCaster records; war3_shadow_scene_input.h names them.
#include <array>
#include <cstdint>

namespace warvk::host::dxvkcore {

struct ShadowOutcome {
  bool ran = false;
  uint32_t casters = 0, replayDraws = 0, drawnCasters = 0, cascades = 0;
  std::array<uint32_t, 4> cascadeDrawn{};
  uint32_t mapExecuted = 0, receiverExecuted = 0;
  // ReceiverRunEarlyReturnReason, ReceiverInputRejectReason, replay refusals.
  uint32_t earlyReturn = 0, inputReject = 0, replayRejected = 0;
  // The receiver had a complete shadow map; shadow TAA's module and mode.
  uint32_t completeMap = 0, taaModule = 0, taaMode = 0, taaHistory = 0, taaInvalidation = 0, pointShadow = 0;
  uint32_t outline = 0;
  // The volumetric light pass after it (war3_shadow_scene_input.h ShadowRunFacts).
  uint32_t volumetric = 0, volumetricComposite = 0;
  int32_t volumetricBackend = -1;
  uint64_t volumetricBytes = 0;
  uint32_t volumetricFogVolumes = 0, volumetricWeather = 0;
  // Casters the shadow map was prepared to draw alpha-tested.
  uint32_t alphaPrepared = 0;
  // The data layer's facts the pass read, and the storage the pass owns.
  float gameTime = 0.0f;
  uint64_t tombstoneSerial = 0, stagePolicyRevision = 0, resourceBytes = 0;
  // ShadowCaster records of the frame: delivered, and refused by
  // ShadowCasterRefusal.
  uint32_t received = 0, refused = 0;
  std::array<uint32_t, 4> refusals{};
  // Casters taken from dynamic buffers, and the bytes copied for them.
  uint32_t copied = 0;
  uint64_t copiedBytes = 0;

  // ---- frame evidence only (evidence_recorder.h); nothing above reads it ----
  // The frame's casters as the executor copied them: how many were checked,
  // and how many had an index outside the draw's range, a read outside the
  // buffer, a position that is not a number, a position far outside any map,
  // or a triangle edge longer than any model's.
  struct Audit {
    uint32_t checked = 0, indexOutsideRange = 0, outsideBuffer = 0, notNumber = 0, farPosition = 0, longEdge = 0;
    float longestEdge = 0.0f, largestExtent = 0.0f;
    // The first odd casters of the frame, as text.
    std::array<char, 480> odd{};
  } audit;
  // What the pass decided about the directional map of this frame, by its own
  // reconciliation: it kept the previous map (and why), or had none.
  uint32_t reuseMap = 0, holdInvalidCsm = 0, holdEmptyReplay = 0, holdIdentityChurn = 0, csmFallback = 0;
  uint32_t noCompleteMap = 0, replayLastReason = 0, partialPrevented = 0, skippedNoResources = 0;
  // The camera, sun and cascades the receiver drew with: a hash of each, and
  // how far each moved since the frame before (the pass's own units).
  uint64_t cameraHash = 0, sunHash = 0, csmHash = 0;
  uint64_t cameraDelta = 0, sunDelta = 0, csmDelta = 0, snappedCenterDelta = 0, texelSizeDelta = 0;
  uint64_t mapRenderSerial = 0;
};

} // namespace warvk::host::dxvkcore
