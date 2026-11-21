// CPU contracts used by the x86 caster mirror and the host's replay input.
// Build as a Win32 console fixture. No device, window or game is needed.
#include "../src/d3d9/war3/effects/war3_shadow_scene.h"
#include "../src/d3d9/war3/platform/war3_proxy_diag_block.h"
#include "../src/d3d9/war3/render/war3_shadow_replay_validation.cpp"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

// Windows defines these obsolete segmented-pointer aliases.
#undef near
#undef far

namespace scene = dxvk::war3::effects::scene;
namespace replay = dxvk::war3::render;
constexpr uint64_t Chunk = scene::CasterCopyChunkBytes;

static void Check(bool ok, int line) {
  if (!ok) {
    std::fprintf(stderr, "caster fixture failed at line %d\n", line);
    std::exit(1);
  }
}
#define CHECK(x) Check(bool(x), __LINE__)

static void BudgetAndDemand() {
  scene::CasterCopyPlan plan;
  scene::CasterCopyAccount pool;
  CHECK(pool.append({13, 2}, Chunk, plan));
  CHECK(plan.vertex.newChunk == Chunk && plan.vertex.offset == 0);
  CHECK(plan.index.newChunk == 0 && plan.index.offset == 16 && pool.used == 32);
  CHECK(!pool.append({Chunk - 32, 1}, Chunk, plan));
  CHECK(pool.frameBytes == Chunk && pool.used == 32);
  CHECK(pool.append({Chunk - 32, 0}, Chunk, plan));
  CHECK(!pool.append({1, 0}, Chunk, plan));
  CHECK(pool.append({Chunk + 1, 2}, 4 * Chunk, plan));
  CHECK(plan.vertex.newChunk == Chunk + 16 && plan.index.newChunk == Chunk);
  CHECK(pool.frameBytes == 3 * Chunk + 16 && pool.used == 16);

  scene::CasterPoolMirror mirror;
  CHECK(mirror.wants({Chunk - 32, 17}, 8192));
  CHECK(mirror.wants({1, 0}, 8192));
  CHECK(mirror.wants({0, 0}, 8192));
  CHECK(mirror.wants({Chunk, Chunk}, 8192));
  CHECK(mirror.casters == 4 && mirror.omitted == 0 && mirror.demand.frameBytes == 4 * Chunk);
  CHECK(mirror.wants({uint64_t(1) << 32, 0}, 8192));
  CHECK(mirror.demand.frameBytes > UINT32_MAX);
  mirror = {};
  CHECK(mirror.wants({0, 0}, 1));
  CHECK(!mirror.wants({0, 0}, 1));
  CHECK(mirror.omitted == 1 && mirror.demand.frameBytes == 0);
  std::puts("alignment, chunk rollovers, atomic refusal and demand after exhaustion passed");
}

static void AccountParity() {
  // A separate unlimited next-fit packer counts what the x86 classifier
  // sees. Include hits, static streams and trimmed formerly oversized VBs.
  std::mt19937 random(127);
  for (unsigned frame = 0; frame < 500; ++frame) {
    uint64_t expected = 0, remaining = 0;
    scene::CasterPoolMirror mirror;
    scene::CasterCopyAccount host, untrimmedHost;
    scene::CasterCopyPlan plan;
    for (unsigned draw = 0; draw < 100; ++draw) {
      const bool alpha = random() % 2;
      const uint32_t vertices = random() % 4 ? random() % 140000 : 0;
      const uint32_t indices = random() % 3 ? 3 * (random() % 22000) : 0;
      const auto sizes = scene::CasterCopySizes::forDraw(vertices, alpha, indices, 2);
      CHECK(sizes.vertex == uint64_t(vertices) * (alpha ? 20 : 12) && sizes.index == uint64_t(indices) * 2);
      for (uint64_t bytes : {sizes.vertex, sizes.index}) {
        const uint64_t aligned = (bytes + 15) / 16 * 16;
        if (aligned > remaining) {
          remaining = std::max(Chunk, aligned);
          expected += remaining;
        }
        remaining -= aligned;
      }
      CHECK(mirror.wants(sizes, 8192));
      const bool fits = expected <= scene::CasterCopyFrameBytes;
      CHECK(mirror.demand.frameBytes == expected);
      if (fits) {
        CHECK(untrimmedHost.append(sizes, scene::CasterCopyFrameBytes, plan));
        CHECK(untrimmedHost.frameBytes == expected);
        const uint32_t copiedVertices = random() % (vertices + 1);
        const auto actual = scene::CasterCopySizes::forDraw(copiedVertices, alpha, random() % 2 ? indices : 0, 2);
        CHECK(host.append(actual, scene::CasterCopyFrameBytes, plan));
        CHECK(host.frameBytes <= expected);
      }
    }
  }
  std::puts("500 frames, 50000 declared requests and admitted host prefixes passed");
}

static void Extract(uint32_t stride, uint32_t position, bool alpha, uint32_t uv) {
  constexpr uint32_t Count = 11;
  const size_t length = (Count - 1) * stride + std::max(position + 12, alpha ? uv + 8 : 0);
  const uint32_t extent = std::max(position + 12, alpha ? uv + 8 : 0);
  CHECK(scene::CasterVerticesReadable(length + 3, 3, 0, Count, stride, extent));
  CHECK(!scene::CasterVerticesReadable(length + 2, 3, 0, Count, stride, extent));
  CHECK(!scene::CasterVerticesReadable(length + 3, 3, -1, Count, stride, extent));
  std::vector<uint8_t> input(length + 3);
  for (size_t i = 0; i < input.size(); ++i) input[i] = uint8_t(i * 73 + 19);
  const uint32_t packed = scene::CasterVertexStride(alpha);
  std::vector<uint8_t> output(Count * packed + 2, 0xcc);
  scene::CopyCasterVertices(output.data() + 1, input.data() + 3, Count, stride, position, alpha, uv);
  CHECK(output.front() == 0xcc && output.back() == 0xcc);
  for (uint32_t i = 0; i < Count; ++i) {
    const uint8_t* source = input.data() + 3 + i * stride;
    const uint8_t* result = output.data() + 1 + i * packed;
    CHECK(std::memcmp(result, source + position, 12) == 0);
    if (alpha) CHECK(std::memcmp(result + 12, source + uv, 8) == 0);
  }
}

static void TrimAndReplay() {
  for (uint16_t minimum : {37, 65529}) {
    scene::CasterIndexSpan span;
    for (int delta : {3, 1, 6, 3, 0, 6}) span.include(uint16_t(minimum + delta));
    CHECK(span.min == minimum && span.count() == 7);
    for (int32_t base : {-17, 0, 8}) {
      for (bool alpha : {false, true}) {
        const auto address = scene::CasterIndexedAddress::from(base, span.min, true);
        const auto unchanged = scene::CasterIndexedAddress::from(base, span.min, false);
        CHECK(unchanged.replayBase == base);
        replay::War3ShadowReplayValidationInput input;
        input.expectedMapEpoch = input.drawMapEpoch = input.expectedDeviceEpoch = input.drawDeviceEpoch = 1;
        input.worldMatrixFinite = input.indexed = input.indexBufferPresent = true;
        input.indexCount = 6;
        input.indexTypeBytes = 2;
        input.indexBufferSize = 12;
        input.vertexOffset = address.replayBase;
        input.minVertexIndex = span.min;
        input.numVertices = span.count();
        const uint32_t stride = scene::CasterVertexStride(alpha);
        input.position = {true, span.count() * stride, stride, 0, 12};
        input.uvRequired = alpha;
        input.uv = {true, input.position.size, stride, 12, 8};
        CHECK(address.firstVertex == int64_t(base) + minimum);
        for (int delta : {0, 1, 3, 6})
          CHECK(int32_t(minimum) + delta + input.vertexOffset == delta);
        CHECK(replay::ValidateWar3ShadowReplayDraw(input));
        ++input.numVertices;
        CHECK(!replay::ValidateWar3ShadowReplayDraw(input));
        input.actualIndexDomainKnown = true;
        input.actualIndexMin = span.min;
        input.actualIndexMax = span.max;
        input.minVertexIndex = 0;
        input.numVertices = 65536;
        CHECK(replay::ValidateWar3ShadowReplayDraw(input));
        ++input.actualIndexMax;
        CHECK(!replay::ValidateWar3ShadowReplayDraw(input));
      }
    }
  }
  std::puts("trimmed 16-bit spans, signed bases, opaque and alpha final replay range checks passed");
}

static void Reuse() {
  const scene::CasterSliceKey original{19, 64, 2, 15, 32, 4, 20};
  scene::CasterRecentSlice<unsigned> recent{original, 71};
  unsigned slice = 0;
  CHECK(recent.find(original, slice) && slice == 71);
  for (uint64_t nextUpload : {20u, 24u, 31u, 90u}) {
    auto changed = original;
    changed.generation = nextUpload;
    CHECK(!recent.find(changed, slice));
  }
  auto changed = original; ++changed.slot; CHECK(!recent.find(changed, slice));
  changed = original; ++changed.offset; CHECK(!recent.find(changed, slice));
  changed = original; ++changed.count; CHECK(!recent.find(changed, slice));
  changed = original; ++changed.stride; CHECK(!recent.find(changed, slice));
  changed = original; ++changed.position; CHECK(!recent.find(changed, slice));
  changed = original; ++changed.uv; CHECK(!recent.find(changed, slice));
  changed = original; changed.uv = UINT32_MAX; CHECK(!recent.find(changed, slice));
  recent = {};
  CHECK(!recent.find(original, slice));
  CHECK(!recent.find({}, slice));
  std::puts("slice hit, generation, slot, layout, range and frame invalidation passed");
}

static void ExactBounds() {
  // Five padded vertices at base=-3, minIndex=4. Index 5 is a hole.
  float vb[][5] = {{0,-1000,0,0,0}, {0,-2,1,3,0}, {0,999,999,999,0},
                    {0,4,-3,2,0}, {0,4000,0,0,0}};
  uint16_t ib[] = {6,4,6};
  const auto address = scene::CasterIndexedAddress::from(-3, 4, true);
  const auto* src = reinterpret_cast<const uint8_t*>(&vb[address.firstVertex][1]);
  const auto* indices = reinterpret_cast<const uint8_t*>(ib);
  CHECK(!scene::ReadCasterBounds(nullptr, 20, 3, nullptr, 0, 0).known);
  scene::CasterSliceKey vk{7,24,1,3,20,4}, ik{8,0,2,3,2};
  scene::CasterBoundsCache cache;
  const auto local = cache.get(vk, ik, src, indices, 4);
  CHECK(local.known && (local.box == scene::CasterBox{-2,-3,2,4,1,3}));
  const std::array<float,16> transform{0,2,0,0, -3,0,0,0, 0,0,-4,0, 20,30,40,1};
  scene::CasterBox world;
  CHECK(scene::CasterWorldBounds(local, transform, world));
  const scene::CasterBox expected{17,26,28,29,38,32};
  for (unsigned j=0; j<6; ++j) {
    CHECK(j < 3 ? world[j] <= expected[j] : world[j] >= expected[j]);
    CHECK(std::abs(world[j] - expected[j]) < .001f);
  }
  auto moved = transform;
  moved[12] += 10;
  // No source bytes on this lookup. Only the same VB and frozen IB may hit.
  CHECK(scene::CasterWorldBounds(cache.get(vk, ik, nullptr, indices, 4), moved, world));
  CHECK(std::abs(world[0] - 27) < .001f);
  ib[0] = 5;
  ++ik.generation;
  CHECK(cache.get(vk, ik, src, indices, 4).box[3] == 999);
  vb[2][1] = std::numeric_limits<float>::quiet_NaN();
  ++vk.generation;
  CHECK(!cache.get(vk, ik, src, indices, 4).known);
  ib[0] = 6;
  ++ik.generation;
  CHECK(cache.get(vk, ik, src, indices, 4).known);
  CHECK(!cache.get(vk, {}, src, nullptr, 4).known);
  vb[2][1] = 999;
  ++vk.generation;
  CHECK(cache.get(vk, {}, src, nullptr, 4).box[3] == 999);
  moved[3] = .25f;
  CHECK(!scene::CasterWorldBounds(local, moved, world));
  moved = transform;
  moved[12] = std::numeric_limits<float>::infinity();
  CHECK(!scene::CasterWorldBounds(local, moved, world));
  std::puts("exact sparse index domain, declared span, negative base, stride, affine world and dedupe passed");
}

static void CascadeBounds() {
  const std::array<float,16> light{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  const double limit = scene::CasterClipLimit(1024, .7f, 6, 1, 3);
  CHECK(limit == 1 + 28./1024);
  const float edge = float(limit);
  CHECK(scene::CasterOutsideCascade({edge+.001f,0,0,3,1,1}, light, limit));
  CHECK(scene::CasterOutsideCascade({0,-4,0,1,-edge-.001f,1}, light, limit));
  CHECK(!scene::CasterOutsideCascade({edge,0,0,3,1,1}, light, limit));
  CHECK(!scene::CasterOutsideCascade({-.5f,-.5f,-10000,.5f,.5f,-9999}, light, limit));
  CHECK(!scene::CasterOutsideCascade({-2,-2,20,2,2,30}, light, limit));
  CHECK(!scene::CasterOutsideCascade({-1,-1,.2f,1,1,.8f}, light, limit));
  std::puts("guarded clip XY, edge equality, enclosing box and both depth-clamped half spaces passed");
}

static void LayerPressure() {
  scene::CasterCascadeFootprints f;
  f.count = 4;
  f.limit = 1;
  for (unsigned c = 0; c < 4; ++c) {
    f.light[c] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    f.light[c][0] = 1.f / float(1 << c);
  }
  const scene::CasterBox near{0,0,0,.5f,.5f,1}, far{3,0,0,3.1f,.5f,1};
  CHECK(f.touches(near) == 15 && f.touches(far) == 12);
  CHECK(f.priority(true, near) == 0 && f.priority(true, far) == 2);
  CHECK(f.priority(true, {9,0,0,10,1,1}) == 4 && f.priority(false, {}) == 0);
  CHECK((15 & ~f.touches(far)) == 3);
  f.light[0][15] = 0;
  CHECK(f.touches(far) == 13);
  f.light[0][15] = 1;
  scene::CasterLayerIdentity key{2, 3, 4, 5, 6};
  CHECK(scene::CasterCompleteLayers(f, &far, 1, 1, true) == 3);
  CHECK(scene::CasterCompleteLayers(f, &far, 1, 2, true) == 0); // one missing record
  scene::CasterPoolMirror capped;
  for (unsigned i = 0; i < 8192; ++i) CHECK(capped.wants({0, 0}, 8192));
  CHECK(!capped.wants({1, 0}, 8192) && capped.omitted == 1 && capped.casters == 8193);
  CHECK(capped.demand.frameBytes == Chunk);
  CHECK(scene::CasterCompleteLayers(f, nullptr, 0, capped.omitted, true) == 0);
  CHECK(scene::CasterCompleteLayers(f, nullptr, 0, 1, true) == 0); // unknown box
  CHECK(scene::CasterCompleteLayers(f, &far, 1, 1, false) == 0); // wrong stamp or reason
  scene::CasterLayerPublication publication;
  CHECK(publication.settle(5, 15, 15, false, key) == 0);
  for (unsigned n = 1; n <= 8; ++n) CHECK(publication.settle(5+n, 15, 3, true, key) == 12);
  CHECK(publication.settle(14, 15, 3, true, key) == 0);
  CHECK(publication.settle(15, 15, 15, true, key) == 0);
  CHECK(publication.settle(16, 15, 0, true, key) == 15);
  CHECK(publication.settle(17, 15, 3, false, key) == 0);
  CHECK(publication.settle(18, 15, 0, true, key) == 3);
  for (unsigned change = 0; change < key.size(); ++change) {
    publication.settle(20, 15, 15, true, key);
    ++key[change];
    CHECK(publication.settle(21, 15, 1, true, key) == 0);
    CHECK(publication.settle(22, 15, 0, true, key) == 1);
  }
  std::puts("cascade footprints, exact omission, hold 8/9, invalidation and recovery passed");
}

static void FrameAdmission() {
  scene::CasterCascadeFootprints f;
  f.count = 4;
  f.limit = 1;
  for (unsigned c = 0; c < 4; ++c)
    f.light[c] = {1.f / float(1 << c),0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  const scene::CasterLocalBounds near{{0,0,0,.5f,.5f,1}, true}, far{{1.5f,0,0,1.6f,.5f,1}, true};
  scene::CasterAdmission admission;
  scene::CasterCopyAccount pool;
  uint64_t limit = Chunk;
  auto draw = [&](const scene::CasterLocalBounds& box, scene::CasterCopySizes misses) {
    unsigned tier = 0;
    auto next = pool;
    scene::CasterCopyPlan plan;
    const bool fits = next.append(misses, limit, plan);
    if (!admission.allows(misses, box, limit, fits, next, tier)) return false;
    admission.accepted(tier, misses.vertex + misses.index);
    pool = next;
    return true;
  };
  auto finish = [&] { admission.finish(f); pool = {}; };

  scene::CasterLayerPublication layers;
  const scene::CasterLayerIdentity identity{1,1,1,1,1};
  CHECK(layers.settle(1, 15, 15, false, identity) == 0);
  for (unsigned age = 1; age <= 10; ++age) {
    const bool farFits = draw(far, {600000, 0});
    const bool nearFits = draw(near, {600000, 0});
    CHECK(farFits == (age == 1) && nearFits == (age != 1));
    const auto& omitted = nearFits ? far.box : near.box;
    const auto complete = scene::CasterCompleteLayers(f, &omitted, 1, 1, true);
    CHECK(complete == (age == 1 ? 0u : 1u));
    CHECK(layers.settle(age + 1, 15, complete, true, identity) == (age == 1 ? 15u : age <= 8 ? 14u : 0u));
    // A far draw can share both slices even after the near draw used the pool.
    CHECK(draw(far, {0, 0}) && pool.used == 600000);
    finish();
  }

  // 600000 + ceil(600000/32) reserved. At 16-byte alignment, the
  // last usable far cursor is 429824. The next 16 bytes must be refused.
  CHECK(draw(far, {429824, 0}) && !draw(far, {0, 1}));
  CHECK(draw(near, {600000, 0}));
  CHECK(!draw(far, {0, 16})); // the margin is still reserved
  finish();
  // The preceding misses fit without reservations, including the refused
  // single byte. Ordinary next-fit must take the far caster first again.
  CHECK(draw(far, {600000, 0}) && !draw(near, {600000, 0}));
  finish();
  CHECK(draw(near, {600000, 0}) && draw(far, {288, 0})); // nearer bytes already served
  CHECK(!draw(far, {600000, 0}));
  finish();
  CHECK(draw(far, {288, 0}) && draw(near, {288, 0}));
  finish();

  // Warm a near slice and charge only its small IB miss on subsequent
  // draws. Charging the cache hits would wrongly keep reservations alive.
  CHECK(draw(near, {600000, 0}));
  for (unsigned i = 0; i < 100; ++i) CHECK(draw(near, {0, 16}));
  finish();
  CHECK(draw(far, {600000, 0}) && !draw(near, {600000, 0}));
  finish();

  // Fill the producer's entire record window on each frame. The first
  // half is far; the second half needs the entire pool for near casters.
  constexpr unsigned half = scene::CasterFrameRecordLimit / 2;
  const scene::CasterCopySizes slice{Chunk / half, 0};
  for (unsigned frame = 0; frame < 32; ++frame) {
    unsigned admittedFar = 0, admittedNear = 0;
    for (unsigned i = 0; i < half; ++i) admittedFar += draw(far, slice);
    for (unsigned i = 0; i < half; ++i) admittedNear += draw(near, slice);
    CHECK(admittedFar + admittedNear == half);
    if (frame) CHECK(admittedFar == 0 && admittedNear == half);
    finish();
  }
  admission.finish({}); // discarded frame or epoch change
  pool = {};
  CHECK(draw(far, {600000, 0}) && !draw(near, {600000, 0}));
  admission.finish({});
  pool = {};
  limit = 2 * Chunk;
  const scene::CasterCopySizes fragmented{680 * 1024, 0};
  CHECK(draw(far, fragmented) && draw(far, fragmented) && !draw(near, fragmented));
  finish();
  CHECK(draw(far, fragmented) && !draw(far, fragmented) && draw(near, fragmented));
  std::puts("first overflow, near reservation, margin, reuse, fragmentation, recovery and 32 full record windows passed");
}

static void CasterClassControls() {
  using Class = scene::CasterClass;
  struct Case { unsigned category, stage, kind; bool proven; Class result; };
  for (const Case t : {Case{2,1,1,true,Class::Terrain}, {2,10,0,true,Class::Doodad},
      {3,10,0,false,Class::Doodad}, {3,10,2,true,Class::Building}, {3,11,1,true,Class::Unit},
      {3,11,3,true,Class::Destructible}, {3,11,4,true,Class::Other}, {4,7,5,true,Class::Other},
      {3,11,0,true,Class::Unit}, {3,11,0,false,Class::Other}, {3,13,0,false,Class::Other}})
    CHECK(scene::ClassifyCaster(t.category, t.stage, t.kind, t.proven) == t.result);
  struct Settings { bool castShadow = true; float maxShadowLength = 0; };
  std::array<Settings,5> classes;
  warvk::diag::CasterWords words{warvk::diag::CasterWord(false, 1.25f), 0,
      warvk::diag::CasterWord(true, 8), warvk::diag::CasterWord(false, 0), 0xffffffffu};
  warvk::diag::ReadCasterWords(classes, words);
  CHECK(!classes[0].castShadow && classes[0].maxShadowLength == 1.25f);
  CHECK(classes[1].castShadow && classes[1].maxShadowLength == 0);
  CHECK(classes[2].castShadow && classes[2].maxShadowLength == 8);
  CHECK(!classes[3].castShadow && !classes[3].maxShadowLength && classes[4].castShadow);
  // Two views of a fixture-owned mapping, with no game, host or Present.
  namespace diag = warvk::diag;
  const DWORD pid = GetCurrentProcessId();
  wchar_t name[64];
  diag::BlockName(name, 64, pid);
  diag::CasterSettingsView reader, writer;
  reader.open(pid);
  CHECK(reader.read() == diag::CasterWords{});
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(diag::Block), name);
  CHECK(mapping && GetLastError() != ERROR_ALREADY_EXISTS);
  auto* block = static_cast<diag::Block*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(diag::Block)));
  CHECK(block);
  block->bytes = sizeof(*block);
  block->version = diag::Version;
  block->gamePid = pid;
  block->magic = diag::Magic;
  writer.open(pid);
  reader.open(pid);
  writer.publish(words);
  CHECK(reader.read() == words);
  auto next = words;
  next[1] = diag::CasterWord(false, 4.125f);
  InterlockedIncrement(reinterpret_cast<volatile LONG*>(&block->reservedGame[diag::ClassCount]));
  block->reservedGame[1] = next[1];
  CHECK(reader.read() == words);
  InterlockedIncrement(reinterpret_cast<volatile LONG*>(&block->reservedGame[diag::ClassCount]));
  CHECK(reader.read() == next);
  UnmapViewOfFile(block);
  CloseHandle(mapping);
  CHECK(scene::CasterHeightScale(1, 3, 4, -2) == 0.4f);
  CHECK(scene::CasterHeightScale(0, 3, 4, -2) == 1);
  CHECK(scene::CasterHeightScale(4, 3, 4, -2) == 1 && scene::CasterHeightScale(1,0,0,1) == 1);

  const float unknown = scene::CasterPartBase::unknown;
  auto body = scene::CasterBase(8, 0,0,0, 80);
  auto foot = scene::CasterBase(8, 12,30,22, 30);
  auto other = scene::CasterBase(9, 0,0,0, 4);
  auto anon = scene::CasterBase(0, 1,0,50, 30);
  auto exact = scene::CasterBase(0, 0,0,0, 32);
  auto missing = scene::CasterBase(0, 0,0,0, unknown);
  scene::CasterBases bases;
  bases.reset(4);
  for (auto part : {body, foot, other, missing}) bases.add(part);
  // A placed object (building, tree) stands on its lowest placed origin even
  // when a part reaches lower; a unit drawn in world space stands on its lowest
  // vertex. Both are shared by every part of the object.
  CHECK(bases.resolve(body, true) == 22 && bases.resolve(foot, true) == 22);
  CHECK(bases.resolve(body, false) == 30 && bases.resolve(foot, false) == 30);
  CHECK(bases.resolve(other, true) == 4 && bases.resolve(other, false) == 4);
  CHECK(bases.resolve(anon, true) == 50 && bases.resolve(anon, false) == 30);
  CHECK(bases.resolve(exact, true) == 32 && !std::isfinite(bases.resolve(missing, false)));
  CHECK(scene::CasterBasePrefersOrigin(scene::CasterClass::Building) &&
        scene::CasterBasePrefersOrigin(scene::CasterClass::Destructible) &&
        !scene::CasterBasePrefersOrigin(scene::CasterClass::Unit));
  bases.reset(1);
  bases.add(body);
  CHECK(bases.resolve(body, true) == 80 && bases.resolve(body, false) == 80);

  using Matrix = std::array<std::array<float,4>,4>;
  const Matrix light{{{1,0,2,0},{0,3,0,0},{2,-4,3,0},{10,11,12,1}}};
  const auto flat = scene::CasterLightMatrix(light, .25f, 22);
  for (float height : {0.f, 4.f, 80.f})
    for (unsigned j = 0; j < 4; ++j)
      CHECK(flat[2][j]*(22+height) + flat[3][j] == light[2][j]*(22+height*.25f) + light[3][j]);
  const auto untouched = scene::CasterLightMatrix(light, 1, 22);
  CHECK(!std::memcmp(&untouched, &light, sizeof(light)));
  CHECK(scene::CasterLightMatrix(light, .25f, unknown) == light);
  const scene::CasterBox box{3,4,80,6,7,100};
  const scene::CasterBox extended{3,4,22,6,7,100};
  CHECK(scene::ExtendCasterBox(box, 22) == extended);
  CHECK(scene::ExtendCasterBox(box, 110)[5] == 110);
  std::puts("class policy, word defaults, sun, shared base, matrix and conservative box passed");
}

static void RebasedIndexContent() {
  const uint16_t original[]{30,40,39,30}, shifted[]{130,140,139,130}, different[]{0,10,6,0};
  std::vector<uint16_t> scratch;
  const auto rebase = [&](const uint16_t* p) { return scene::RebaseCasterIndices(scratch,
      reinterpret_cast<const uint8_t*>(p), 4, p[0]); };
  std::array<uint8_t,8> saved;
  std::memcpy(saved.data(), rebase(original), 8);
  CHECK(!std::memcmp(saved.data(), rebase(shifted), 8));
  scene::CasterIndexTable<unsigned> table;
  const uint64_t hash = scene::CasterIndexHash(saved.data(), 8);
  unsigned hit = 0;
  const auto slot = table.find(hash, saved.data(), 8, hit);
  CHECK(!hit);
  table.insert(slot, hash, saved.data(), 8, 14u);
  // The table compares with its own copy: the inserted source (in the host, a
  // slice in write-combined GPU memory) is never read again.
  const auto kept = saved;
  saved.fill(0xee);
  table.find(hash, rebase(shifted), 8, hit);
  CHECK(hit == 14);
  saved = kept;
  hit = 0;
  table.find(hash, reinterpret_cast<const uint8_t*>(different), 8, hit);
  CHECK(!hit);
  table.clear();
  table.find(hash, saved.data(), 8, hit);
  CHECK(!hit);
  CHECK(scene::CasterIndexedAddress::from(-20, 30, true).rebased(30) == 0);
  CHECK(scene::CasterIndexedAddress::from(-20, 30, false).rebased(30) == 10);
  std::puts("normalized index bytes, collision refusal, frame lifetime and signed replay base passed");
}

static void QuantizedCaster() {
  const std::array<float,16> world{0,1.25f,0,0, -1.5f,0,.25f,0, .4f,.2f,2,0, 123,-87,58,1};
  scene::CasterLocalBounds local{{-20,-100,10,180,350,200},true};
  scene::CasterQuantization q;
  CHECK(q.prepare(local, world));
  std::mt19937 random(127);
  double maxError = 0;
  for (unsigned i = 0; i < 8192; ++i) {
    float p[3];
    for (unsigned j = 0; j < 3; ++j) {
      const float f = i < 2 ? float(i) : float(random() >> 8) / 16777216.f;
      p[j] = local.box[j] + f*(local.box[j+3]-local.box[j]);
    }
    uint16_t packed[4]{};
    q.write(reinterpret_cast<uint8_t*>(packed), reinterpret_cast<const uint8_t*>(p), 12, 1);
    CHECK(packed[3] == UINT16_MAX);
    for (unsigned j = 0; j < 3; ++j) {
      float actual = q.world[12+j];
      double expected = world[12+j];
      for (unsigned axis = 0; axis < 3; ++axis) {
        actual += float(packed[axis])/65535.f * q.world[4*axis+j];
        expected += double(p[axis]) * world[4*axis+j];
      }
      maxError = std::max(maxError, std::abs(actual - expected));
      CHECK(std::abs(actual - expected) <= 1.0/128);
    }
  }
  // A half-step near the largest admitted world extent, on every axis.
  const scene::CasterLocalBounds wide{{0,0,0,930,930,930},true};
  const std::array<float,16> identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  CHECK(q.prepare(wide, identity));
  const float mid = 930.0f * 10000.5f / 65535.0f;
  const float p[3]{mid,mid,mid};
  uint16_t packed[4];
  q.write(reinterpret_cast<uint8_t*>(packed), reinterpret_cast<const uint8_t*>(p), 12, 1);
  for (unsigned j = 0; j < 3; ++j) {
    const double error = std::abs(float(packed[j])/65535.0f * q.world[5*j] - mid);
    CHECK(error > 0.007 && error <= 1.0/128);
    maxError = std::max(maxError, error);
  }
  auto bad = local;
  bad.box[3] = bad.box[0];
  CHECK(!q.prepare(bad, world));
  bad.box[3] = 5000;
  CHECK(!q.prepare(bad, world));
  bad.box[3] = std::numeric_limits<float>::quiet_NaN();
  CHECK(!q.prepare(bad, world));
  auto cancelled = identity;
  cancelled[12] = -1000000.f;
  CHECK(!q.prepare({{1000000,0,0,1000010,10,10},true}, cancelled));
  auto scaled = world;
  scaled[1] *= 100;
  CHECK(!q.prepare(local, scaled));
  scene::CasterCopyAccount budget;
  scene::CasterCopyPlan plan;
  CHECK(budget.append({50000*8,72}, Chunk, plan));
  CHECK(budget.append({50000*8,0}, Chunk, plan));
  CHECK(budget.frameBytes == Chunk && budget.used == 800080);
  std::printf("quantization, transformed error (peak %.8f), unsuitable bounds and 1 MiB packed admission passed\n", maxError);
}

int wmain() {
  CasterClassControls();
  RebasedIndexContent();
  QuantizedCaster();
  FrameAdmission();
  LayerPressure();
  BudgetAndDemand();
  AccountParity();
  Extract(12, 0, false, 0);
  Extract(20, 0, true, 12);
  Extract(32, 4, false, 0);
  Extract(32, 0, true, 24);
  Extract(39, 25, true, 3);
  std::puts("5 interleaved vertex extraction layouts and output sentinels passed");
  TrimAndReplay();
  Reuse();
  ExactBounds();
  CascadeBounds();
  return 0;
}
