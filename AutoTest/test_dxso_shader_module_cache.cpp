// REAL shared DXSO cache/compiler, CPU only. The registrar is an observer,
// not Vulkan registration; this does not certify GPU/driver cache retirement.
#include "../src/dxso/dxso_shader_module_cache.h"
#include "../src/util/log/log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace dxvk { Logger Logger::s_instance("dxso-shader-module-cache-test.log"); }
using namespace dxvk;
namespace {
  unsigned checks = 0;
  void check(bool good, unsigned line) {
    ++checks;
    if (!good) {
      std::fprintf(stderr, "DXSO cache check failed line=%u\n", line);
      std::exit(1);
    }
  }
#define CHECK(x) check(bool(x), __LINE__)
  using Words = std::vector<uint32_t>;
  Words vertex() {
    return {0xfffe0300u,
      0x0200001fu, 0x80000000u, 0x900f0000u,
      0x0200001fu, 0x80000000u, 0xe00f0000u,
      0x02000001u, 0xe00f0000u, 0x90e40000u, 0x0000ffffu};
  }
  Words pixel(uint32_t red = 0x3f800000u) {
    return {0xffff0300u,
      0x05000051u, 0xa00f0000u, red, 0, 0, 0x3f800000u, // def c0
      0x02000001u, 0x800f0800u, 0xa0e40000u, 0x0000ffffu};
  }
  DxsoShaderModuleDomain domain(const void* device) {
    return {device, {}, {256, 16, 16, 1}, {224, 16, 16, 1}};
  }
  const DxsoShaderModuleCache::Validate accept = [](const DxsoCompiledModule&, bool) {};
  template<typename F> bool throws(F&& f) {
    try { f(); return false; } catch (...) { return true; }
  }
  auto get(DxsoShaderModuleCache& cache, const DxsoShaderModuleDomain& d,
      const Words& words, const DxsoShaderModuleCache::Validate& validator = accept) {
    return cache.getOrCompile(d, words[0] == 0xfffe0300u
      ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT,
      words.data(), uint32_t(words.size() * 4), validator);
  }

  void identityAndDomain() {
    int deviceIdentity = 0, otherDevice = 0;
    const auto d = domain(&deviceIdentity);
    unsigned registered = 0;
    DxsoShaderModuleCache cache(d, [&](const Rc<DxvkShader>&) { ++registered; });
    const auto words = pixel();
    auto first = get(cache, d, words);
    const auto cookie = first->shader->getCookie();
    CHECK(first->info.shaderStage() == VK_SHADER_STAGE_FRAGMENT_BIT);
    CHECK(first->usedRTs == 1 && first->usedSamplers == 0);
    CHECK(first->maxDefinedFloat == 0 && first->constants.size() == 1);
    CHECK(first->constants[0].float32[0] == 1.0f);
    first.reset();
    const auto copiedWords = words; // Different caller storage, identical tokens.
    const auto again = get(cache, d, copiedWords);
    CHECK(again->shader->getCookie() == cookie && registered == 1);
    CHECK(again->constants.size() == 1 && again->constants[0].float32[0] == 1.0f);
    const auto changed = get(cache, d, pixel(0x3f000000u));
    CHECK(changed->shader->getCookie() != cookie);
    CHECK(changed->constants[0].float32[0] == 0.5f);
    auto commented = words;
    commented.insert(commented.end() - 1, {0x0001fffeu, 0x12345678u});
    const auto longer = get(cache, d, commented);
    CHECK(longer->shader->getCookie() != cookie && longer->constants.size() == 1);
    const auto vs = get(cache, d, vertex());
    CHECK(vs->shader->getCookie() != cookie && vs->info.shaderStage() == VK_SHADER_STAGE_VERTEX_BIT);
    CHECK(vs->isgn.elemCount == 1 && vs->usedSamplers == 0);
    auto suffix = words; suffix.push_back(0);
    CHECK(throws([&] { get(cache, d, suffix); }));
    CHECK(throws([&] { cache.getOrCompile(d, VK_SHADER_STAGE_VERTEX_BIT,
      words.data(), uint32_t(words.size() * 4), accept); }));
    const auto before = cache.stats();
    for (unsigned i = 0; i < 18; ++i) {
      auto bad = d;
      switch (i) {
        case 0: bad.deviceIdentity = &otherDevice; break;
        case 1: bad.moduleInfo.options.strictConstantCopies = true; break;
        case 2: bad.moduleInfo.options.strictPow = true; break;
        case 3: bad.moduleInfo.options.invariantPosition = true; break;
        case 4: bad.moduleInfo.options.forceSamplerTypeSpecConstants = true; break;
        case 5: bad.moduleInfo.options.forceSampleRateShading = true; break;
        case 6: bad.moduleInfo.options.vertexFloatConstantBufferAsSSBO = true; break;
        case 7: bad.moduleInfo.options.robustness2Supported = true; break;
        case 8: bad.moduleInfo.options.sincosEmulation = true; break;
        case 9: bad.moduleInfo.options.d3d9FloatEmulation = static_cast<D3D9FloatEmulation>(1); break;
        case 10: ++bad.vertexLayout.floatCount; break;
        case 11: ++bad.vertexLayout.intCount; break;
        case 12: ++bad.vertexLayout.boolCount; break;
        case 13: ++bad.vertexLayout.bitmaskCount; break;
        case 14: ++bad.pixelLayout.floatCount; break;
        case 15: ++bad.pixelLayout.intCount; break;
        case 16: ++bad.pixelLayout.boolCount; break;
        case 17: ++bad.pixelLayout.bitmaskCount; break;
      }
      CHECK(!d.matches(bad));
      CHECK(throws([&] { get(cache, bad, words); }));
    }
    CHECK(cache.stats().compilesSucceeded == before.compilesSucceeded && registered == 4);
    CHECK(before.compilesSucceeded == 4 && before.registrations == 4 && before.readyEntries == 4);
    CHECK(before.hits == 1 && before.failedEntries == 0);
    CHECK(before.keyBytes == (words.size() * 2 + commented.size() + vertex().size()) * 4);
    CHECK(before.keyCapacityBytes >= before.keyBytes);
    CHECK(before.constantBytes == 3 * sizeof(DxsoDefinedConstant));
    CHECK(before.constantCapacityBytes >= before.constantBytes);
    CHECK(throws([&] { DxsoShaderModuleCache invalid(domain(nullptr), [](const auto&) {}); }));
    CHECK(throws([&] { DxsoShaderModuleCache invalid(d, {}); }));
    CHECK(throws([&] { get(cache, d, words, {}); }));
  }

  void failureOwnership() {
    int identity = 0;
    const auto d = domain(&identity);
    unsigned registered = 0;
    DxsoShaderModuleCache cache(d, [&](const auto&) { ++registered; });
    CHECK(throws([&] { get(cache, d, pixel(), [](const auto&, bool) {
      throw std::runtime_error("validation refused"); }); }));
    CHECK(registered == 0 && cache.stats().readyEntries == 0 && cache.stats().validationFailures == 1);
    const auto accepted = get(cache, d, pixel());
    CHECK(accepted && registered == 1 && cache.stats().compilesSucceeded == 2);
    CHECK(throws([&] { get(cache, d, pixel(), [](const auto&, bool) {
      throw std::runtime_error("cached admission refused"); }); }));
    CHECK(registered == 1 && cache.stats().validationFailures == 2);

    Rc<DxvkShader> uncertainPartialReference;
    unsigned registrationAttempts = 0;
    DxsoShaderModuleCache failed(d, [&](const Rc<DxvkShader>& shader) {
      ++registrationAttempts;
      uncertainPartialReference = shader;
      throw std::runtime_error("partial registration");
    });
    CHECK(throws([&] { get(failed, d, pixel()); }));
    CHECK(throws([&] { get(failed, d, pixel()); }));
    const auto failure = failed.stats();
    CHECK(registrationAttempts == 1 && uncertainPartialReference != nullptr);
    CHECK(failure.registrationFailures == 1 && failure.failedEntries == 1);
    CHECK(failure.readyEntries == 0 && failure.registrations == 0 && failure.compilesSucceeded == 1);
    CHECK(failure.keyBytes == pixel().size() * 4);

    std::weak_ptr<const DxsoCompiledModule> weak;
    {
      DxsoShaderModuleCache owned(d, [](const auto&) {});
      auto value = get(owned, d, pixel()); weak = value; value.reset();
      CHECK(!weak.expired());
    }
    CHECK(weak.expired()); // CPU payload owner only, not driver retirement.
  }

  void race(bool sameKey) {
    int identity = 0;
    const auto d = domain(&identity);
    unsigned registrations = 0;
    DxsoShaderModuleCache cache(d, [&](const auto&) { ++registrations; });
    std::mutex mutex;
    std::condition_variable condition;
    unsigned arrived = 0;
    const auto rendezvous = [&](const auto&, bool fresh) {
      if (!fresh) return;
      std::unique_lock<std::mutex> lock(mutex);
      ++arrived; condition.notify_all();
      if (!condition.wait_for(lock, std::chrono::seconds(5), [&] { return arrived == 2; }))
        throw std::runtime_error("CPU compiler race rendezvous timed out");
    };
    DxsoShaderModuleCache::Module values[2];
    std::exception_ptr failures[2];
    const auto run = [&](unsigned index) {
      try { values[index] = get(cache, d,
        pixel(!sameKey && index ? 0x3f000000u : 0x3f800000u), rendezvous); }
      catch (...) { failures[index] = std::current_exception(); }
    };
    std::thread first(run, 0), second(run, 1);
    first.join(); second.join();
    CHECK(!failures[0] && !failures[1] && arrived == 2);
    const auto stats = cache.stats();
    CHECK(stats.requests == 2 && stats.compileAttempts == 2 && stats.compilesSucceeded == 2);
    CHECK(registrations == (sameKey ? 1u : 2u) && stats.registrations == registrations);
    CHECK(stats.readyEntries == registrations && stats.failedEntries == 0);
    CHECK(stats.raceLosers == (sameKey ? 1u : 0u) && stats.hits == stats.raceLosers);
    CHECK((values[0]->shader.ptr() == values[1]->shader.ptr()) == sameKey);
    CHECK((values[0]->shader->getCookie() == values[1]->shader->getCookie()) == sameKey);
  }
}

int main() {
#ifdef _WIN32
  CHECK(GetModuleHandleW(L"vulkan-1.dll") == nullptr);
#endif
  try {
    identityAndDomain(); failureOwnership(); race(true); race(false);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "DXSO cache exception: %s\n", e.what()); return 2;
  }
#ifdef _WIN32
  CHECK(GetModuleHandleW(L"vulkan-1.dll") == nullptr);
#endif
  std::printf("DXSO_SHADER_MODULE_CACHE_CPU checks=%u PASS noDeviceCreated=true\n", checks);
  return 0;
}
