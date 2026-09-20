#pragma once
#include "producer_inbox.h"
#include "slot_ledger.h"

namespace warvk::host::envelope {
inline constexpr uint32_t HeaderBytes = 80;
inline constexpr uint32_t MaxPayloadBytes = slots::SlotBytes - HeaderBytes;
inline constexpr inbox::Limits InboxLimits{UINT32_MAX, MaxPayloadBytes};
enum class Error {
  None, NullBuffer, Size, Capacity, Overlap, Magic, Version, Header, Flags,
  Scope, SourceFrame, Attempt, Transfer, Lease, Order, Exhausted, Terminal
};
struct Header {
  inbox::Scope scope;
  uint64_t sourceFrame = 0, attempt = 0, transfer = 0;
};
// Borrow only during synchronous consumption of an immutable host-private copy.
// This view is not a lease, and none of its native layout is sent over the wire.
struct View {
  Header header;
  const uint8_t* data = nullptr;
  uint32_t bytes = 0;
};
Error Encode(Header header, const uint8_t* payload, uint32_t bytes,
             uint8_t* output, size_t capacity, size_t& written) noexcept;
Error Decode(const uint8_t* input, size_t bytes, View& output) noexcept;
const char* ErrorName(Error error) noexcept;
struct Limits { uint64_t transfer = UINT64_MAX; uint32_t attempt = UINT32_MAX; };
struct Progress { uint64_t sourceFrame = 0, attempt = 0, transfer = 0; Error fault = Error::None; };

// Payload order owner only. Caller still needs an actual SlotLedger ReadPermit
// and immutable private bytes. A forged Key here is NOT proof of a real lease.
class Validator final {
public:
  explicit Validator(inbox::Scope scope, Limits limits = {}) noexcept;
  Validator(const Validator&) = delete;
  Validator& operator=(const Validator&) = delete;
  Validator(Validator&&) = delete;
  Validator& operator=(Validator&&) = delete;
  Error accept(const slots::Key& key, const uint8_t* bytes, size_t count, View& output) noexcept;
  Progress progress() const noexcept { return m_progress; }
private:
  Error fail(Error error) noexcept { m_progress.fault=error; return error; }
  const inbox::Scope m_scope;
  const Limits m_limits;
  Progress m_progress;
};
}
