#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace dxvk::war3::render {

// Frame-local value ledger for a cache whose nodes may be rehashed, erased or
// rebuilt under the same logical key. Records never retain node addresses.
// The monotonic ordinal closes the erase/reinsert ABA case while callers
// continue to perform their canonical map lookup and full entry validation.
template <typename Key>
class War3DrawTimeActiveLedger {
public:
  struct EntryStamp {
    uint64_t frameSerial = 0u;
    uint64_t activationOrdinal = 0u;
  };

  struct Record {
    Key key = {};
    uint64_t frameSerial = 0u;
    uint64_t activationOrdinal = 0u;
  };

  explicit War3DrawTimeActiveLedger(
      uint64_t nextActivationOrdinal = 1u) noexcept
  : m_nextActivationOrdinal(
        nextActivationOrdinal != 0u
            ? nextActivationOrdinal
            : std::numeric_limits<uint64_t>::max()) { }

  void beginFrame(uint64_t frameSerial) noexcept {
    if (!m_usable || frameSerial == 0u || m_frameSerial == frameSerial)
      return;
    m_records.clear();
    m_frameSerial = frameSerial;
  }

  bool activate(uint64_t frameSerial, const Key& key, EntryStamp& stamp) {
    beginFrame(frameSerial);
    if (!usableForFrame(frameSerial))
      return false;
    if (stamp.frameSerial == frameSerial && stamp.activationOrdinal != 0u)
      return true;
    if (m_nextActivationOrdinal ==
        std::numeric_limits<uint64_t>::max()) {
      m_records.clear();
      m_usable = false;
      return false;
    }

    const uint64_t ordinal = m_nextActivationOrdinal++;
    m_records.push_back(Record{key, frameSerial, ordinal});
    stamp.frameSerial = frameSerial;
    stamp.activationOrdinal = ordinal;
    return true;
  }

  bool usableForFrame(uint64_t frameSerial) const noexcept {
    return m_usable && frameSerial != 0u && m_frameSerial == frameSerial;
  }

  static bool matches(const Record& record,
                      const EntryStamp& stamp) noexcept {
    return record.frameSerial == stamp.frameSerial &&
           record.activationOrdinal == stamp.activationOrdinal &&
           record.activationOrdinal != 0u;
  }

  const std::vector<Record>& records() const noexcept {
    return m_records;
  }

  void resetSession() noexcept {
    m_records.clear();
    m_frameSerial = 0u;
    // Never rewind the ordinal. A key reused by a later map/device session
    // cannot become indistinguishable from a record created by an old one.
  }

private:
  std::vector<Record> m_records;
  uint64_t m_frameSerial = 0u;
  uint64_t m_nextActivationOrdinal = 1u;
  bool m_usable = true;
};

}  // namespace dxvk::war3::render
