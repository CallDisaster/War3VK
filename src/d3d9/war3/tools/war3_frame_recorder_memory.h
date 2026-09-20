#pragma once
#include <cstdint>
#include <cstdlib>

namespace dxvk::war3::tools::evidence {
// Process VA and commit are different from RAM, VRAM and free disk space.
// This is admission headroom, NOT a reservation or a guarantee against other
// threads/driver allocations. No policy here changes the host EXE's LAA flag.
inline constexpr uint64_t RecorderProcessHeadroom = 256ull * 1024 * 1024;
// 2026-09-19 玩家实机遇到 recorder-process-address-space-headroom（32 位进程 VA 吃紧）：
// 允许**只下调**预留（下限 64 MiB，上限仍是默认 256 MiB），供用户自救；
// 语义不变：仍然 fail-closed，只是门槛更贴近真实需求（ring ≈ 数十 MB + 对齐余量）。
inline uint64_t RecorderProcessHeadroomBytes() noexcept {
  static const uint64_t value = []() noexcept -> uint64_t {
    const char* s = std::getenv("DXVK_WAR3_FRAME_EVIDENCE_VA_HEADROOM_MB");
    if (!s) return RecorderProcessHeadroom;
    const long mb = std::atol(s);
    if (mb < 64) return RecorderProcessHeadroom;
    const uint64_t bytes = uint64_t(mb) * 1024ull * 1024ull;
    return bytes < RecorderProcessHeadroom ? bytes : RecorderProcessHeadroom;
  }();
  return value;
}
struct RecorderMemory {
  bool valid = false, contiguousKnown = false;
  uint64_t totalVirtual = 0, availableVirtual = 0, availableCommit = 0;
  uint64_t largestFreeRegion = 0;
};
enum class RecorderMemoryReject : uint32_t {
  None, Query, AddressSpace, Commit, Contiguous
};
inline RecorderMemoryReject RecorderMemoryAdmission(const RecorderMemory& m,
    uint64_t additionalBytes, uint64_t contiguousBytes = 0) noexcept {
  if (!m.valid || !m.totalVirtual || m.availableVirtual > m.totalVirtual)
    return RecorderMemoryReject::Query;
  // Subtraction form prevents overflow even for hostile/maximal requests.
  const uint64_t headroom = RecorderProcessHeadroomBytes();
  if (m.availableVirtual < headroom ||
      additionalBytes > m.availableVirtual - headroom)
    return RecorderMemoryReject::AddressSpace;
  if (m.availableCommit < headroom ||
      additionalBytes > m.availableCommit - headroom)
    return RecorderMemoryReject::Commit;
  if (contiguousBytes && (!m.contiguousKnown ||
      contiguousBytes > m.largestFreeRegion))
    return RecorderMemoryReject::Contiguous;
  return RecorderMemoryReject::None;
}
inline const char* RecorderMemoryReason(RecorderMemoryReject reason) noexcept {
  switch (reason) {
    case RecorderMemoryReject::None: return "";
    case RecorderMemoryReject::Query: return "recorder-process-memory-query-failed";
    case RecorderMemoryReject::AddressSpace: return "recorder-process-address-space-headroom";
    case RecorderMemoryReject::Commit: return "recorder-process-commit-headroom";
    case RecorderMemoryReject::Contiguous: return "recorder-contiguous-address-space";
  }
  return "recorder-process-memory-invalid-reason";
}
// VirtualQuery is optional and bounded, used only at CPU ring admission.
// No process memory reads, VM reservations, GPU calls or heap allocations.
RecorderMemory QueryRecorderMemory(bool contiguous) noexcept;
} // namespace dxvk::war3::tools::evidence
