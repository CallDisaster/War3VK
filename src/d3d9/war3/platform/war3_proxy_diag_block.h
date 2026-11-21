#pragma once

// Proxy mode diagnostics: frame evidence (Ctrl+Shift+C), the performance log
// and the render statistics panel. The picture, the frame timing and the GPU
// are the x64 host's, the keyboard and the panel are the game's, so the two
// processes share one small block of plain numbers. The game creates it
// (Local\WarVKDiag-<game pid>),
// the host opens it by the process id its pipe authenticated.
//
// Five reserved game words carry the player's directional caster settings.
// The sixth is their publication sequence. The player's sun (the sun dial:
// the real shadow length and the colour curve) follows the frames, with its
// own sequence (version 3).
// Without a matching block the host uses the defaults for these controls.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
#include <cwchar>

namespace warvk::diag {

constexpr uint32_t Magic = 0x47444b57u; // "WKDG"
constexpr uint32_t Version = 6u;
constexpr uint32_t GameFrames = 512u;
constexpr uint32_t PathChars = 260u;

enum EvidenceState : int32_t {
  EvidenceOff = 0,      // the host has no recorder (or is not attached)
  EvidenceRecording,    // the ring is filling: a request saves it
  EvidenceTriggered,    // a request arrived: the frames after it are being kept
  EvidenceSaving,       // the pictures are being written
  EvidenceSaved,        // the package of the last request is complete
  EvidenceFailed,       // the last request could not be saved (evidenceError)
};

enum PerfState : int32_t { PerfIdle = 0, PerfRecording, PerfWriting, PerfWritten, PerfFailed };

// One game frame as the data layer saw it, written at its Present.
struct GameFrame {
  uint64_t qpc;          // QueryPerformanceCounter at the Present
  uint32_t frame;        // the data layer's frame number (0: the slot is empty)
  uint32_t intervalUs;   // since the Present before it
  // bit0 the world scene ran, bit1 BeforeUi was inserted, bit2 at Present,
  // bit3 the previous frame's camera was used, bit4 no camera, bit5 the
  // records were stopped.
  uint32_t flags;
  uint32_t cameraTier;
  uint32_t castersConsidered, castersSent, castersOmitted;
  uint32_t casterTerrain, casterDoodads, casterUnits, casterWorldObjects;
  uint32_t casterBytes;  // what the casters read from dynamic buffers
  float timeOfDay;
  uint32_t demandCasters;
  uint64_t demandBytes;
  uint32_t admittedBytes;
  uint32_t reserved;
};

static_assert(sizeof(GameFrame) == 80, "the same layout in both processes");

// What the host publishes at every Present for the render statistics panel.
struct HostStats {
  volatile uint32_t sequence;  // odd while the host writes; read again if it moved
  uint32_t width, height;
  uint32_t draws, drawBatches; // of the last frame
  uint32_t intervalUs, averageIntervalUs;
  uint32_t worldThisFrame;
  uint64_t frames, worldFrames;
  // The last frame with a world scene: what the shadow pass did with it.
  uint32_t shadowRan, casters, refused, copied, replayDraws, drawnCasters, cascades;
  uint32_t cascadeDrawn[4];
  uint32_t mapExecuted, receiverExecuted, completeMap, reuseMap;
  uint32_t earlyReturn, inputReject, replayRejected, replayLastReason, partialPrevented;
  uint32_t holdInvalidCsm, holdEmptyReplay, holdIdentityChurn, csmFallback, noCompleteMap, skippedNoResources;
  uint32_t taaMode, taaHistory, pointShadow, outline, volumetric, volumetricComposite, alphaPrepared;
  uint32_t effectsRecorded;    // bit0 SSAO, bit1 snow, bit2 AA, bit3 bloom
  uint32_t recordCasters, recordOmitted;
  uint32_t auditChecked, auditOdd;
  uint32_t reserved0;
  uint64_t copiedBytes, mapRenderSerial;
  // Storage the passes own, and the evidence ring.
  uint64_t shadowBytes, volumetricBytes, effectBytes, evidenceRingBytes;
  // The host process, refreshed a few times a second.
  uint64_t processPrivate, processPeakPrivate, processWorkingSet;
  // Its GPU memory. Local: the video memory heaps; shared: the others.
  uint64_t localHeap, localBudget, localCommitted, localAllocated, localUsed;
  uint64_t sharedCommitted, sharedAllocated, sharedUsed;
};

struct Block {
  uint32_t magic, version, bytes, gamePid;
  // ---- game -> host -------------------------------------------------------
  volatile int32_t evidenceRequest;  // counts Ctrl+Shift+C (and the panel's button)
  volatile int32_t perfWanted;       // 1 while the player records a performance log
  volatile int32_t perfSession;      // counts the starts
  volatile int32_t gameFrameHead;    // frames written; the newest is (head - 1) % GameFrames
  uint32_t gameWidth, gameHeight;
  uint32_t reservedGame[6];
  wchar_t outputDirectory[PathChars]; // where packages and logs go; the game creates it
  // ---- host -> game -------------------------------------------------------
  volatile int32_t hostPid;          // nonzero once the host attached
  volatile int32_t evidenceState;    // EvidenceState
  volatile int32_t evidenceRequestSeen; // the request evidenceState describes
  volatile int32_t evidenceFrames;   // frames in the ring
  volatile int32_t evidenceMillis;   // the time they span
  volatile int32_t evidenceSaved, evidenceTotal;
  volatile int32_t evidencePackages; // packages completed in this session
  volatile int32_t perfState;        // PerfState
  volatile int32_t perfRows;
  volatile int32_t perfSessionSeen;
  uint32_t reservedHost[5];
  wchar_t evidencePath[PathChars];   // the last package
  wchar_t perfPath[PathChars];       // the last performance log
  char evidenceError[160];
  HostStats stats;
  GameFrame frames[GameFrames];
  // ---- game -> host, version 3 ----------------------------------------------
  uint32_t sunStyle[2 + 4 * 6 + 1]; // SunStyleWords, then their sequence
  // ---- game -> host, version 4 ----------------------------------------------
  uint32_t water[1 + 8 + 1];        // LegacyWaterWords, then their sequence
  // Under the water sequence: the first five settings date from v5. The
  // sixth is caustics; writers clear the last two words for future settings.
  alignas(8) uint32_t waterInPlace[8];
};

constexpr size_t Version4Bytes = offsetof(Block, waterInPlace);
constexpr unsigned Version5TailWords = 5;
// sizeof the v5 struct included its trailing alignment padding.
constexpr size_t Version5Bytes = Version4Bytes +
    (Version5TailWords * sizeof(uint32_t) + alignof(Block) - 1) / alignof(Block) * alignof(Block);
static_assert(Version4Bytes < Version5Bytes && Version5Bytes < sizeof(Block));
static_assert(alignof(Block) == alignof(GameFrame) && alignof(Block) == alignof(HostStats));
static_assert(Version4Bytes == (offsetof(Block, water) + sizeof(Block::water) + alignof(Block) - 1)
                               / alignof(Block) * alignof(Block));

// The name both ends use.
inline void BlockName(wchar_t* out, size_t chars, unsigned long gamePid) {
  std::swprintf(out, chars, L"Local\\WarVKDiag-%lu", gamePid);
}

constexpr unsigned ClassCount = 5;
using CasterWords = std::array<uint32_t, ClassCount>;

inline uint32_t CasterWord(bool cast, float length) {
  const uint32_t q = std::isfinite(length) && length > 0
      ? std::max(1u, uint32_t(std::min(8.0f, length) * 65536.0f + 0.5f)) : 0;
  return 0x80000000u | (cast ? 0x40000000u : 0u) | q;
}
template<class Classes>
inline void ReadCasterWords(Classes& classes, const CasterWords& words) {
  for (unsigned i = 0; i < ClassCount; ++i) {
    const uint32_t w = words[i], q = w & 0x3fffffffu;
    classes[i] = {};
    if ((w & 0x80000000u) && q <= 8u * 65536u) {
      classes[i].castShadow = (w & 0x40000000u) != 0;
      classes[i].maxShadowLength = float(q) / 65536.0f;
    }
  }
}

// The player's sun: a word of switches (bit 31: written; bit 0 the real
// shadow length, bit 1 the colour curve, bit 2 the path; bits 8..23 the
// path's noon point, -1..1 in 65535 steps), the noon altitude, then per key
// the colour and the brightness, as float bits.
constexpr unsigned SunStyleWordCount = 2 + 4 * 6;
using SunStyleWords = std::array<uint32_t, SunStyleWordCount>;
static_assert(sizeof(Block::sunStyle) == (SunStyleWordCount + 1) * 4, "the sun words and their sequence");

inline uint32_t FloatWord(float v) { uint32_t w; std::memcpy(&w, &v, 4); return w; }
inline float WordFloat(uint32_t w) { float v; std::memcpy(&v, &w, 4); return v; }

template<class Style>
inline SunStyleWords EncodeSunStyle(const Style& s) {
  SunStyleWords w{};
  const float north = std::isfinite(s.pathNorth) ? std::clamp(s.pathNorth, -1.0f, 1.0f) : -0.5f;
  const uint32_t pathWord = uint32_t(std::lround((north + 1.0f) * 0.5f * 65535.0f)) & 0xFFFFu;
  w[0] = 0x80000000u | (s.realShadowLength ? 1u : 0u) | (s.customTint ? 2u : 0u) |
         (s.pathSet ? 4u : 0u) | (pathWord << 8);
  w[1] = FloatWord(s.noonAltitudeDeg);
  for (unsigned k = 0; k < 6; ++k) {
    w[2 + 4 * k] = FloatWord(s.tint[k].r);
    w[3 + 4 * k] = FloatWord(s.tint[k].g);
    w[4 + 4 * k] = FloatWord(s.tint[k].b);
    w[5 + 4 * k] = FloatWord(s.tint[k].intensity);
  }
  return w;
}

// The defaults for a block never written; the default for a number that is
// not finite or out of its range.
template<class Style>
inline void DecodeSunStyle(Style& s, const SunStyleWords& w) {
  s = Style{};
  if (!(w[0] & 0x80000000u))
    return;
  static_assert(Style::KeyCount == 6, "six keys on the wire");
  const auto value = [](uint32_t word, float lo, float hi, float fallback) {
    const float v = WordFloat(word);
    return std::isfinite(v) && v >= lo && v <= hi ? v : fallback;
  };
  s.realShadowLength = (w[0] & 1u) != 0;
  s.customTint = (w[0] & 2u) != 0;
  s.pathSet = (w[0] & 4u) != 0;
  s.pathNorth = float((w[0] >> 8) & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
  s.noonAltitudeDeg = value(w[1], 10.0f, 90.0f, s.noonAltitudeDeg);
  for (unsigned k = 0; k < 6; ++k) {
    auto& key = s.tint[k];
    key.r = value(w[2 + 4 * k], 0.0f, 1.0f, key.r);
    key.g = value(w[3 + 4 * k], 0.0f, 1.0f, key.g);
    key.b = value(w[4 + 4 * k], 0.0f, 1.0f, key.b);
    key.intensity = value(w[5 + 4 * k], 0.0f, 3.0f, key.intensity);
  }
}

// The player's water: a word of switches (bit 31: written; bit 0 the surface
// pass), then the shadow's fade start and end, the surface's shadow, the
// reflection, the glint, the ripples' size and speed and the clarity, as
// float bits.
constexpr unsigned LegacyWaterWords = 1 + 8;
constexpr unsigned WaterTailSettings = 6;
constexpr unsigned WaterTailWords = 8;
constexpr unsigned WaterWordCount = LegacyWaterWords + WaterTailWords;
constexpr uint32_t InPlaceWaterWordsPresent = 0x40000000u;
using WaterWords = std::array<uint32_t, WaterWordCount>;
static_assert(sizeof(Block::water) == (LegacyWaterWords + 1) * 4, "the version 4 water sequence stays put");
static_assert(sizeof(Block::waterInPlace) == (WaterWordCount - LegacyWaterWords) * 4);

template<class Water>
inline WaterWords EncodeWater(const Water& s) {
  WaterWords w{};
  w[0] = 0x80000000u | InPlaceWaterWordsPresent | (s.surfaceEnabled ? 1u : 0u);
  w[1] = FloatWord(s.shadowFadeStart);
  w[2] = FloatWord(s.shadowFadeEnd);
  w[3] = FloatWord(s.surfaceShadow);
  w[4] = FloatWord(s.reflection);
  w[5] = FloatWord(s.specular);
  w[6] = FloatWord(s.waveScale);
  w[7] = FloatWord(s.waveSpeed);
  w[8] = FloatWord(s.clarity);
  w[9] = uint32_t(s.inPlace);
  w[10] = FloatWord(s.style);
  w[11] = FloatWord(s.depthColor);
  w[12] = FloatWord(s.foam);
  w[13] = FloatWord(s.refraction);
  w[14] = FloatWord(s.caustics);
  return w;
}

// The defaults for a block never written; the default for a number that is
// not finite or out of its range.
template<class Water>
inline void DecodeWater(Water& s, const WaterWords& w) {
  s = Water{};
  if (!(w[0] & 0x80000000u))
    return;
  const auto value = [](uint32_t word, float lo, float hi, float fallback) {
    const float v = WordFloat(word);
    return std::isfinite(v) && v >= lo && v <= hi ? v : fallback;
  };
  s.surfaceEnabled = (w[0] & 1u) != 0;
  s.shadowFadeStart = value(w[1], 0.0f, 512.0f, s.shadowFadeStart);
  s.shadowFadeEnd = value(w[2], 1.0f, 1024.0f, s.shadowFadeEnd);
  s.surfaceShadow = value(w[3], 0.0f, 1.0f, s.surfaceShadow);
  s.reflection = value(w[4], 0.0f, 1.0f, s.reflection);
  s.specular = value(w[5], 0.0f, 2.0f, s.specular);
  s.waveScale = value(w[6], 0.0f, 2.0f, s.waveScale);
  s.waveSpeed = value(w[7], 0.0f, 3.0f, s.waveSpeed);
  s.clarity = value(w[8], 0.0f, 1.0f, s.clarity);
  if (w[0] & InPlaceWaterWordsPresent) {
    if (w[9] <= 2) s.inPlace = int(w[9]);
    s.style = value(w[10], 0.0f, 1.0f, s.style);
    s.depthColor = value(w[11], 0.0f, 2.0f, s.depthColor);
    s.foam = value(w[12], 0.0f, 2.0f, s.foam);
    s.refraction = value(w[13], 0.0f, 2.0f, s.refraction);
    s.caustics = value(w[14], 0.0f, 2.0f, s.caustics);
  }
}

#ifdef _WIN32
// Both host views use the same checked extent. Old producers own only their
// declared prefix, even when the mapping occupies a whole system page.
inline Block* MapCompatibleBlock(HANDLE mapping, DWORD pid) {
  const auto* header = static_cast<const uint32_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 16));
  if (!header) return nullptr;
  const uint32_t version = header[1];
  const size_t bytes = version == 4u ? Version4Bytes : version == 5u ? Version5Bytes :
                       version == Version ? sizeof(Block) : 0;
  const bool match = bytes && header[0] == Magic && header[2] == bytes && header[3] == pid;
  UnmapViewOfFile(header);
  if (!match) return nullptr;
  auto* b = static_cast<Block*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, bytes));
  if (b && (b->magic != Magic || b->version != version || b->bytes != bytes || b->gamePid != pid)) {
    UnmapViewOfFile(b);
    return nullptr;
  }
  return b;
}

// Independent of the evidence recorder, which first attaches at Present.
class CasterSettingsView {
public:
  CasterSettingsView() = default;
  CasterSettingsView(const CasterSettingsView&) = delete;
  ~CasterSettingsView() { if (block) UnmapViewOfFile(block); }
  void open(DWORD pid) {
    if (block || !pid) return;
    wchar_t name[64];
    BlockName(name, 64, pid);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!mapping) return;
    block = MapCompatibleBlock(mapping, pid);
    CloseHandle(mapping);
    if (block)
      waterTailWords = block->version == 4u ? 0u : block->version == 5u ? Version5TailWords : WaterTailWords;
  }
  void publish(const CasterWords& words) {
    if (!block) return;
    auto* p = reinterpret_cast<volatile LONG*>(block->reservedGame);
    InterlockedIncrement(p + ClassCount);
    for (unsigned i = 0; i < ClassCount; ++i) p[i] = LONG(words[i]);
    InterlockedIncrement(p + ClassCount);
  }
  bool perfWanted() const { return block && block->perfWanted; }
  void publishSun(const SunStyleWords& words) {
    if (!block) return;
    auto* p = reinterpret_cast<volatile LONG*>(block->sunStyle);
    InterlockedIncrement(p + SunStyleWordCount);
    for (unsigned i = 0; i < SunStyleWordCount; ++i) p[i] = LONG(words[i]);
    InterlockedIncrement(p + SunStyleWordCount);
  }
  SunStyleWords readSun() {
    if (!block) return {};
    auto* p = reinterpret_cast<volatile LONG*>(block->sunStyle);
    const LONG seq = InterlockedCompareExchange(p + SunStyleWordCount, 0, 0);
    if (!(seq & 1)) {
      SunStyleWords next;
      for (unsigned i = 0; i < SunStyleWordCount; ++i) next[i] = uint32_t(p[i]);
      if (seq == InterlockedCompareExchange(p + SunStyleWordCount, 0, 0)) lastSun = next;
    }
    return lastSun;
  }
  void publishWater(const WaterWords& words) {
    if (!block) return;
    auto* p = reinterpret_cast<volatile LONG*>(block->water);
    InterlockedIncrement(p + LegacyWaterWords);
    for (unsigned i = 0; i < LegacyWaterWords; ++i) p[i] = LONG(words[i]);
    if (waterTailWords) {
      auto* tail = reinterpret_cast<volatile LONG*>(block->waterInPlace);
      for (unsigned i = 0; i < waterTailWords; ++i)
        tail[i] = i < WaterTailSettings ? LONG(words[LegacyWaterWords + i]) : 0;
    } else p[0] = LONG(words[0] & ~InPlaceWaterWordsPresent);
    InterlockedIncrement(p + LegacyWaterWords);
  }
  WaterWords readWater() {
    if (!block) return {};
    auto* p = reinterpret_cast<volatile LONG*>(block->water);
    const LONG seq = InterlockedCompareExchange(p + LegacyWaterWords, 0, 0);
    if (!(seq & 1)) {
      WaterWords next{};
      for (unsigned i = 0; i < LegacyWaterWords; ++i) next[i] = uint32_t(p[i]);
      if (waterTailWords) {
        // A missing v5 caustics word takes the settings default, even though
        // zero is an intentional and valid value when the word is present.
        next[14] = 0x7fc00000u;
        const auto* tail = reinterpret_cast<const volatile LONG*>(block->waterInPlace);
        for (unsigned i = 0; i < std::min(waterTailWords, WaterTailSettings); ++i)
          next[LegacyWaterWords + i] = uint32_t(tail[i]);
      } else next[0] &= ~InPlaceWaterWordsPresent;
      if (seq == InterlockedCompareExchange(p + LegacyWaterWords, 0, 0)) lastWater = next;
    }
    return lastWater;
  }
  CasterWords read() {
    if (!block) return {};
    auto* p = reinterpret_cast<volatile LONG*>(block->reservedGame);
    const LONG seq = InterlockedCompareExchange(p + ClassCount, 0, 0);
    if (!(seq & 1)) {
      CasterWords next;
      for (unsigned i = 0; i < ClassCount; ++i) next[i] = uint32_t(p[i]);
      if (seq == InterlockedCompareExchange(p + ClassCount, 0, 0)) last = next;
    }
    return last;
  }
private:
  Block* block = nullptr;
  unsigned waterTailWords = 0;
  CasterWords last{};
  SunStyleWords lastSun{};
  WaterWords lastWater{};
};
#endif

} // namespace warvk::diag
