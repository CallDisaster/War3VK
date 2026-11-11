#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dxvk::d3d9::migration {

// Diagnostic observations, not a D3D9 implementation, IPC contract, admission
// decision, or proof that any command reached the GPU. Census is the pure,
// externally serialized bookkeeping implementation. ConcurrentCensus is the
// production boundary: owned argument/local-key values, one bounded try-cell,
// and copied snapshots. Neither implementation discovers pointers or performs
// heap allocation, callbacks, device/GPU calls, logging, or file I/O.
enum class Api : uint8_t {
  Reset, Present, Clear, BeginScene, EndScene,
  CreateTexture, CreateVolumeTexture, CreateCubeTexture,
  CreateVertexBuffer, CreateIndexBuffer, CreateRenderTarget,
  CreateDepthStencilSurface, CreateOffscreenPlainSurface,
  UpdateSurface, UpdateTexture, GetRenderTargetData, StretchRect,
  SetRenderTarget, SetDepthStencilSurface,
  SetTransform, SetViewport, SetMaterial, SetLight, LightEnable,
  SetRenderState, SetTexture, SetTextureStageState, SetSamplerState,
  SetScissorRect, CreateStateBlock, BeginStateBlock, EndStateBlock,
  CreateVertexDeclaration, SetVertexDeclaration, SetFVF,
  CreateVertexShader, CreatePixelShader, SetVertexShader, SetPixelShader,
  SetVertexShaderConstantF, SetPixelShaderConstantF,
  SetStreamSource, SetIndices,
  DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP,
  DrawIndexedPrimitiveUP, ProcessVertices,
  Count,
};

inline constexpr std::size_t ApiCount = std::size_t(Api::Count);
static_assert(ApiCount == 48);

inline constexpr std::array<const char*, ApiCount> ApiNames = {{
  "Reset", "Present", "Clear", "BeginScene", "EndScene",
  "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture",
  "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget",
  "CreateDepthStencilSurface", "CreateOffscreenPlainSurface",
  "UpdateSurface", "UpdateTexture", "GetRenderTargetData", "StretchRect",
  "SetRenderTarget", "SetDepthStencilSurface",
  "SetTransform", "SetViewport", "SetMaterial", "SetLight", "LightEnable",
  "SetRenderState", "SetTexture", "SetTextureStageState", "SetSamplerState",
  "SetScissorRect", "CreateStateBlock", "BeginStateBlock", "EndStateBlock",
  "CreateVertexDeclaration", "SetVertexDeclaration", "SetFVF",
  "CreateVertexShader", "CreatePixelShader", "SetVertexShader", "SetPixelShader",
  "SetVertexShaderConstantF", "SetPixelShaderConstantF",
  "SetStreamSource", "SetIndices",
  "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP",
  "DrawIndexedPrimitiveUP", "ProcessVertices",
}};

inline constexpr const char* ApiName(Api api) noexcept {
  return std::size_t(api) < ApiCount ? ApiNames[std::size_t(api)] : "Unclassified";
}

enum class DrawKind : uint8_t {
  Unknown, Primitive, IndexedPrimitive, PrimitiveUP, IndexedPrimitiveUP, Count,
};

// There is currently no proven caller-origin discriminator. Do not rename
// observations as "game draws": local WarVK calls may enter the same methods.
enum class SourceAttribution : uint8_t { Unknown };

enum DrawKnown : uint32_t {
  PrimitiveTypeKnown    = 1u << 0,
  PrimitiveCountKnown   = 1u << 1,
  ShaderMaskKnown       = 1u << 2,
  FvfKnown              = 1u << 3,
  StreamStrideKnown     = 1u << 4,
  TextureStageMaskKnown = 1u << 5,
  VertexCountKnown      = 1u << 6,
  IndexFormatKnown      = 1u << 7,
};

inline constexpr std::size_t DrawFieldCount = 8;
inline constexpr uint32_t AllDrawFieldsKnown = (1u << DrawFieldCount) - 1u;

struct DrawFacts {
  DrawKind kind = DrawKind::Unknown;
  SourceAttribution source = SourceAttribution::Unknown;
  uint32_t knownMask = 0;
  uint32_t primitiveType = 0;     // D3DPRIMITIVETYPE numeric value, if known
  uint32_t primitiveCount = 0;
  uint32_t shaderMask = 0;        // bit 0: VS bound, bit 1: PS bound; 0 is valid
  uint32_t fvf = 0;
  uint32_t streamStride = 0;      // caller-selected stream, not all streams
  uint32_t textureStageMask = 0;  // caller's observed stages; no discovery here
  uint32_t vertexCount = 0;
  uint32_t indexFormat = 0;       // D3DFORMAT numeric value, if known
};

enum class TextureKind : uint8_t { Unknown, Texture2D, Cube, Volume, Count };

enum TextureKnown : uint32_t {
  WidthKnown  = 1u << 0,
  HeightKnown = 1u << 1,
  DepthKnown  = 1u << 2,
  LevelsKnown = 1u << 3,
  FormatKnown = 1u << 4,
  UsageKnown  = 1u << 5,
  PoolKnown   = 1u << 6,
};

inline constexpr std::size_t TextureFieldCount = 7;
inline constexpr uint32_t AllTextureFieldsKnown = (1u << TextureFieldCount) - 1u;

struct TextureFacts {
  TextureKind kind = TextureKind::Unknown;
  uint32_t knownMask = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t levels = 0; // requested 0 means full mip chain; not "unknown"
  uint32_t format = 0;
  uint32_t usage = 0;
  uint32_t pool = 0;
};

inline constexpr std::size_t FactSampleCapacity = 32;
inline constexpr std::size_t KeySampleCapacity = 32;
inline constexpr std::size_t VsKeyWords = 5;
inline constexpr std::size_t FsKeyWords = 16;

template<std::size_t Words, std::size_t Capacity>
struct KeySamples {
  using Key = std::array<uint32_t, Words>;
  std::array<Key, Capacity> keys = {};
  uint32_t storedUnique = 0;
  uint64_t observed = 0;
  uint64_t duplicates = 0;
  // Counts observations refused after capacity, NOT exact unseen unique keys.
  // A repeated unretained key is counted again because no space remains for it.
  uint64_t dropped = 0;
  uint64_t missing = 0;
};

using VsKeySamples = KeySamples<VsKeyWords, KeySampleCapacity>;
using FsKeySamples = KeySamples<FsKeyWords, KeySampleCapacity>;

struct Snapshot {
  uint32_t schemaVersion = 1;
  uint64_t observedCalls = 0;
  std::array<uint64_t, ApiCount> apiCalls = {};
  uint64_t unclassifiedCalls = 0;
  uint64_t drawAttempts = 0;
  std::array<uint64_t, std::size_t(DrawKind::Count)> drawKinds = {};
  uint64_t invalidDrawKinds = 0;
  // D3D primitive values 1..6 have their own slots. Slot zero is a known,
  // invalid API value, not missing data. Missing/other values stay separate.
  std::array<uint64_t, 7> primitiveTypes = {};
  uint64_t primitiveTypeMissing = 0;
  uint64_t primitiveTypeInvalid = 0;
  std::array<uint64_t, 4> shaderPairs = {};
  uint64_t shaderPairMissing = 0;
  uint64_t shaderPairInvalid = 0;
  uint64_t unknownSourceDrawAttempts = 0;
  uint64_t invalidSourceAttributions = 0;
  std::array<uint64_t, DrawFieldCount> drawMissingFields = {};
  uint64_t drawUnknownMaskBits = 0;
  uint32_t maxPrimitiveCount = 0;
  uint32_t maxStreamStride = 0;
  uint32_t maxVertexCount = 0;
  uint32_t fvfBitUnion = 0;
  uint32_t textureStageMaskUnion = 0;
  std::array<DrawFacts, FactSampleCapacity> drawSamples = {};
  uint32_t drawSampleCount = 0;
  uint64_t drawSamplesDropped = 0;
  uint64_t observedTextureRequests = 0;
  std::array<uint64_t, std::size_t(TextureKind::Count)> textureKinds = {};
  uint64_t invalidTextureKinds = 0;
  std::array<uint64_t, TextureFieldCount> textureMissingFields = {};
  uint64_t textureUnknownMaskBits = 0;
  uint32_t maxTextureWidth = 0;
  uint32_t maxTextureHeight = 0;
  uint32_t maxTextureDepth = 0;
  uint32_t maxRequestedMipLevels = 0;
  uint64_t fullMipChainRequests = 0;
  std::array<TextureFacts, FactSampleCapacity> textureSamples = {};
  uint32_t textureSampleCount = 0;
  uint64_t textureSamplesDropped = 0;
  VsKeySamples vsKeys;
  FsKeySamples fsKeys;
  // Number of counter-update attempts whose exact result was unrepresentable.
  // Once this is nonzero, arithmetic equalities may only be lower bounds.
  uint64_t saturatedUpdates = 0;
};

// Shared production helper, public so its UINT64_MAX boundary can be tested
// without exposing mutable diagnostic state or looping 2^64 times.
inline bool SaturatingAdd(uint64_t& value, uint64_t amount = 1) noexcept {
  const uint64_t max = std::numeric_limits<uint64_t>::max();
  if (amount > max - value) {
    value = max;
    return true;
  }
  value += amount;
  return false;
}

class Census {
public:
  // Cold reset only. The surrounding owner establishes the observation window;
  // this component does not infer map/device lifetime or caller identity.
  void reset() noexcept { m_snapshot = Snapshot{}; }

  void noteCall(Api api) noexcept {
    add(m_snapshot.observedCalls);
    if (std::size_t(api) < ApiCount)
      add(m_snapshot.apiCalls[std::size_t(api)]);
    else
      add(m_snapshot.unclassifiedCalls);
  }

  // Call at the public attempt boundary. Independent from noteCall so the
  // integration can distinguish API entry totals from sampled draw facts.
  void noteDrawAttempt(const DrawFacts& facts) noexcept {
    auto& s = m_snapshot;
    add(s.drawAttempts);
    if (std::size_t(facts.kind) < s.drawKinds.size())
      add(s.drawKinds[std::size_t(facts.kind)]);
    else
      add(s.invalidDrawKinds);
    add(s.unknownSourceDrawAttempts);
    if (facts.source != SourceAttribution::Unknown)
      add(s.invalidSourceAttributions);
    noteMissingFields(facts.knownMask, s.drawMissingFields);
    if (facts.knownMask & ~AllDrawFieldsKnown)
      add(s.drawUnknownMaskBits);
    if (!(facts.knownMask & PrimitiveTypeKnown)) {
      add(s.primitiveTypeMissing);
    } else if (facts.primitiveType < s.primitiveTypes.size()) {
      add(s.primitiveTypes[facts.primitiveType]);
      if (!facts.primitiveType)
        add(s.primitiveTypeInvalid);
    } else {
      add(s.primitiveTypeInvalid);
    }
    if (!(facts.knownMask & ShaderMaskKnown))
      add(s.shaderPairMissing);
    else if (facts.shaderMask < s.shaderPairs.size())
      add(s.shaderPairs[facts.shaderMask]);
    else
      add(s.shaderPairInvalid);
    if (facts.knownMask & PrimitiveCountKnown)
      maximize(s.maxPrimitiveCount, facts.primitiveCount);
    if (facts.knownMask & StreamStrideKnown)
      maximize(s.maxStreamStride, facts.streamStride);
    if (facts.knownMask & VertexCountKnown)
      maximize(s.maxVertexCount, facts.vertexCount);
    if (facts.knownMask & FvfKnown)
      s.fvfBitUnion |= facts.fvf;
    if (facts.knownMask & TextureStageMaskKnown)
      s.textureStageMaskUnion |= facts.textureStageMask;
    if (s.drawSampleCount < s.drawSamples.size())
      s.drawSamples[s.drawSampleCount++] = facts;
    else
      add(s.drawSamplesDropped);
  }

  // Requested parameters only; this is not a successful allocation census.
  void noteTextureRequest(const TextureFacts& facts) noexcept {
    auto& s = m_snapshot;
    add(s.observedTextureRequests);
    if (std::size_t(facts.kind) < s.textureKinds.size())
      add(s.textureKinds[std::size_t(facts.kind)]);
    else
      add(s.invalidTextureKinds);
    noteMissingFields(facts.knownMask, s.textureMissingFields);
    if (facts.knownMask & ~AllTextureFieldsKnown)
      add(s.textureUnknownMaskBits);
    if (facts.knownMask & WidthKnown)
      maximize(s.maxTextureWidth, facts.width);
    if (facts.knownMask & HeightKnown)
      maximize(s.maxTextureHeight, facts.height);
    if (facts.knownMask & DepthKnown)
      maximize(s.maxTextureDepth, facts.depth);
    if (facts.knownMask & LevelsKnown) {
      maximize(s.maxRequestedMipLevels, facts.levels);
      if (!facts.levels)
        add(s.fullMipChainRequests);
    }
    if (s.textureSampleCount < s.textureSamples.size())
      s.textureSamples[s.textureSampleCount++] = facts;
    else
      add(s.textureSamplesDropped);
  }

  // Keys are the local build's diagnostic representation, NOT a portable ABI
  // or cross-process wire layout. Values must already be owned local words.
  void noteFixedFunctionVsKey(const VsKeySamples::Key& key) noexcept {
    noteKey(m_snapshot.vsKeys, key);
  }
  void noteFixedFunctionFsKey(const FsKeySamples::Key& key) noexcept {
    noteKey(m_snapshot.fsKeys, key);
  }
  void noteFixedFunctionVsKeyMissing() noexcept {
    add(m_snapshot.vsKeys.observed);
    add(m_snapshot.vsKeys.missing);
  }
  void noteFixedFunctionFsKeyMissing() noexcept {
    add(m_snapshot.fsKeys.observed);
    add(m_snapshot.fsKeys.missing);
  }

  // Pure-component borrowed read; its external owner must serialize access.
  // Production callers use ConcurrentCensus::trySnapshot instead. A borrowed
  // reference to this state never escapes that wrapper's acquired try-cell.
  const Snapshot& snapshot() const noexcept { return m_snapshot; }

private:
  friend struct ConcurrentCensusTestAccess;

  void add(uint64_t& value) noexcept {
    if (SaturatingAdd(value))
      SaturatingAdd(m_snapshot.saturatedUpdates);
  }

  static void maximize(uint32_t& maximum, uint32_t value) noexcept {
    if (value > maximum)
      maximum = value;
  }

  template<std::size_t Count>
  void noteMissingFields(uint32_t knownMask,
                        std::array<uint64_t, Count>& missing) noexcept {
    for (std::size_t i = 0; i < Count; ++i)
      if (!(knownMask & (uint32_t(1) << i)))
        add(missing[i]);
  }

  template<std::size_t Words, std::size_t Capacity>
  void noteKey(KeySamples<Words, Capacity>& samples,
               const std::array<uint32_t, Words>& key) noexcept {
    add(samples.observed);
    for (std::size_t i = 0; i < samples.storedUnique; ++i) {
      if (samples.keys[i] == key) {
        add(samples.duplicates);
        return;
      }
    }
    if (samples.storedUnique < Capacity)
      samples.keys[samples.storedUnique++] = key;
    else
      add(samples.dropped);
  }

  Snapshot m_snapshot;
};

static_assert(sizeof(Census) <= 8192, "Census must remain fixed and small");

struct CensusCopy {
  Snapshot values;
  uint64_t sequence = 0;
};

// Independent, non-coherent observations: these atomics do NOT describe the
// same instant as CensusCopy or even as each other. A zero observation never
// certifies losslessness. Transaction kinds have different units, so accepted
// calls and these refusal observations must not be summed as total API calls.
struct ObservationRefusals {
  uint32_t recordBusy = 0;
  uint32_t snapshotBusy = 0;
  // Sticky boolean (0 or 1), NOT a number of saturated increments. A set bit
  // forbids exact/lossless certification, including after the counter wraps.
  uint32_t counterSaturated = 0;
};

class ConcurrentCensus {
public:
  // There is no reset: the containing device owns this observation lifetime.
  // Ordinary API calls only. Grouped draw/texture/Present enums are recorded as
  // unclassified misuse, never as a valid half transaction. true means that
  // the observation (including error evidence) was accepted, not API success.
  bool noteCall(Api api) noexcept {
    TryCell cell(m_cell);
    if (!cell.acquired())
      return recordRefused();
    m_census.noteCall(isGroupedApi(api) ? Api::Count : api);
    observeDataSaturation();
    return true;
  }

  // API and facts have one acceptance boundary; refusal changes neither.
  // Unknown/invalid kinds remain unclassified evidence and cannot certify a
  // valid trace. Facts must already be caller-owned argument/local POD values.
  bool noteDraw(const DrawFacts& facts) noexcept {
    TryCell cell(m_cell);
    if (!cell.acquired())
      return recordRefused();
    m_census.noteCall(drawApi(facts.kind));
    m_census.noteDrawAttempt(facts);
    observeDataSaturation();
    return true;
  }

  bool noteTexture(const TextureFacts& facts) noexcept {
    TryCell cell(m_cell);
    if (!cell.acquired())
      return recordRefused();
    m_census.noteCall(textureApi(facts.kind));
    m_census.noteTextureRequest(facts);
    observeDataSaturation();
    return true;
  }

  bool noteVsKey(const VsKeySamples::Key& key) noexcept {
    TryCell cell(m_cell);
    if (!cell.acquired())
      return recordRefused();
    m_census.noteFixedFunctionVsKey(key);
    observeDataSaturation();
    return true;
  }

  bool noteFsKey(const FsKeySamples::Key& key) noexcept {
    TryCell cell(m_cell);
    if (!cell.acquired())
      return recordRefused();
    m_census.noteFixedFunctionFsKey(key);
    observeDataSaturation();
    return true;
  }

  // The accepted API counter IS the Present count: there is no second mutable
  // counter which can diverge from it. A refused call leaves milestone intact.
  bool notePresent(bool& milestone) noexcept {
    TryCell cell(m_cell);
    if (!cell.acquired())
      return recordRefused();
    m_census.noteCall(Api::Present);
    const auto count = m_census.snapshot().apiCalls[std::size_t(Api::Present)];
    milestone = count == 1 || count == 64 || count == 256 || count == 1024
             || count == 4096 || count == 16384 || count == 65536;
    observeDataSaturation();
    return true;
  }

  // All coherent state and its sequence are copied under one cell. Only the
  // returned copy may be serialized after release. Failure (busy or exhausted
  // sequence) leaves every output byte unchanged. No waiting or retry occurs.
  bool trySnapshot(CensusCopy& out) noexcept {
    TryCell cell(m_cell);
    if (!cell.acquired()) {
      m_snapshotBusy.increment();
      return false;
    }
    if (m_sequence == std::numeric_limits<uint64_t>::max()) {
      m_counterSaturated.store(1, std::memory_order_release);
      return false;
    }
    out.values = m_census.snapshot();
    out.values.schemaVersion = 2;
    out.sequence = ++m_sequence;
    return true;
  }

  ObservationRefusals observationRefusals() const noexcept {
    // Loading a sticky flag first makes the completed-overflow interpretation
    // explicit. A concurrent overflow may still yield mixed moments; that is
    // intentional non-coherence, not an exact paired snapshot.
    const auto recordOverflow = m_recordBusy.overflow.load(std::memory_order_acquire);
    const auto snapshotOverflow = m_snapshotBusy.overflow.load(std::memory_order_acquire);
    ObservationRefusals result;
    result.recordBusy = recordOverflow ? std::numeric_limits<uint32_t>::max()
      : m_recordBusy.count.load(std::memory_order_relaxed);
    result.snapshotBusy = snapshotOverflow ? std::numeric_limits<uint32_t>::max()
      : m_snapshotBusy.count.load(std::memory_order_relaxed);
    result.counterSaturated = recordOverflow | snapshotOverflow
      | m_counterSaturated.load(std::memory_order_acquire);
    return result;
  }

private:
  // The friend has no production definition and adds no runtime test callback.
  friend struct ConcurrentCensusTestAccess;

  class TryCell {
  public:
    explicit TryCell(std::atomic_flag& cell) noexcept
    : m_cell(cell), m_acquired(!cell.test_and_set(std::memory_order_acquire)) { }
    ~TryCell() noexcept {
      if (m_acquired)
        m_cell.clear(std::memory_order_release);
    }
    bool acquired() const noexcept { return m_acquired; }
    TryCell(const TryCell&) = delete;
    TryCell& operator=(const TryCell&) = delete;
  private:
    std::atomic_flag& m_cell;
    bool m_acquired;
  };

  struct BusyCounter {
    // One fixed-width fetch_add, no compare/exchange or retry loop. On wrap,
    // latch overflow separately. Once that completed overflow is observed,
    // exported count is UINT32_MAX forever, not the wrapped raw counter. The
    // unsynchronized moment between fetch_add and latch is NOT exact evidence.
    void increment() noexcept {
      if (count.fetch_add(1, std::memory_order_relaxed)
            == std::numeric_limits<uint32_t>::max())
        overflow.store(1, std::memory_order_release);
    }
    std::atomic<uint32_t> count { 0 };
    std::atomic<uint32_t> overflow { 0 };
  };

  bool recordRefused() noexcept {
    m_recordBusy.increment();
    return false;
  }

  void observeDataSaturation() noexcept {
    if (m_census.snapshot().saturatedUpdates)
      m_counterSaturated.store(1, std::memory_order_release);
  }

  static bool isGroupedApi(Api api) noexcept {
    return api == Api::Present || api == Api::DrawPrimitive
      || api == Api::DrawIndexedPrimitive || api == Api::DrawPrimitiveUP
      || api == Api::DrawIndexedPrimitiveUP || api == Api::CreateTexture
      || api == Api::CreateCubeTexture || api == Api::CreateVolumeTexture;
  }

  static Api drawApi(DrawKind kind) noexcept {
    switch (kind) {
      case DrawKind::Primitive:          return Api::DrawPrimitive;
      case DrawKind::IndexedPrimitive:   return Api::DrawIndexedPrimitive;
      case DrawKind::PrimitiveUP:        return Api::DrawPrimitiveUP;
      case DrawKind::IndexedPrimitiveUP: return Api::DrawIndexedPrimitiveUP;
      default:                          return Api::Count;
    }
  }

  static Api textureApi(TextureKind kind) noexcept {
    switch (kind) {
      case TextureKind::Texture2D: return Api::CreateTexture;
      case TextureKind::Cube:      return Api::CreateCubeTexture;
      case TextureKind::Volume:    return Api::CreateVolumeTexture;
      default:                    return Api::Count;
    }
  }

  std::atomic_flag m_cell = ATOMIC_FLAG_INIT;
  Census m_census;
  uint64_t m_sequence = 0;
  BusyCounter m_recordBusy;
  BusyCounter m_snapshotBusy;
  std::atomic<uint32_t> m_counterSaturated { 0 };
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
  "Diagnostic refusal accounting requires lock-free 32-bit atomics");
static_assert(std::is_trivially_copyable<Snapshot>::value);
static_assert(std::is_trivially_copyable<CensusCopy>::value);
static_assert(sizeof(CensusCopy) <= 8192, "Copied census must remain fixed and small");
static_assert(sizeof(ConcurrentCensus) <= 8192, "Try-cell census must remain fixed and small");

}
