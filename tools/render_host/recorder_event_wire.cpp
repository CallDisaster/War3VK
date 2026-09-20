#include "recorder_event_wire.h"
#include <cstring>

namespace warvk::host::recorder {

namespace {

static inline uint16_t ReadU16Le(const uint8_t* p) noexcept {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

static inline uint32_t ReadU32Le(const uint8_t* p) noexcept {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static inline uint64_t ReadU64Le(const uint8_t* p) noexcept {
  return static_cast<uint64_t>(p[0]) |
         (static_cast<uint64_t>(p[1]) << 8) |
         (static_cast<uint64_t>(p[2]) << 16) |
         (static_cast<uint64_t>(p[3]) << 24) |
         (static_cast<uint64_t>(p[4]) << 32) |
         (static_cast<uint64_t>(p[5]) << 40) |
         (static_cast<uint64_t>(p[6]) << 48) |
         (static_cast<uint64_t>(p[7]) << 56);
}

static inline void WriteU16Le(uint8_t* p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

static inline void WriteU32Le(uint8_t* p, uint32_t v) noexcept {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

static inline void WriteU64Le(uint8_t* p, uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
  }
}

static inline bool Overlaps(const void* a, size_t an, const void* b, size_t bn) noexcept {
  if (!a || !b || an == 0 || bn == 0) return false;
  const auto x = reinterpret_cast<uintptr_t>(a);
  const auto y = reinterpret_cast<uintptr_t>(b);
  if (an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y) return true;
  return x < y + bn && y < x + an;
}

} // namespace

WireError Encode(const Header& header, const Event* events, uint8_t* dst, size_t out_size, size_t& written) noexcept {
  // Bounded event input range calculation (overflow-safe)
  size_t eventBytes = 0;
  if (events && header.count > 0) {
    if (sizeof(Event) > SIZE_MAX / static_cast<size_t>(header.count))
      return WireError::Overlap; // Range overflow is treated as Overlap
    eventBytes = static_cast<size_t>(header.count) * sizeof(Event);
  }

  // 1. Output reference aliasing check:
  // If &written aliases any input or destination range, return Overlap IMMEDIATELY
  // without modifying written, preserving all sources.
  if (Overlaps(&written, sizeof(size_t), &header, sizeof(Header)))
    return WireError::Overlap;
  if (dst && out_size > 0 && Overlaps(&written, sizeof(size_t), dst, out_size))
    return WireError::Overlap;
  if (events && eventBytes > 0 && Overlaps(&written, sizeof(size_t), events, eventBytes))
    return WireError::Overlap;

  // Now written is proven independent and safe to initialize
  written = 0;

  if (!dst) return WireError::Null;

  // 2. Destination overlap check: dst with header or events
  if (out_size > 0 && Overlaps(dst, out_size, &header, sizeof(Header)))
    return WireError::Overlap;
  if (events && eventBytes > 0 && out_size > 0 && Overlaps(dst, out_size, events, eventBytes))
    return WireError::Overlap;

  // 3. Header validation
  if (header.op != Op::Begin && header.op != Op::Data && header.op != Op::Seal)
    return WireError::Opcode;
  if (header.capacity < 4 || header.capacity > MaxHistoryEvents)
    return WireError::Capacity;
  if (header.session == 0)
    return WireError::Session;
  if (header.ordinal == 0)
    return WireError::Ordinal;
  if (header.op == Op::Begin && header.ordinal != 1)
    return WireError::Ordinal;
  if (header.op == Op::Begin && header.trigger != NoTrigger)
    return WireError::Trigger;
  if (header.op == Op::Seal && header.trigger != NoTrigger && header.trigger > header.accepted)
    return WireError::Trigger;

  if (header.count > MaxEvents)
    return WireError::Shape;
  if (header.op == Op::Begin && (header.count != 0 || header.reason != 0))
    return WireError::Shape;
  if (header.op == Op::Data && (header.count == 0 || header.reason != 0))
    return WireError::Shape;
  if (header.op == Op::Seal && (header.count != 0 || header.reason < 1 || header.reason > 4))
    return WireError::Shape;

  if (header.op == Op::Begin && (header.attempted != 0 || header.lost != 0 || header.accepted != 0))
    return WireError::Totals;
  if (header.op == Op::Data && (header.attempted != 0 || header.lost != 0 || header.accepted != 0))
    return WireError::Totals;
  if (header.op == Op::Seal && (header.accepted > header.attempted || header.lost != header.attempted - header.accepted))
    return WireError::Totals;

  if (header.count > 0 && !events)
    return WireError::Null;

  const size_t payloadBytes = static_cast<size_t>(header.count) * EventBytes;
  const size_t required = HeaderBytes + payloadBytes;
  if (out_size < required)
    return WireError::Size;

  // 4. Validate all events before writing any output
  for (uint32_t i = 0; i < header.count; ++i) {
    const Event& ev = events[i];
    if (ev.session != header.session)
      return WireError::Session;
    if (ev.sequence == 0 || ev.sequence == UINT64_MAX)
      return WireError::EventValue;
    const uint32_t k = static_cast<uint32_t>(ev.kind);
    if (k < 1 || k > 18)
      return WireError::EventValue;
    if (std::memchr(ev.label.data(), '\0', ev.label.size()) == nullptr)
      return WireError::EventValue;
  }

  // 5. Header serialization (80 bytes, explicit little-endian)
  std::memcpy(dst + 0, "WVE1", 4);
  WriteU16Le(dst + 4, 1);
  WriteU16Le(dst + 6, 0);
  WriteU16Le(dst + 8, static_cast<uint16_t>(header.op));
  WriteU16Le(dst + 10, 0);
  WriteU32Le(dst + 12, HeaderBytes);
  WriteU32Le(dst + 16, static_cast<uint32_t>(payloadBytes));
  WriteU32Le(dst + 20, header.count);
  WriteU64Le(dst + 24, header.session);
  WriteU64Le(dst + 32, header.ordinal);
  WriteU64Le(dst + 40, header.trigger);
  WriteU64Le(dst + 48, header.attempted);
  WriteU64Le(dst + 56, header.lost);
  WriteU32Le(dst + 64, header.capacity);
  WriteU32Le(dst + 68, header.reason);
  WriteU64Le(dst + 72, header.accepted);

  // 6. Events serialization (392 bytes each, explicit little-endian)
  uint8_t* p = dst + HeaderBytes;
  for (uint32_t i = 0; i < header.count; ++i) {
    const Event& ev = events[i];
    WriteU64Le(p + 0, ev.sequence);
    WriteU64Le(p + 8, ev.session);
    WriteU64Le(p + 16, ev.parent);
    WriteU64Le(p + 24, ev.qpc);
    WriteU64Le(p + 32, ev.key.owner);
    WriteU64Le(p + 40, ev.key.frame);
    WriteU64Le(p + 48, ev.key.mapEpoch);
    WriteU64Le(p + 56, ev.key.deviceEpoch);
    WriteU32Le(p + 64, ev.thread);
    WriteU32Le(p + 68, static_cast<uint32_t>(ev.kind));
    std::memcpy(p + 72, ev.label.data(), 32);
    for (size_t j = 0; j < 12; ++j) {
      WriteU64Le(p + 104 + j * 8, ev.data[j]);
    }
    for (size_t j = 0; j < 48; ++j) {
      WriteU32Le(p + 200 + j * 4, ev.bits[j]);
    }
    p += EventBytes;
  }

  written = required;
  return WireError::None;
}

WireError DecodeEvent(const uint8_t* src, size_t in_size, Event& out) noexcept {
  // 1. Check if output reference itself aliases src
  if (src && in_size > 0 && Overlaps(&out, sizeof(Event), src, in_size))
    return WireError::Overlap;

  // Now safe to clear out
  out = Event{};
  if (!src) return WireError::Null;
  if (in_size != EventBytes) return WireError::Size;

  const uint64_t seq = ReadU64Le(src + 0);
  if (seq == 0 || seq == UINT64_MAX) return WireError::EventValue;
  const uint64_t sess = ReadU64Le(src + 8);
  if (sess == 0) return WireError::Session;
  const uint32_t kindVal = ReadU32Le(src + 68);
  if (kindVal < 1 || kindVal > 18) return WireError::EventValue;
  if (std::memchr(src + 72, '\0', 32) == nullptr) return WireError::EventValue;

  out.sequence = seq;
  out.session = sess;
  out.parent = ReadU64Le(src + 16);
  out.qpc = ReadU64Le(src + 24);
  out.key.owner = ReadU64Le(src + 32);
  out.key.frame = ReadU64Le(src + 40);
  out.key.mapEpoch = ReadU64Le(src + 48);
  out.key.deviceEpoch = ReadU64Le(src + 56);
  out.thread = ReadU32Le(src + 64);
  out.kind = static_cast<dxvk::war3::tools::evidence::Kind>(kindVal);
  std::memcpy(out.label.data(), src + 72, 32);
  for (size_t j = 0; j < 12; ++j) {
    out.data[j] = ReadU64Le(src + 104 + j * 8);
  }
  for (size_t j = 0; j < 48; ++j) {
    out.bits[j] = ReadU32Le(src + 200 + j * 4);
  }
  return WireError::None;
}

WireError Decode(const uint8_t* src, size_t in_size, View& out) noexcept {
  // 1. Check if output reference itself aliases src
  if (src && in_size > 0 && Overlaps(&out, sizeof(View), src, in_size))
    return WireError::Overlap;

  // Now safe to clear out
  out = View{};
  if (!src) return WireError::Null;
  if (in_size < HeaderBytes) return WireError::Size;

  if (std::memcmp(src, "WVE1", 4) != 0) return WireError::Magic;
  const uint16_t major = ReadU16Le(src + 4);
  const uint16_t minor = ReadU16Le(src + 6);
  if (major != 1 || minor != 0) return WireError::Version;

  const uint16_t opVal = ReadU16Le(src + 8);
  if (opVal < 1 || opVal > 3) return WireError::Opcode;
  const Op op = static_cast<Op>(opVal);

  const uint16_t flags = ReadU16Le(src + 10);
  if (flags != 0) return WireError::Flags;

  const uint32_t headerBytes = ReadU32Le(src + 12);
  if (headerBytes != HeaderBytes) return WireError::Header;

  const uint32_t payloadBytes = ReadU32Le(src + 16);
  const uint32_t count = ReadU32Le(src + 20);
  if (count > MaxEvents) return WireError::Shape;
  if (payloadBytes != count * EventBytes) return WireError::Shape;

  const size_t required = HeaderBytes + static_cast<size_t>(payloadBytes);
  if (in_size != required) return WireError::Size;

  const uint64_t session = ReadU64Le(src + 24);
  if (session == 0) return WireError::Session;

  const uint64_t ordinal = ReadU64Le(src + 32);
  if (ordinal == 0) return WireError::Ordinal;
  if (op == Op::Begin && ordinal != 1) return WireError::Ordinal;

  const uint64_t trigger = ReadU64Le(src + 40);
  if (op == Op::Begin && trigger != NoTrigger) return WireError::Trigger;

  const uint64_t attempted = ReadU64Le(src + 48);
  const uint64_t lost = ReadU64Le(src + 56);
  const uint32_t capacity = ReadU32Le(src + 64);
  if (capacity < 4 || capacity > MaxHistoryEvents) return WireError::Capacity;
  const uint32_t reason = ReadU32Le(src + 68);
  const uint64_t accepted = ReadU64Le(src + 72);

  if (op == Op::Begin && (count != 0 || reason != 0)) return WireError::Shape;
  if (op == Op::Data && (count == 0 || reason != 0)) return WireError::Shape;
  if (op == Op::Seal && (count != 0 || reason < 1 || reason > 4)) return WireError::Shape;

  if (op == Op::Begin && (attempted != 0 || lost != 0 || accepted != 0)) return WireError::Totals;
  if (op == Op::Data && (attempted != 0 || lost != 0 || accepted != 0)) return WireError::Totals;
  if (op == Op::Seal && (accepted > attempted || lost != attempted - accepted)) return WireError::Totals;
  if (op == Op::Seal && trigger != NoTrigger && trigger > accepted) return WireError::Trigger;

  // Validate all events in payload
  if (count > 0) {
    const uint8_t* p = src + HeaderBytes;
    for (uint32_t i = 0; i < count; ++i) {
      Event ev{};
      const WireError evErr = DecodeEvent(p, EventBytes, ev);
      if (evErr != WireError::None) {
        out = View{};
        return evErr;
      }
      if (ev.session != session) {
        out = View{};
        return WireError::Session;
      }
      p += EventBytes;
    }
  }

  out.header.op = op;
  out.header.count = count;
  out.header.session = session;
  out.header.ordinal = ordinal;
  out.header.trigger = trigger;
  out.header.attempted = attempted;
  out.header.lost = lost;
  out.header.capacity = capacity;
  out.header.reason = reason;
  out.header.accepted = accepted;
  out.events = (count > 0) ? (src + HeaderBytes) : nullptr;
  return WireError::None;
}

const char* ErrorName(WireError err) noexcept {
  switch (err) {
#define WV_NAME(x) case WireError::x: return #x
    WV_NAME(None);
    WV_NAME(Null);
    WV_NAME(Size);
    WV_NAME(Capacity);
    WV_NAME(Overlap);
    WV_NAME(Magic);
    WV_NAME(Version);
    WV_NAME(Header);
    WV_NAME(Opcode);
    WV_NAME(Flags);
    WV_NAME(Shape);
    WV_NAME(Session);
    WV_NAME(Ordinal);
    WV_NAME(Trigger);
    WV_NAME(Totals);
    WV_NAME(EventValue);
#undef WV_NAME
  }
  return "Unknown";
}

} // namespace warvk::host::recorder
