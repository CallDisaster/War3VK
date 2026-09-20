#include "shared_slots.h"
#include "win32_security.h"
#include <bcrypt.h>
#include <cstring>

namespace warvk::host::lab {
namespace {
[[noreturn]] void Fail(Stage stage) { throw Failure(Fault::System, stage, GetLastError()); }
class SlotLock {
public:
  SlotLock(HANDLE mutex, HANDLE peer, SlotIoStats& stats) : m_mutex(mutex) {
    HANDLE waits[] = {mutex, peer};
    const auto result = WaitForMultipleObjects(2, waits, FALSE, DeadlineMs);
    if (result == WAIT_OBJECT_0) {
      m_owned = true;
      const auto peerState = WaitForSingleObject(peer, 0);
      if (peerState != WAIT_TIMEOUT) {
        release();
        if (peerState == WAIT_OBJECT_0) throw Failure(Fault::PeerExited, Stage::SlotMutex);
        Fail(Stage::SlotMutex);
      }
      return;
    }
    if (result == WAIT_ABANDONED_0) {
      // Windows granted ownership, but the previous writer may be incomplete.
      // Release the acquired mutex without touching its protected data.
      ++stats.abandoned;
      if (!ReleaseMutex(m_mutex)) Fail(Stage::SlotMutex);
      throw Failure(Fault::Abandoned, Stage::SlotMutex, WAIT_ABANDONED_0);
    }
    if (result == WAIT_TIMEOUT) {
      ++stats.mutexTimeouts;
      throw Failure(Fault::Timeout, Stage::SlotMutex, WAIT_TIMEOUT);
    }
    if (result == WAIT_OBJECT_0 + 1)
      throw Failure(Fault::PeerExited, Stage::SlotMutex);
    Fail(Stage::SlotMutex);
  }
  ~SlotLock() { if (m_owned) ReleaseMutex(m_mutex); }
  void release() {
    if (!m_owned || !ReleaseMutex(m_mutex)) Fail(Stage::SlotMutex);
    m_owned = false;
  }
  SlotLock(const SlotLock&) = delete;
  SlotLock& operator=(const SlotLock&) = delete;
private:
  HANDLE m_mutex;
  bool m_owned = false;
};
struct Algorithm {
  BCRYPT_ALG_HANDLE handle = nullptr;
  ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
}

Digest Sha256(const uint8_t* bytes, size_t count) {
  if (!bytes || count > slots::SlotBytes)
    throw Failure(Fault::System, Stage::SlotCopy, ERROR_INVALID_PARAMETER);
  Algorithm algorithm;
  if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    throw Failure(Fault::System, Stage::SlotCopy, ERROR_GEN_FAILURE);
  Digest result{};
  if (BCryptHash(algorithm.handle, nullptr, 0, const_cast<PUCHAR>(bytes), ULONG(count),
      result.data(), ULONG(result.size())) != 0)
    throw Failure(Fault::System, Stage::SlotCopy, ERROR_GEN_FAILURE);
  return result;
}

std::wstring SharedSlots::MappingName(ipc::Nonce nonce) {
  return L"Local\\warvk-rh1-" + NonceText(nonce) + L"-data";
}
std::wstring SharedSlots::MutexName(ipc::Nonce nonce, uint32_t slot) {
  if (slot >= slots::SlotCount)
    throw Failure(Fault::SlotLease, Stage::Setup);
  return L"Local\\warvk-rh1-" + NonceText(nonce) + L"-slot" + std::to_wstring(slot);
}

SharedSlots::SharedSlots(ipc::Nonce nonce, MappingRole role) : m_nonce(nonce), m_role(role) {
  if (nonce.empty() || (role != MappingRole::OwnerWriter && role != MappingRole::Reader))
    throw Failure(Fault::SlotLease, Stage::Setup);
  if (role == MappingRole::OwnerWriter) {
    LogonSecurity security(SecurityObject::Mapping);
    m_mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE,
      0, slots::PoolBytes, MappingName(nonce).c_str()));
    DWORD error = GetLastError();
    if (!m_mapping || error == ERROR_ALREADY_EXISTS)
      throw Failure(Fault::System, Stage::Setup, error);
  } else {
    m_mapping.reset(OpenFileMappingW(FILE_MAP_READ, FALSE, MappingName(nonce).c_str()));
    if (!m_mapping) Fail(Stage::Setup);
  }
  for (uint32_t slot = 0; slot < slots::SlotCount; ++slot) {
    if (role == MappingRole::OwnerWriter) {
      LogonSecurity security(SecurityObject::Mutex);
      m_mutexes[slot].reset(CreateMutexExW(security.get(), MutexName(nonce, slot).c_str(), 0,
        SYNCHRONIZE | MUTEX_MODIFY_STATE));
      DWORD error = GetLastError();
      if (!m_mutexes[slot] || error == ERROR_ALREADY_EXISTS)
        throw Failure(Fault::System, Stage::Setup, error);
    } else {
      m_mutexes[slot].reset(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, MutexName(nonce, slot).c_str()));
      if (!m_mutexes[slot]) Fail(Stage::Setup);
    }
  }
  m_view.set(MapViewOfFile(m_mapping.get(), role == MappingRole::Reader ? FILE_MAP_READ : FILE_MAP_WRITE,
    0, 0, slots::PoolBytes));
  if (!m_view.get()) Fail(Stage::Setup);
  MEMORY_BASIC_INFORMATION region{};
  if (VirtualQuery(m_view.get(), &region, sizeof(region)) != sizeof(region)) Fail(Stage::Setup);
  m_protection = region.Protect & 0xff;
  if (region.RegionSize < slots::PoolBytes || region.State != MEM_COMMIT || region.Type != MEM_MAPPED ||
      m_protection != (role == MappingRole::Reader ? PAGE_READONLY : PAGE_READWRITE))
    throw Failure(Fault::System, Stage::Setup, ERROR_INVALID_DATA);
}

void SharedSlots::validate(const slots::Key& key) const {
  if (!(key.connection == m_nonce) || key.slot >= slots::SlotCount || !key.bytes ||
      key.bytes > slots::SlotBytes || !key.generation || !key.map || !key.device || !key.frame)
    throw Failure(Fault::SlotLease, Stage::SlotCopy);
}

void SharedSlots::writeOnce(const slots::Key& key, const uint8_t* data, size_t count, HANDLE peer) {
  validate(key);
  if (m_role != MappingRole::OwnerWriter || !data || count != key.bytes ||
      key.generation <= m_lastWritten[key.slot])
    throw Failure(Fault::SlotLease, Stage::SlotCopy);
  m_lastWritten[key.slot] = key.generation; // Burn before waiting/copy; no ambiguous retry.
  SlotLock lock(m_mutexes[key.slot].get(), peer, m_stats);
  auto* target = static_cast<uint8_t*>(m_view.get()) + size_t(key.slot) * slots::SlotBytes;
  std::memcpy(target, data, count);
  lock.release(); // Successful publication requires an observed unlock too.
  ++m_stats.writes;
  m_stats.writtenBytes += count;
}

void SharedSlots::copyPrivate(const slots::ReadPermit& permit, Sample& destination, HANDLE peer) {
  if (!permit.active() || m_role != MappingRole::Reader)
    throw Failure(Fault::SlotLease, Stage::SlotCopy);
  const auto key = permit.key();
  validate(key);
  SlotLock lock(m_mutexes[key.slot].get(), peer, m_stats);
  const auto* source = static_cast<const uint8_t*>(m_view.get()) + size_t(key.slot) * slots::SlotBytes;
  std::memcpy(destination.data(), source, key.bytes);
  lock.release();
  ++m_stats.copies;
  m_stats.copiedBytes += key.bytes;
}
} // namespace warvk::host::lab
