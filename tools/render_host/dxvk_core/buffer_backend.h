#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace warvk::host::dxvkcore {

// Development-only, real DXVK resource backend. Construct and call on one
// dedicated x64 host worker; it is not a game-thread or D3D9 proxy interface.
// The executable supplies the shared DXVK Logger singleton.
class BufferBackend final {
public:
  static constexpr uint32_t SlotCount = 4;
  static constexpr uint32_t MaxBufferBytes = 262144;
  static constexpr uint32_t MaxUploadBytes = 65440;
  static constexpr uint64_t MaxLiveBytes = uint64_t(SlotCount) * MaxBufferBytes;

  struct Verification {
    uint64_t hash = 0; // FNV-1a 64 of bytes read back from the actual GPU buffer.
    uint32_t bytes = 0;
  };

  struct Stats {
    // submitted means accepted by the production DXVK submission queue, NOT
    // GPU completion. The next two counters require distinct actual proofs.
    uint64_t submitted = 0;
    uint64_t gpuCompleted = 0;
    uint64_t retired = 0;
    uint64_t created = 0;
    uint64_t destroyed = 0;
    uint64_t uploadedBytes = 0;
    uint64_t verifiedBytes = 0;
    // Committed resource sizes only: not physical allocation/residency, not
    // total host/device memory, and not measured savings in the x86 process.
    uint64_t liveBytes = 0;
    uint64_t peakLiveBytes = 0;
    uint32_t liveResources = 0;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t stagingMemoryFlags = 0;
    uint32_t readbackMemoryFlags = 0;
    bool normalTeardown = false;
    bool quarantined = false;
    std::string adapter;
  };

  BufferBackend();
  ~BufferBackend() noexcept;
  BufferBackend(const BufferBackend&) = delete;
  BufferBackend& operator=(const BufferBackend&) = delete;
  BufferBackend(BufferBackend&&) = delete;
  BufferBackend& operator=(BufferBackend&&) = delete;

  // Slots are zero-based and cannot be replaced while live. Newly created
  // storage is undefined until uploaded. No full CPU resource mirror is kept.
  void create(uint32_t slot, uint32_t size);
  // Any byte offset/count is supported; count must be 1..MaxUploadBytes and
  // fit the destination. The caller's bytes are owned before this call returns.
  void upload(uint32_t slot, uint32_t offset, const uint8_t* data, uint32_t count);
  // Caller must initialize every resource byte before using the hash as an
  // oracle. This returns actual GPU bytes, never a hash of the upload payload.
  Verification verify(uint32_t slot);
  void destroy(uint32_t slot);
  // Requires every slot destroyed. Success is recorded only after the actual
  // DXVK teardown returns. An outer isolated-process watchdog is still needed.
  void finish();

  // Worker-only, or after the worker is joined. Not a concurrent live snapshot.
  const Stats& stats() const noexcept { return m_stats; }

private:
  struct Owner;
  Owner& active();
  void quarantine() noexcept;

  std::unique_ptr<Owner> m_owner;
  Stats m_stats;
};

} // namespace warvk::host::dxvkcore
