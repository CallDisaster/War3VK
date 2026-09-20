#pragma once
#include "slot_ledger.h"
#include "slot_wire.h"

namespace warvk::host::lab {
// Synchronous borrowed inputs only. Return a static error string or nullptr;
// never retain a permit, byte pointer, or view beyond consume().
class SlotPayloadConsumer {
public:
  virtual ~SlotPayloadConsumer() = default;
  virtual uint64_t capabilities() const noexcept = 0;
  virtual const char* begin(ipc::Nonce nonce, uint64_t map, uint64_t device) = 0;
  virtual const char* consume(const slots::ReadPermit& permit, const uint8_t* bytes, size_t count) = 0;
  // Optional CPU application terminal gates. Existing RH1 sample consumers
  // keep their old behavior. Called BEFORE the corresponding successful ACK.
  virtual const char* end() { return nullptr; }
  virtual const char* close() { return nullptr; }
};
int RunSlotHost(int argc, wchar_t** argv, SlotPayloadConsumer* consumer = nullptr);
}
