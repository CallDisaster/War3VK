#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxvk::war3::render {

enum class War3GpuWorkloadConsumer : uint32_t {
  DirectionalCsm = 0u,
  VolumeSun = 1u,
  PointShadow = 2u,
  Count = 3u,
};

enum class War3GpuWorkloadRejectReason : uint32_t {
  None = 0u,
  InvalidFrame = 1u,
  InvalidRequest = 2u,
  ArithmeticOverflow = 3u,
  DrawBudget = 4u,
  VertexBudget = 5u,
  IndexBudget = 6u,
};

struct War3GpuWorkloadCost {
  uint64_t draws = 0u;
  uint64_t vertices = 0u;
  uint64_t indices = 0u;
  bool valid = true;
};

struct War3GpuWorkloadLimits {
  // These limits admit the historical 4 x 8192 directional replay ceiling,
  // but prevent optional volume/point consumers from multiplying it towards
  // the old 245k-draw algebraic upper bound. Vertex/index limits independently
  // catch a small number of exceptionally large draws.
  uint64_t maxDraws = 32768u;
  uint64_t maxVertices = 64ull * 1024ull * 1024ull;
  uint64_t maxIndices = 192ull * 1024ull * 1024ull;

  constexpr bool valid() const noexcept {
    return maxDraws != 0u && maxVertices != 0u && maxIndices != 0u;
  }
};

struct War3GpuWorkloadConsumerDiagnostics {
  War3GpuWorkloadCost requested = {};
  War3GpuWorkloadCost accepted = {};
  uint64_t requestedItems = 0u;
  uint64_t acceptedItems = 0u;
  uint64_t acceptedReservations = 0u;
  uint64_t rejectedReservations = 0u;
};

struct War3GpuWorkloadGovernorDiagnostics {
  uint64_t frameSerial = 0u;
  War3GpuWorkloadLimits limits = {};
  War3GpuWorkloadCost used = {};
  std::array<War3GpuWorkloadConsumerDiagnostics,
             static_cast<size_t>(War3GpuWorkloadConsumer::Count)>
      consumers = {};
  uint32_t lastRejectedConsumer =
      static_cast<uint32_t>(War3GpuWorkloadConsumer::Count);
  uint32_t lastRejectReason =
      static_cast<uint32_t>(War3GpuWorkloadRejectReason::None);
  uint64_t arithmeticOverflowRejectCount = 0u;
  uint64_t pointLastCompleteHoldCount = 0u;
  uint64_t pointPublicationInvalidatedCount = 0u;
};

struct War3GpuPointShadowLightIdentity {
  int32_t id = 0;
  float positionX = 0.0f;
  float positionY = 0.0f;
  float positionZ = 0.0f;
  float range = 0.0f;
  float shadowIntensity = 0.0f;
};

/**
 * Exact ownership tuple for retaining a complete point-shadow cube when the
 * optional update is rejected before command recording.
 *
 * This deliberately excludes caster content: a budget rejection may retain a
 * stale but complete cube, but it must never pair it with another map/device,
 * resource allocation, settings revision, light generation, or light layout.
 */
struct War3GpuPointShadowPublicationIdentity {
  static constexpr uint32_t kMaxLights = 4u;

  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  uint64_t resourceGeneration = 0u;
  uint64_t lightGeneration = 0u;
  uint64_t settingsRevision = 0u;
  uint32_t resolution = 0u;
  uint32_t capacityLights = 0u;
  uint32_t lightCount = 0u;
  bool complete = false;
  std::array<War3GpuPointShadowLightIdentity, kMaxLights> lights = {};
};

/** Exact, pure-value admission for retaining a last-complete point cube. */
bool War3GpuCanHoldPointShadowLastComplete(
    const War3GpuPointShadowPublicationIdentity& current,
    const War3GpuPointShadowPublicationIdentity& published) noexcept;

/**
 * Pure-value, render-owner-only workload governor.
 *
 * A reservation is all-or-nothing across draws, vertices, indices and items.
 * It owns no Vulkan/DXVK resources and performs no allocation or logging.
 */
class War3GpuWorkloadGovernor final {
public:
  explicit War3GpuWorkloadGovernor(
      const War3GpuWorkloadLimits& limits = {}) noexcept;

  void beginFrame(uint64_t frameSerial,
                  const War3GpuWorkloadLimits& limits = {}) noexcept;

  bool tryReserve(War3GpuWorkloadConsumer consumer, uint64_t itemCount,
                  const War3GpuWorkloadCost& cost) noexcept;

  void notePointShadowBudgetFallback(bool heldLastComplete) noexcept;

  const War3GpuWorkloadGovernorDiagnostics& diagnostics() const noexcept {
    return m_diagnostics;
  }

  static bool checkedAdd(uint64_t lhs, uint64_t rhs,
                         uint64_t& result) noexcept;
  static bool checkedMultiply(uint64_t lhs, uint64_t rhs,
                              uint64_t& result) noexcept;

  /** Add one draw repeated by a complete cascade/face multiplicity. */
  static bool addRepeatedDraw(War3GpuWorkloadCost& total,
                              uint64_t repeatCount, uint64_t vertexCount,
                              uint64_t indexCount) noexcept;

private:
  static size_t consumerIndex(War3GpuWorkloadConsumer consumer) noexcept;
  void reject(War3GpuWorkloadConsumer consumer,
              War3GpuWorkloadRejectReason reason) noexcept;

  War3GpuWorkloadGovernorDiagnostics m_diagnostics = {};
};

} // namespace dxvk::war3::render
