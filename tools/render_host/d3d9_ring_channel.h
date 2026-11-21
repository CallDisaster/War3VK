#pragma once
#include "d3d9_command_ring.h"
#include "d3d9_high_va.h"
#include "win32_security.h"
#include <memory>
#include <stdexcept>
#include <string>

// Windows objects of the D9C2 command ring: two named regions and two named
// auto-reset events per connection nonce, created by the x86 owner with the
// logon-SID policy of the other RH channels and opened by the host.
//
//   producer region  x86 read/write, host read-only
//   consumer region  host read/write, x86 read-only
//
// A wild write in the game process cannot reach the host's bookkeeping or a
// reply, and the host cannot alter what x86 published.
namespace warvk::host::d9::ring {
inline std::wstring ObjectName(ipc::Nonce nonce, const wchar_t* part) {
  return L"Local\\warvk-d9ring-" + lab::NonceText(nonce) + L"-" + part;
}
// How long a parked side sleeps before it rechecks its predicate anyway. A
// lost or damaged notification costs one such interval, never a deadlock.
constexpr DWORD RecheckMs = 1000;
// Bounded busy polling before a side parks: about the cost of the two thread
// switches it avoids when the peer answers at once.
constexpr uint32_t SpinPolls = 2000;

class Region {
public:
  // `high`: null keeps the plain mapping call, as does a process that is not
  // large-address-aware. Otherwise the view goes above 2 GiB when it can and
  // is mapped the plain way when it cannot (d3d9_high_va.h).
  Region(const std::wstring& name, uint32_t bytes, bool create, bool write, const highva::Process* high = nullptr,
         unsigned methods = highva::AllMethods) : m_bytes(bytes) {
    if (create) {
      lab::LogonSecurity security(lab::SecurityObject::Mapping);
      m_mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE, 0, bytes, name.c_str()));
      // An existing object of this name is not ours, whoever made it.
      if (!m_mapping || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("command ring region creation");
    } else {
      m_mapping.reset(OpenFileMappingW(write ? FILE_MAP_WRITE | FILE_MAP_READ : FILE_MAP_READ, FALSE, name.c_str()));
      if (!m_mapping) throw std::runtime_error("command ring region open");
    }
    if (high) m_view = highva::MapHigh(m_mapping.get(), write, bytes, *high, m_placement, methods);
    if (!m_view) m_view = MapViewOfFile(m_mapping.get(), write ? FILE_MAP_WRITE | FILE_MAP_READ : FILE_MAP_READ, 0, 0, bytes);
    if (!m_view) throw std::runtime_error("command ring region map");
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(m_view, &region, sizeof(region)) != sizeof(region) || region.RegionSize < bytes
        || region.State != MEM_COMMIT || region.Type != MEM_MAPPED
        || (region.Protect & 0xff) != DWORD(write ? PAGE_READWRITE : PAGE_READONLY)) {
      UnmapViewOfFile(m_view);
      throw std::runtime_error("command ring region is not the expected mapping");
    }
  }
  ~Region() { UnmapViewOfFile(m_view); }
  Region(const Region&) = delete;
  Region& operator=(const Region&) = delete;
  uint8_t* base() const noexcept { return static_cast<uint8_t*>(m_view); }
  uint32_t bytes() const noexcept { return m_bytes; }
  const highva::Placement& placement() const noexcept { return m_placement; }
private:
  lab::Handle m_mapping;
  void* m_view = nullptr;
  uint32_t m_bytes;
  highva::Placement m_placement;
};

inline lab::Handle CreateRingEvent(const std::wstring& name) {
  lab::LogonSecurity security(lab::SecurityObject::Event);
  lab::Handle event(CreateEventExW(security.get(), name.c_str(), 0, SYNCHRONIZE | EVENT_MODIFY_STATE));
  if (!event || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("command ring event creation");
  return event;
}
inline lab::Handle OpenRingEvent(const std::wstring& name) {
  lab::Handle event(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name.c_str()));
  if (!event) throw std::runtime_error("command ring event open");
  return event;
}

// x86: creates everything before AttachRing is sent. Fresh regions are zero.
class OwnerChannel {
public:
  // `high`: see Region. Both views are read and written by the ring code
  // alone; replies are copied out of the consumer region, never pointed into.
  OwnerChannel(ipc::Nonce nonce, uint32_t capacity, const highva::Process* high = nullptr,
               unsigned methods = highva::AllMethods)
    : m_capacity(ValidCapacity(capacity) ? capacity : throw std::runtime_error("command ring capacity")),
      m_producer(ObjectName(nonce, L"producer"), ControlBytes + capacity, true, true, high, methods),
      m_consumer(ObjectName(nonce, L"consumer"), ControlBytes + ReplyCapacity, true, false, high, methods),
      m_work(CreateRingEvent(ObjectName(nonce, L"work"))),
      m_progress(CreateRingEvent(ObjectName(nonce, L"progress"))) {
    InitializeProducerBlock(*new (m_producer.base()) ProducerBlock{}, nonce, capacity);
  }
  ProducerBlock* producerBlock() const noexcept { return reinterpret_cast<ProducerBlock*>(m_producer.base()); }
  uint8_t* data() const noexcept { return m_producer.base() + ControlBytes; }
  const ConsumerBlock* consumerBlock() const noexcept { return reinterpret_cast<const ConsumerBlock*>(m_consumer.base()); }
  const uint8_t* reply() const noexcept { return m_consumer.base() + ControlBytes; }
  HANDLE work() const noexcept { return m_work.get(); }
  HANDLE progress() const noexcept { return m_progress.get(); }
  uint32_t capacity() const noexcept { return m_capacity; }
  const Region& producerRegion() const noexcept { return m_producer; }
  const Region& consumerRegion() const noexcept { return m_consumer; }
private:
  uint32_t m_capacity;
  Region m_producer, m_consumer;
  lab::Handle m_work, m_progress;
};

// Host: opens what the verified parent created. Refuses a producer region
// that is not this session's or has already been written to.
class PeerChannel {
public:
  PeerChannel(ipc::Nonce nonce, uint32_t capacity)
    : m_capacity(ValidCapacity(capacity) ? capacity : throw std::runtime_error("command ring capacity")),
      m_producer(ObjectName(nonce, L"producer"), ControlBytes + capacity, false, false),
      m_consumer(ObjectName(nonce, L"consumer"), ControlBytes + ReplyCapacity, false, true),
      m_work(OpenRingEvent(ObjectName(nonce, L"work"))),
      m_progress(OpenRingEvent(ObjectName(nonce, L"progress"))) {
    if (!ValidProducerBlock(*producerBlock(), nonce, capacity)) throw std::runtime_error("command ring producer region");
    // The consumer region must be fresh too: this host is its only writer.
    for (uint32_t i = 0; i < sizeof(ConsumerBlock); ++i)
      if (m_consumer.base()[i]) throw std::runtime_error("command ring consumer region is not fresh");
    InitializeConsumerBlock(*new (m_consumer.base()) ConsumerBlock{}, nonce);
  }
  const ProducerBlock* producerBlock() const noexcept { return reinterpret_cast<const ProducerBlock*>(m_producer.base()); }
  const uint8_t* data() const noexcept { return m_producer.base() + ControlBytes; }
  ConsumerBlock* consumerBlock() const noexcept { return reinterpret_cast<ConsumerBlock*>(m_consumer.base()); }
  uint8_t* reply() const noexcept { return m_consumer.base() + ControlBytes; }
  HANDLE work() const noexcept { return m_work.get(); }
  HANDLE progress() const noexcept { return m_progress.get(); }
  uint32_t capacity() const noexcept { return m_capacity; }
private:
  uint32_t m_capacity;
  Region m_producer, m_consumer;
  lab::Handle m_work, m_progress;
};
} // namespace warvk::host::d9::ring
