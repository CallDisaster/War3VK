// Actual prepare/reset/prepareRequestedSlot functions, fake resource allocation.
// No Vulkan instance or file I/O. Real fence/readback timing is a separate gate.
#include "../src/d3d9/war3/tools/war3_async_screenshot_core.h"
#include "../src/util/util_error.h"
#include "../src/util/util_string.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <stdexcept>

namespace dxvk::war3::tools {
using screenshot::State;
template<class T> using Rc = std::shared_ptr<T>;
struct Logger {
  static void info(const std::string&) { }
  static void err(const std::string&) { }
};
struct Registry { std::mutex mutex; };
Registry& registry() { static Registry r; return r; }
struct DxvkBufferCreateInfo {
  uint64_t size = 0; unsigned usage = 0, stages = 0, access = 0;
  const char* debugName = nullptr;
};
struct DxvkFenceCreateInfo { uint64_t initial = 0; };
struct DxvkFence { };
struct DxvkBuffer {
  DxvkBufferCreateInfo value;
  const auto& info() const { return value; }
  DxvkBuffer* storage() { return this; }
  unsigned getMemoryProperties() { return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; }
  void* mapPtr(unsigned) { return this; }
};
struct DxvkImage {
  struct Info {
    VkImageType type = VK_IMAGE_TYPE_2D;
    unsigned sampleCount = VK_SAMPLE_COUNT_1_BIT, numLayers = 1, mipLevels = 1;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent3D extent{};
  } value;
  const Info& info() const { return value; }
};
struct Device {
  unsigned bufferCreates = 0, fenceCreates = 0;
  bool fail = false;
  VkResult getDeviceStatus() { return VK_SUCCESS; }
  Rc<DxvkBuffer> createBuffer(const DxvkBufferCreateInfo& info, unsigned) {
    if (fail) throw DxvkError("injected");
    ++bufferCreates; auto p = std::make_shared<DxvkBuffer>(); p->value = info; return p;
  }
  Rc<DxvkFence> createFence(DxvkFenceCreateInfo) { ++fenceCreates; return std::make_shared<DxvkFence>(); }
};
struct AsyncScreenshot {
  struct Impl {
    struct Slot {
      std::atomic<State> state{State::Retired};
      Rc<DxvkBuffer> buffer; Rc<DxvkFence> fence;
      uint64_t value = 0;
      uint32_t width = 0, height = 0;
      std::shared_ptr<int> history;
    };
    std::array<Slot, screenshot::SlotCount> slots;
    Rc<Device> device = std::make_shared<Device>();
    std::atomic<bool> available{false}, fault{false};
    bool sourceReady = false;
    uint32_t width = 0, height = 0;
    uint64_t presentOrdinal = 0;
    std::atomic<unsigned> burstRemaining{0}, cancelled{0};
    std::condition_variable wake;
#include "screenshot_request_probe.inc"
  };
  std::unique_ptr<Impl> m = std::make_unique<Impl>();
  void reset();
  void beginPresent();
  void prepare(const Rc<DxvkImage>& image, bool advanceBurst = true);
};
#include "screenshot_cold_probe.inc"
}
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); ++checks; } while (0)
int main() {
  unsigned checks = 0;
  try {
    using namespace dxvk::war3::tools;
    for (auto size : {std::pair{1920u,1080u}, {2560u,1440u}, {3840u,2160u}}) {
      AsyncScreenshot shot;
      auto image = std::make_shared<DxvkImage>(); image->value.extent = {size.first,size.second,1};
      for (unsigned n = 0; n != 1000; ++n) { shot.beginPresent(); shot.prepare(image); }
      CHECK(shot.m->available); CHECK(shot.m->device->bufferCreates == 0);
      CHECK(shot.m->device->fenceCreates == 0);
      auto& slot = shot.m->slots[0];
      CHECK(screenshot::Claim(slot.state, State::Free, State::Requested));
      CHECK(screenshot::Claim(slot.state, State::Requested, State::Preparing));
      CHECK(shot.m->prepareRequestedSlot(slot));
      CHECK(slot.buffer->info().size == uint64_t(size.first)*size.second*4);
      CHECK(shot.m->device->bufferCreates == 1 && shot.m->device->fenceCreates == 1);
      auto csReference = slot.buffer;
      slot.state = State::Submitted;
      shot.prepare(image); CHECK(slot.buffer == csReference); // never recycle in-flight
      shot.reset(); CHECK(slot.buffer == csReference && slot.state == State::Submitted);
      image->value.extent = {640,480,1}; shot.prepare(image);
      CHECK(slot.width == size.first); // old submitted image remains immutable
      slot.state = State::Retired; // worker's actual fence + encoder completion boundary
      shot.prepare(image); CHECK(!slot.buffer && !slot.fence && slot.value == 0);
      CHECK(csReference->info().size == uint64_t(size.first)*size.second*4); // independent Rc survives
      CHECK(screenshot::Claim(slot.state, State::Free, State::Preparing));
      CHECK(shot.m->prepareRequestedSlot(slot));
      CHECK(slot.width == 640 && slot.height == 480);
      slot.state = State::Retired; shot.prepare(image);
      shot.m->burstRemaining = 3; shot.prepare(image);
      CHECK(slot.state == State::Requested && !slot.buffer); // intent alone is not a buffer
      image->value.sampleCount = VK_SAMPLE_COUNT_4_BIT; shot.prepare(image);
      CHECK(!shot.m->available && slot.state == State::Retired);
      // Reclaim even on a skipped Present (no prepare/image available).
      slot.buffer = csReference; slot.state = State::Retired;
      shot.beginPresent(); CHECK(!slot.buffer && slot.state == State::Free);
      CHECK(csReference->info().size == uint64_t(size.first)*size.second*4);
    }
    AsyncScreenshot fault; fault.m->width=2560; fault.m->height=1440; fault.m->device->fail=true;
    CHECK(!fault.m->prepareRequestedSlot(fault.m->slots[0])); CHECK(fault.m->fault && !fault.m->available);
    std::cout << "screenshot production functions checks=" << checks << " failures=0 (fake resources)\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
