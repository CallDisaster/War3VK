#include "../war3_tracked_vk_pipeline.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

  using dxvk::Rc;
  using dxvk::war3::render::War3TrackedVkPipeline;

  int g_failures = 0;

  void Check(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAILED: " << message << '\n';
      ++g_failures;
    }
  }

  class FakeCompletedCommandList {

  public:

    void track(const Rc<War3TrackedVkPipeline>& owner) {
      m_inFlight.push_back(owner);
    }

    void completeGpuWork() {
      m_inFlight.clear();
    }

  private:

    std::vector<Rc<War3TrackedVkPipeline>> m_inFlight;

  };

  void TestCacheReleaseWaitsForGpuCompletion() {
    uint32_t destroyCount = 0u;
    const VkPipeline fakeHandle = (VkPipeline)(uintptr_t{0x1234u});

    FakeCompletedCommandList submitted;
    Rc<War3TrackedVkPipeline> cacheOwner =
        new War3TrackedVkPipeline(fakeHandle, [&] (VkPipeline handle) {
          Check(handle == fakeHandle, "destructor must receive adopted handle");
          ++destroyCount;
        });

    submitted.track(cacheOwner);
    cacheOwner = nullptr; // hot reload / bias cache invalidation
    Check(destroyCount == 0u,
          "pipeline must not be destroyed before GPU completion");

    submitted.completeGpuWork();
    Check(destroyCount == 1u,
          "pipeline must be reclaimed after the final GPU owner completes");
  }

  void TestUnusedPipelineCanRetireImmediately() {
    uint32_t destroyCount = 0u;
    Rc<War3TrackedVkPipeline> owner = new War3TrackedVkPipeline(
        (VkPipeline)(uintptr_t{0x5678u}),
        [&] (VkPipeline) { ++destroyCount; });
    owner = nullptr;
    Check(destroyCount == 1u,
          "pipeline never submitted to GPU may retire immediately");
  }

}

int main() {
  TestCacheReleaseWaitsForGpuCompletion();
  TestUnusedPipelineCanRetireImmediately();
  if (g_failures != 0) {
    std::cerr << g_failures << " tracked pipeline test(s) failed\n";
    return 1;
  }
  std::cout << "war3_tracked_vk_pipeline_test: PASS\n";
  return 0;
}
