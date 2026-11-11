#include "../src/d3d9/war3/tools/war3_native_render_input_export.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#define NATIVE_COST_NOINLINE __declspec(noinline)
#else
#define NATIVE_COST_NOINLINE __attribute__((noinline))
#endif

// Single-threaded allocation trap. Only the explicit capture windows count;
// library/runtime initialization and reporting are outside those windows.
static volatile bool allocationWindow = false;
static volatile uint64_t allocationCalls = 0;
static void noteAllocation() noexcept {
  if (allocationWindow) ++allocationCalls;
}

void* operator new(std::size_t size) {
  noteAllocation();
  if (void* result = std::malloc(size ? size : 1)) return result;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return ::operator new(size, std::nothrow);
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { std::free(value); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  noteAllocation();
  const auto align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
  if (void* result = _aligned_malloc(size ? size : 1, align)) return result;
#else
  const auto length = size ? size : 1;
  if (length <= std::numeric_limits<std::size_t>::max() - (align - 1)) {
    const auto padded = (length + align - 1) & ~(align - 1);
    if (void* result = std::aligned_alloc(align, padded)) return result;
  }
#endif
  throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return ::operator new(size, alignment, std::nothrow);
}
void operator delete(void* value, std::align_val_t) noexcept {
#if defined(_WIN32)
  _aligned_free(value);
#else
  std::free(value);
#endif
}
void operator delete[](void* value, std::align_val_t alignment) noexcept { ::operator delete(value, alignment); }
void operator delete(void* value, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(value, alignment); }
void operator delete[](void* value, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(value, alignment); }
void operator delete(void* value, std::align_val_t alignment, const std::nothrow_t&) noexcept { ::operator delete(value, alignment); }
void operator delete[](void* value, std::align_val_t alignment, const std::nothrow_t&) noexcept { ::operator delete(value, alignment); }

namespace n = dxvk::war3::render::native_input;
namespace e = dxvk::war3::tools::evidence;

static NATIVE_COST_NOINLINE n::NativeDrawMaterialFacts captureForCost(bool enabled,
    const n::MaterialFactsKey& key, const n::MaterialFactsViews& views) noexcept {
  // Keep each actual helper invocation observable to the compiler. This is a
  // compiler barrier, not a runtime fence, lock, or additional lifecycle proof.
#if defined(_MSC_VER)
  _ReadWriteBarrier();
#else
  __asm__ __volatile__("" ::: "memory");
#endif
  return n::CaptureMaterialFacts(enabled, key, views);
}

static uint64_t bitChecksum(const n::NativeDrawMaterialFacts& facts) noexcept {
  // Consume identity and a raw word from every captured array. Exhaustive
  // array equality is checked once outside the timed loops below.
  return facts.renderFrame ^ facts.mapEpoch ^ facts.deviceEpoch ^ facts.renderablePart
    ^ facts.layerIndex ^ facts.shaderMask ^ facts.schemaVersion ^ facts.available
    ^ (uint64_t(facts.material[3]) << 32) ^ facts.textureTransform0[15]
    ^ facts.renderStates[16] ^ facts.textureStage0[9] ^ facts.sampler0[7];
}

static void assertUnknown(const n::NativeDrawMaterialFacts& facts) {
  assert(facts.schemaVersion == 1 && facts.available == 0);
  assert(facts.renderFrame == 0 && facts.mapEpoch == 0 && facts.deviceEpoch == 0);
  assert(facts.renderablePart == 0 && facts.layerIndex == 0 && facts.shaderMask == 0);
  for (auto word : facts.renderStates) assert(word == 0);
  for (auto word : facts.textureStage0) assert(word == 0);
  for (auto word : facts.sampler0) assert(word == 0);
  for (auto word : facts.material) assert(word == 0);
  for (auto word : facts.textureTransform0) assert(word == 0);
}

int main() {
  constexpr uint64_t captures = 100000;
  uint32_t render[n::RenderStateCount] = {};
  uint32_t stage[n::StageStateCount] = {};
  uint32_t sampler[n::SamplerStateCount] = {};
  uint32_t material[n::MaterialWordCount] = {};
  uint32_t transform[n::TextureTransformWordCount] = {};
  for (size_t i = 0; i < n::RenderStateCount; ++i) render[i] = 100u + uint32_t(i);
  for (size_t i = 0; i < n::StageStateCount; ++i) stage[i] = 200u + uint32_t(i);
  for (size_t i = 0; i < n::SamplerStateCount; ++i) sampler[i] = 300u + uint32_t(i);
  for (size_t i = 0; i < n::MaterialWordCount; ++i) material[i] = n::FloatBits(float(i));
  for (size_t i = 0; i < n::TextureTransformWordCount; ++i) transform[i] = n::FloatBits(float(i));
  material[3] = n::FloatBits(-0.0f);
  sampler[7] = n::FloatBits(-0.0f);
  const n::MaterialFactsViews views{render, stage, sampler, material, transform};
  const auto poison = reinterpret_cast<const uint32_t*>(uintptr_t(1));
  const n::MaterialFactsViews poisonViews{poison, poison, poison, poison, poison};
  n::MaterialFactsKey key{1, 7, 9, 0x123400, 0, 3};

  // Unknown enabled inputs must also short-circuit before any poison read.
  auto unkeyed = key; unkeyed.renderFrame = 0;
  auto missing = poisonViews; missing.material = nullptr;
  allocationWindow = true;
  const auto disabledUnknown = captureForCost(false, key, poisonViews);
  const auto unkeyedUnknown = captureForCost(true, unkeyed, poisonViews);
  const auto missingUnknown = captureForCost(true, key, missing);
  const auto known = captureForCost(true, key, views);
  allocationWindow = false;
  assertUnknown(disabledUnknown); assertUnknown(unkeyedUnknown); assertUnknown(missingUnknown);
  assert(n::MaterialFactsMatches(known, key));
  assert(std::memcmp(known.renderStates, render, sizeof(render)) == 0);
  assert(std::memcmp(known.textureStage0, stage, sizeof(stage)) == 0);
  assert(std::memcmp(known.sampler0, sampler, sizeof(sampler)) == 0);
  assert(std::memcmp(known.material, material, sizeof(material)) == 0);
  assert(std::memcmp(known.textureTransform0, transform, sizeof(transform)) == 0);
  assert(allocationCalls == 0);

  const uint64_t expectedSeed = key.mapEpoch ^ key.deviceEpoch ^ key.renderablePart
    ^ key.layerIndex ^ key.shaderMask ^ uint64_t(1) ^ uint64_t(1)
    ^ (uint64_t(material[3]) << 32) ^ transform[15] ^ render[16] ^ stage[9] ^ sampler[7];
  uint64_t expectedEnabled = 0;
  for (uint64_t frame = 1; frame <= captures; ++frame) expectedEnabled += expectedSeed ^ frame;

  uint64_t enabledChecksum = 0;
  const auto enabledStart = std::chrono::steady_clock::now();
  allocationWindow = true;
  for (uint64_t frame = 1; frame <= captures; ++frame) {
    key.renderFrame = frame;
    enabledChecksum += bitChecksum(captureForCost(true, key, views));
  }
  allocationWindow = false;
  const auto enabledStop = std::chrono::steady_clock::now();
  assert(enabledChecksum == expectedEnabled && allocationCalls == 0);

  uint64_t disabledChecksum = 0;
  const auto disabledStart = std::chrono::steady_clock::now();
  allocationWindow = true;
  for (uint64_t frame = 1; frame <= captures; ++frame) {
    key.renderFrame = frame;
    disabledChecksum += bitChecksum(captureForCost(false, key, poisonViews));
  }
  allocationWindow = false;
  const auto disabledStop = std::chrono::steady_clock::now();
  assert(disabledChecksum == captures && allocationCalls == 0);

  const double enabledNs = std::chrono::duration<double, std::nano>(enabledStop - enabledStart).count() / captures;
  const double disabledNs = std::chrono::duration<double, std::nano>(disabledStop - disabledStart).count() / captures;
  std::printf("sizeofNativeDrawMaterialFacts=%zu sizeofFinalShadowMaterialFacts=%zu captures_per_path=%llu\n",
    sizeof(n::NativeDrawMaterialFacts), sizeof(e::FinalShadowMaterialFacts),
    static_cast<unsigned long long>(captures));
  std::printf("capture_window_new_calls=%llu enabled_bit_checksum=%llu disabled_bit_checksum=%llu\n",
    static_cast<unsigned long long>(allocationCalls), static_cast<unsigned long long>(enabledChecksum),
    static_cast<unsigned long long>(disabledChecksum));
  std::printf("host_enabled_ns_per_op=%.3f host_disabled_poison_ns_per_op=%.3f\n", enabledNs, disabledNs);
  std::printf("PASS CPU-only observation: timings include noinline wrapper/checksum; not game-frame cost or a zero-cost claim\n");
}
