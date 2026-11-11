// Development-only x64 bootstrap of the ACTUAL production DXVK core. This is
// not a D3D9 proxy or a replacement Vulkan backend, and never opens a game.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <windows.h>

#include "../../../src/dxvk/dxvk_device.h"
#include "../../../src/dxvk/dxvk_instance.h"
#include "../../../src/util/log/log.h"

namespace dxvk {
  // Each final executable/DLL provides the singleton for the shared util lib.
  Logger Logger::s_instance("warvk-dxvk-core-host.log");
}

namespace {
  using Clock = std::chrono::steady_clock;
  constexpr size_t kBufferBytes = 4096;
  constexpr uint64_t kGpuWaitNs = 5'000'000'000ull;
  constexpr auto kCpuWait = std::chrono::seconds(5);
  constexpr std::array<uint32_t, 3> kPatterns = {
    0xA55A13C7u, 0x19E27B04u, 0xDEAD0123u,
  };
  static_assert(sizeof(void*) == 8, "This host must be a real x64 executable");
  static_assert(kBufferBytes % sizeof(uint32_t) == 0);

  // Declaration order intentionally keeps the real device and instance alive
  // beyond their consumers. On a failure with unknown GPU completion, this
  // owner is retained until terminal process teardown; it is never unwound as
  // if a CPU error proved the GPU was finished. Only the successful path runs
  // the normal DXVK destruction path and may report normalOwnerTeardown=true.
  struct CoreOwner {
    dxvk::Rc<dxvk::DxvkInstance> instance;
    dxvk::Rc<dxvk::DxvkAdapter> adapter;
    dxvk::Rc<dxvk::DxvkDevice> device;
    dxvk::Rc<dxvk::DxvkContext> context;
    dxvk::Rc<dxvk::DxvkBuffer> source;
    dxvk::Rc<dxvk::DxvkBuffer> readback;
    dxvk::Rc<dxvk::DxvkFence> completion;
    dxvk::Rc<dxvk::DxvkCommandList> commands;
    dxvk::DxvkSubmitStatus submission;

    void shutdown() {
      // DxvkDevice destruction performs the core's real queue drain and worker
      // joins. A separate runner watchdog must bound this whole process. A
      // watchdog kill is a failure, never a successful normal teardown.
      commands = nullptr;
      context = nullptr;
      completion = nullptr;
      readback = nullptr;
      source = nullptr;
      device = nullptr;
      adapter = nullptr;
      instance = nullptr;
    }
  };

  std::string jsonString(const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : value) {
      if (c == '"' || c == '\\') {
        result += '\\';
        result += char(c);
      } else if (c < 0x20) {
        result += "\\u00";
        result += hex[c >> 4];
        result += hex[c & 15];
      } else {
        result += char(c);
      }
    }
    result += '"';
    return result;
  }

  void require(bool condition, const std::string& message) {
    if (!condition)
      throw std::runtime_error(message);
  }

  bool gameModuleLoaded() {
    // No module enumeration of another process, hooks, game APIs, or IPC.
    return GetModuleHandleW(L"Game.dll") != nullptr
        || GetModuleHandleW(L"War3.exe") != nullptr
        || GetModuleHandleW(L"d3d9.dll") != nullptr;
  }

  void requireDeviceHealthy(CoreOwner& owner) {
    const VkResult result = owner.device->getDeviceStatus();
    require(result == VK_SUCCESS,
      "DXVK device status: " + std::to_string(static_cast<int>(result)));
  }

  void waitSubmitted(CoreOwner& owner) {
    const auto deadline = Clock::now() + kCpuWait;
    for (;;) {
      const VkResult result = owner.submission.result.load(std::memory_order_acquire);
      if (result != VK_NOT_READY) {
        require(result == VK_SUCCESS,
          "DXVK submission failed: " + std::to_string(static_cast<int>(result)));
        return;
      }
      requireDeviceHealthy(owner);
      require(Clock::now() < deadline, "DXVK CPU submission did not settle in 5 seconds");
      Sleep(1);
    }
  }

  void waitGpuAndRetirement(CoreOwner& owner, uint64_t serial) {
    // Observe the semaphore signalled by DxvkContext::signalFence on the real
    // core command list. We do not submit raw Vk command buffers or substitute
    // a CPU reply for this GPU signal. DxvkFence::wait is unbounded, so the lab
    // uses the same semaphore through the core's device function table with a
    // bounded wait. The core still owns all resource tracking and submission.
    const VkSemaphore semaphore = owner.completion->handle();
    VkSemaphoreWaitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 1;
    wait.pSemaphores = &semaphore;
    wait.pValues = &serial;
    const VkResult result = owner.device->vkd()->vkWaitSemaphores(
      owner.device->handle(), &wait, kGpuWaitNs);
    if (result != VK_SUCCESS)
      owner.device->notifyDeviceErrorFromDriverResult(result);
    require(result == VK_SUCCESS,
      "DXVK GPU timeline wait failed: " + std::to_string(static_cast<int>(result)));

    // A signalled public fence and the core finish thread's reference retirement
    // are different milestones. Require both before poisoning/reusing storage.
    // DxvkAccess::Read tests outstanding reads AND writes in production code.
    const auto deadline = Clock::now() + kCpuWait;
    while (owner.source->isInUse(dxvk::DxvkAccess::Read)
        || owner.readback->isInUse(dxvk::DxvkAccess::Read)) {
      requireDeviceHealthy(owner);
      require(Clock::now() < deadline, "DXVK resource tracking did not retire in 5 seconds");
      Sleep(1);
    }
    requireDeviceHealthy(owner);
  }

  [[noreturn]] void terminalFailure(std::unique_ptr<CoreOwner>& owner,
      const std::string& stage, const std::string& message) {
    // Do not destroy potentially live GPU objects during exception unwinding.
    // This isolated lab process is the containment boundary, not the game or
    // renderer's future recoverable IPC policy. Outer timeout remains required.
    (void)owner.release();
    std::cout << "{\"schema\":\"warvk-dxvk-core-bootstrap/v1\",\"success\":false,"
      << "\"stage\":" << jsonString(stage) << ",\"error\":" << jsonString(message)
      << ",\"normalOwnerTeardown\":false,\"rendererMigrated\":false,"
      << "\"memorySavingsProven\":false}" << std::endl;
    ExitProcess(2);
  }
}

int main(int argc, char**) {
  std::unique_ptr<CoreOwner> owner;
  std::string stage = "arguments";
  const auto started = Clock::now();
  try {
    require(argc == 1, "This isolated lab takes no game paths, HWNDs, or IPC arguments");
    require(!gameModuleLoaded(), "Unexpected game or D3D9 module in the isolated host");
    owner = std::make_unique<CoreOwner>();

    stage = "production-instance-adapter-device";
    owner->instance = new dxvk::DxvkInstance(dxvk::DxvkInstanceFlag::ClientApiIsD3D9);
    owner->adapter = owner->instance->enumAdapters(0);
    require(owner->adapter != nullptr, "The production DXVK core exposed no compatible adapter");
    std::string compatibilityError;
    const bool compatible = owner->adapter->isCompatible(compatibilityError);
    require(compatible, "Adapter zero is incompatible: " + compatibilityError);
    const auto properties = owner->adapter->deviceProperties().core.properties;
    require(properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU,
      "A software CPU adapter does not satisfy this GPU bootstrap gate");
    owner->device = owner->adapter->createDevice();
    owner->context = owner->device->createContext();

    stage = "production-allocator-buffers";
    dxvk::DxvkBufferCreateInfo sourceInfo;
    sourceInfo.size = kBufferBytes;
    sourceInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    sourceInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    sourceInfo.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    sourceInfo.debugName = "x64-core-lab-gpu-source";
    owner->source = owner->device->createBuffer(sourceInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    dxvk::DxvkBufferCreateInfo readbackInfo;
    readbackInfo.size = kBufferBytes;
    readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readbackInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    readbackInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    readbackInfo.debugName = "x64-core-lab-readback";
    constexpr VkMemoryPropertyFlags readbackFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    owner->readback = owner->device->createBuffer(readbackInfo, readbackFlags);
    require((owner->readback->memFlags() & readbackFlags) == readbackFlags,
      "Production allocator did not expose coherent host-visible readback");
    require((owner->source->memFlags() & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0,
      "Production allocator did not expose device-local source storage");
    const auto sourceMemoryFlags = owner->source->memFlags();
    const auto readbackMemoryFlags = owner->readback->memFlags();
    dxvk::DxvkFenceCreateInfo fenceInfo = {};
    fenceInfo.initialValue = 0;
    owner->completion = owner->device->createFence(fenceInfo);

    size_t checkedWords = 0;
    std::array<uint64_t, kPatterns.size()> observedSums = {};
    for (size_t iteration = 0; iteration < kPatterns.size(); ++iteration) {
      const uint64_t serial = iteration + 1;
      stage = "pattern-" + std::to_string(serial) + "-record";
      // Reacquire the map pointer; the production allocator owns storage identity.
      void* before = owner->readback->mapPtr(0);
      require(before != nullptr, "Readback mapping is unavailable before submission");
      std::memset(before, 0xCD, kBufferBytes);
      owner->context->beginRecording(owner->device->createCommandList());
      owner->context->clearBuffer(owner->source, 0, kBufferBytes, kPatterns[iteration]);
      owner->context->copyBuffer(owner->readback, 0, owner->source, 0, kBufferBytes);
      owner->context->signalFence(owner->completion, serial);
      owner->context->endFrame();
      owner->commands = owner->context->endRecording(nullptr);

      stage = "pattern-" + std::to_string(serial) + "-submission";
      // The core does not reset this caller-owned status to NOT_READY for us.
      owner->submission.result.store(VK_NOT_READY, std::memory_order_release);
      owner->device->submitCommandList(owner->commands, nullptr, serial, &owner->submission);
      waitSubmitted(*owner);

      stage = "pattern-" + std::to_string(serial) + "-gpu-and-retirement";
      waitGpuAndRetirement(*owner, serial);
      stage = "pattern-" + std::to_string(serial) + "-readback";
      const void* after = owner->readback->mapPtr(0);
      require(after != nullptr, "Readback mapping is unavailable after GPU completion");
      for (size_t word = 0; word < kBufferBytes / sizeof(uint32_t); ++word) {
        uint32_t observed = 0;
        std::memcpy(&observed, static_cast<const uint8_t*>(after) + word * sizeof(uint32_t), sizeof(observed));
        require(observed == kPatterns[iteration], "GPU readback mismatch at word " + std::to_string(word));
        observedSums[iteration] += observed;
        ++checkedWords;
      }
      owner->commands = nullptr;
    }

    stage = "normal-production-owner-teardown";
    owner->shutdown();
    owner.reset();
    require(!gameModuleLoaded(), "Unexpected game/D3D9 module loaded during the core test");
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();

    // Emit success only AFTER real device/context/allocator teardown returned.
    // The independent runner must additionally require exit 0 and validate the
    // numeric golden (three patterns, 1024 words each), PE bitness and provenance.
    std::cout << "{\"schema\":\"warvk-dxvk-core-bootstrap/v1\",\"success\":true,"
      << "\"processBits\":64,\"actualDxvkCore\":true,\"adapter\":" << jsonString(properties.deviceName)
      << ",\"vendorId\":" << properties.vendorID << ",\"deviceId\":" << properties.deviceID
      << ",\"bufferBytesEach\":" << kBufferBytes
      << ",\"sourceMemoryFlags\":" << sourceMemoryFlags
      << ",\"readbackMemoryFlags\":" << readbackMemoryFlags
      << ",\"submitted\":" << kPatterns.size() << ",\"gpuTimelineCompleted\":" << kPatterns.size()
      << ",\"resourceTrackingRetired\":" << kPatterns.size() << ",\"checkedWords\":" << checkedWords
      << ",\"observedWordSums\":[" << observedSums[0] << ',' << observedSums[1] << ',' << observedSums[2] << ']'
      << ",\"normalOwnerTeardown\":true,\"elapsedMs\":" << elapsedMs
      << ",\"gameTouched\":false,\"d3d9Proxy\":false,\"presentTested\":false,"
      << "\"warvkPassesMigrated\":false,\"rendererMigrated\":false,\"memorySavingsProven\":false}"
      << std::endl;
    return 0;
  } catch (const dxvk::DxvkError& error) {
    terminalFailure(owner, stage, error.message());
  } catch (const std::exception& error) {
    terminalFailure(owner, stage, error.what());
  } catch (...) {
    terminalFailure(owner, stage, "unknown exception");
  }
}
