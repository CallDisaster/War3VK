#include "buffer_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <windows.h>

#include "../../../src/dxvk/dxvk_device.h"
#include "../../../src/dxvk/dxvk_instance.h"

namespace warvk::host::dxvkcore {
namespace {
  using Clock = std::chrono::steady_clock;
  constexpr uint64_t GpuWaitNs = 5'000'000'000ull;
  constexpr auto CpuWait = std::chrono::seconds(5);
  constexpr VkMemoryPropertyFlags CoherentMemory =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  static_assert(sizeof(void*) == 8, "BufferBackend is an actual x64-only host");
  static_assert(BufferBackend::MaxLiveBytes == 1048576);

  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }

  void canAdd(uint64_t value, uint64_t increment) {
    require(increment <= std::numeric_limits<uint64_t>::max() - value,
      "DXVK buffer backend counter exhausted");
  }
}

// Declaration order keeps the real device/instance alive beyond all users.
// All objects potentially referenced by a submission (including its status)
// are owned here. Failure retains this entire owner until the isolated process
// ends, rather than allowing stack unwinding to free live GPU/worker state.
struct BufferBackend::Owner {
  struct Slot {
    dxvk::Rc<dxvk::DxvkBuffer> buffer;
    uint32_t size = 0;
  };

  const DWORD thread = GetCurrentThreadId();
  dxvk::Rc<dxvk::DxvkInstance> instance;
  dxvk::Rc<dxvk::DxvkAdapter> adapter;
  dxvk::Rc<dxvk::DxvkDevice> device;
  dxvk::Rc<dxvk::DxvkContext> context;
  std::array<Slot, SlotCount> slots;
  dxvk::Rc<dxvk::DxvkBuffer> staging;
  dxvk::Rc<dxvk::DxvkBuffer> readback;
  dxvk::Rc<dxvk::DxvkFence> completion;
  dxvk::Rc<dxvk::DxvkCommandList> commands;
  dxvk::DxvkSubmitStatus submission;
  uint64_t lastSerial = 0;

  void healthy() const {
    const VkResult result = device->getDeviceStatus();
    if (result != VK_SUCCESS)
      throw std::runtime_error("DXVK buffer device status: "
        + std::to_string(static_cast<int>(result)));
  }

  Slot& live(uint32_t index) {
    require(index < SlotCount, "DXVK buffer slot out of range");
    require(slots[index].buffer != nullptr && slots[index].size != 0,
      "DXVK buffer slot is not live");
    return slots[index];
  }

  bool resourcesInUse() const {
    // Production DxvkAccess::Read includes outstanding reads AND writes.
    if (staging->isInUse(dxvk::DxvkAccess::Read)
        || readback->isInUse(dxvk::DxvkAccess::Read))
      return true;
    for (const Slot& slot : slots) {
      if (slot.buffer != nullptr && slot.buffer->isInUse(dxvk::DxvkAccess::Read))
        return true;
    }
    return false;
  }

  void waitRetired() {
    const auto deadline = Clock::now() + CpuWait;
    while (resourcesInUse()) {
      healthy();
      require(Clock::now() < deadline,
        "DXVK buffer usage tracking did not retire in 5 seconds");
      Sleep(1);
    }
    healthy();
  }

  void begin() {
    healthy();
    require(commands == nullptr, "DXVK buffer command list still owned");
    // Retain the command list before recording, including recording failures.
    commands = device->createCommandList();
    context->beginRecording(commands);
  }

  void submitAndRetire(Stats& stats) {
    canAdd(lastSerial, 1);
    canAdd(stats.submitted, 1);
    canAdd(stats.gpuCompleted, 1);
    canAdd(stats.retired, 1);
    const uint64_t serial = lastSerial + 1;
    context->signalFence(completion, serial);
    context->endFrame();
    commands = context->endRecording(nullptr);
    // DXVK does not initialize this caller-owned status for each submission.
    submission.result.store(VK_NOT_READY, std::memory_order_release);
    device->submitCommandList(commands, nullptr, serial, &submission);
    ++stats.submitted;
    lastSerial = serial;

    const auto deadline = Clock::now() + CpuWait;
    for (;;) {
      const VkResult result = submission.result.load(std::memory_order_acquire);
      if (result != VK_NOT_READY) {
        if (result != VK_SUCCESS)
          throw std::runtime_error("DXVK buffer submission failed: "
            + std::to_string(static_cast<int>(result)));
        break;
      }
      healthy();
      require(Clock::now() < deadline,
        "DXVK buffer CPU submission did not settle in 5 seconds");
      Sleep(1);
    }

    // Wait on the semaphore signalled by the production Context/command list.
    // This is not a raw parallel VkQueue path, a CPU ACK, or an unbounded fence
    // wait. The actual queue and resource tracking remain owned by DXVK.
    const VkSemaphore semaphore = completion->handle();
    VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wait.semaphoreCount = 1;
    wait.pSemaphores = &semaphore;
    wait.pValues = &serial;
    const VkResult result = device->vkd()->vkWaitSemaphores(
      device->handle(), &wait, GpuWaitNs);
    if (result != VK_SUCCESS) {
      device->notifyDeviceErrorFromDriverResult(result);
      throw std::runtime_error("DXVK buffer GPU timeline wait failed: "
        + std::to_string(static_cast<int>(result)));
    }
    ++stats.gpuCompleted;

    // A timeline signal alone does not prove the DXVK finish thread has
    // retired its resource use. Require both before a map write or Release.
    waitRetired();
    ++stats.retired;
    commands = nullptr;
  }

  void shutdown() {
    // Only called with no live slots and all submissions/GPU uses settled.
    // Real DXVK destructor joins/drains are still bounded by the process gate,
    // not claimed to be time-bounded by the per-submission waits above.
    commands = nullptr;
    context = nullptr;
    completion = nullptr;
    readback = nullptr;
    staging = nullptr;
    device = nullptr;
    adapter = nullptr;
    instance = nullptr;
  }
};

BufferBackend::BufferBackend() {
  try {
    m_owner = std::make_unique<Owner>();
    Owner& owner = *m_owner;
    owner.instance = new dxvk::DxvkInstance(dxvk::DxvkInstanceFlag::ClientApiIsD3D9);
    owner.adapter = owner.instance->enumAdapters(0);
    require(owner.adapter != nullptr, "DXVK buffer backend found no adapter");
    std::string compatibilityError;
    if (!owner.adapter->isCompatible(compatibilityError))
      throw std::runtime_error("DXVK buffer adapter incompatible: " + compatibilityError);
    const auto properties = owner.adapter->deviceProperties().core.properties;
    require(properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU,
      "DXVK buffer backend requires a non-CPU GPU adapter");
    m_stats.adapter = properties.deviceName;
    m_stats.vendorId = properties.vendorID;
    m_stats.deviceId = properties.deviceID;
    owner.device = owner.adapter->createDevice();
    owner.context = owner.device->createContext();

    // One fixed coherent upload allocation is reused only after GPU and usage
    // retirement. copyBuffer supports arbitrary byte ranges; updateBuffer's
    // four-byte alignment would silently narrow the accepted wire contract.
    dxvk::DxvkBufferCreateInfo stagingInfo;
    stagingInfo.size = MaxUploadBytes;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.stages = VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    stagingInfo.access = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    stagingInfo.debugName = "x64-buffer-host-staging";
    owner.staging = owner.device->createBuffer(stagingInfo, CoherentMemory);
    require((owner.staging->memFlags() & CoherentMemory) == CoherentMemory,
      "DXVK buffer staging is not coherent host-visible memory");
    m_stats.stagingMemoryFlags = owner.staging->memFlags();

    dxvk::DxvkBufferCreateInfo readbackInfo;
    readbackInfo.size = MaxBufferBytes;
    readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readbackInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    readbackInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    readbackInfo.debugName = "x64-buffer-host-readback";
    owner.readback = owner.device->createBuffer(readbackInfo, CoherentMemory);
    require((owner.readback->memFlags() & CoherentMemory) == CoherentMemory,
      "DXVK buffer readback is not coherent host-visible memory");
    m_stats.readbackMemoryFlags = owner.readback->memFlags();

    dxvk::DxvkFenceCreateInfo fenceInfo = {};
    fenceInfo.initialValue = 0;
    owner.completion = owner.device->createFence(fenceInfo);
    owner.healthy();
  } catch (...) {
    quarantine();
    throw;
  }
}

BufferBackend::~BufferBackend() noexcept {
  // finish() is the only normal teardown path. An omitted finish or exception
  // never initiates an implicit wait/join or releases possibly live GPU state.
  quarantine();
}

BufferBackend::Owner& BufferBackend::active() {
  require(m_owner != nullptr, "DXVK buffer backend is finished or quarantined");
  require(m_owner->thread == GetCurrentThreadId(),
    "DXVK buffer backend called outside its owner worker");
  return *m_owner;
}

void BufferBackend::quarantine() noexcept {
  if (m_owner) {
    m_stats.quarantined = true;
    (void)m_owner.release();
  }
}

void BufferBackend::create(uint32_t slot, uint32_t size) {
  try {
    Owner& owner = active();
    require(slot < SlotCount, "DXVK buffer create slot out of range");
    require(size != 0 && size <= MaxBufferBytes, "DXVK buffer create size out of range");
    require(owner.slots[slot].buffer == nullptr && owner.slots[slot].size == 0,
      "DXVK buffer create would replace a live slot");
    require(m_stats.liveBytes <= MaxLiveBytes - size,
      "DXVK buffer logical resource cap exceeded");
    canAdd(m_stats.created, 1);
    owner.healthy();

    dxvk::DxvkBufferCreateInfo info;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
      | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT
      | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    info.debugName = "x64-buffer-host-resource";
    owner.slots[slot].buffer = owner.device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    require((owner.slots[slot].buffer->memFlags() & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0,
      "DXVK buffer resource is not device-local storage");
    owner.slots[slot].size = size;
    ++m_stats.created;
    ++m_stats.liveResources;
    m_stats.liveBytes += size;
    if (m_stats.peakLiveBytes < m_stats.liveBytes)
      m_stats.peakLiveBytes = m_stats.liveBytes;
  } catch (...) {
    quarantine();
    throw;
  }
}

void BufferBackend::upload(uint32_t slot, uint32_t offset, const uint8_t* data, uint32_t count) {
  try {
    Owner& owner = active();
    Owner::Slot& resource = owner.live(slot);
    require(data != nullptr && count != 0 && count <= MaxUploadBytes,
      "DXVK buffer upload source/size invalid");
    require(offset <= resource.size && count <= resource.size - offset,
      "DXVK buffer upload range out of bounds");
    canAdd(m_stats.uploadedBytes, count);
    owner.waitRetired();
    void* mapping = owner.staging->mapPtr(0);
    require(mapping != nullptr, "DXVK buffer upload mapping unavailable");
    std::memcpy(mapping, data, count);

    owner.begin();
    owner.context->copyBuffer(resource.buffer, offset, owner.staging, 0, count);
    owner.submitAndRetire(m_stats);
    m_stats.uploadedBytes += count;
  } catch (...) {
    quarantine();
    throw;
  }
}

BufferBackend::Verification BufferBackend::verify(uint32_t slot) {
  try {
    Owner& owner = active();
    Owner::Slot& resource = owner.live(slot);
    canAdd(m_stats.verifiedBytes, resource.size);
    owner.waitRetired();
    owner.begin();
    owner.context->copyBuffer(owner.readback, 0, resource.buffer, 0, resource.size);
    owner.submitAndRetire(m_stats);

    const auto* bytes = static_cast<const uint8_t*>(owner.readback->mapPtr(0));
    require(bytes != nullptr, "DXVK buffer verification mapping unavailable");
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < resource.size; ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ull;
    }
    m_stats.verifiedBytes += resource.size;
    return { hash, resource.size };
  } catch (...) {
    quarantine();
    throw;
  }
}

void BufferBackend::destroy(uint32_t slot) {
  try {
    Owner& owner = active();
    Owner::Slot& resource = owner.live(slot);
    canAdd(m_stats.destroyed, 1);
    owner.waitRetired();
    const uint32_t bytes = resource.size;
    require(m_stats.liveResources != 0 && m_stats.liveBytes >= bytes,
      "DXVK buffer logical resource accounting mismatch");
    resource.buffer = nullptr;
    resource.size = 0;
    --m_stats.liveResources;
    m_stats.liveBytes -= bytes;
    ++m_stats.destroyed;
  } catch (...) {
    quarantine();
    throw;
  }
}

void BufferBackend::finish() {
  try {
    Owner& owner = active();
    require(m_stats.liveResources == 0 && m_stats.liveBytes == 0,
      "DXVK buffer finish requires all resources destroyed");
    for (const Owner::Slot& slot : owner.slots)
      require(slot.buffer == nullptr && slot.size == 0,
        "DXVK buffer finish found an unaccounted live slot");
    require(m_stats.submitted == m_stats.gpuCompleted && m_stats.gpuCompleted == m_stats.retired,
      "DXVK buffer finish has unfinished submissions");
    owner.waitRetired();
    owner.shutdown();
    m_owner.reset();
    m_stats.normalTeardown = true;
  } catch (...) {
    quarantine();
    throw;
  }
}

} // namespace warvk::host::dxvkcore
