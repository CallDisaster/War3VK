#pragma once

#include "d3d9_migration_census_serialize.h"
#include <ostream>

namespace dxvk::d3d9::migration {

// Caller-module attribution for the migration census, kept apart from the
// frozen v2 snapshot. The public entry passes its own return address; it is
// classified against cold module ranges (Game.dll, this DLL) and never
// dereferenced. A public method inlined into another frame of this DLL would
// be attributed to that frame's caller, so the reader treats the split as an
// observation, not a proof of origin. Same bounded try-cell policy as v2.
enum class Caller : uint8_t { Game, Self, Other, Count };

inline constexpr std::size_t CallerCount = std::size_t(Caller::Count);
inline constexpr std::array<const char*, CallerCount> CallerNames = {{ "game", "self", "other" }};

struct ModuleRange {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  bool contains(uintptr_t address) const noexcept { return address >= begin && address < end; }
};

struct CallerRanges {
  ModuleRange game;
  ModuleRange self;
  Caller classify(const void* address) const noexcept {
    const auto value = reinterpret_cast<uintptr_t>(address);
    if (game.contains(value)) return Caller::Game;
    if (self.contains(value)) return Caller::Self;
    return Caller::Other;
  }
};

enum class StateKind : uint8_t { Render, TextureStage, Sampler };

inline constexpr std::size_t CallerTextureClassCapacity = 32;
inline constexpr std::size_t CallerFvfCapacity = 32;
// The current x64 development proxy's per-draw primitive bound.
inline constexpr uint32_t ProxyPrimitiveBound = 4096;

// One distinct requested texture shape class per caller; counts are attempts.
struct CallerTextureClass {
  uint32_t caller = 0;
  uint32_t kind = 0;
  uint32_t format = 0;
  uint32_t usage = 0;
  uint32_t pool = 0;
  uint32_t maxWidth = 0;
  uint32_t maxHeight = 0;
  uint32_t maxLevels = 0;
  uint64_t count = 0;
  uint64_t fullMipChainRequests = 0;
};

struct CallerAccount {
  std::array<uint64_t, ApiCount> apiCalls = {};
  std::array<uint64_t, std::size_t(DrawKind::Count)> drawKinds = {};
  std::array<uint64_t, 7> primitiveTypes = {};  // slot 0 and >6 counted as invalid
  uint64_t primitiveTypeInvalid = 0;
  uint64_t primitiveCountOverProxyBound = 0;
  uint32_t maxPrimitiveCount = 0;
  uint32_t maxVertexCount = 0;
  std::array<uint32_t, 8> renderStates = {};   // D3DRENDERSTATETYPE bits 0..255
  uint64_t textureStageStates = 0;             // D3DTEXTURESTAGESTATETYPE bits 0..63
  uint32_t samplerStates = 0;                  // D3DSAMPLERSTATETYPE bits 0..31
  uint64_t stateOutOfRange = 0;
  std::array<uint32_t, CallerFvfCapacity> fvfs = {};
  uint32_t fvfCount = 0;
  uint64_t fvfsDropped = 0;
};

struct CallerSnapshot {
  std::array<CallerAccount, CallerCount> callers = {};
  std::array<CallerTextureClass, CallerTextureClassCapacity> textureClasses = {};
  uint32_t textureClassCount = 0;
  uint64_t textureClassesDropped = 0;
  uint64_t invalidCallerApis = 0;
  uint64_t saturatedUpdates = 0;
};

class CallerCensus {
public:
  void noteCall(Api api, Caller caller) noexcept {
    if (std::size_t(api) >= ApiCount || std::size_t(caller) >= CallerCount) {
      add(m_snapshot.invalidCallerApis);
      return;
    }
    add(account(caller).apiCalls[std::size_t(api)]);
  }

  void noteState(Api api, Caller caller, StateKind kind, uint32_t index) noexcept {
    noteCall(api, caller);
    if (std::size_t(caller) >= CallerCount) return;
    auto& a = account(caller);
    const uint32_t bound = kind == StateKind::Render ? 256u : kind == StateKind::TextureStage ? 64u : 32u;
    if (index >= bound) {
      add(a.stateOutOfRange);
      return;
    }
    if (kind == StateKind::Render) a.renderStates[index / 32] |= 1u << (index % 32);
    else if (kind == StateKind::TextureStage) a.textureStageStates |= uint64_t(1) << index;
    else a.samplerStates |= 1u << index;
  }

  void noteFvf(Caller caller, uint32_t fvf) noexcept {
    noteCall(Api::SetFVF, caller);
    if (std::size_t(caller) >= CallerCount) return;
    auto& a = account(caller);
    for (uint32_t i = 0; i < a.fvfCount; ++i)
      if (a.fvfs[i] == fvf) return;
    if (a.fvfCount < a.fvfs.size()) a.fvfs[a.fvfCount++] = fvf;
    else add(a.fvfsDropped);
  }

  void noteDraw(Caller caller, DrawKind kind, uint32_t primitiveType,
      uint32_t primitiveCount, uint32_t vertexCount, bool vertexCountKnown) noexcept {
    const Api api = kind == DrawKind::Primitive ? Api::DrawPrimitive
      : kind == DrawKind::IndexedPrimitive ? Api::DrawIndexedPrimitive
      : kind == DrawKind::PrimitiveUP ? Api::DrawPrimitiveUP
      : kind == DrawKind::IndexedPrimitiveUP ? Api::DrawIndexedPrimitiveUP : Api::Count;
    noteCall(api, caller);
    if (std::size_t(caller) >= CallerCount || api == Api::Count) return;
    auto& a = account(caller);
    add(a.drawKinds[std::size_t(kind)]);
    if (primitiveType && primitiveType < a.primitiveTypes.size()) add(a.primitiveTypes[primitiveType]);
    else add(a.primitiveTypeInvalid);
    if (primitiveCount > ProxyPrimitiveBound) add(a.primitiveCountOverProxyBound);
    if (primitiveCount > a.maxPrimitiveCount) a.maxPrimitiveCount = primitiveCount;
    if (vertexCountKnown && vertexCount > a.maxVertexCount) a.maxVertexCount = vertexCount;
  }

  void noteTexture(Caller caller, const TextureFacts& facts) noexcept {
    const Api api = facts.kind == TextureKind::Texture2D ? Api::CreateTexture
      : facts.kind == TextureKind::Cube ? Api::CreateCubeTexture
      : facts.kind == TextureKind::Volume ? Api::CreateVolumeTexture : Api::Count;
    noteCall(api, caller);
    if (std::size_t(caller) >= CallerCount || api == Api::Count) return;
    auto& s = m_snapshot;
    CallerTextureClass* entry = nullptr;
    for (uint32_t i = 0; i < s.textureClassCount && !entry; ++i) {
      auto& c = s.textureClasses[i];
      if (c.caller == uint32_t(caller) && c.kind == uint32_t(facts.kind) && c.format == facts.format
          && c.usage == facts.usage && c.pool == facts.pool)
        entry = &c;
    }
    if (!entry) {
      if (s.textureClassCount == s.textureClasses.size()) {
        add(s.textureClassesDropped);
        return;
      }
      entry = &s.textureClasses[s.textureClassCount++];
      entry->caller = uint32_t(caller); entry->kind = uint32_t(facts.kind);
      entry->format = facts.format; entry->usage = facts.usage; entry->pool = facts.pool;
    }
    add(entry->count);
    if (!facts.levels) add(entry->fullMipChainRequests);
    if (facts.width > entry->maxWidth) entry->maxWidth = facts.width;
    if (facts.height > entry->maxHeight) entry->maxHeight = facts.height;
    if (facts.levels > entry->maxLevels) entry->maxLevels = facts.levels;
  }

  const CallerSnapshot& snapshot() const noexcept { return m_snapshot; }

private:
  CallerAccount& account(Caller caller) noexcept { return m_snapshot.callers[std::size_t(caller)]; }

  void add(uint64_t& value) noexcept {
    if (SaturatingAdd(value))
      SaturatingAdd(m_snapshot.saturatedUpdates);
  }

  CallerSnapshot m_snapshot;
};

struct CallerCensusCopy {
  CallerSnapshot values;
  uint64_t sequence = 0;
  uint32_t recordBusy = 0;  // independent atomic read, not coherent with values
};

// Production boundary: one bounded try-cell, no waiting or retry. A refused
// observation is counted (saturating) and changes nothing else.
class ConcurrentCallerCensus {
public:
  void setRanges(const CallerRanges& ranges) noexcept { m_ranges = ranges; }  // cold, before publication
  bool gameModuleKnown() const noexcept { return m_ranges.game.end > m_ranges.game.begin; }

  void noteCall(Api api, const void* caller) noexcept {
    Cell cell(*this);
    if (cell.acquired) m_census.noteCall(api, m_ranges.classify(caller));
  }
  void noteState(Api api, StateKind kind, uint32_t index, const void* caller) noexcept {
    Cell cell(*this);
    if (cell.acquired) m_census.noteState(api, m_ranges.classify(caller), kind, index);
  }
  void noteFvf(uint32_t fvf, const void* caller) noexcept {
    Cell cell(*this);
    if (cell.acquired) m_census.noteFvf(m_ranges.classify(caller), fvf);
  }
  void noteDraw(DrawKind kind, uint32_t primitiveType, uint32_t primitiveCount,
      uint32_t vertexCount, bool vertexCountKnown, const void* caller) noexcept {
    Cell cell(*this);
    if (cell.acquired)
      m_census.noteDraw(m_ranges.classify(caller), kind, primitiveType, primitiveCount, vertexCount, vertexCountKnown);
  }
  void noteTexture(const TextureFacts& facts, const void* caller) noexcept {
    Cell cell(*this);
    if (cell.acquired) m_census.noteTexture(m_ranges.classify(caller), facts);
  }

  bool trySnapshot(CallerCensusCopy& out) noexcept {
    Cell cell(*this);
    if (!cell.acquired || m_sequence == std::numeric_limits<uint64_t>::max()) return false;
    out.values = m_census.snapshot();
    out.sequence = ++m_sequence;
    out.recordBusy = m_recordBusy.load(std::memory_order_relaxed);
    return true;
  }

private:
  struct Cell {
    explicit Cell(ConcurrentCallerCensus& owner) noexcept
    : owner(owner), acquired(!owner.m_cell.test_and_set(std::memory_order_acquire)) {
      if (!acquired) {
        // Saturating refusal count; UINT32_MAX means "at least".
        auto value = owner.m_recordBusy.load(std::memory_order_relaxed);
        while (value != std::numeric_limits<uint32_t>::max()
            && !owner.m_recordBusy.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) { }
      }
    }
    ~Cell() noexcept { if (acquired) owner.m_cell.clear(std::memory_order_release); }
    Cell(const Cell&) = delete;
    Cell& operator=(const Cell&) = delete;
    ConcurrentCallerCensus& owner;
    const bool acquired;
  };

  std::atomic_flag m_cell = ATOMIC_FLAG_INIT;
  CallerRanges m_ranges;
  CallerCensus m_census;
  uint64_t m_sequence = 0;
  std::atomic<uint32_t> m_recordBusy { 0 };
};

static_assert(std::is_trivially_copyable<CallerSnapshot>::value);
static_assert(sizeof(CallerCensusCopy) <= 8192, "Copied caller census must remain fixed and small");

// Cold serialization of an owned copy only.
inline void WriteCallerCensus(std::ostream& out, const CallerCensusCopy& copy,
    uint64_t deviceOrdinal, const char* reason, bool gameModuleKnown) {
  const auto& s = copy.values;
  out << "WARVK_D3D9_CALLER_CENSUS {\"schema\":\"warvk-d3d9-caller-census/v1\""
      << ",\"deviceOrdinal\":" << deviceOrdinal
      << ",\"snapshotSequence\":" << copy.sequence
      << ",\"reason\":\"" << reason << "\""
      << ",\"attribution\":\"return-address-module\",\"gameModuleKnown\":" << (gameModuleKnown ? "true" : "false")
      << ",\"recordBusy\":" << copy.recordBusy
      << ",\"proxyPrimitiveBound\":" << ProxyPrimitiveBound
      << ",\"callers\":{";
  for (std::size_t c = 0; c < CallerCount; ++c) {
    const auto& a = s.callers[c];
    if (c) out << ',';
    out << '"' << CallerNames[c] << "\":{\"apiCalls\":{";
    for (std::size_t i = 0; i < ApiCount; ++i)
      out << (i ? "," : "") << '"' << ApiNames[i] << "\":" << a.apiCalls[i];
    out << "},\"drawKinds\":";
    WriteCensusArray(out, a.drawKinds);
    out << ",\"primitiveTypes\":";
    WriteCensusArray(out, a.primitiveTypes);
    out << ",\"primitiveTypeInvalid\":" << a.primitiveTypeInvalid
        << ",\"primitiveCountOverProxyBound\":" << a.primitiveCountOverProxyBound
        << ",\"maxPrimitiveCount\":" << a.maxPrimitiveCount
        << ",\"maxVertexCount\":" << a.maxVertexCount
        << ",\"renderStates\":[";
    bool first = true;
    for (uint32_t i = 0; i < 256; ++i)
      if (a.renderStates[i / 32] & (1u << (i % 32))) { out << (first ? "" : ",") << i; first = false; }
    out << "],\"textureStageStates\":[";
    first = true;
    for (uint32_t i = 0; i < 64; ++i)
      if (a.textureStageStates & (uint64_t(1) << i)) { out << (first ? "" : ",") << i; first = false; }
    out << "],\"samplerStates\":[";
    first = true;
    for (uint32_t i = 0; i < 32; ++i)
      if (a.samplerStates & (1u << i)) { out << (first ? "" : ",") << i; first = false; }
    out << "],\"stateOutOfRange\":" << a.stateOutOfRange << ",\"fvfs\":[";
    for (uint32_t i = 0; i < a.fvfCount; ++i) out << (i ? "," : "") << a.fvfs[i];
    out << "],\"fvfsDropped\":" << a.fvfsDropped << '}';
  }
  out << "},\"textureClasses\":[";
  for (uint32_t i = 0; i < s.textureClassCount; ++i) {
    const auto& t = s.textureClasses[i];
    out << (i ? "," : "") << "{\"caller\":\"" << CallerNames[t.caller < CallerCount ? t.caller : 2]
        << "\",\"kind\":" << t.kind << ",\"format\":" << t.format << ",\"usage\":" << t.usage
        << ",\"pool\":" << t.pool << ",\"count\":" << t.count << ",\"fullMipChainRequests\":" << t.fullMipChainRequests
        << ",\"maxWidth\":" << t.maxWidth << ",\"maxHeight\":" << t.maxHeight << ",\"maxLevels\":" << t.maxLevels << '}';
  }
  out << "],\"textureClassesDropped\":" << s.textureClassesDropped
      << ",\"invalidCallerApis\":" << s.invalidCallerApis
      << ",\"saturatedUpdates\":" << s.saturatedUpdates << '}';
}

}
