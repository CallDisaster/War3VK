#include "war3_async_screenshot.h"
#include "war3_async_screenshot_core.h"
#include "war3_internal_test_api.h"
#include "war3_frame_evidence.h"
#include "war3_frame_history.h"
#include "../hooks/war3_hook_lifecycle.h"
#include "../hooks/war3_native_capture.h"
#include "../../../dxvk/dxvk_device.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <thread>

namespace dxvk::war3::tools {
namespace {
using screenshot::State;
struct Registry {
  std::mutex mutex;
  AsyncScreenshot* owner = nullptr;
};
Registry& registry() {
  // Process-lifetime registry: no static-destruction ordering with D3D9/Hook.
  static auto* value = new Registry;
  return *value;
}
std::atomic<uint64_t> fileSerial{0};

struct File {
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  bool write(const void* data, DWORD bytes) {
    DWORD written = 0;
    return WriteFile(handle, data, bytes, &written, nullptr) && written == bytes;
  }
  bool close() {
    const HANDLE h = handle;
    handle = INVALID_HANDLE_VALUE;
    return CloseHandle(h) != FALSE;
  }
};
} // namespace

bool AsyncScreenshotEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("DXVK_WAR3_ASYNC_SCREENSHOT");
    return !v || std::strcmp(v, "1") == 0;
  }();
  return enabled;
}

struct AsyncScreenshot::Impl {
  struct Slot {
    std::atomic<State> state{State::Retired};
    Rc<DxvkBuffer> buffer;
    Rc<DxvkFence> fence;
    uint64_t value = 0;
    uint64_t serial = 0;
    uint64_t presentOrdinal = 0;
    uint64_t evidenceSession = 0;
    std::shared_ptr<HistoryReadback> history;
    uint32_t width = 0, height = 0;
    std::chrono::steady_clock::time_point submittedAt;
  };
  Rc<DxvkDevice> device;
  std::array<Slot, screenshot::SlotCount> slots;
  std::atomic<bool> available{false}, stop{false}, fault{false};
  std::atomic<uint32_t> inFlight{0}, rejected{0}, cancelled{0};
  std::mutex wakeMutex;
  std::condition_variable wake;
  std::thread worker;
  uint32_t width = 0, height = 0;
  bool warmed = false;
  uint64_t presentOrdinal = 0;
  std::atomic<uint32_t> burstRemaining{0};

  explicit Impl(const Rc<DxvkDevice>& dev) : device(dev) {
    worker = std::thread([this] {
      try { run(); }
      catch (...) {
        fault.store(true, std::memory_order_release);
        available.store(false, std::memory_order_release);
        // No exception may escape a worker into std::terminate.
      }
    });
  }
  ~Impl() {
    stop.store(true, std::memory_order_release);
    wake.notify_one();
    if (worker.joinable()) worker.join();
  }

  bool save(const Slot& slot) {
    std::wstring path;
    wchar_t name[160]{};
    if(slot.history) path=slot.history->path;
    else {
    // Only the worker calls this, after the actual GPU completion query.
    // GetModuleFileNameW(nullptr) locates war3.exe, just like the native saver.
    std::array<wchar_t, 32768> exe{};
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (!length || length >= exe.size()) return false;
    std::wstring dir(exe.data(), length);
    const auto slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    dir.resize(slash);
    dir += L"\\Screenshots";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
      return false;
    SYSTEMTIME now{};
    GetSystemTime(&now);
    std::swprintf(name, 160,
        L"\\WC3ScrnShot_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%llu_p%llu.tga",
        unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay),
        unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond),
        unsigned(now.wMilliseconds), GetCurrentProcessId(),
        static_cast<unsigned long long>(slot.serial),
        static_cast<unsigned long long>(slot.presentOrdinal));
    path = dir + name;
    }
    const std::wstring partial = path + L".part";
    File file;
    file.handle = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) return false;
    const auto* pixels = static_cast<const uint8_t*>(slot.buffer->mapPtr(0));
    if (!screenshot::EncodeTga(slot.width, slot.height, pixels, size_t(slot.width) * 4,
        [&](const void* p, size_t n) { return file.write(p, DWORD(n)); },
        [&] { return stop.load(std::memory_order_acquire) ||
            device->getDeviceStatus() != VK_SUCCESS; })) return false;
    if (!file.close() || stop.load(std::memory_order_acquire) ||
        device->getDeviceStatus() != VK_SUCCESS) return false;
    // No REPLACE_EXISTING; failures preserve the owned .part for diagnostics.
    if (!MoveFileExW(partial.c_str(), path.c_str(), 0)) return false;
    Logger::info(str::format("[AsyncScreenshot] saved id=", slot.serial,
        " size=", slot.width, "x", slot.height, " file=", name));
    if (slot.evidenceSession && !slot.history) {
      evidence::Event event{}; event.kind=evidence::Kind::ScreenshotSaved;
      event.key={uint64_t(reinterpret_cast<uintptr_t>(device.ptr())),0,0,0};
      const char* label="AsyncScreenshotSaved";
      for(size_t n=0;n+1<event.label.size()&&label[n];++n) event.label[n]=label[n];
      event.data[0]=slot.serial; event.data[1]=slot.presentOrdinal;
      event.data[2]=slot.value; event.data[3]=slot.width; event.data[4]=slot.height; event.data[5]=1;
      evidence::Record(slot.evidenceSession,event);
    }
    return true;
  }

  void run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto vkd = device->vkd();
    while (!stop.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(wakeMutex);
        if (!inFlight.load(std::memory_order_acquire)) {
          // Bounded sleep also covers a notification racing the predicate/wait
          // boundary; the producer never needs this worker mutex.
          wake.wait_for(lock, std::chrono::milliseconds(250), [&] {
            return stop.load(std::memory_order_acquire) ||
                inFlight.load(std::memory_order_acquire) || rejected.load() || cancelled.load();
          });
        } else {
          wake.wait_for(lock, std::chrono::milliseconds(2), [&] { return stop.load(); });
        }
      }
      if (stop.load(std::memory_order_acquire)) break;
      const auto rejectedCount = rejected.exchange(0);
      const auto cancelledCount = cancelled.exchange(0);
      if (rejectedCount || cancelledCount)
        Logger::warn(str::format("[AsyncScreenshot] not saved: queue-full=",
            rejectedCount, " reset/unsupported=", cancelledCount));
      for (auto& slot : slots) {
        if (slot.state.load(std::memory_order_acquire) != State::Submitted) continue;
        uint64_t completed = 0;
        const VkResult query = vkd->vkGetSemaphoreCounterValue(
            vkd->device(), slot.fence->handle(), &completed);
        const bool deviceOk = device->getDeviceStatus() == VK_SUCCESS;
        if (query != VK_SUCCESS || !deviceOk) {
          fault.store(true, std::memory_order_release);
          available.store(false, std::memory_order_release);
          slot.state.store(State::Quarantined, std::memory_order_release);
          inFlight.fetch_sub(1);
          if(slot.history)slot.history->done.store(true,std::memory_order_release);
          Logger::err("[AsyncScreenshot] GPU query/device failure; slot quarantined, no pixel read");
          continue;
        }
        const auto readyAt = std::chrono::steady_clock::now();
        if (!screenshot::Readable(query == VK_SUCCESS, deviceOk, completed, slot.value)) {
          if (readyAt - slot.submittedAt > std::chrono::seconds(10)) {
            fault.store(true, std::memory_order_release);
            available.store(false, std::memory_order_release);
            slot.state.store(State::Quarantined, std::memory_order_release);
            inFlight.fetch_sub(1);
            if(slot.history)slot.history->done.store(true,std::memory_order_release);
            Logger::err("[AsyncScreenshot] completion timeout; buffer retained, never read or reused");
          }
          continue;
        }
        bool saved = false;
        if(slot.history)slot.history->observedValue=completed;
        try { saved = save(slot); } catch (...) { }
        if(slot.history){slot.history->success.store(saved);slot.history->done.store(true,std::memory_order_release);}
        if (!saved)
          Logger::err(str::format("[AsyncScreenshot] save failed/cancelled id=", slot.serial,
              "; any partial file retained, no successful-save notification"));
        Logger::info(str::format("[AsyncScreenshot] timing id=", slot.serial,
            " enqueue-to-observed-ready-ms=",
            std::chrono::duration<double, std::milli>(readyAt-slot.submittedAt).count(),
            " worker-save-ms=", std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now()-readyAt).count()));
        inFlight.fetch_sub(1);
        // Neither the GPU nor the encoder accesses this slot after publication.
        slot.state.store(State::Retired, std::memory_order_release);
      }
    }
  }
};

AsyncScreenshot::AsyncScreenshot(const Rc<DxvkDevice>& device)
  : m(std::make_unique<Impl>(device)) { }

std::unique_ptr<AsyncScreenshot> AsyncScreenshot::Create(const Rc<DxvkDevice>& device) {
  if (!AsyncScreenshotEnabled()) return nullptr;
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (r.owner) {
    // The native event carries no swapchain identity. Never capture another
    // window's pixels when an additional swapchain makes the owner ambiguous.
    r.owner->m->fault.store(true, std::memory_order_release);
    r.owner->m->available.store(false, std::memory_order_release);
    Logger::warn("[AsyncScreenshot] additional swapchain; native path retained");
    return nullptr;
  }
  try {
    auto result = std::unique_ptr<AsyncScreenshot>(new AsyncScreenshot(device));
    r.owner = result.get();
    return result;
  } catch (...) {
    Logger::err("[AsyncScreenshot] worker creation failed; native path retained");
    return nullptr;
  }
}

AsyncScreenshot::~AsyncScreenshot() {
  auto& r = registry();
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.owner == this) r.owner = nullptr;
  }
  // Join only at explicit teardown, never Present/Reset. No GPU wait here.
  m.reset();
}

bool RequestNativeAsyncScreenshot() {
  auto& r = registry();
  std::unique_lock<std::mutex> lock(r.mutex, std::try_to_lock);
  return lock.owns_lock() && r.owner && r.owner->request();
}

bool ArmAsyncScreenshotBurstForTest() {
  if (!IsInternalTestApiEnabled() || !hooks::GetMainLoopThreadId() ||
      GetCurrentThreadId() != hooks::GetMainLoopThreadId()) return false;
  auto& r = registry();
  std::unique_lock<std::mutex> lock(r.mutex, std::try_to_lock);
  if (!lock.owns_lock() || !r.owner) return false;
  auto& m = *r.owner->m;
  if (!m.available.load() || m.fault.load()) return false;
  for (auto& slot : m.slots)
    if (slot.state.load(std::memory_order_acquire) != State::Free) return false;
  uint32_t expected = 0;
  return m.burstRemaining.compare_exchange_strong(expected, 3);
}

bool AsyncScreenshot::request() {
  if (!m->available.load(std::memory_order_acquire) || m->fault.load()) return false;
  for (auto& slot : m->slots)
    if (screenshot::Claim(slot.state, State::Free, State::Requested)) return true;
  m->rejected.fetch_add(1);
  m->wake.notify_one();
  return true; // Handled but rejected; never fall back to a blocking burst.
}

void AsyncScreenshot::reset() {
  std::lock_guard<std::mutex> requestLock(registry().mutex);
  m->available.store(false, std::memory_order_release);
  m->warmed = false;
  m->burstRemaining.store(0);
  for (auto& slot : m->slots) {
    if (screenshot::Claim(slot.state, State::Requested, State::Retired))
      m->cancelled.fetch_add(1);
    else
      screenshot::Claim(slot.state, State::Free, State::Retired);
  }
  // Submitted snapshots are immutable and can finish the selected OLD frame.
  // Do not release/reuse them or join the encoder from Reset.
  m->wake.notify_one();
}

void AsyncScreenshot::beginPresent() {
  // Called under the device lock before any early return. A failed/skipped
  // Present is thus visible as a gap, not mislabelled as consecutive captures.
  ++m->presentOrdinal;
}

uint64_t AsyncScreenshot::presentOrdinal() const noexcept {
  return m->presentOrdinal;
}

void AsyncScreenshot::prepare(const Rc<DxvkImage>& image,bool advanceBurst) {
  uint64_t bytes = 0;
  const auto& info = image->info();
  const bool supported = !m->fault.load() && m->device->getDeviceStatus() == VK_SUCCESS &&
      info.type == VK_IMAGE_TYPE_2D && info.sampleCount == VK_SAMPLE_COUNT_1_BIT &&
      info.numLayers == 1 && info.mipLevels == 1 &&
      (info.format == VK_FORMAT_B8G8R8A8_UNORM || info.format == VK_FORMAT_B8G8R8A8_SRGB) &&
      screenshot::Layout(info.extent.width, info.extent.height, bytes);
  if (!supported) { reset(); return; }
  if (!m->warmed || m->width != info.extent.width || m->height != info.extent.height) {
    reset();
    m->width = info.extent.width;
    m->height = info.extent.height;
  }
  try {
    for (auto& slot : m->slots) {
      if (!screenshot::Claim(slot.state, State::Retired, State::Preparing)) continue;
      if (!slot.buffer || slot.buffer->info().size != bytes) {
        DxvkBufferCreateInfo bufferInfo{};
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
        bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        bufferInfo.debugName = "War3 async screenshot staging";
        constexpr auto memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        // Readback, not upload: prefer cached host memory. DXVK may drop only
        // HOST_CACHED if unavailable; coherence remains mandatory. Verify the
        // actual allocation, not memFlags() (which echoes requested flags).
        slot.buffer = m->device->createBuffer(bufferInfo, memory | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        const auto actualMemory = slot.buffer->storage()->getMemoryProperties();
        if ((actualMemory & memory) != memory || !slot.buffer->mapPtr(0))
          throw DxvkError("Async screenshot requires coherent mapped memory");
        Logger::info(str::format("[AsyncScreenshot] staging bytes=", bytes,
            " actual-memory-flags=", actualMemory));
      }
      if (!slot.fence) slot.fence = m->device->createFence(DxvkFenceCreateInfo{0});
      slot.width = m->width;
      slot.height = m->height;
      slot.history.reset();
      slot.state.store(State::Free, std::memory_order_release);
    }
    m->warmed = true;
    m->available.store(true, std::memory_order_release);
    if (advanceBurst&&m->burstRemaining.load()) {
      bool claimed = false;
      for (auto& slot : m->slots) {
        if (screenshot::Claim(slot.state, State::Free, State::Requested)) {
          claimed = true; break;
        }
      }
      if (claimed) --m->burstRemaining;
      else {
        // A contention/gap cancels this diagnostic burst, never waits or claims
        // a later frame was consecutive. The reader checks filename ordinals.
        m->burstRemaining.store(0); ++m->cancelled;
        m->wake.notify_one();
      }
    }
  } catch (...) {
    m->fault.store(true, std::memory_order_release);
    Logger::err("[AsyncScreenshot] staging allocation failed; native path retained");
  }
}

std::optional<AsyncScreenshot::Copy> AsyncScreenshot::take(uint32_t index) {
  auto& slot = m->slots[index];
  if (!screenshot::Claim(slot.state, State::Requested, State::Preparing)) return std::nullopt;
  // A completed old-resolution slot is not eligible until re-prewarmed. Never
  // copy current pixels into an old-size allocation or defer the request.
  if (m->fault.load() || !slot.buffer || !slot.fence || slot.width != m->width ||
      slot.height != m->height || slot.value == UINT64_MAX) {
    slot.state.store(State::Quarantined, std::memory_order_release);
    m->cancelled.fetch_add(1); m->wake.notify_one();
    return std::nullopt;
  }
  slot.serial = fileSerial.fetch_add(1) + 1;
  slot.presentOrdinal = m->presentOrdinal;
  auto value=++slot.value;
  slot.evidenceSession=evidence::ActiveSession();
  if (slot.evidenceSession) {
    evidence::Event event{}; event.kind=evidence::Kind::ScreenshotPrepared;
    event.key={uint64_t(reinterpret_cast<uintptr_t>(m->device.ptr())),0,0,0};
    const char* label="AsyncScreenshotPrepared";
    for(size_t n=0;n+1<event.label.size()&&label[n];++n) event.label[n]=label[n];
    event.data[0]=slot.serial; event.data[1]=slot.presentOrdinal; event.data[2]=value;
    event.data[3]=slot.width; event.data[4]=slot.height;
    evidence::Record(slot.evidenceSession,event);
  }
  return Copy{slot.buffer, slot.fence, value, index, slot.evidenceSession,slot.serial,slot.presentOrdinal};
}

std::optional<AsyncScreenshot::Copy> AsyncScreenshot::takeHistory(const std::shared_ptr<HistoryReadback>& job) {
  if(!job||job->queued.load()||job->width!=m->width||job->height!=m->height||!m->available.load())return {};
  for(uint32_t index=0;index<screenshot::SlotCount;++index){
    auto& slot=m->slots[index];
    if(!screenshot::Claim(slot.state,State::Free,State::Preparing))continue;
    if(!slot.buffer||!slot.fence||slot.width!=job->width||slot.height!=job->height||slot.value==UINT64_MAX){
      slot.state=State::Retired;continue;}
    slot.history=job;slot.serial=fileSerial.fetch_add(1)+1;slot.presentOrdinal=job->present;
    slot.evidenceSession=0;job->targetValue=++slot.value;job->queued.store(true,std::memory_order_release);
    return Copy{slot.buffer,slot.fence,slot.value,index,0,slot.serial,job->present};
  }
  return {};
}

void AsyncScreenshot::submitted(uint32_t index) {
  m->slots[index].submittedAt = std::chrono::steady_clock::now();
  m->inFlight.fetch_add(1);
  m->slots[index].state.store(State::Submitted, std::memory_order_release);
  m->wake.notify_one();
}

void AsyncScreenshot::quarantine(uint32_t index) {
  if(m->slots[index].history)m->slots[index].history->done.store(true,std::memory_order_release);
  m->fault.store(true, std::memory_order_release);
  m->available.store(false, std::memory_order_release);
  m->slots[index].state.store(State::Quarantined, std::memory_order_release);
  // CS/command-list Rc references retain any possibly emitted copy resources.
}

} // namespace dxvk::war3::tools
