#pragma once
#include "slot_ledger.h"
#include "win32_transport.h"

namespace warvk::host::lab {
using Digest = std::array<uint8_t, 32>;
using Sample = std::array<uint8_t, slots::SlotBytes>;
Digest Sha256(const uint8_t* bytes, size_t count);

class MappedView {
public:
  MappedView() = default;
  ~MappedView() { if (m_view) UnmapViewOfFile(m_view); }
  MappedView(const MappedView&) = delete;
  MappedView& operator=(const MappedView&) = delete;
  void set(void* value) noexcept { m_view = value; } // Constructor-only, once.
  void* get() const noexcept { return m_view; }
private:
  void* m_view = nullptr;
};

struct SlotIoStats {
  uint64_t writes = 0, copies = 0, writtenBytes = 0, copiedBytes = 0;
  uint64_t abandoned = 0, mutexTimeouts = 0;
};
enum class MappingRole { OwnerWriter, Reader };
class SharedSlots {
public:
  SharedSlots(ipc::Nonce nonce, MappingRole role);
  SharedSlots(const SharedSlots&) = delete;
  SharedSlots& operator=(const SharedSlots&) = delete;
  // The caller first verifies this key against its exact Reserve request/reply.
  // Locally, each slot generation can write at most once, even after failure.
  void writeOnce(const slots::Key& key, const uint8_t* data, size_t count, HANDLE peer);
  void copyPrivate(const slots::ReadPermit& permit, Sample& destination, HANDLE peer);
  const SlotIoStats& stats() const noexcept { return m_stats; }
  DWORD protection() const noexcept { return m_protection; }
  static std::wstring MappingName(ipc::Nonce nonce);
  static std::wstring MutexName(ipc::Nonce nonce, uint32_t slot);
private:
  void validate(const slots::Key& key) const;
  ipc::Nonce m_nonce;
  MappingRole m_role;
  Handle m_mapping;
  std::array<Handle, slots::SlotCount> m_mutexes;
  MappedView m_view;
  std::array<uint64_t, slots::SlotCount> m_lastWritten{};
  SlotIoStats m_stats;
  DWORD m_protection = 0;
};
} // namespace warvk::host::lab
