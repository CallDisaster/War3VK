#include "slot_wire.h"
#include "slot_ledger.h"
#include <cstring>

namespace warvk::host::slotwire {
Error DecodeHeader(const uint8_t* data, size_t count, Header& out) noexcept {
  out = {};
  if (!data) return Error::NullBuffer;
  if (count < HeaderBytes) return Error::HeaderSize;
  if (std::memcmp(data, "WVS1", 4)) return Error::Magic;
  if (ipc::ReadLe(data + 4, 2) != 1 || ipc::ReadLe(data + 6, 2) != 0) return Error::Version;
  if (ipc::ReadLe(data + 8, 2) != HeaderBytes) return Error::HeaderSize;
  const auto op = ipc::ReadLe(data + 10, 2);
  if (op < uint16_t(Op::Hello) || op > uint16_t(Op::Retire)) return Error::Opcode;
  if (ipc::ReadLe(data + 12, 4) > 1) return Error::Flags;
  if (ipc::ReadLe(data + 20, 4)) return Error::Reserved;
  if (ipc::ReadLe(data + 16, 4) > MaxPayloadBytes) return Error::PayloadLimit;
  Header header;
  header.op = Op(op);
  header.flags = uint32_t(ipc::ReadLe(data + 12, 4));
  header.payloadBytes = uint32_t(ipc::ReadLe(data + 16, 4));
  header.nonce = {ipc::ReadLe(data + 24, 8), ipc::ReadLe(data + 32, 8)};
  header.sequence = ipc::ReadLe(data + 40, 8);
  if (header.nonce.empty()) return Error::EmptyNonce;
  out = header;
  return Error::None;
}
Error Decode(const uint8_t* data, size_t count, View& out) noexcept {
  out = {};
  Header header;
  auto error = DecodeHeader(data, count, header);
  if (error != Error::None) return error;
  if (count - HeaderBytes != header.payloadBytes) return Error::PacketSize;
  out = {header, data + HeaderBytes};
  return Error::None;
}
Error Encode(const Header& h, const uint8_t* payload, size_t count,
             uint8_t* output, size_t capacity, size_t& written) noexcept {
  written = 0;
  if (!output || (count && !payload)) return Error::NullBuffer;
  if (count > MaxPayloadBytes) return Error::PayloadLimit;
  if (h.payloadBytes != count || capacity < HeaderBytes || capacity - HeaderBytes < count)
    return Error::PacketSize;
  std::array<uint8_t, HeaderBytes> bytes{};
  std::memcpy(bytes.data(), "WVS1", 4);
  ipc::WriteLe(bytes.data() + 4, 1, 2);
  ipc::WriteLe(bytes.data() + 8, HeaderBytes, 2);
  ipc::WriteLe(bytes.data() + 10, uint16_t(h.op), 2);
  ipc::WriteLe(bytes.data() + 12, h.flags, 4);
  ipc::WriteLe(bytes.data() + 16, count, 4);
  ipc::WriteLe(bytes.data() + 24, h.nonce.low, 8);
  ipc::WriteLe(bytes.data() + 32, h.nonce.high, 8);
  ipc::WriteLe(bytes.data() + 40, h.sequence, 8);
  Header checked;
  auto error = DecodeHeader(bytes.data(), bytes.size(), checked);
  if (error != Error::None) return error;
  if (count) std::memmove(output + HeaderBytes, payload, count);
  std::memcpy(output, bytes.data(), bytes.size());
  written = HeaderBytes + count;
  return Error::None;
}
std::array<uint8_t, 24> Hello(uint32_t sender, uint32_t peer, uint64_t capabilities) noexcept {
  std::array<uint8_t, 24> bytes{};
  ipc::WriteLe(bytes.data(), sender, 4);
  ipc::WriteLe(bytes.data() + 4, peer, 4);
  ipc::WriteLe(bytes.data() + 8, capabilities, 8);
  ipc::WriteLe(bytes.data() + 16, slots::SlotCount, 4);
  ipc::WriteLe(bytes.data() + 20, slots::SlotBytes, 4);
  return bytes;
}
const char* ErrorName(Error e) noexcept {
  switch (e) {
#define RH1_WIRE_ERROR(x) case Error::x: return #x;
    RH1_WIRE_ERROR(None) RH1_WIRE_ERROR(NullBuffer) RH1_WIRE_ERROR(HeaderSize)
    RH1_WIRE_ERROR(Magic) RH1_WIRE_ERROR(Version) RH1_WIRE_ERROR(Opcode) RH1_WIRE_ERROR(Flags)
    RH1_WIRE_ERROR(Reserved) RH1_WIRE_ERROR(PayloadLimit) RH1_WIRE_ERROR(PacketSize) RH1_WIRE_ERROR(EmptyNonce)
#undef RH1_WIRE_ERROR
  }
  return "Unknown";
}
} // namespace warvk::host::slotwire
