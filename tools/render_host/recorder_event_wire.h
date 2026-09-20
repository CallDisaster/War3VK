#pragma once
#include "../../src/d3d9/war3/tools/war3_frame_evidence_core.h"
#include <cstddef>
#include <cstdint>

// CPU event records only. Native Event layout, pointers and GPU handles are
// never transported as objects. See the recorder-offload E1 contract.
namespace warvk::host::recorder {
using Event = dxvk::war3::tools::evidence::Event;
inline constexpr uint32_t HeaderBytes = 80;
inline constexpr uint32_t EventBytes = 392;
inline constexpr uint32_t MaxEvents = 160;
inline constexpr uint32_t MaxPacketBytes = HeaderBytes + MaxEvents * EventBytes;
inline constexpr uint32_t MaxHistoryEvents = 262144;
inline constexpr uint64_t NoTrigger = UINT64_MAX;
enum class Op : uint16_t { Begin = 1, Data = 2, Seal = 3 };
enum class WireError { None, Null, Size, Capacity, Overlap, Magic, Version,
  Header, Opcode, Flags, Shape, Session, Ordinal, Trigger, Totals, EventValue };
struct Header {
  Op op = Op::Begin;
  uint32_t count = 0;
  uint64_t session = 0, ordinal = 0, trigger = NoTrigger;
  uint64_t attempted = 0, lost = 0;
  uint32_t capacity = 0, reason = 0;
  uint64_t accepted = 0;
};
struct View {
  Header header{};
  const uint8_t* events = nullptr;
};
// Failures clear written/View/Event except when that output reference aliases
// input/destination: reject Overlap without writes (clearing would corrupt the
// input). No heap/OS calls. Encode validates the whole source before writing.
WireError Encode(const Header&, const Event*, uint8_t*, size_t, size_t& written) noexcept;
WireError Decode(const uint8_t*, size_t, View&) noexcept;
WireError DecodeEvent(const uint8_t*, size_t, Event&) noexcept;
const char* ErrorName(WireError) noexcept;
}
