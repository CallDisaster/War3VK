#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// RH2 candidate laboratory only. Not linked into the shipping D3D9 DLL.
// One calling thread, one image and at most one frame in flight. The IPC owner
// must validate session/epoch/frame tickets; native handle integers are NOT
// authority and must be duplicated via the exact owned peer process handle.
namespace warvk::host::gpu {

// Only a successfully enumerated, known deviceUUID/driverUUID mismatch uses
// this type. Loader, capability, allocation and other runtime failures do NOT.
class AdapterMismatch : public std::runtime_error {
public:
  explicit AdapterMismatch(const std::string& message) : std::runtime_error(message) { }
};

struct ShaderWords {
  const uint32_t* data = nullptr;
  size_t words = 0;
};

struct ExportedImage {
  uint32_t width = 0, height = 0, format = 0, memoryTypeIndex = 0;
  uint64_t allocationBytes = 0;
  std::array<uint8_t, 16> deviceUUID{}, driverUUID{};
  uint64_t imageMemoryHandle = 0;
  uint64_t readySemaphoreHandle = 0;
  uint64_t consumedSemaphoreHandle = 0;
};

struct BackendStats {
  std::string deviceName;
  std::array<uint8_t, 16> deviceUUID{}, driverUUID{};
  // Vulkan allocation sizes, not driver-private memory, residency or process VA.
  // ownedDeviceBytes = allocations created here (no imported-image double count).
  // sharedImageBytes describes the same physical payload in BOTH processes.
  uint64_t ownedDeviceBytes = 0, sharedImageBytes = 0;
  uint64_t geometryBytes = 0, geometryPayloadBytes = 0, readbackBytes = 0;
  uint64_t uploadStagingPeakBytes = 0;
  uint64_t drawCount = 0, submitCount = 0, fenceWaitCount = 0;
  uint64_t consumerWaitCount = 0, readbackCount = 0;
  uint32_t imageExternalFeatures = 0, semaphoreExternalFeatures = 0;
  bool validationEnabled = false;
  // Process cumulative, not exclusive per-instance counts. Also includes
  // messages from any previously destroyed instance in this executable.
  uint64_t validationErrors = 0, validationWarnings = 0;
  bool faulted = false;
};

struct ValidationTotals {
  uint64_t instancesEnabled = 0, errors = 0, warnings = 0;
};
// Read AFTER all GPU objects are destroyed as well as during operation, so
// destruction-time validation cannot disappear with a PImpl. Process cumulative.
ValidationTotals processValidationTotals() noexcept;

class Renderer {
public:
  Renderer(uint32_t width, uint32_t height, ShaderWords vertex, ShaderWords fragment);
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // Borrowed NT handles remain owned by Renderer. Never CloseHandle these
  // originals at the caller. Duplicate them into the exact consuming process.
  ExportedImage exportImage() const;
  // Real indexed device-local geometry. Variants 0..3 select red/green/blue/yellow.
  // The first frame has no consumed wait; every subsequent frame consumes the
  // preceding GPU semaphore before the shared image can be written again.
  void render(uint32_t variant);
  // Required before normal destruction after the final frame; may also be used
  // after EVERY frame. Consumes that frame's GPU semaphore with a bounded fence
  // wait, not a CPU acknowledgement. Idempotent if no consumer is outstanding.
  // Leaves image ownership EXTERNAL; a later render still acquires GENERAL.
  void settleLastConsumer();
  BackendStats stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

class Importer {
public:
  // All three handles must already belong to this process. They are borrowed:
  // this constructor duplicates temporary local handles, imports and closes its
  // duplicates. The caller retains ownership on BOTH success and failure.
  explicit Importer(const ExportedImage& image);
  ~Importer();
  Importer(const Importer&) = delete;
  Importer& operator=(const Importer&) = delete;

  // TEST ORACLE ONLY: ready wait -> external acquire -> GPU copy -> external
  // release -> consumed signal -> bounded fence wait -> host read. This is not
  // the product presentation route; no image bytes traverse the IPC channel.
  std::vector<uint8_t> readback();
  BackendStats stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// No vkDeviceWaitIdle is used. Every explicit fence wait is limited to 5 s.
// An unknown in-flight failure permanently faults the object and quarantines
// driver resources until process exit: freeing live GPU objects would be unsafe.
// Therefore the executable owner MUST supply an external watchdog/owned Job.

} // namespace warvk::host::gpu
