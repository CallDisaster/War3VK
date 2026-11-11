// Executes the production header, CPU only. It proves bounded observation
// bookkeeping, not D3D9 integration, game attribution, GPU execution, or speed.
#include "../src/d3d9/d3d9_migration_census.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace m = dxvk::d3d9::migration;

static std::atomic<bool> countAllocations { false };
static std::atomic<uint32_t> allocationCount { 0 };
static unsigned checks = 0;

void* operator new(std::size_t bytes) {
  if (countAllocations)
    ++allocationCount;
  if (void* p = std::malloc(bytes ? bytes : 1))
    return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static void check(bool value, const char* expression, unsigned line) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL line %u: %s\n", line, expression);
    std::exit(1);
  }
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)

static_assert(std::is_trivially_copyable<m::Snapshot>::value);
static_assert(std::is_same<decltype(std::declval<const m::Census&>().snapshot()),
                           const m::Snapshot&>::value);
static_assert(noexcept(std::declval<m::Census&>().noteCall(m::Api::Reset)));
static_assert(noexcept(std::declval<m::Census&>().noteDrawAttempt(m::DrawFacts{})));
static_assert(noexcept(std::declval<m::Census&>().noteTextureRequest(m::TextureFacts{})));
static_assert(sizeof(m::Census) <= 8192);
static_assert(std::is_trivially_copyable<m::CensusCopy>::value);
static_assert(std::is_trivially_copyable<m::ObservationRefusals>::value);
static_assert(!std::is_copy_constructible<m::ConcurrentCensus>::value);
static_assert(noexcept(std::declval<m::ConcurrentCensus&>().noteCall(m::Api::Clear)));
static_assert(noexcept(std::declval<m::ConcurrentCensus&>().noteDraw(m::DrawFacts{})));
static_assert(noexcept(std::declval<m::ConcurrentCensus&>().noteTexture(m::TextureFacts{})));
static_assert(noexcept(std::declval<m::ConcurrentCensus&>().noteVsKey(m::VsKeySamples::Key{})));
static_assert(noexcept(std::declval<m::ConcurrentCensus&>().noteFsKey(m::FsKeySamples::Key{})));
static_assert(noexcept(std::declval<m::ConcurrentCensus&>().notePresent(std::declval<bool&>())));
static_assert(noexcept(std::declval<m::ConcurrentCensus&>().trySnapshot(std::declval<m::CensusCopy&>())));
static_assert(noexcept(std::declval<const m::ConcurrentCensus&>().observationRefusals()));
static_assert(sizeof(m::ConcurrentCensus) <= 8192);
static_assert(sizeof(m::CensusCopy) <= 8192);

// Test-only cold seeding and a directly held cell, not a callback in production.
// rawState is called only while this test itself holds the cell. Cold seeds are
// applied before any worker starts; none of these seams escape to integration.
namespace dxvk::d3d9::migration {
struct ConcurrentCensusTestAccess {
  static bool acquire(ConcurrentCensus& census) noexcept {
    return !census.m_cell.test_and_set(std::memory_order_acquire);
  }
  static void release(ConcurrentCensus& census) noexcept {
    census.m_cell.clear(std::memory_order_release);
  }
  static void rawState(const ConcurrentCensus& census,
                       std::array<unsigned char, sizeof(Snapshot)>& bytes) noexcept {
    std::memcpy(bytes.data(), &census.m_census.snapshot(), bytes.size());
  }
  static uint64_t sequence(const ConcurrentCensus& census) noexcept {
    return census.m_sequence;
  }
  static void seedSequence(ConcurrentCensus& census, uint64_t value) noexcept {
    census.m_sequence = value;
  }
  static void seedBusy(ConcurrentCensus& census, uint32_t records, uint32_t snapshots) noexcept {
    census.m_recordBusy.count.store(records, std::memory_order_relaxed);
    census.m_snapshotBusy.count.store(snapshots, std::memory_order_relaxed);
  }
  static void seedCalls(ConcurrentCensus& census, uint64_t value) noexcept {
    auto& state = census.m_census.m_snapshot;
    state.observedCalls = value;
    state.apiCalls[std::size_t(Api::Clear)] = value;
  }
};
}

// A test runner safety net, not synchronization used by the recorder. If a
// regression spins on same-thread reentry or deadlocks the handshake, terminate
// with a distinct failure instead of leaving a native test process behind.
class TestWatchdog {
public:
  TestWatchdog() : m_thread([this] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!m_done.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::fputs("FAIL census concurrency watchdog expired after 30 seconds\n", stderr);
        std::_Exit(2);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }) { }
  ~TestWatchdog() {
    m_done.store(true, std::memory_order_release);
    m_thread.join();
  }
private:
  std::atomic<bool> m_done { false };
  std::thread m_thread;
};

static m::DrawFacts drawFacts() {
  m::DrawFacts result;
  result.kind = m::DrawKind::IndexedPrimitive;
  result.knownMask = m::AllDrawFieldsKnown;
  result.primitiveType = 4;
  result.primitiveCount = 2;
  result.shaderMask = 3;
  result.fvf = 0x112;
  result.streamStride = 40;
  result.textureStageMask = 5;
  result.vertexCount = 6;
  result.indexFormat = 101;
  return result;
}

static m::TextureFacts textureFacts() {
  m::TextureFacts result;
  result.kind = m::TextureKind::Texture2D;
  result.knownMask = m::AllTextureFieldsKnown;
  result.width = 2048;
  result.height = 1024;
  result.depth = 1;
  result.levels = 11;
  result.format = 21;
  result.usage = 0;
  result.pool = 1;
  return result;
}

static void testEmptyAndApiCalls() {
  m::Census census;
  const auto& snapshot = census.snapshot();
  CHECK(snapshot.schemaVersion == 1);
  CHECK(snapshot.observedCalls == 0);
  CHECK(snapshot.drawAttempts == 0);
  CHECK(snapshot.observedTextureRequests == 0);
  CHECK(snapshot.drawSampleCount == 0);
  CHECK(snapshot.textureSampleCount == 0);
  CHECK(snapshot.vsKeys.storedUnique == 0);
  CHECK(snapshot.fsKeys.storedUnique == 0);
  CHECK(snapshot.saturatedUpdates == 0);
  for (std::size_t i = 0; i < m::ApiCount; ++i) {
    CHECK(snapshot.apiCalls[i] == 0);
    CHECK(std::strcmp(m::ApiName(m::Api(i)), "Unclassified") != 0);
    census.noteCall(m::Api(i));
    census.noteCall(m::Api(i));
    CHECK(snapshot.apiCalls[i] == 2);
  }
  census.noteCall(m::Api::Count);
  census.noteCall(m::Api(255));
  CHECK(snapshot.observedCalls == m::ApiCount * 2 + 2);
  CHECK(snapshot.unclassifiedCalls == 2);
  CHECK(std::strcmp(m::ApiName(m::Api::Count), "Unclassified") == 0);
  CHECK(std::strcmp(m::ApiName(m::Api(255)), "Unclassified") == 0);
  CHECK(snapshot.drawAttempts == 0); // calls do not fabricate draw facts
}

static void testDrawKnownZeroAndMissing() {
  m::Census census;
  m::DrawFacts knownZero;
  knownZero.kind = m::DrawKind::Primitive;
  knownZero.knownMask = m::AllDrawFieldsKnown;
  knownZero.primitiveType = 1;
  census.noteDrawAttempt(knownZero);
  const auto& s = census.snapshot();
  CHECK(s.observedCalls == 0); // no implicit public-call double counting
  CHECK(s.drawAttempts == 1);
  CHECK(s.shaderPairs[0] == 1); // no VS/PS is observed, not missing
  CHECK(s.shaderPairMissing == 0);
  CHECK(s.drawSamples[0].knownMask == m::AllDrawFieldsKnown);
  CHECK(s.drawSamples[0].fvf == 0);
  CHECK(s.drawSamples[0].streamStride == 0);
  CHECK(s.unknownSourceDrawAttempts == 1);
  for (auto value : s.drawMissingFields)
    CHECK(value == 0);
  m::DrawFacts missing;
  // Untrusted values for fields not marked known must not pollute maxima.
  missing.primitiveType = UINT32_MAX;
  missing.primitiveCount = UINT32_MAX;
  missing.shaderMask = UINT32_MAX;
  missing.streamStride = UINT32_MAX;
  missing.vertexCount = UINT32_MAX;
  missing.fvf = UINT32_MAX;
  missing.textureStageMask = UINT32_MAX;
  census.noteDrawAttempt(missing);
  CHECK(s.drawAttempts == 2);
  CHECK(s.primitiveTypeMissing == 1);
  CHECK(s.primitiveTypeInvalid == 0);
  CHECK(s.shaderPairMissing == 1);
  CHECK(s.shaderPairInvalid == 0);
  CHECK(s.maxPrimitiveCount == 0);
  CHECK(s.maxStreamStride == 0);
  CHECK(s.maxVertexCount == 0);
  CHECK(s.fvfBitUnion == 0);
  CHECK(s.textureStageMaskUnion == 0);
  for (auto value : s.drawMissingFields)
    CHECK(value == 1);
  census.noteDrawAttempt(drawFacts());
  CHECK(s.maxPrimitiveCount == 2);
  CHECK(s.maxStreamStride == 40);
  CHECK(s.maxVertexCount == 6);
  CHECK(s.fvfBitUnion == 0x112);
  CHECK(s.textureStageMaskUnion == 5);
  CHECK(s.unknownSourceDrawAttempts == 3);
}

static void testDrawHistogramsAndInvalidFacts() {
  m::Census census;
  auto facts = drawFacts();
  for (uint32_t primitive = 1; primitive <= 6; ++primitive) {
    facts.primitiveType = primitive;
    census.noteDrawAttempt(facts);
  }
  const auto& s = census.snapshot();
  for (uint32_t primitive = 1; primitive <= 6; ++primitive)
    CHECK(s.primitiveTypes[primitive] == 1);
  facts.primitiveType = 4;
  for (uint32_t pair = 0; pair < 4; ++pair) {
    facts.shaderMask = pair;
    census.noteDrawAttempt(facts);
  }
  CHECK(s.shaderPairs[0] == 1);
  CHECK(s.shaderPairs[1] == 1);
  CHECK(s.shaderPairs[2] == 1);
  CHECK(s.shaderPairs[3] == 7);
  for (uint32_t kind = 0; kind < uint32_t(m::DrawKind::Count); ++kind) {
    facts.kind = m::DrawKind(kind);
    census.noteDrawAttempt(facts);
  }
  CHECK(s.drawKinds[0] == 1);
  CHECK(s.drawKinds[1] == 1);
  CHECK(s.drawKinds[2] == 11);
  CHECK(s.drawKinds[3] == 1);
  CHECK(s.drawKinds[4] == 1);
  facts.kind = m::DrawKind(255);
  facts.source = m::SourceAttribution(255);
  facts.primitiveType = 0;
  facts.shaderMask = 4;
  facts.knownMask |= 1u << 31;
  census.noteDrawAttempt(facts);
  facts.primitiveType = UINT32_MAX;
  census.noteDrawAttempt(facts);
  CHECK(s.invalidDrawKinds == 2);
  CHECK(s.invalidSourceAttributions == 2);
  CHECK(s.primitiveTypes[0] == 1);
  CHECK(s.primitiveTypeInvalid == 2);
  CHECK(s.shaderPairInvalid == 2);
  CHECK(s.drawUnknownMaskBits == 2);
  CHECK(s.unknownSourceDrawAttempts == s.drawAttempts);
  CHECK(s.drawSamples[15].source == m::SourceAttribution(255));
}

static void testTextureRequests() {
  m::Census census;
  auto facts = textureFacts();
  census.noteTextureRequest(facts);
  facts.kind = m::TextureKind::Cube;
  facts.width = 512;
  facts.height = 512;
  facts.levels = 0;
  census.noteTextureRequest(facts);
  facts.kind = m::TextureKind::Volume;
  facts.depth = 128;
  facts.levels = 8;
  census.noteTextureRequest(facts);
  m::TextureFacts missing;
  missing.width = UINT32_MAX;
  missing.height = UINT32_MAX;
  missing.depth = UINT32_MAX;
  missing.levels = UINT32_MAX;
  census.noteTextureRequest(missing);
  const auto& s = census.snapshot();
  CHECK(s.observedTextureRequests == 4);
  CHECK(s.observedCalls == 0);
  CHECK(s.maxTextureWidth == 2048);
  CHECK(s.maxTextureHeight == 1024);
  CHECK(s.maxTextureDepth == 128);
  CHECK(s.maxRequestedMipLevels == 11);
  CHECK(s.fullMipChainRequests == 1);
  for (auto value : s.textureKinds)
    CHECK(value == 1);
  for (auto value : s.textureMissingFields)
    CHECK(value == 1);
  CHECK(s.textureSamples[1].levels == 0);
  CHECK(s.textureSamples[1].knownMask & m::LevelsKnown);
  facts.kind = m::TextureKind(255);
  facts.knownMask |= 1u << 31;
  census.noteTextureRequest(facts);
  CHECK(s.invalidTextureKinds == 1);
  CHECK(s.textureUnknownMaskBits == 1);
  CHECK(s.observedTextureRequests == 5);
}

static void testFactSampleCapacity() {
  m::Census census;
  auto draw = drawFacts();
  auto texture = textureFacts();
  for (uint32_t i = 0; i < 1000; ++i) {
    draw.primitiveCount = i;
    texture.width = i;
    census.noteDrawAttempt(draw);
    census.noteTextureRequest(texture);
  }
  const auto& s = census.snapshot();
  CHECK(s.drawAttempts == 1000);
  CHECK(s.observedTextureRequests == 1000);
  CHECK(s.drawSampleCount == m::FactSampleCapacity);
  CHECK(s.textureSampleCount == m::FactSampleCapacity);
  CHECK(s.drawSamplesDropped == 1000 - m::FactSampleCapacity);
  CHECK(s.textureSamplesDropped == 1000 - m::FactSampleCapacity);
  CHECK(s.maxPrimitiveCount == 999);
  CHECK(s.maxTextureWidth == 999);
  for (std::size_t i = 0; i < m::FactSampleCapacity; ++i) {
    CHECK(s.drawSamples[i].primitiveCount == i);
    CHECK(s.textureSamples[i].width == i);
  }
}

static void testFixedFunctionKeys() {
  m::Census census;
  m::VsKeySamples::Key vs = {};
  m::FsKeySamples::Key fs = {};
  census.noteFixedFunctionVsKeyMissing();
  census.noteFixedFunctionFsKeyMissing();
  const auto& s = census.snapshot();
  CHECK(s.vsKeys.observed == 1 && s.vsKeys.missing == 1);
  CHECK(s.fsKeys.observed == 1 && s.fsKeys.missing == 1);
  CHECK(s.vsKeys.storedUnique == 0 && s.fsKeys.storedUnique == 0);
  for (uint32_t i = 0; i < m::KeySampleCapacity; ++i) {
    // Last-word variation detects accidentally sampling only a key prefix.
    vs[m::VsKeyWords - 1] = i;
    fs[m::FsKeyWords - 1] = i;
    census.noteFixedFunctionVsKey(vs);
    census.noteFixedFunctionFsKey(fs);
    census.noteFixedFunctionVsKey(vs);
    census.noteFixedFunctionFsKey(fs);
  }
  CHECK(s.vsKeys.storedUnique == m::KeySampleCapacity);
  CHECK(s.fsKeys.storedUnique == m::KeySampleCapacity);
  CHECK(s.vsKeys.duplicates == m::KeySampleCapacity);
  CHECK(s.fsKeys.duplicates == m::KeySampleCapacity);
  CHECK(s.vsKeys.dropped == 0 && s.fsKeys.dropped == 0);
  vs[m::VsKeyWords - 1] = 500;
  fs[m::FsKeyWords - 1] = 500;
  census.noteFixedFunctionVsKey(vs);
  census.noteFixedFunctionFsKey(fs);
  census.noteFixedFunctionVsKey(vs);
  census.noteFixedFunctionFsKey(fs);
  CHECK(s.vsKeys.dropped == 2 && s.fsKeys.dropped == 2);
  // Existing keys remain recognizable even when capacity is already full.
  vs = {};
  fs = {};
  census.noteFixedFunctionVsKey(vs);
  census.noteFixedFunctionFsKey(fs);
  CHECK(s.vsKeys.duplicates == m::KeySampleCapacity + 1);
  CHECK(s.fsKeys.duplicates == m::KeySampleCapacity + 1);
  CHECK(s.vsKeys.keys[0] == vs && s.fsKeys.keys[0] == fs);
  CHECK(s.vsKeys.observed == s.vsKeys.storedUnique + s.vsKeys.duplicates
                              + s.vsKeys.dropped + s.vsKeys.missing);
  CHECK(s.fsKeys.observed == s.fsKeys.storedUnique + s.fsKeys.duplicates
                              + s.fsKeys.dropped + s.fsKeys.missing);
  for (std::size_t i = 0; i < m::KeySampleCapacity; ++i) {
    CHECK(s.vsKeys.keys[i][m::VsKeyWords - 1] == i);
    CHECK(s.fsKeys.keys[i][m::FsKeyWords - 1] == i);
  }
}

static void testSaturatingArithmetic() {
  uint64_t value = 0;
  CHECK(!m::SaturatingAdd(value, 0));
  CHECK(value == 0);
  CHECK(!m::SaturatingAdd(value));
  CHECK(value == 1);
  CHECK(!m::SaturatingAdd(value, UINT64_MAX - 1));
  CHECK(value == UINT64_MAX);
  CHECK(!m::SaturatingAdd(value, 0));
  CHECK(m::SaturatingAdd(value));
  CHECK(value == UINT64_MAX);
  value = UINT64_MAX - 5;
  CHECK(m::SaturatingAdd(value, 6));
  CHECK(value == UINT64_MAX);
  value = 7;
  CHECK(m::SaturatingAdd(value, UINT64_MAX));
  CHECK(value == UINT64_MAX);
}

static void testResetAndIndependentSnapshot() {
  m::Census census;
  census.noteCall(m::Api::DrawPrimitive);
  census.noteDrawAttempt(drawFacts());
  census.noteTextureRequest(textureFacts());
  census.noteFixedFunctionVsKey({});
  census.noteFixedFunctionFsKey({});
  const m::Snapshot copy = census.snapshot();
  const m::Snapshot* borrowed = &census.snapshot();
  census.reset();
  CHECK(&census.snapshot() == borrowed);
  CHECK(copy.observedCalls == 1 && copy.drawAttempts == 1);
  CHECK(copy.observedTextureRequests == 1);
  CHECK(copy.vsKeys.storedUnique == 1 && copy.fsKeys.storedUnique == 1);
  const auto& s = census.snapshot();
  CHECK(s.schemaVersion == 1);
  CHECK(s.observedCalls == 0 && s.drawAttempts == 0);
  CHECK(s.observedTextureRequests == 0);
  CHECK(s.drawSampleCount == 0 && s.textureSampleCount == 0);
  CHECK(s.drawSamplesDropped == 0 && s.textureSamplesDropped == 0);
  CHECK(s.vsKeys.observed == 0 && s.fsKeys.observed == 0);
  CHECK(s.vsKeys.storedUnique == 0 && s.fsKeys.storedUnique == 0);
  CHECK(s.saturatedUpdates == 0);
}

static void testNoAllocations() {
  // Verify the interception seam first, outside the component proof window.
  allocationCount = 0;
  countAllocations = true;
  void* seam = ::operator new(16);
  countAllocations = false;
  ::operator delete(seam);
  CHECK(allocationCount == 1);
  allocationCount = 0;
  countAllocations = true;
  {
    m::Census census;
    m::VsKeySamples::Key vs = {};
    m::FsKeySamples::Key fs = {};
    auto draw = drawFacts();
    auto texture = textureFacts();
    for (uint32_t i = 0; i < 5000; ++i) {
      census.noteCall(m::Api(i % m::ApiCount));
      census.noteDrawAttempt(draw);
      census.noteTextureRequest(texture);
      vs.back() = i;
      fs.back() = i;
      census.noteFixedFunctionVsKey(vs);
      census.noteFixedFunctionFsKey(fs);
      census.noteFixedFunctionVsKeyMissing();
      census.noteFixedFunctionFsKeyMissing();
    }
    const m::Snapshot copy = census.snapshot();
    CHECK(copy.drawAttempts == 5000);
    census.reset();
  }
  countAllocations = false;
  CHECK(allocationCount == 0);
}

static m::DrawFacts argumentDraw(m::DrawKind kind) {
  m::DrawFacts result;
  result.kind = kind;
  result.knownMask = m::PrimitiveTypeKnown | m::PrimitiveCountKnown;
  result.primitiveType = 4;
  result.primitiveCount = 3;
  if (kind == m::DrawKind::PrimitiveUP || kind == m::DrawKind::IndexedPrimitiveUP) {
    result.knownMask |= m::StreamStrideKnown;
    result.streamStride = 32;
  }
  if (kind == m::DrawKind::IndexedPrimitive || kind == m::DrawKind::IndexedPrimitiveUP) {
    result.knownMask |= m::VertexCountKnown;
    result.vertexCount = 9;
  }
  if (kind == m::DrawKind::IndexedPrimitiveUP) {
    result.knownMask |= m::IndexFormatKnown;
    result.indexFormat = 101;
  }
  return result;
}

static bool isMilestone(uint64_t count) {
  return count == 1 || count == 64 || count == 256 || count == 1024
      || count == 4096 || count == 16384 || count == 65536;
}

static bool coherentGroupedSnapshot(const m::Snapshot& snapshot) {
  uint64_t apiTotal = 0;
  for (auto count : snapshot.apiCalls)
    apiTotal += count;
  const uint64_t drawTotal = snapshot.apiCalls[std::size_t(m::Api::DrawPrimitive)]
    + snapshot.apiCalls[std::size_t(m::Api::DrawIndexedPrimitive)]
    + snapshot.apiCalls[std::size_t(m::Api::DrawPrimitiveUP)]
    + snapshot.apiCalls[std::size_t(m::Api::DrawIndexedPrimitiveUP)];
  const uint64_t textureTotal = snapshot.apiCalls[std::size_t(m::Api::CreateTexture)]
    + snapshot.apiCalls[std::size_t(m::Api::CreateCubeTexture)]
    + snapshot.apiCalls[std::size_t(m::Api::CreateVolumeTexture)];
  uint64_t drawKindTotal = 0;
  uint64_t textureKindTotal = 0;
  for (auto count : snapshot.drawKinds)
    drawKindTotal += count;
  for (auto count : snapshot.textureKinds)
    textureKindTotal += count;
  return snapshot.schemaVersion == 2 && snapshot.unclassifiedCalls == 0
    && snapshot.saturatedUpdates == 0 && apiTotal == snapshot.observedCalls
    && drawTotal == snapshot.drawAttempts && drawKindTotal == drawTotal
    && textureTotal == snapshot.observedTextureRequests && textureKindTotal == textureTotal
    && snapshot.drawSampleCount + snapshot.drawSamplesDropped == drawTotal
    && snapshot.textureSampleCount + snapshot.textureSamplesDropped == textureTotal
    && snapshot.shaderPairMissing == drawTotal && snapshot.shaderPairInvalid == 0
    && snapshot.shaderPairs == std::array<uint64_t, 4>{}
    && snapshot.fvfBitUnion == 0 && snapshot.textureStageMaskUnion == 0
    && snapshot.unknownSourceDrawAttempts == drawTotal;
}

static void testConcurrentGroupedTransactions() {
  m::ConcurrentCensus census;
  m::CensusCopy empty;
  CHECK(census.trySnapshot(empty));
  CHECK(empty.sequence == 1 && empty.values.schemaVersion == 2);
  CHECK(empty.values.observedCalls == 0);
  CHECK(census.noteCall(m::Api::Clear));
  for (uint32_t kind = 1; kind < uint32_t(m::DrawKind::Count); ++kind)
    CHECK(census.noteDraw(argumentDraw(m::DrawKind(kind))));
  for (uint32_t kind = 1; kind < uint32_t(m::TextureKind::Count); ++kind) {
    auto facts = textureFacts();
    facts.kind = m::TextureKind(kind);
    CHECK(census.noteTexture(facts));
  }
  CHECK(census.noteVsKey({}));
  CHECK(census.noteVsKey({}));
  CHECK(census.noteFsKey({}));
  CHECK(census.noteFsKey({}));
  bool presentsAccepted = true;
  bool milestonesCorrect = true;
  unsigned milestones = 0;
  for (uint32_t count = 1; count <= 65536; ++count) {
    bool milestone = !isMilestone(count);
    presentsAccepted &= census.notePresent(milestone);
    milestonesCorrect &= milestone == isMilestone(count);
    milestones += milestone;
  }
  CHECK(presentsAccepted && milestonesCorrect && milestones == 7);
  m::CensusCopy copy;
  CHECK(census.trySnapshot(copy));
  CHECK(copy.sequence == 2);
  CHECK(coherentGroupedSnapshot(copy.values));
  CHECK(copy.values.observedCalls == 65544);
  CHECK(copy.values.apiCalls[std::size_t(m::Api::Present)] == 65536);
  CHECK(copy.values.apiCalls[std::size_t(m::Api::Clear)] == 1);
  CHECK(copy.values.drawAttempts == 4);
  CHECK(copy.values.observedTextureRequests == 3);
  for (std::size_t kind = 1; kind < std::size_t(m::DrawKind::Count); ++kind)
    CHECK(copy.values.drawKinds[kind] == 1);
  for (std::size_t kind = 1; kind < std::size_t(m::TextureKind::Count); ++kind)
    CHECK(copy.values.textureKinds[kind] == 1);
  CHECK(copy.values.drawSamples[0].knownMask == 0x03);
  CHECK(copy.values.drawSamples[1].knownMask == 0x43);
  CHECK(copy.values.drawSamples[2].knownMask == 0x13);
  CHECK(copy.values.drawSamples[3].knownMask == 0xd3);
  CHECK(copy.values.vsKeys.observed == 2 && copy.values.vsKeys.duplicates == 1);
  CHECK(copy.values.fsKeys.observed == 2 && copy.values.fsKeys.duplicates == 1);
  CHECK(empty.sequence == 1 && empty.values.observedCalls == 0);
  const auto refusals = census.observationRefusals();
  CHECK(refusals.recordBusy == 0 && refusals.snapshotBusy == 0);
  CHECK(refusals.counterSaturated == 0);
}

static void testGroupedBypassIsUnclassified() {
  m::ConcurrentCensus census;
  const m::Api grouped[] = {
    m::Api::Present, m::Api::DrawPrimitive, m::Api::DrawIndexedPrimitive,
    m::Api::DrawPrimitiveUP, m::Api::DrawIndexedPrimitiveUP,
    m::Api::CreateTexture, m::Api::CreateCubeTexture, m::Api::CreateVolumeTexture,
  };
  for (auto api : grouped)
    CHECK(census.noteCall(api));
  CHECK(census.noteCall(m::Api::Count));
  CHECK(census.noteCall(m::Api(255)));
  m::CensusCopy copy;
  CHECK(census.trySnapshot(copy));
  CHECK(copy.values.observedCalls == 10 && copy.values.unclassifiedCalls == 10);
  for (auto api : grouped)
    CHECK(copy.values.apiCalls[std::size_t(api)] == 0);
  CHECK(copy.values.drawAttempts == 0 && copy.values.observedTextureRequests == 0);
  CHECK(!coherentGroupedSnapshot(copy.values));
  // Invalid grouped facts also become visible errors, not arbitrary API tags.
  CHECK(census.noteDraw(m::DrawFacts{}));
  auto invalidDraw = m::DrawFacts{};
  invalidDraw.kind = m::DrawKind(255);
  CHECK(census.noteDraw(invalidDraw));
  CHECK(census.noteTexture(m::TextureFacts{}));
  auto invalidTexture = m::TextureFacts{};
  invalidTexture.kind = m::TextureKind(255);
  CHECK(census.noteTexture(invalidTexture));
  CHECK(census.trySnapshot(copy));
  CHECK(copy.values.unclassifiedCalls == 14);
  CHECK(copy.values.drawAttempts == 2 && copy.values.invalidDrawKinds == 1);
  CHECK(copy.values.observedTextureRequests == 2 && copy.values.invalidTextureKinds == 1);
}

static void testTryCellSameThreadReentryAndWholeRefusal() {
  m::ConcurrentCensus census;
  const auto draw = argumentDraw(m::DrawKind::IndexedPrimitive);
  CHECK(census.noteDraw(draw));
  m::CensusCopy before;
  CHECK(census.trySnapshot(before));
  CHECK(m::ConcurrentCensusTestAccess::acquire(census));
  std::array<unsigned char, sizeof(m::Snapshot)> oldState;
  m::ConcurrentCensusTestAccess::rawState(census, oldState);
  const auto oldSequence = m::ConcurrentCensusTestAccess::sequence(census);
  m::CensusCopy refused;
  std::memset(&refused, 0xa5, sizeof(refused));
  std::array<unsigned char, sizeof(refused)> oldOutput;
  std::memcpy(oldOutput.data(), &refused, oldOutput.size());
  bool milestone = true;
  // Calling on the same owning thread must refuse immediately, not recurse or
  // spin. Draw/texture API counters and facts must both remain unchanged.
  CHECK(!census.noteCall(m::Api::Clear));
  CHECK(!census.noteDraw(draw));
  CHECK(!census.noteTexture(textureFacts()));
  CHECK(!census.noteVsKey({}));
  CHECK(!census.noteFsKey({}));
  CHECK(!census.notePresent(milestone));
  CHECK(milestone);
  CHECK(!census.trySnapshot(refused));
  CHECK(std::memcmp(&refused, oldOutput.data(), oldOutput.size()) == 0);
  std::array<unsigned char, sizeof(m::Snapshot)> newState;
  m::ConcurrentCensusTestAccess::rawState(census, newState);
  CHECK(oldState == newState);
  CHECK(m::ConcurrentCensusTestAccess::sequence(census) == oldSequence);
  const auto refusals = census.observationRefusals();
  CHECK(refusals.recordBusy == 6 && refusals.snapshotBusy == 1);
  CHECK(refusals.counterSaturated == 0);
  m::ConcurrentCensusTestAccess::release(census);
  // Refusal did not poison the cell; subsequent accepted observations work.
  CHECK(census.noteDraw(draw));
  CHECK(census.noteTexture(textureFacts()));
  CHECK(census.trySnapshot(refused));
  CHECK(refused.sequence == before.sequence + 1);
  CHECK(refused.values.drawAttempts == 2);
  CHECK(refused.values.apiCalls[std::size_t(m::Api::DrawIndexedPrimitive)] == 2);
  CHECK(refused.values.observedTextureRequests == 1);
  CHECK(refused.values.apiCalls[std::size_t(m::Api::CreateTexture)] == 1);
  CHECK(coherentGroupedSnapshot(refused.values));
}

static void testBoundedRefusalOverflowAndSequenceSaturation() {
  m::ConcurrentCensus overflow;
  m::ConcurrentCensusTestAccess::seedBusy(overflow, UINT32_MAX - 1, UINT32_MAX - 1);
  CHECK(m::ConcurrentCensusTestAccess::acquire(overflow));
  // Completion/join matters: the exported independent observations are not a
  // coherent snapshot while the increment and overflow-latch store are racing.
  std::atomic<uint32_t> errors { 0 };
  std::thread worker([&] {
    m::CensusCopy copy;
    for (uint32_t i = 0; i < 5; ++i) {
      if (overflow.noteCall(m::Api::Clear) || overflow.trySnapshot(copy))
        errors.fetch_add(1, std::memory_order_relaxed);
    }
  });
  worker.join();
  CHECK(errors.load(std::memory_order_relaxed) == 0);
  const auto completeOverflow = overflow.observationRefusals();
  CHECK(completeOverflow.recordBusy == UINT32_MAX);
  CHECK(completeOverflow.snapshotBusy == UINT32_MAX);
  CHECK(completeOverflow.counterSaturated == 1);
  m::ConcurrentCensusTestAccess::release(overflow);
  CHECK(overflow.noteCall(m::Api::Clear));
  m::CensusCopy copy;
  CHECK(overflow.trySnapshot(copy));
  CHECK(copy.values.observedCalls == 1 && copy.sequence == 1);
  CHECK(overflow.observationRefusals().counterSaturated == 1);
  CHECK(overflow.observationRefusals().recordBusy == UINT32_MAX);

  m::ConcurrentCensus sequence;
  m::ConcurrentCensusTestAccess::seedSequence(sequence, UINT64_MAX - 1);
  CHECK(sequence.trySnapshot(copy));
  CHECK(copy.sequence == UINT64_MAX);
  std::array<unsigned char, sizeof(copy)> lastOutput;
  std::memcpy(lastOutput.data(), &copy, lastOutput.size());
  CHECK(sequence.noteCall(m::Api::Clear));
  CHECK(!sequence.trySnapshot(copy));
  CHECK(std::memcmp(&copy, lastOutput.data(), lastOutput.size()) == 0);
  CHECK(sequence.observationRefusals().counterSaturated == 1);
  CHECK(sequence.observationRefusals().snapshotBusy == 0);

  m::ConcurrentCensus data;
  m::ConcurrentCensusTestAccess::seedCalls(data, UINT64_MAX);
  CHECK(data.noteCall(m::Api::Clear));
  CHECK(data.trySnapshot(copy));
  CHECK(copy.values.observedCalls == UINT64_MAX);
  CHECK(copy.values.apiCalls[std::size_t(m::Api::Clear)] == UINT64_MAX);
  CHECK(copy.values.saturatedUpdates == 2);
  CHECK(data.observationRefusals().counterSaturated == 1);
  CHECK(!coherentGroupedSnapshot(copy.values));
}

static void testConcurrentWritersAndSnapshots() {
  constexpr uint32_t writerCount = 3;
  constexpr uint32_t iterations = 20000;
  m::ConcurrentCensus census;
  std::atomic<uint32_t> ready { 0 };
  std::atomic<bool> start { false };
  std::atomic<uint32_t> preflightTurn { 0 };
  std::atomic<bool> bulkStart { false };
  std::atomic<uint32_t> halfway { 0 };
  std::atomic<bool> resume { false };
  std::atomic<uint32_t> writersDone { 0 };
  std::atomic<uint32_t> errors { 0 };
  std::array<uint32_t, writerCount> accepted = {};
  uint32_t snapshotsAccepted = 0;
  uint32_t snapshotsRefused = 0;
  uint32_t intermediateSnapshots = 0;
  uint64_t intermediateCalls = 0;
  auto writer = [&](uint32_t id) {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    // One ordered preflight per writer ensures all three fact domains exist.
    // The bulk phase below is genuinely concurrent; these waits are test-only.
    while (preflightTurn.load(std::memory_order_acquire) != id)
      std::this_thread::yield();
    uint32_t successes = 0;
    const auto draw = argumentDraw(m::DrawKind::IndexedPrimitive);
    const auto texture = textureFacts();
    auto record = [&] {
      bool milestone = false;
      const bool result = id == 0 ? census.noteDraw(draw)
        : id == 1 ? census.noteTexture(texture) : census.notePresent(milestone);
      if (result) {
        ++successes;
        if (id == 2 && milestone != isMilestone(successes))
          errors.fetch_add(1, std::memory_order_relaxed);
      }
      return result;
    };
    if (!record())
      errors.fetch_add(1, std::memory_order_relaxed);
    preflightTurn.fetch_add(1, std::memory_order_release);
    while (!bulkStart.load(std::memory_order_acquire))
      std::this_thread::yield();
    for (uint32_t i = 0; i < iterations; ++i) {
      if (i == iterations / 2) {
        halfway.fetch_add(1, std::memory_order_release);
        while (!resume.load(std::memory_order_acquire))
          std::this_thread::yield();
      }
      record();
    }
    accepted[id] = successes;
    writersDone.fetch_add(1, std::memory_order_release);
  };
  std::array<std::thread, writerCount> writers;
  for (uint32_t i = 0; i < writerCount; ++i)
    writers[i] = std::thread(writer, i);
  std::thread reader([&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    while (preflightTurn.load(std::memory_order_acquire) != writerCount)
      std::this_thread::yield();
    bulkStart.store(true, std::memory_order_release);
    m::CensusCopy copy;
    uint64_t previousCalls = 0;
    uint64_t previousDraws = 0;
    uint64_t previousTextures = 0;
    uint64_t previousSequence = 0;
    while (writersDone.load(std::memory_order_acquire) != writerCount) {
      if (census.trySnapshot(copy)) {
        ++snapshotsAccepted;
        if (!coherentGroupedSnapshot(copy.values)
            || copy.sequence != previousSequence + 1
            || copy.values.observedCalls < previousCalls
            || copy.values.drawAttempts < previousDraws
            || copy.values.observedTextureRequests < previousTextures)
          errors.fetch_add(1, std::memory_order_relaxed);
        previousCalls = copy.values.observedCalls;
        previousDraws = copy.values.drawAttempts;
        previousTextures = copy.values.observedTextureRequests;
        previousSequence = copy.sequence;
        if (!resume.load(std::memory_order_relaxed)
            && halfway.load(std::memory_order_acquire) == writerCount) {
          // All writers are halfway, not complete. This copied observation
          // precedes their second half and proves actual snapshot interleaving.
          ++intermediateSnapshots;
          intermediateCalls = copy.values.observedCalls;
          resume.store(true, std::memory_order_release);
        }
      } else {
        ++snapshotsRefused;
        if (copy.sequence != previousSequence)
          errors.fetch_add(1, std::memory_order_relaxed);
      }
      std::this_thread::yield();
    }
  });
  while (ready.load(std::memory_order_acquire) != writerCount + 1)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto& thread : writers)
    thread.join();
  reader.join();
  CHECK(errors.load(std::memory_order_relaxed) == 0);
  CHECK(snapshotsAccepted > 0 && intermediateSnapshots == 1);
  uint32_t acceptedTotal = 0;
  for (auto count : accepted) {
    CHECK(count > 0 && count <= iterations + 1);
    acceptedTotal += count;
  }
  CHECK(acceptedTotal > writerCount);
  m::CensusCopy finalCopy;
  CHECK(census.trySnapshot(finalCopy));
  CHECK(coherentGroupedSnapshot(finalCopy.values));
  CHECK(finalCopy.sequence == uint64_t(snapshotsAccepted) + 1);
  CHECK(finalCopy.values.observedCalls == acceptedTotal);
  CHECK(finalCopy.values.observedCalls > intermediateCalls);
  CHECK(finalCopy.values.drawAttempts == accepted[0]);
  CHECK(finalCopy.values.observedTextureRequests == accepted[1]);
  CHECK(finalCopy.values.apiCalls[std::size_t(m::Api::Present)] == accepted[2]);
  const auto refusals = census.observationRefusals();
  CHECK(refusals.recordBusy == writerCount * (iterations + 1) - acceptedTotal);
  CHECK(refusals.snapshotBusy == snapshotsRefused);
  CHECK(refusals.counterSaturated == 0);
  std::printf("Try-cell concurrency attempted=%u accepted=%u recordBusy=%u snapshots=%u snapshotBusy=%u halfwaySnapshots=%u failures=0\n",
    writerCount * (iterations + 1), acceptedTotal, refusals.recordBusy,
    snapshotsAccepted, snapshotsRefused, intermediateSnapshots);
}

static void testConcurrentCensusNoAllocations() {
  allocationCount = 0;
  countAllocations = true;
  {
    m::ConcurrentCensus census;
    const auto draw = argumentDraw(m::DrawKind::IndexedPrimitiveUP);
    const auto texture = textureFacts();
    m::VsKeySamples::Key vs = {};
    m::FsKeySamples::Key fs = {};
    m::CensusCopy copy;
    bool allAccepted = true;
    for (uint32_t i = 0; i < 5000; ++i) {
      bool milestone = false;
      allAccepted &= census.noteCall(m::Api::Clear);
      allAccepted &= census.noteDraw(draw);
      allAccepted &= census.noteTexture(texture);
      allAccepted &= census.notePresent(milestone);
      vs.back() = i;
      fs.back() = i;
      allAccepted &= census.noteVsKey(vs);
      allAccepted &= census.noteFsKey(fs);
      if (!(i % 97))
        allAccepted &= census.trySnapshot(copy);
    }
    CHECK(allAccepted);
    CHECK(census.trySnapshot(copy));
    CHECK(copy.values.observedCalls == 20000);
    CHECK(copy.values.drawAttempts == 5000 && copy.values.observedTextureRequests == 5000);
    CHECK(coherentGroupedSnapshot(copy.values));
    CHECK(m::ConcurrentCensusTestAccess::acquire(census));
    CHECK(!census.noteDraw(draw));
    CHECK(!census.noteTexture(texture));
    CHECK(!census.trySnapshot(copy));
    const auto refusals = census.observationRefusals();
    CHECK(refusals.recordBusy == 2 && refusals.snapshotBusy == 1);
    m::ConcurrentCensusTestAccess::release(census);
  }
  countAllocations = false;
  CHECK(allocationCount == 0);
}

int main() {
  testEmptyAndApiCalls();
  testDrawKnownZeroAndMissing();
  testDrawHistogramsAndInvalidFacts();
  testTextureRequests();
  testFactSampleCapacity();
  testFixedFunctionKeys();
  testSaturatingArithmetic();
  testResetAndIndependentSnapshot();
  testNoAllocations();
  testConcurrentGroupedTransactions();
  testGroupedBypassIsUnclassified();
  testConcurrentCensusNoAllocations();
  {
    TestWatchdog watchdog;
    testTryCellSameThreadReentryAndWholeRefusal();
    testBoundedRefusalOverflowAndSequenceSaturation();
    testConcurrentWritersAndSnapshots();
  }
  std::printf("Try-cell census bytes=%zu copyBytes=%zu allocationWindow=0\n",
    sizeof(m::ConcurrentCensus), sizeof(m::CensusCopy));
  std::printf("D3D9 migration census CPU checks=%u failures=0 bytes=%zu allocationWindow=0\n",
    checks, sizeof(m::Census));
  return 0;
}
