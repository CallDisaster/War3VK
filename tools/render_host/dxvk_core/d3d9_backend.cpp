#include "d3d9_backend.h"
#include "d3d9_factory.h"
#include "d3d9_batch_state.h"
#include "d3d9_fixed_state.h"
#include "host_surface_target.h"
#include "../d3d9_shader_tokens.h"
#include "../d3d9_declaration_wire.h"
#include "../d3d9_topology.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "../../../src/dxvk/dxvk_device.h"
#include "../../../src/dxvk/dxvk_instance.h"
#include "../../../src/dxvk/dxvk_presenter.h"
#include "../../../src/dxvk/dxvk_swapchain_blitter.h"
#include "../../../src/dxso/dxso_module.h"
#include "../../../src/dxso/dxso_modinfo.h"
#include "../../../src/dxso/dxso_shader_module_cache.h"
#include "../../../src/dxso/dxso_util.h"
#include "../../../src/d3d9/d3d9_fixed_function.h"
#include "../../../src/d3d9/d3d9_spec_constants.h"
#include "../../../src/d3d9/d3d9_sampler_key.h"
#include "../../../src/wsi/wsi_window.h"

namespace warvk::host::dxvkcore {
namespace {
  using namespace dxvk;
  using Clock = std::chrono::steady_clock;
  constexpr auto CpuWait = std::chrono::seconds(5);
  constexpr uint64_t GpuWaitNs = 5'000'000'000ull;
  class WaitTimer {
  public:
    explicit WaitTimer(uint64_t* destination) : m_destination(destination) {
      if (m_destination)
        m_start = Clock::now();
    }
    ~WaitTimer() {
      if (m_destination)
        *m_destination += std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now() - m_start).count();
    }
  private:
    uint64_t* m_destination;
    Clock::time_point m_start{};
  };
  constexpr VkMemoryPropertyFlags CoherentMemory =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  constexpr VkShaderStageFlags ShaderStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  // One readback band always fits one D9C2 reply; bands are whole rows.
  constexpr uint32_t ReadbackBandBytes = d9::MaxReplyPayload;
  constexpr D3D9ConstantLayout VertexLayout = { caps::MaxFloatConstantsVS, caps::MaxOtherConstants,
    caps::MaxOtherConstants, 1 };
  constexpr D3D9ConstantLayout PixelLayout = { caps::MaxSM3FloatConstantsPS, caps::MaxOtherConstants,
    caps::MaxOtherConstants, 1 };
  constexpr uint32_t PixelSamplerSlot = computeResourceSlotId(
    DxsoProgramType::PixelShader, DxsoBindingType::Image, 0);
  static_assert(sizeof(void*) == 8, "D3D9Backend must be built as an x64 host");
  static_assert(VK_NOT_READY == 1, "backend statistics use the Vulkan not-ready sentinel");
  static_assert(sizeof(D3D9RenderStateInfo) <= MaxSharedPushDataSize);
  static_assert(D3D9Backend::FloatRegisters == d3d9tokens::FloatRegisters,
    "shader scanner and backend constant profile must stay identical");

  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }

  void requireProfile(bool condition, const char* message) {
    if (!condition)
      throw D3D9Backend::ProfileRejected(message);
  }

  void canAdd(uint64_t value, uint64_t increment) {
    require(increment <= std::numeric_limits<uint64_t>::max() - value,
      "DXVK D3D9 counter exhausted");
  }

  void addResource(D3D9Backend::Stats& stats, uint64_t bytes) {
    canAdd(stats.liveLogicalBytes, bytes);
    ++stats.liveResources;
    stats.liveLogicalBytes += bytes;
    stats.peakLogicalBytes = std::max(stats.peakLogicalBytes, stats.liveLogicalBytes);
  }

  void removeResource(D3D9Backend::Stats& stats, uint64_t bytes) {
    require(stats.liveResources != 0 && stats.liveLogicalBytes >= bytes,
      "DXVK D3D9 resource accounting mismatch");
    --stats.liveResources;
    stats.liveLogicalBytes -= bytes;
  }

  VkFormat declarationFormat(uint32_t type) {
    switch (type) {
      case D3DDECLTYPE_FLOAT1: return VK_FORMAT_R32_SFLOAT;
      case D3DDECLTYPE_FLOAT2: return VK_FORMAT_R32G32_SFLOAT;
      case D3DDECLTYPE_FLOAT3: return VK_FORMAT_R32G32B32_SFLOAT;
      case D3DDECLTYPE_FLOAT4: return VK_FORMAT_R32G32B32A32_SFLOAT;
      case D3DDECLTYPE_D3DCOLOR: return VK_FORMAT_B8G8R8A8_UNORM;
      case D3DDECLTYPE_UBYTE4: return VK_FORMAT_R8G8B8A8_USCALED;
      case D3DDECLTYPE_SHORT2: return VK_FORMAT_R16G16_SSCALED;
      case D3DDECLTYPE_SHORT4: return VK_FORMAT_R16G16B16A16_SSCALED;
      case D3DDECLTYPE_UBYTE4N: return VK_FORMAT_R8G8B8A8_UNORM;
      case D3DDECLTYPE_SHORT2N: return VK_FORMAT_R16G16_SNORM;
      case D3DDECLTYPE_SHORT4N: return VK_FORMAT_R16G16B16A16_SNORM;
      case D3DDECLTYPE_USHORT2N: return VK_FORMAT_R16G16_UNORM;
      case D3DDECLTYPE_USHORT4N: return VK_FORMAT_R16G16B16A16_UNORM;
      case D3DDECLTYPE_FLOAT16_2: return VK_FORMAT_R16G16_SFLOAT;
      case D3DDECLTYPE_FLOAT16_4: return VK_FORMAT_R16G16B16A16_SFLOAT;
      default: throw std::runtime_error("DXVK D3D9 unsupported vertex declaration type");
    }
  }

  uint32_t declarationSize(uint32_t type) {
    const auto size=dxvk::DecltypeSize(D3DDECLTYPE(type));
    require(size!=0,"DXVK D3D9 unsupported vertex declaration size");
    return size;
  }

  VkBlendFactor blendFactor(uint32_t factor, bool alpha) {
    switch (factor) {
      case D3DBLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
      case D3DBLEND_ONE: return VK_BLEND_FACTOR_ONE;
      case D3DBLEND_SRCCOLOR: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
      case D3DBLEND_INVSRCCOLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      case D3DBLEND_SRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
      case D3DBLEND_INVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      case D3DBLEND_DESTALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
      case D3DBLEND_INVDESTALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
      case D3DBLEND_DESTCOLOR: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
      case D3DBLEND_INVDESTCOLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      case D3DBLEND_SRCALPHASAT: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
      default: throw std::runtime_error("DXVK D3D9 unsupported blend factor");
    }
  }

  VkBlendOp blendOp(uint32_t op) {
    require(op >= D3DBLENDOP_ADD && op <= D3DBLENDOP_MAX, "DXVK D3D9 unsupported blend operation");
    return static_cast<VkBlendOp>(op - D3DBLENDOP_ADD);
  }

  // Prevalidated by ValidDrawTopology; D3D9 has no primitive restart.
  VkPrimitiveTopology topology(D3DPRIMITIVETYPE type) {
    switch (type) {
      case D3DPT_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      case D3DPT_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      case D3DPT_TRIANGLELIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      case D3DPT_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      case D3DPT_TRIANGLEFAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      default: throw std::runtime_error("DXVK D3D9 unsupported topology");
    }
  }

  VkCullModeFlags cullMode(uint32_t mode) {
    switch (mode) {
      case D3DCULL_NONE: return VK_CULL_MODE_NONE;
      case D3DCULL_CW: return VK_CULL_MODE_FRONT_BIT;
      case D3DCULL_CCW: return VK_CULL_MODE_BACK_BIT;
      default: throw std::runtime_error("DXVK D3D9 unsupported cull mode");
    }
  }

  DxvkImageViewKey viewKey(VkFormat format, VkImageUsageFlags usage,
      VkImageAspectFlags aspect, VkImageLayout layout) {
    DxvkImageViewKey key;
    key.viewType = VK_IMAGE_VIEW_TYPE_2D;
    key.usage = usage;
    key.format = format;
    key.layout = layout;
    key.aspects = aspect;
    key.mipCount = 1;
    key.layerCount = 1;
    return key;
  }

  constexpr VkImageSubresourceLayers ColorLayer = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
}

// Every resource that can outlive a call, including the submission status and
// command list, belongs to this owner. An exception retains the WHOLE owner
// until process exit. There is no second Vulkan queue/allocator/fence system.
struct D3D9Backend::Owner {
  struct Buffer {
    Rc<DxvkBuffer> object;
    uint32_t size = 0, initializedPrefix = 0;
    bool index16 = false, dynamic = false;
    // Only the x64 IB has a bounded CPU shadow for validating addressed vertex
    // ranges. VB bytes and textures are not retained as parallel CPU mirrors.
    std::vector<uint8_t> indexBytes;
  };
  struct Texture {
    Rc<DxvkImage> image;
    Rc<DxvkImageView> view;
    Rc<DxvkImageView> srgbView;
    uint32_t width = 0, height = 0;
    D3D9_VK_FORMAT_MAPPING mapping{};
    d9::TextureLayout layout;
    uint32_t initializedLevels=0,lod=0;
    bool uploaded = false;
  };
  struct Shader {
    Rc<DxvkShader> object;
    DxsoIsgn inputs;
    DxsoDefinedConstants constants;
    bool needsConstantCopies = false;
    uint32_t tokenBytes = 0;
  };
  struct Declaration {
    std::array<D3DVERTEXELEMENT9, MaxDeclarationElements> elements{};
    uint32_t count = 0;
  };
  struct DrawFlight {
    Rc<DxvkBuffer> vertex, pixel;
    // Stable owner storage: the submission thread must never write into an
    // expired stack or a slot reused before its exact submission result.
    DxvkSubmitStatus status;
    uint64_t serial = 0;
    Clock::time_point submissionDeadline{};
  };

  const DWORD thread = GetCurrentThreadId();
  Rc<DxvkInstance> instance;
  Rc<DxvkAdapter> adapter;
  Rc<DxvkDevice> device;
  Rc<DxvkContext> context;
  std::array<Buffer, BufferSlots> buffers;
  std::array<Texture, TextureSlots> textures;
  std::array<Shader, ShaderSlots> vertexShaders, pixelShaders;
  std::array<Declaration, DeclarationSlots> declarations;
  Rc<DxvkImage> color, depth, acquiredImage;
  Rc<DxvkImageView> colorView, depthView, colorSampleView, acquiredView;
  std::array<Rc<DxvkSampler>,d9::PixelTextureStages> samplers;
  d9::SamplerState samplerState{true};
  std::array<d9::SamplerState,d9::AdditionalTextureStages> additionalSamplers{};
  d9::EffectsState effects;
  // Native D3D9DeviceEx: chosen from VK_EXT_depth_bias_control at creation.
  DxvkDepthBiasRepresentation depthBiasRepresentation{
    VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT, false};
  float depthBiasScale = 0.0f;
  std::unique_ptr<D3D9Options> d3d9Options;
  Rc<DxvkBuffer> staging, readback, presentedReadback, vertexConstants, pixelConstants, sharedConstants, specConstants, clipPlanes;
  Rc<DxvkBuffer> fixedVertexConstants,fixedPixelConstants,nullVertex;
  D3D9SpecializationInfo specialization;
  std::unique_ptr<D3D9FFShader> fixedVertexShader,fixedPixelShader;
  Rc<DxvkBuffer> batchVertexConstants, batchPixelConstants;
  uint32_t batchVertexStride = 0, batchPixelStride = 0;
  std::array<DrawFlight, MaxDrawFlights> drawFlights;
  uint32_t nextFlight = 0;
  bool flightMode = false, groupMode = false;
  uint32_t recordedGroupCount = 0;
  // Stream profile, native D3D9ConstantBuffer::Alloc shape: each draw takes
  // fresh aligned slices of one host-visible UBO per binding. Exhaustion
  // renames storage on the recording context, and invalidateBuffer hands the
  // old allocation to that command list, so no slice is rewritten before GPU
  // retirement. Draws stay in ONE open list until a sync point or the cap.
  struct ConstantRing {
    Rc<DxvkBuffer> buffer;
    uint32_t stride = 0, offset = 0;
  };
  ConstantRing streamVertex, streamPixel, streamFixedVertex, streamFixedPixel;
  bool streamMode = false, streamOpen = false;
  uint32_t streamListDraws = 0;
  // Allocated only for the explicit diagnostic WSI profile. This is a frozen
  // CPU copy, not a borrowed mapping or an offscreen-source fallback.
  std::vector<uint8_t> presentedBytes;
  Rc<DxvkFence> completion;
  Rc<DxvkCommandList> commands;
  DxvkSubmitStatus submission;
  Rc<Presenter> presenter;
  Rc<DxvkSwapchainBlitter> blitter;
  Rc<sync::Fence> presentSignal;
  // Asynchronous queue feedback must never point at a present() stack frame.
  DxvkSubmitStatus presentSubmission;
  DxsoModuleInfo moduleInfo;
  std::unique_ptr<DxsoShaderModuleCache> shaderModules;
  uint64_t lastSerial = 0, lastEvent = 0, lastPresentFrame = 0;
  uint32_t width = 0, height = 0;
  size_t presentedSize() const { return size_t(width) * height * 4; }
  // X8R8G8B8: the same BGRA8 storage, but destination alpha reads as one.
  bool opaque = false;
  // The diagnostic snapshot belongs to the current extent; Reset drops it.
  bool presentedCaptured = false;
  HWND window = nullptr;
  uint32_t windowProcessId = 0, windowThreadId = 0;
  std::wstring windowMarker;
  // Original window fields remain the authenticated peer identity, even when
  // this host owns the separate HWND used by the production WSI surface.
  std::unique_ptr<surface::HostSurfaceTarget> surfaceTarget;
  bool pending = false, diagnosticPresentedReadback = false;

  void healthy() const {
    const VkResult result = device->getDeviceStatus();
    if (result != VK_SUCCESS)
      throw std::runtime_error("DXVK D3D9 device status: " + std::to_string(static_cast<int>(result)));
  }

  // With resized, a window whose identity holds but whose client no longer
  // matches the back buffer reports that instead of failing.
  void validateWindow(bool* resized = nullptr) const {
    if (surfaceTarget) {
      surfaceTarget->validate();
      return;
    }
    require(window != nullptr && windowProcessId != 0 && windowThreadId != 0
      && !windowMarker.empty(), "DXVK D3D9 WSI window identity is incomplete");
    require(IsWindow(window) != FALSE, "DXVK D3D9 WSI window was destroyed");
    DWORD actualProcessId = 0;
    const DWORD actualThreadId = GetWindowThreadProcessId(window, &actualProcessId);
    require(actualProcessId == windowProcessId && actualThreadId == windowThreadId,
      "DXVK D3D9 WSI window no longer belongs to the authenticated peer");
    require(GetPropW(window, windowMarker.c_str()) == reinterpret_cast<HANDLE>(uintptr_t(1)),
      "DXVK D3D9 WSI window session marker is missing or replaced");
    RECT client{};
    require(GetClientRect(window, &client) != FALSE && client.left == 0 && client.top == 0,
      "DXVK D3D9 WSI client rectangle query failed");
    const bool matches = uint32_t(client.right) == width && uint32_t(client.bottom) == height;
    if (!matches && resized) { *resized = true; return; }
    require(matches, "DXVK D3D9 WSI client extent differs from the negotiated back buffer");
  }

  HWND surfaceWindow() const {
    if (surfaceTarget)
      return surfaceTarget->validatedWindow();
    validateWindow();
    return window;
  }

  template<typename T, size_t N>
  T& at(std::array<T, N>& slots, uint32_t id) {
    require(id != 0 && id <= N, "DXVK D3D9 typed resource ID out of range");
    return slots[id - 1];
  }

  Buffer& liveBuffer(uint32_t id) {
    auto& buffer = at(buffers, id);
    require(buffer.object != nullptr, "DXVK D3D9 buffer is not live");
    return buffer;
  }

  bool resourcesInUse() const {
    const std::array<Rc<DxvkBuffer>, 17> internal = {
      staging, readback, presentedReadback, vertexConstants, pixelConstants, sharedConstants, specConstants, clipPlanes,
      fixedVertexConstants,fixedPixelConstants,nullVertex,
      batchVertexConstants, batchPixelConstants,
      streamVertex.buffer, streamPixel.buffer, streamFixedVertex.buffer, streamFixedPixel.buffer };
    for (const auto& buffer : internal)
      if (buffer != nullptr && buffer->isInUse(DxvkAccess::Read))
        return true;
    for (const auto& flight : drawFlights)
      if (flightInUse(flight)) return true;
    if ((color != nullptr && color->isInUse(DxvkAccess::Read))
        || (depth != nullptr && depth->isInUse(DxvkAccess::Read))
        || (acquiredImage != nullptr && acquiredImage->isInUse(DxvkAccess::Read)))
      return true;
    for (const auto& buffer : buffers)
      if (buffer.object != nullptr && buffer.object->isInUse(DxvkAccess::Read))
        return true;
    for (const auto& texture : textures)
      if (texture.image != nullptr && texture.image->isInUse(DxvkAccess::Read))
        return true;
    return false;
  }

  static bool flightInUse(const DrawFlight& flight) {
    return (flight.vertex != nullptr && flight.vertex->isInUse(DxvkAccess::Read))
      || (flight.pixel != nullptr && flight.pixel->isInUse(DxvkAccess::Read));
  }

  bool checkFlightSubmissions(bool wait) {
    bool ready = true;
    for (auto& flight : drawFlights) {
      if (!flight.serial) continue;
      auto result = flight.status.result.load(std::memory_order_acquire);
      if (result == VK_NOT_READY && wait)
        result = device->waitForSubmissionUntil(&flight.status, flight.submissionDeadline);
      require(result == VK_SUCCESS || (!wait && result == VK_NOT_READY
        && Clock::now() < flight.submissionDeadline), "DXVK D3D9 flight submission failed or expired");
      ready &= result == VK_SUCCESS;
    }
    return ready;
  }

  void releaseSettledFlights(Stats& stats) {
    for (auto& flight : drawFlights) {
      if (!flight.serial) continue;
      require(flight.status.result.load(std::memory_order_acquire) == VK_SUCCESS
        && flight.serial <= stats.gpuCompleted && !flightInUse(flight),
        "DXVK D3D9 flight release without all original retirement witnesses");
      flight.serial = 0;
      ++stats.flightRetirements;
    }
  }

  void waitRetired() {
    const auto deadline = Clock::now() + CpuWait;
    require(device->waitForResourceRetirementUntil([this] {
      return !resourcesInUse() || device->getDeviceStatus() != VK_SUCCESS;
    }, deadline), "DXVK D3D9 resource use did not retire in 5 seconds");
    healthy();
  }

  uint64_t pollTimeline(Stats& stats) {
    const bool submissionsReady = checkFlightSubmissions(false);
    uint64_t value = 0;
    const VkResult result = device->vkd()->vkGetSemaphoreCounterValue(device->handle(), completion->handle(), &value);
    if (result != VK_SUCCESS) {
      device->notifyDeviceErrorFromDriverResult(result);
      throw std::runtime_error("DXVK D3D9 timeline query failed: " + std::to_string(static_cast<int>(result)));
    }
    require(value <= lastSerial, "DXVK D3D9 unexpected timeline value");
    stats.gpuCompleted = std::max(stats.gpuCompleted, value);
    // A fast GPU may signal before the submission worker publishes its CPU
    // result. Do not recycle stable status storage based on that signal alone.
    // A list still being recorded is not a settled submitted prefix.
    if (pending && submissionsReady && value == lastSerial && !recording() && !resourcesInUse()) {
      stats.retired = lastSerial;
      commands = nullptr;
      pending = false;
      releaseSettledFlights(stats);
    }
    return value;
  }

  void settle(Stats& stats, bool reuseProfile = false) {
    flushRecordedGroup(stats);
    flushStream(stats);
    // In particular, do not issue a new Vulkan wait after a failed WSI call
    // has already latched terminal device status on the production queue.
    healthy();
    {
      WaitTimer timer(reuseProfile ? &stats.reuseWait.cpuSubmissionNs : nullptr);
      checkFlightSubmissions(true);
    }
    if (!pending) {
      WaitTimer timer(reuseProfile ? &stats.reuseWait.retirementNs : nullptr);
      waitRetired();
      return;
    }
    const VkSemaphore semaphore = completion->handle();
    VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wait.semaphoreCount = 1;
    wait.pSemaphores = &semaphore;
    wait.pValues = &lastSerial;
    VkResult result;
    {
      WaitTimer timer(reuseProfile ? &stats.reuseWait.gpuWaitNs : nullptr);
      if (reuseProfile)
        ++stats.reuseWait.gpuCalls;
      result = device->vkd()->vkWaitSemaphores(device->handle(), &wait, GpuWaitNs);
    }
    if (result != VK_SUCCESS) {
      device->notifyDeviceErrorFromDriverResult(result);
      throw std::runtime_error("DXVK D3D9 GPU wait failed: " + std::to_string(static_cast<int>(result)));
    }
    stats.gpuCompleted = lastSerial;
    // Semaphore completion alone is not DXVK finish-thread resource retirement.
    {
      WaitTimer timer(reuseProfile ? &stats.reuseWait.retirementNs : nullptr);
      waitRetired();
    }
    stats.retired = lastSerial;
    commands = nullptr;
    pending = false;
    releaseSettledFlights(stats);
  }

  DrawFlight& beginDrawFlight(Stats& stats) {
    healthy();
    if(groupMode && !recordedGroupCount) {
      // A group uses the SAME four backings. Before starting the next group,
      // all previous GPU/use witnesses settle; no backing budget is enlarged.
      bool occupied = false;
      for (const auto& old : drawFlights) {
        if (old.serial) {
          ++stats.flightReuses;
          occupied = true;
        }
      }
      const bool measure = stats.reuseWaitProfile && occupied;
      WaitTimer timer(measure ? &stats.reuseWait.totalNs : nullptr);
      if (measure)
        ++stats.reuseWait.episodes;
      settle(stats, measure);
      nextFlight=0;
    }
    checkFlightSubmissions(false);
    auto& flight = drawFlights[nextFlight];
    if (flight.serial) {
      ++stats.flightReuses;
      const bool measure = stats.reuseWaitProfile;
      WaitTimer totalTimer(measure ? &stats.reuseWait.totalNs : nullptr);
      if (measure)
        ++stats.reuseWait.episodes;
      {
        WaitTimer timer(measure ? &stats.reuseWait.cpuSubmissionNs : nullptr);
        require(device->waitForSubmissionUntil(&flight.status, flight.submissionDeadline) == VK_SUCCESS,
          "DXVK D3D9 flight CPU submission did not finish");
      }
      const VkSemaphore semaphore = completion->handle();
      VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
      wait.semaphoreCount = 1; wait.pSemaphores = &semaphore; wait.pValues = &flight.serial;
      VkResult result;
      {
        WaitTimer timer(measure ? &stats.reuseWait.gpuWaitNs : nullptr);
        if (measure)
          ++stats.reuseWait.gpuCalls;
        result = device->vkd()->vkWaitSemaphores(device->handle(), &wait, GpuWaitNs);
      }
      if (result != VK_SUCCESS) {
        device->notifyDeviceErrorFromDriverResult(result);
        throw std::runtime_error("DXVK D3D9 flight GPU wait failed");
      }
      {
        WaitTimer timer(measure ? &stats.reuseWait.retirementNs : nullptr);
        require(device->waitForResourceRetirementUntil([&] {
          return !flightInUse(flight) || device->getDeviceStatus() != VK_SUCCESS;
        }, Clock::now() + CpuWait), "DXVK D3D9 flight buffers did not retire");
      }
      healthy();
      stats.gpuCompleted = std::max(stats.gpuCompleted, flight.serial);
      // GPU-completed != fully retired prefix. Only settle/poll may publish
      // retired after checking ALL author/internal resources.
      flight.serial = 0; ++stats.flightRetirements;
    }
    if (flight.vertex == nullptr) {
      const auto uniform = [&](uint32_t bytes, const char* name) {
        return coherentBuffer(bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ShaderStages,
          VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT, name);
      };
      flight.vertex = uniform(batchVertexStride * MaxDrawBatch, "x64-d3d9-flight-vs");
      flight.pixel = uniform(batchPixelStride * MaxDrawBatch, "x64-d3d9-flight-ps");
      stats.batchConstantBytes += uint64_t(batchVertexStride + batchPixelStride) * MaxDrawBatch;
    }
    // These aliases select a flight for recordDraw. All flights retain their
    // own backing until the existing DXVK retirement path authorizes reuse.
    batchVertexConstants = flight.vertex; batchPixelConstants = flight.pixel;
    if(!groupMode||!recordedGroupCount) {
      require(commands == nullptr, "DXVK D3D9 flight encountered active recording");
      commands = device->createCommandList();
      context->beginRecording(commands);
    } else require(commands != nullptr,"DXVK D3D9 group lost recording");
    return flight;
  }

  void flushRecordedGroup(Stats& stats) {
    if(!recordedGroupCount)return;
    require(groupMode&&recordedGroupCount<=MaxDrawFlights&&commands!=nullptr,
      "DXVK D3D9 invalid recorded group");
    canAdd(stats.groupedSubmissions,1);
    // submit(false) waits for the REAL CPU queue result. Only that successful
    // common result is copied to the four stable flight witnesses. GPU and
    // resource retirement remain pending under the original owner rules.
    submit(stats,false);
    for(uint32_t i=0;i<recordedGroupCount;++i) {
      auto& flight=drawFlights[i];require(!flight.serial,"DXVK D3D9 group overwrite");
      flight.serial=lastSerial;flight.status.result.store(VK_SUCCESS,std::memory_order_release);
      flight.submissionDeadline=Clock::now()+CpuWait;
    }
    stats.flightSubmissions+=recordedGroupCount;
    stats.maxOccupiedFlightSlots=std::max(stats.maxOccupiedFlightSlots,recordedGroupCount);
    ++stats.groupedSubmissions;recordedGroupCount=0;
  }

  void submitDrawFlight(Stats& stats, DrawFlight& flight) {
    if(groupMode) {
      require(recordedGroupCount<MaxDrawFlights && &flight==&drawFlights[recordedGroupCount],
        "DXVK D3D9 group flight order");
      ++recordedGroupCount;nextFlight=recordedGroupCount%MaxDrawFlights;
      if(recordedGroupCount==MaxDrawFlights)flushRecordedGroup(stats);
      return;
    }
    canAdd(lastSerial, 1);
    require(!flight.serial, "DXVK D3D9 flight still owned");
    const auto serial = lastSerial + 1;
    context->signalFence(completion, serial);
    context->endFrame();
    commands = context->endRecording(nullptr);
    flight.status.result.store(VK_NOT_READY, std::memory_order_release);
    flight.submissionDeadline = Clock::now() + CpuWait;
    // Production queue takes the command-list reference. No GPU completion is
    // inferred from its bounded CPU enqueue, and every status lives in Owner.
    device->submitCommandList(commands, nullptr, serial, &flight.status);
    flight.serial = serial; lastSerial = serial; stats.submitted = serial;
    pending = true; commands = nullptr;
    ++stats.flightSubmissions;
    uint32_t occupied = 0;
    for (const auto& f : drawFlights) occupied += f.serial != 0;
    stats.maxOccupiedFlightSlots = std::max(stats.maxOccupiedFlightSlots, occupied);
    nextFlight = (nextFlight + 1) % MaxDrawFlights;
  }

  bool recording() const noexcept { return recordedGroupCount != 0 || streamOpen; }

  void beginStream() {
    healthy();
    if (streamOpen)
      return;
    // Unlike begin(), opening a stream list never settles earlier GPU work.
    require(commands == nullptr, "DXVK D3D9 stream encountered an owned command list");
    commands = device->createCommandList();
    context->beginRecording(commands);
    streamOpen = true;
  }

  uint32_t ringSlice(ConstantRing& ring, Stats& stats) {
    require(streamOpen && ring.buffer != nullptr, "DXVK D3D9 constant ring outside a stream list");
    if (ring.buffer->info().size - ring.offset < ring.stride) {
      context->invalidateBuffer(ring.buffer, ring.buffer->allocateStorage());
      ring.offset = 0;
      ++stats.streamConstantRenames;
    }
    const uint32_t offset = ring.offset;
    ring.offset += ring.stride;
    return offset;
  }

  // CPU submission result only. GPU completion and resource retirement stay
  // with settle(), events and the unchanged teardown witnesses.
  void flushStream(Stats& stats) {
    if (!streamOpen)
      return;
    canAdd(stats.streamSubmissions, 1);
    submit(stats, false);
    commands = nullptr;
    streamOpen = false;
    ++stats.streamSubmissions;
    stats.largestStreamList = std::max(stats.largestStreamList, streamListDraws);
    streamListDraws = 0;
    // Bounded in-flight stream: at most MaxStreamLatency unretired serials
    // ahead of the GPU, like native frame latency. Normal frames never block.
    if (lastSerial <= MaxStreamLatency || stats.gpuCompleted >= lastSerial - MaxStreamLatency)
      return;
    const uint64_t target = lastSerial - MaxStreamLatency;
    uint64_t value = 0;
    VkResult result = device->vkd()->vkGetSemaphoreCounterValue(device->handle(), completion->handle(), &value);
    if (result == VK_SUCCESS && value < target) {
      ++stats.streamLatencyWaits;
      const VkSemaphore semaphore = completion->handle();
      VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
      wait.semaphoreCount = 1; wait.pSemaphores = &semaphore; wait.pValues = &target;
      result = device->vkd()->vkWaitSemaphores(device->handle(), &wait, GpuWaitNs);
      value = target;
    }
    if (result != VK_SUCCESS) {
      device->notifyDeviceErrorFromDriverResult(result);
      throw std::runtime_error("DXVK D3D9 stream latency wait failed");
    }
    require(value <= lastSerial, "DXVK D3D9 unexpected timeline value");
    stats.gpuCompleted = std::max(stats.gpuCompleted, value);
  }

  void waitPresentRetired(Stats& stats) {
    if (!lastPresentFrame)
      return;
    require(presentSignal != nullptr, "DXVK D3D9 WSI frame signal is absent");
    const auto deadline = Clock::now() + CpuWait;
    for (;;) {
      healthy();
      const uint64_t value = presentSignal->value();
      require(value <= lastPresentFrame, "DXVK D3D9 WSI frame signal exceeded its owner");
      if (value == lastPresentFrame)
        break;
      require(Clock::now() < deadline, "DXVK D3D9 WSI frame did not retire in 5 seconds");
      Sleep(1);
    }
    // The production finish queue (and optional present-wait worker) advanced
    // this signal. It is deliberately not called a display/pixel completion.
    stats.presentRetired = lastPresentFrame;
  }

  void begin(Stats& stats) {
    settle(stats);
    healthy();
    require(commands == nullptr, "DXVK D3D9 command list still owned");
    commands = device->createCommandList();
    context->beginRecording(commands);
  }

  void submit(Stats& stats, bool waitForGpu) {
    canAdd(lastSerial, 1);
    const uint64_t serial = lastSerial + 1;
    context->signalFence(completion, serial);
    context->endFrame();
    commands = context->endRecording(nullptr);
    submission.result.store(VK_NOT_READY, std::memory_order_release);
    device->submitCommandList(commands, nullptr, serial, &submission);
    lastSerial = serial;
    stats.submitted = serial;
    pending = true;
    const VkResult result = device->waitForSubmissionUntil(&submission, Clock::now() + CpuWait);
    if (result != VK_SUCCESS)
      throw std::runtime_error("DXVK D3D9 submit failed or timed out: " + std::to_string(static_cast<int>(result)));
    if (waitForGpu)
      settle(stats);
  }

  Rc<DxvkBuffer> coherentBuffer(uint32_t bytes, VkBufferUsageFlags usage,
      VkPipelineStageFlags stages, VkAccessFlags access, const char* name) {
    DxvkBufferCreateInfo info;
    info.size = bytes;
    info.usage = usage;
    info.stages = stages | VK_PIPELINE_STAGE_HOST_BIT;
    info.access = access;
    info.debugName = name;
    Rc<DxvkBuffer> buffer = device->createBuffer(info, CoherentMemory);
    require((buffer->memFlags() & CoherentMemory) == CoherentMemory,
      "DXVK D3D9 internal buffer is not coherent host-visible memory");
    require(buffer->mapPtr(0) != nullptr, "DXVK D3D9 internal mapping unavailable");
    return buffer;
  }

  void copyAcquiredImage(const Rc<DxvkCommandList>& external) {
    require(diagnosticPresentedReadback && presentedReadback != nullptr
      && presentedBytes.size() == presentedSize() && acquiredImage != nullptr,
      "DXVK D3D9 presented readback storage is unavailable");
    const auto& info = acquiredImage->info();
    require(info.extent.width == width && info.extent.height == height && info.extent.depth == 1
      && info.numLayers == 1 && info.mipLevels == 1 && info.sampleCount == VK_SAMPLE_COUNT_1_BIT
      && (info.format == VK_FORMAT_B8G8R8A8_UNORM || info.format == VK_FORMAT_B8G8R8A8_SRGB)
      && info.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
      && info.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
      && (info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
      "DXVK D3D9 actual swapchain image does not support the BGRA8 readback profile");

    // The production blitter has already transitioned this acquired image to
    // PRESENT_SRC. Copy on that SAME command list, while we still own it, and
    // restore the layout before the WSI semaphore signal/queuePresent.
    // Use the real DXVK allocation offset, not an assumed VkBuffer offset 0.
    const auto slice = presentedReadback->getSliceInfo(0, presentedSize());
    VkImageMemoryBarrier2 imageBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = acquiredImage->handle();
    imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkBufferMemoryBarrier2 bufferBarrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    bufferBarrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    bufferBarrier.srcAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    bufferBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.buffer = slice.buffer;
    bufferBarrier.offset = slice.offset;
    bufferBarrier.size = slice.size;
    VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &imageBarrier;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &bufferBarrier;
    external->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);

    VkBufferImageCopy2 region = { VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 };
    region.bufferOffset = slice.offset;
    region.imageSubresource = ColorLayer;
    region.imageExtent = {width, height, 1};
    VkCopyImageToBufferInfo2 copy = { VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2 };
    copy.srcImage = acquiredImage->handle();
    copy.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copy.dstBuffer = slice.buffer;
    copy.regionCount = 1;
    copy.pRegions = &region;
    external->cmdCopyImageToBuffer(DxvkCmdBuffer::ExecBuffer, &copy);

    imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    imageBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bufferBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    bufferBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bufferBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    external->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
    external->track(acquiredImage, DxvkAccess::Read);
    external->track(presentedReadback, DxvkAccess::Write);
  }

  Rc<DxvkImage> image(uint32_t w, uint32_t h, VkFormat format, bool target, bool isDepth,uint32_t levels=1,
      VkFormat srgb=VK_FORMAT_UNDEFINED) {
    DxvkImageCreateInfo info;
    const VkFormat formats[]={format,srgb};
    if(srgb!=VK_FORMAT_UNDEFINED){info.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;info.viewFormatCount=2;info.viewFormats=formats;}
    info.type = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = {w, h, 1};
    info.numLayers = 1;
    info.mipLevels = levels;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    if (target) {
      info.usage |= isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      info.stages |= isDepth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
        : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      info.access |= isDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
        : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (!target || (!isDepth && window != nullptr)) {
      // The WSI source is the same real D3D9 color target, sampled by the
      // production swapchain blitter, not a separate renderer or CPU image.
      info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
      info.stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      info.access |= VK_ACCESS_SHADER_READ_BIT;
    }
    if (target && !isDepth && window != nullptr) {
      info.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      // Same contract as a production D3D9 back buffer: this is a context
      // handoff/default-layout flag, NOT cross-process shared memory.
      info.shared = VK_TRUE;
    }
    info.layout = VK_IMAGE_LAYOUT_GENERAL;
    info.debugName = target ? (isDepth ? "x64-d3d9-depth" : "x64-d3d9-color") : "x64-d3d9-texture";
    return device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }

  // The color/depth pair at the current extent. An opaque target keeps the
  // BGRA8 storage, as native X8R8G8B8 does; its presented view reads alpha as one.
  void createTargets() {
    color = image(width, height, VK_FORMAT_B8G8R8A8_UNORM, true, false);
    depth = image(width, height, VK_FORMAT_D32_SFLOAT, true, true);
    colorView = color->createView(viewKey(VK_FORMAT_B8G8R8A8_UNORM,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL));
    depthView = depth->createView(viewKey(VK_FORMAT_D32_SFLOAT,
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_GENERAL));
    if (window != nullptr) {
      auto key = viewKey(VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
      if (opaque)
        key.packedSwizzle = DxvkImageViewKey::packSwizzle({VK_COMPONENT_SWIZZLE_R,
          VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE});
      colorSampleView = color->createView(key);
    }
  }

  void bindTargets() {
    DxvkRenderTargets targets{};
    targets.color[0].view = colorView;
    targets.depth.view = depthView;
    context->bindRenderTargets(std::move(targets), 0);
  }

  void fillConstants(const Rc<DxvkBuffer>& buffer, const D3D9ConstantLayout& layout,
      const std::array<float, FloatRegisters * 4>& values, const Shader& shader, uint32_t offset = 0) {
    auto* bytes = static_cast<uint8_t*>(buffer->mapPtr(offset));
    std::memset(bytes, 0, layout.totalSize());
    std::memcpy(bytes + layout.floatOffset(), values.data(), values.size() * sizeof(float));
    if (shader.needsConstantCopies) {
      for (const auto& constant : shader.constants) {
        require(constant.uboIdx < FloatRegisters, "DXVK D3D9 shader constant copy out of profile");
        std::memcpy(bytes + layout.floatOffset() + constant.uboIdx * 16, constant.float32, 16);
      }
    }
  }

  void unbindAuthorResources() {
    context->bindVertexBuffer(0, DxvkBufferSlice(), 0);
    context->bindIndexBuffer(DxvkBufferSlice(), VK_INDEX_TYPE_UINT16);
    for(uint32_t i=0;i<d9::PixelTextureStages;++i)
      context->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, PixelSamplerSlot+i, nullptr);
    context->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(nullptr);
    context->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(nullptr);
  }

  bool canCleanWindowFault(const Stats& stats) const {
    // CPU witnesses only; never try a speculative GPU wait after a failure.
    // shutdown() requires empty typed author tables. Broader resource-bearing
    // and in-flight failures remain quarantined until their own teardown gate.
    if (thread != GetCurrentThreadId() || device == nullptr
        || device->getDeviceStatus() != VK_SUCCESS || pending || commands != nullptr
        || acquiredImage != nullptr || acquiredView != nullptr || resourcesInUse()
        || stats.liveResources != 0 || stats.liveLogicalBytes != 0
        || stats.submitted != stats.gpuCompleted || stats.gpuCompleted != stats.retired
        || stats.retired != lastSerial || stats.acquiredImages != stats.presentQueued
        || stats.presentQueued != stats.presentApiSucceeded
        || stats.presentApiSucceeded != stats.presentGpuCompleted
        || stats.presentGpuCompleted != stats.presentRetired
        || stats.presentRetired != lastPresentFrame || presentSignal == nullptr
        || presentSignal->value() != lastPresentFrame)
      return false;
    for (const auto& b : buffers) if (b.object != nullptr) return false;
    for (const auto& t : textures) if (t.image != nullptr || t.view != nullptr) return false;
    for (const auto& s : vertexShaders) if (s.object != nullptr) return false;
    for (const auto& s : pixelShaders) if (s.object != nullptr) return false;
    for (const auto& d : declarations) if (d.count) return false;
    return true;
  }

  void shutdown(Stats& stats, uint64_t operationDeadline, bool allowWindowFault = false) {
    // Real DXVK destructors retain their own worker join/drain behavior; the
    // host process timeout, not this function, bounds a broken driver teardown.
    // signalFrame precedes the finish thread dropping its Presenter reference
    // and popping its entry. Keep OUR device/Presenter references alive until
    // that CPU queue tail is drained, so the final device destruction cannot
    // be handed back to the finish thread (which would wait/join on itself).
    // This original API also calls Vulkan device idle: healthy teardown only,
    // never a speculative wait on the quarantine/unknown-device branch.
    healthy();
    device->waitForIdle();
    healthy();
    stats.shutdownQueueDrained = true;
    unbindAuthorResources();
    // finish() has already observed the real GPU/resource/frame milestones.
    // Let production Presenter own WSI fence/swapchain destruction. Its driver
    // waits still require the external owned-process watchdog on a hang.
    if (presenter != nullptr)
      presenter->destroyResources();
    presenter = nullptr;
    // WSI destruction can itself latch device loss while waiting/resetting a
    // present fence. A returned destructor alone is not healthy retirement.
    healthy();
    // Presenter destructor has now destroyed the real VkSurface, following the
    // existing healthy GPU/frame/resource retirement and queue-tail drain.
    if (surfaceTarget)
      surfaceTarget->closeAfterSurfaceRetired(operationDeadline, allowWindowFault);
    blitter = nullptr;
    presentSignal = nullptr;
    acquiredView = nullptr;
    acquiredImage = nullptr;
    colorSampleView = nullptr;
    context = nullptr;
    commands = nullptr;
    completion = nullptr;
    for(auto& sampler:samplers)sampler=nullptr;
    colorView = nullptr;
    depthView = nullptr;
    color = nullptr;
    depth = nullptr;
    staging = nullptr;
    readback = nullptr;
    presentedReadback = nullptr;
    vertexConstants = nullptr;
    pixelConstants = nullptr;
    fixedVertexConstants=nullptr;fixedPixelConstants=nullptr;nullVertex=nullptr;
    fixedVertexShader.reset();fixedPixelShader.reset();
    batchVertexConstants = nullptr;
    batchPixelConstants = nullptr;
    for (auto& flight : drawFlights) {
      require(!flight.serial, "DXVK D3D9 shutdown with an unretired draw flight");
      flight.vertex = nullptr; flight.pixel = nullptr;
    }
    require(!streamOpen, "DXVK D3D9 shutdown with an open stream list");
    for (auto* ring : { &streamVertex, &streamPixel, &streamFixedVertex, &streamFixedPixel })
      ring->buffer = nullptr;
    sharedConstants = nullptr;
    specConstants = nullptr;
    clipPlanes = nullptr;
    shaderModules.reset();
    device = nullptr;
    adapter = nullptr;
    instance = nullptr;
  }
};

D3D9Backend::D3D9Backend() : m_owner(std::make_unique<Owner>()) { }
D3D9Backend::~D3D9Backend() noexcept { quarantine(); }

D3D9Backend::Owner& D3D9Backend::active(Admission admission) {
  require(m_owner != nullptr, "DXVK D3D9 backend finished or quarantined");
  require(m_owner->thread == GetCurrentThreadId(), "DXVK D3D9 call outside its owner thread");
  // Shared entry for all new GPU work. Present alone first selects its
  // already-proven quiescent fault cleanup; it validates before acquisition.
  // Idle transport wake/cancellation is a separate product-owner boundary.
  if (admission == Admission::Work && m_owner->surfaceTarget)
    m_owner->surfaceTarget->validate();
  return *m_owner;
}

void D3D9Backend::quarantine() noexcept {
  if (m_owner) {
    m_stats.quarantined = true;
    if (m_owner->surfaceTarget) {
      m_owner->surfaceTarget->onOwnerQuarantined();
      // Backend-thread snapshot NOW, not a claim about subsequent OS teardown.
      // WndProc never logs; quarantining must not silently lose known faults.
      m_owner->surfaceTarget->report("owner-quarantine-snapshot");
    }
    (void)m_owner.release();
  }
}

void D3D9Backend::initialize(uint32_t width, uint32_t height, HWND window,
    uint32_t windowProcessId, uint32_t windowThreadId, const wchar_t* windowMarker,
    bool diagnosticPresentedReadback, HANDLE authenticatedPeerProcess, surface::Mode surfaceMode,
    const D3D9Factory* factory, bool opaqueTarget, uint32_t interval) {
  const uint64_t initializationDeadline = GetTickCount64() + 5000;
  try {
    Owner& owner = active();
    require(!m_stats.initialized && owner.instance == nullptr, "DXVK D3D9 duplicate initialize");
    require(d9::ValidTargetExtent(width, height), "DXVK D3D9 target dimensions out of profile");
    // Host-owned surface windows are still created at the 64x64 lab extent.
    require(surfaceMode == surface::Mode::Peer || (width == d9::Width && height == d9::Height),
      "DXVK D3D9 host-owned surface supports only the 64x64 lab extent");
    require(!diagnosticPresentedReadback || window != nullptr,
      "DXVK D3D9 presented readback requires the WSI profile");
    require(d9::ValidInterval(window != nullptr, interval), "DXVK D3D9 presentation interval outside profile");
    owner.diagnosticPresentedReadback = diagnosticPresentedReadback;
    owner.width = width;
    owner.height = height;
    owner.opaque = opaqueTarget;
    wchar_t flights[8]{};
    const DWORD flightLength = GetEnvironmentVariableW(L"WARVK_D3D9_GPU_FLIGHTS", flights, 8);
    require(!flightLength || (flightLength == 1 && flights[0] == L'4'),
      "DXVK D3D9 unknown bounded flight development profile");
    owner.flightMode = flightLength != 0;
    wchar_t waitProfile[8]{};
    const DWORD waitProfileLength = GetEnvironmentVariableW(L"WARVK_D3D9_WAIT_PROFILE", waitProfile, 8);
    require(!waitProfileLength || (waitProfileLength == 1 && waitProfile[0] == L'1'),
      "DXVK D3D9 unknown wait diagnostic mode");
    m_stats.reuseWaitProfile = waitProfileLength != 0;
    m_stats.flightCapacity = owner.flightMode ? MaxDrawFlights : 1;
    wchar_t group[8]{};const DWORD groupLength=GetEnvironmentVariableW(L"WARVK_D3D9_GPU_GROUP",group,8);
    require(!groupLength||(owner.flightMode&&groupLength==1&&group[0]==L'4'),
      "DXVK D3D9 submission group requires four bounded flights");
    owner.groupMode=groupLength!=0;m_stats.submissionGroupCapacity=owner.groupMode?MaxDrawFlights:1;
    wchar_t reuse[8]{};const DWORD reuseLength=GetEnvironmentVariableW(L"WARVK_D3D9_BATCH_STATE",reuse,8);
    require(!reuseLength||(reuseLength==1&&reuse[0]==L'1'),"DXVK D3D9 unknown batch state profile");
    m_stats.batchStateReuse=reuseLength!=0;
    wchar_t stream[8]{};
    const DWORD streamLength = GetEnvironmentVariableW(L"WARVK_D3D9_STREAM", stream, 8);
    require(!streamLength || (streamLength == 1 && stream[0] == L'1' && !owner.flightMode && !m_stats.reuseWaitProfile),
      "DXVK D3D9 stream profile excludes flights, groups and the flight wait profile");
    owner.streamMode = m_stats.streamMode = streamLength != 0;
    wchar_t admission[8]{};
    const DWORD admissionLength = GetEnvironmentVariableW(L"WARVK_D3D9_ADMISSION_PROFILE", admission, 8);
    require(!admissionLength || (admissionLength == 1 && admission[0] == L'1'),
      "DXVK D3D9 unknown admission diagnostic mode");
    m_stats.admissionProfile = admissionLength != 0;
    if (window != nullptr) {
      require(windowMarker != nullptr, "DXVK D3D9 WSI requires a session window marker");
      size_t markerLength = 0;
      while (markerLength < 128 && windowMarker[markerLength])
        ++markerLength;
      require(markerLength != 0 && markerLength < 128, "DXVK D3D9 WSI marker is outside its bounded profile");
      owner.window = window;
      owner.windowProcessId = windowProcessId;
      owner.windowThreadId = windowThreadId;
      owner.windowMarker.assign(windowMarker, markerLength);
      if (surfaceMode != surface::Mode::Peer) {
        owner.surfaceTarget = std::make_unique<surface::HostSurfaceTarget>();
        owner.surfaceTarget->start(surfaceMode, window, windowProcessId, windowThreadId,
          authenticatedPeerProcess, windowMarker, initializationDeadline);
      }
      owner.validateWindow();
      m_stats.windowProcessId = windowProcessId;
      m_stats.windowThreadId = windowThreadId;
    } else {
      require(windowProcessId == 0 && windowThreadId == 0 && windowMarker == nullptr
        && authenticatedPeerProcess == nullptr && surfaceMode == surface::Mode::Peer,
        "DXVK D3D9 offscreen initialize has unexpected WSI identity");
    }
    owner.instance = factory ? factory->instance() : Rc<DxvkInstance>(new DxvkInstance(DxvkInstanceFlag::ClientApiIsD3D9));
    owner.adapter = factory ? factory->adapter() : owner.instance->enumAdapters(0);
    require(owner.adapter != nullptr, "DXVK D3D9 found no adapter");
    std::string compatibilityError;
    if (!owner.adapter->isCompatible(compatibilityError))
      throw std::runtime_error("DXVK D3D9 adapter incompatible: " + compatibilityError);
    const auto properties = owner.adapter->deviceProperties().core.properties;
    require(properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU, "DXVK D3D9 requires a hardware GPU");
    m_stats.adapter = properties.deviceName;
    m_stats.vendorId = properties.vendorID;
    m_stats.deviceId = properties.deviceID;
    owner.device = owner.adapter->createDevice();
    // As native creation and SetDepthStencilSurface: the scale is the depth
    // target's R value, and the target is always D32_SFLOAT (D16 maps to it).
    const auto& biasControl = owner.device->features().extDepthBiasControl;
    if (biasControl.depthBiasControl) {
      owner.depthBiasRepresentation.depthBiasExact = biasControl.depthBiasExact;
      if (biasControl.floatRepresentation)
        owner.depthBiasRepresentation.depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT;
      else if (biasControl.leastRepresentableValueForceUnormRepresentation)
        owner.depthBiasRepresentation.depthBiasRepresentation =
          VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
    }
    owner.depthBiasScale = owner.depthBiasRepresentation.depthBiasRepresentation == VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT
      ? 1.0f : GetDepthBufferRValue(VK_FORMAT_D32_SFLOAT, int32_t(properties.vendorID),
          owner.depthBiasRepresentation.depthBiasExact, owner.depthBiasRepresentation.depthBiasRepresentation
            == VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT);
    const uint64_t uniformAlignment = owner.adapter->deviceProperties().core.properties.limits.minUniformBufferOffsetAlignment;
    require(uniformAlignment && uniformAlignment <= 65536, "DXVK D3D9 uniform alignment outside bounded batch");
    const auto aligned = [&](uint32_t bytes) { return uint32_t((bytes + uniformAlignment - 1) / uniformAlignment * uniformAlignment); };
    owner.batchVertexStride = aligned(VertexLayout.totalSize());
    owner.batchPixelStride = aligned(PixelLayout.totalSize());
    if (owner.streamMode) {
      // Real pipeline stages: invalidateBuffer dirties descriptors through the
      // buffer's derived shader stages, so a rename must reach VS and PS.
      const auto ring = [&](uint32_t bytes, const char* name) {
        Owner::ConstantRing result;
        result.stride = aligned(bytes);
        require(result.stride <= StreamRingBytes, "DXVK D3D9 stream slice exceeds its ring");
        result.buffer = owner.coherentBuffer(StreamRingBytes / result.stride * result.stride,
          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT, name);
        m_stats.batchConstantBytes += result.buffer->info().size;
        return result;
      };
      owner.streamVertex = ring(VertexLayout.totalSize(), "x64-d3d9-stream-vs");
      owner.streamPixel = ring(PixelLayout.totalSize(), "x64-d3d9-stream-ps");
      owner.streamFixedVertex = ring(sizeof(D3D9FixedFunctionVS), "x64-d3d9-stream-fixed-vs");
      owner.streamFixedPixel = ring(sizeof(D3D9FixedFunctionPS), "x64-d3d9-stream-fixed-ps");
    }
    owner.context = owner.device->createContext();
    owner.d3d9Options=std::make_unique<D3D9Options>(owner.device,owner.instance->config());
    const auto& options=*owner.d3d9Options;
    owner.moduleInfo.options = DxsoOptions(owner.device, options, VertexLayout.floatSize());
    const DxsoShaderModuleDomain shaderDomain { owner.device.ptr(), owner.moduleInfo,
      VertexLayout, PixelLayout };
    owner.shaderModules = std::make_unique<DxsoShaderModuleCache>(shaderDomain,
      [device = owner.device.ptr()](const Rc<DxvkShader>& shader) {
        device->registerShader(shader);
        if (shader->needsCompile())
          device->requestCompileShader(shader);
      });

    owner.staging = owner.coherentBuffer(MaxBufferBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, "x64-d3d9-staging");
    owner.readback = owner.coherentBuffer(ReadbackBandBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, "x64-d3d9-readback");
    const auto uniform = [&](uint32_t size, const char* name) {
      return owner.coherentBuffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT, name);
    };
    owner.vertexConstants = uniform(VertexLayout.totalSize(), "x64-d3d9-vs-constants");
    owner.pixelConstants = uniform(PixelLayout.totalSize(), "x64-d3d9-ps-constants");
    owner.nullVertex=owner.coherentBuffer(16,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
      VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,"x64-d3d9-null-vertex");
    const float nullVertex[4]={0,0,0,1};std::memcpy(owner.nullVertex->mapPtr(0),nullVertex,16);
    owner.sharedConstants = uniform(sizeof(D3D9SharedPS), "x64-d3d9-shared-constants");
    owner.specConstants = uniform(D3D9SpecializationInfo::UBOSize, "x64-d3d9-spec-constants");
    owner.clipPlanes = uniform(sizeof(D3D9ClipPlane) * caps::MaxClipPlanes, "x64-d3d9-clip-planes");
    std::memset(owner.sharedConstants->mapPtr(0), 0, sizeof(D3D9SharedPS));
    std::memset(owner.clipPlanes->mapPtr(0), 0, sizeof(D3D9ClipPlane) * caps::MaxClipPlanes);
    D3D9SpecializationInfo spec{};
    spec.set<SpecAlphaCompareOp>(VK_COMPARE_OP_ALWAYS);
    owner.specialization=spec;
    std::memcpy(owner.specConstants->mapPtr(0), spec.data.data(), spec.UBOSize);
    for (uint32_t i = 0; i < spec.data.size(); ++i)
      owner.context->setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, i, spec.data[i]);
    owner.context->bindUniformBuffer(ShaderStages, getSpecConstantBufferSlot(), DxvkBufferSlice(owner.specConstants));
    owner.context->bindUniformBuffer(VK_SHADER_STAGE_VERTEX_BIT,
      computeResourceSlotId(DxsoProgramType::VertexShader, DxsoBindingType::ConstantBuffer, VSConstantBuffer),
      DxvkBufferSlice(owner.vertexConstants));
    owner.context->bindUniformBuffer(VK_SHADER_STAGE_FRAGMENT_BIT,
      computeResourceSlotId(DxsoProgramType::PixelShader, DxsoBindingType::ConstantBuffer, PSConstantBuffer),
      DxvkBufferSlice(owner.pixelConstants));
    owner.context->bindUniformBuffer(VK_SHADER_STAGE_FRAGMENT_BIT,
      computeResourceSlotId(DxsoProgramType::PixelShader, DxsoBindingType::ConstantBuffer, PSShared),
      DxvkBufferSlice(owner.sharedConstants));
    owner.context->bindUniformBuffer(VK_SHADER_STAGE_VERTEX_BIT,
      computeResourceSlotId(DxsoProgramType::VertexShader, DxsoBindingType::ConstantBuffer, VSClipPlanes),
      DxvkBufferSlice(owner.clipPlanes));
    D3D9RenderStateInfo renderInfo{};
    owner.context->pushData(ShaderStages, 0, sizeof(renderInfo), &renderInfo);

    DxvkSamplerKey samplerKey{};
    samplerKey.setFilter(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    samplerKey.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    samplerKey.setDepthCompare(false, VK_COMPARE_OP_LESS_OR_EQUAL);
    samplerKey.setLodRange(0, 0, 0);
    owner.samplers[0] = owner.device->createSampler(samplerKey);
    owner.context->bindResourceSampler(VK_SHADER_STAGE_FRAGMENT_BIT, PixelSamplerSlot, Rc<DxvkSampler>(owner.samplers[0]));
    owner.createTargets();
    if (window != nullptr) {
      owner.presentSignal = new sync::Fence(0);
      PresenterDesc desc{};
      desc.diagnosticReadback = diagnosticPresentedReadback;
      owner.validateWindow();
      // Potential WSI lifetime is claimed ONCE, before any Presenter callback.
      // Its legitimate (re)creation callbacks below only revalidate/select.
      if (owner.surfaceTarget)
        owner.surfaceTarget->claimSurface();
      owner.presenter = new Presenter(owner.device, owner.presentSignal, desc,
        [ownerPtr = &owner] (VkSurfaceKHR* surface) {
          // Rechecked on every production surface (re)creation, not only the
          // initial packet. The whole owner outlives its Presenter callback.
          const HWND actualWindow = ownerPtr->surfaceWindow();
          const auto vki = ownerPtr->device->adapter()->vki();
          const VkResult result = wsi::createSurface(actualWindow, vki->getLoaderProc(), vki->instance(), surface);
          if (ownerPtr->surfaceTarget)
            ownerPtr->surfaceTarget->noteSurfaceResult(actualWindow, static_cast<int32_t>(result));
          return result;
        });
      owner.presenter->setSurfaceExtent({width, height});
      owner.presenter->setSurfaceFormat({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
      owner.presenter->setSyncInterval(interval == d9::IntervalImmediate ? 0 : 1);
      owner.presenter->setFrameRateLimit(0, 1);
      const VkResult swapchainStatus = owner.presenter->checkSwapChainStatus();
      if (swapchainStatus != VK_SUCCESS)
        throw std::runtime_error("DXVK D3D9 WSI swapchain creation refused: "
          + std::to_string(static_cast<int>(swapchainStatus)));
      owner.blitter = new DxvkSwapchainBlitter(owner.device, nullptr);
      if (diagnosticPresentedReadback) {
        owner.presentedReadback = owner.coherentBuffer(owner.presentedSize(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          "x64-d3d9-presented-readback");
        owner.presentedBytes.resize(owner.presentedSize());
      }
    }
    DxvkFenceCreateInfo fenceInfo{};
    owner.completion = owner.device->createFence(fenceInfo);
    owner.begin(m_stats);
    owner.context->initImage(owner.color, VK_IMAGE_LAYOUT_UNDEFINED);
    owner.context->initImage(owner.depth, VK_IMAGE_LAYOUT_UNDEFINED);
    owner.submit(m_stats, true);
    m_stats.width = width;
    m_stats.height = height;
    m_stats.opaqueTarget = opaqueTarget;
    m_stats.syncInterval = interval;
    m_stats.presenterImplemented = owner.presenter != nullptr;
    m_stats.initialized = true;
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::reset(uint32_t width, uint32_t height, uint32_t interval, bool opaqueTarget, bool keepResources) {
  try {
    Owner& owner = active();
    require(m_stats.initialized, "DXVK D3D9 is not initialized");
    require(d9::ValidTargetExtent(width, height) && d9::ValidInterval(owner.window != nullptr, interval)
      && (owner.surfaceTarget == nullptr || (width == d9::Width && height == d9::Height)),
      "DXVK D3D9 reset outside profile");
    for (const auto& buffer : owner.buffers)
      require(keepResources || buffer.object == nullptr, "DXVK D3D9 reset with a live DEFAULT-pool buffer");
    // Every earlier draw, clear, readback and frame retires before the old
    // targets are released; the next clear or draw binds the new ones.
    owner.settle(m_stats);
    owner.waitPresentRetired(m_stats);
    require(owner.acquiredImage == nullptr && owner.acquiredView == nullptr,
      "DXVK D3D9 reset while a WSI image is owned");
    owner.unbindAuthorResources();
    owner.context->bindRenderTargets(DxvkRenderTargets{}, 0);
    owner.colorSampleView = nullptr;
    owner.colorView = nullptr;
    owner.depthView = nullptr;
    owner.color = nullptr;
    owner.depth = nullptr;
    owner.width = width;
    owner.height = height;
    owner.opaque = opaqueTarget;
    owner.createTargets();
    if (owner.presenter != nullptr) {
      owner.validateWindow();
      owner.presenter->setSurfaceExtent({width, height});
      owner.presenter->setSyncInterval(interval == d9::IntervalImmediate ? 0 : 1);
      const VkResult swapchainStatus = owner.presenter->checkSwapChainStatus();
      if (swapchainStatus != VK_SUCCESS)
        throw std::runtime_error("DXVK D3D9 WSI swapchain recreation refused: "
          + std::to_string(static_cast<int>(swapchainStatus)));
      if (owner.diagnosticPresentedReadback) {
        owner.presentedReadback = owner.coherentBuffer(owner.presentedSize(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          "x64-d3d9-presented-readback");
        owner.presentedBytes.assign(owner.presentedSize(), 0);
      }
    }
    owner.presentedCaptured = false;
    owner.begin(m_stats);
    owner.context->initImage(owner.color, VK_IMAGE_LAYOUT_UNDEFINED);
    owner.context->initImage(owner.depth, VK_IMAGE_LAYOUT_UNDEFINED);
    owner.submit(m_stats, true);
    m_stats.width = width;
    m_stats.height = height;
    m_stats.opaqueTarget = opaqueTarget;
    m_stats.syncInterval = interval;
    ++m_stats.resets;
    if (keepResources) ++m_stats.windowResets;
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::createBuffer(uint32_t id, uint32_t size, bool index16, bool dynamic) {
  try {
    Owner& owner = active();
    require(m_stats.initialized, "DXVK D3D9 is not initialized");
    auto& slot = owner.at(owner.buffers, id);
    require(slot.object == nullptr && size != 0 && size <= MaxBufferBytes && (!index16 || size % 2 == 0),
      "DXVK D3D9 buffer create outside profile or live replacement");
    DxvkBufferCreateInfo info;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | (index16 ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | (index16 ? VK_ACCESS_INDEX_READ_BIT : VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
    info.debugName = index16 ? "x64-d3d9-index-buffer" : "x64-d3d9-vertex-buffer";
    // D3D9 DYNAMIC, as DXVK maps it: host-visible storage written in place.
    slot.object = dynamic ? owner.coherentBuffer(size, info.usage, info.stages, info.access | VK_ACCESS_HOST_WRITE_BIT,
        index16 ? "x64-d3d9-dynamic-index-buffer" : "x64-d3d9-dynamic-vertex-buffer")
      : owner.device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    slot.size = size;
    slot.index16 = index16;
    slot.dynamic = dynamic;
    if (dynamic)
      ++m_stats.dynamicBuffers;
    if (index16)
      slot.indexBytes.resize(size);
    addResource(m_stats, size);
    ++m_stats.buffersCreated;
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::uploadBuffer(uint32_t id, uint32_t offset, const uint8_t* bytes, uint32_t count, d9::UploadMode mode) {
  try {
    Owner& owner = active();
    auto& slot = owner.liveBuffer(id);
    require(bytes != nullptr && count != 0 && offset <= slot.size && count <= slot.size - offset,
      "DXVK D3D9 buffer upload outside bounds");
    // The finite profile initializes a contiguous prefix, then permits arbitrary
    // updates within/adjacent to that prefix. Gaps are refused, not zero-assumed.
    require(offset <= slot.initializedPrefix, "DXVK D3D9 noncontiguous first buffer upload is unsupported");
    require(slot.dynamic || mode == d9::UploadMode::Ordered, "DXVK D3D9 dynamic Lock mode on a static buffer");
    canAdd(m_stats.uploadedBytes, count);
    if (slot.dynamic) {
      // NoOverwrite writes bytes no earlier draw reads. Discard in a stream
      // list renames the storage: the list keeps the old allocation for the
      // draws recorded before it, and its bytes carry over, so the initialized
      // prefix (and the IB shadow) still describe the new storage. Any other
      // write first settles every earlier use.
      if (mode == d9::UploadMode::Discard && owner.streamMode) {
        owner.beginStream();
        const auto* old = static_cast<const uint8_t*>(slot.object->mapPtr(0));
        owner.context->invalidateBuffer(slot.object, slot.object->allocateStorage());
        std::memcpy(slot.object->mapPtr(0), old, slot.initializedPrefix);
        ++m_stats.discardRenames;
        m_stats.discardCarriedBytes += slot.initializedPrefix;
      } else if (mode != d9::UploadMode::NoOverwrite) {
        owner.settle(m_stats);
        ++m_stats.uploadSettles;
      }
      std::memcpy(static_cast<uint8_t*>(slot.object->mapPtr(0)) + offset, bytes, count);
      ++(mode == d9::UploadMode::Discard ? m_stats.discardUploads
        : mode == d9::UploadMode::NoOverwrite ? m_stats.noOverwriteUploads : m_stats.orderedDynamicUploads);
    } else {
      owner.settle(m_stats);
      ++m_stats.uploadSettles;
      std::memcpy(owner.staging->mapPtr(0), bytes, count);
      owner.begin(m_stats);
      owner.context->copyBuffer(slot.object, offset, owner.staging, 0, count);
      owner.submit(m_stats, true);
    }
    if (slot.index16)
      std::memcpy(slot.indexBytes.data() + offset, bytes, count);
    slot.initializedPrefix = std::max(slot.initializedPrefix, offset + count);
    m_stats.uploadedBytes += count;
    ++m_stats.uploads;
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::createTexture(uint32_t id, uint32_t width, uint32_t height) {
  require(width<=MaxDimension&&height<=MaxDimension,"legacy texture profile extent");
  d9::TextureLayout layout;
  require(d9::MakeTextureLayout(width,height,1,layout),"texture extent");
  createTexture2D(id,layout);
}
void D3D9Backend::createTexture2D(uint32_t id,const d9::TextureLayout& layout) {
  try {
    Owner& owner = active();
    require(m_stats.initialized, "DXVK D3D9 is not initialized");
    auto& slot = owner.at(owner.textures, id);
    d9::TextureLayout checked;
    require(slot.image == nullptr && d9::MakeTextureLayout(layout.width,layout.height,layout.levels,checked,layout.format)
        &&checked.bytes==layout.bytes,
      "DXVK D3D9 texture create outside profile or live replacement");
    const auto mapping=ConvertFormatUnfixed(D3D9Format(checked.format));
    require(mapping.IsValid()&&mapping.Aspect==VK_IMAGE_ASPECT_COLOR_BIT
        &&mapping.ConversionFormatInfo.FormatType==D3D9ConversionFormat_None,"texture format requires unsupported conversion");
    slot.image = owner.image(checked.width,checked.height,mapping.FormatColor,false,false,checked.levels,mapping.FormatSrgb);
    auto key=viewKey(mapping.FormatColor,VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    key.packedSwizzle=DxvkImageViewKey::packSwizzle(mapping.Swizzle);slot.mapping=mapping;
    key.mipCount=checked.levels;slot.view=slot.image->createView(key);
    if(mapping.FormatSrgb!=VK_FORMAT_UNDEFINED){key.format=mapping.FormatSrgb;slot.srgbView=slot.image->createView(key);}
    slot.width=checked.width;slot.height=checked.height;slot.layout=checked;
    owner.begin(m_stats);
    owner.context->initImage(slot.image, VK_IMAGE_LAYOUT_UNDEFINED);
    owner.submit(m_stats, true);
    addResource(m_stats,checked.bytes);
    ++m_stats.texturesCreated;
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::uploadTexture(uint32_t id, const uint8_t* bgra, uint32_t count) {
  const auto& slot=active().at(active().textures,id);
  require(slot.layout.levels==1&&count==slot.layout.bytes,"legacy texture upload requires one level");
  uploadTextureRegion(id,{0,0,0,slot.width,slot.height,count},bgra);
}
void D3D9Backend::uploadTextureRegion(uint32_t id,const d9::TextureRegion& region,const uint8_t* bgra) {
  try {
    Owner& owner = active();
    auto& slot = owner.at(owner.textures, id);
    d9::TextureRegion checked;
    require(slot.image!=nullptr&&bgra&&d9::MakeTextureRegion(slot.layout,region.level,region.x,region.y,region.width,region.height,checked)
        &&checked.bytes==region.bytes,"texture subresource upload domain");
    const auto count=checked.bytes;
    canAdd(m_stats.uploadedBytes, count);
    owner.settle(m_stats);
    if(owner.staging->info().size<count)
      owner.staging=owner.coherentBuffer(count,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT,"x64-d3d9-texture-upload");
    std::memcpy(owner.staging->mapPtr(0), bgra, count);
    owner.begin(m_stats);
    const VkImageSubresourceLayers layer={VK_IMAGE_ASPECT_COLOR_BIT,checked.level,0,1};
    owner.context->copyBufferToImage(slot.image, layer, {int32_t(checked.x),int32_t(checked.y),0}, {checked.width,checked.height,1},
      owner.staging, 0, 0, 0, slot.mapping.FormatColor);
    owner.submit(m_stats, true);
    if(d9::FullTextureLevel(slot.layout,checked))slot.initializedLevels|=1u<<checked.level;
    slot.uploaded=slot.initializedLevels==((1u<<slot.layout.levels)-1);
    m_stats.uploadedBytes += count;
    ++m_stats.uploads;
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::setTextureLod(uint32_t id,uint32_t lod) {
  try {
    auto& owner=active();auto& slot=owner.at(owner.textures,id);
    require(slot.image!=nullptr&&lod<slot.layout.levels,"texture LOD domain");
    owner.settle(m_stats);
    auto key=viewKey(slot.mapping.FormatColor,VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    key.packedSwizzle=DxvkImageViewKey::packSwizzle(slot.mapping.Swizzle);
    key.mipIndex=lod;key.mipCount=slot.layout.levels-lod;
    auto view=slot.image->createView(key);Rc<DxvkImageView> srgb;
    if(slot.mapping.FormatSrgb!=VK_FORMAT_UNDEFINED){key.format=slot.mapping.FormatSrgb;srgb=slot.image->createView(key);}
    slot.view=std::move(view);slot.srgbView=std::move(srgb);slot.lod=lod;
  }catch(...){quarantine();throw;}
}

void D3D9Backend::setSamplerState(const d9::SamplerState& state) {
  requireProfile(d9::ValidSamplerState(state),"invalid sampler state");
  // CPU API state only. Previously recorded draws already own their immutable
  // sampler/view descriptors through the existing DXVK command/resource path.
  active().samplerState=state;
}

void D3D9Backend::setAdditionalSamplers(const std::array<d9::SamplerState,d9::AdditionalTextureStages>& states) {
  for(const auto& state:states)requireProfile(d9::ValidSamplerState(state),"invalid additional sampler state");
  active().additionalSamplers=states;
}

void D3D9Backend::setEffects(const d9::EffectsState& state) {
  requireProfile(d9::ValidEffects(state),"invalid alpha/fog/scissor state");
  active().effects=state;
}

void D3D9Backend::createShader(uint32_t id, ShaderStage stage, const uint32_t* originalTokens, uint32_t wordCount) {
  try {
    Owner& owner = active();
    require(m_stats.initialized, "DXVK D3D9 is not initialized");
    require(stage == ShaderStage::Vertex || stage == ShaderStage::Pixel, "DXVK D3D9 invalid shader stage");
    auto& slot = stage == ShaderStage::Vertex ? owner.at(owner.vertexShaders, id) : owner.at(owner.pixelShaders, id);
    require(slot.object == nullptr, "DXVK D3D9 shader would replace a live slot");
    const auto scan = d3d9tokens::Scan(originalTokens, wordCount);
    require(scan.accepted() && scan.bytes == uint64_t(wordCount) * 4,
      "DXVK D3D9 bytecode refused by bounded token scanner");
    require(scan.stage == (stage == ShaderStage::Vertex ? d3d9tokens::Stage::Vertex : d3d9tokens::Stage::Pixel),
      "DXVK D3D9 shader stage disagrees with bytecode");
    // No unbounded reader is entered before the owned packet's exact extent and
    // every supported instruction/operand have been checked by the scanner.
    DxsoReader reader(reinterpret_cast<const char*>(originalTokens));
    DxsoModule module(reader);
    const VkShaderStageFlagBits shaderStage = stage == ShaderStage::Vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    require(module.info().shaderStage() == shaderStage && module.info().majorVersion() == 3
      && module.info().minorVersion() == 0, "DXVK D3D9 expected original SM3 bytecode");
    const auto analysis = module.analyze();
    require(analysis.bytecodeByteLength == scan.bytes, "DXVK D3D9 scanner/compiler byte extent mismatch");
    const DxsoShaderModuleDomain domain { owner.device.ptr(), owner.moduleInfo, VertexLayout, PixelLayout };
    const auto compiled = owner.shaderModules->getOrCompile(domain, shaderStage,
      originalTokens, scan.bytes, [&](const DxsoCompiledModule& result, bool) {
        require(result.meta.maxConstIndexF <= FloatRegisters && result.meta.maxConstIndexI == 0
          && result.meta.maxConstIndexB == 0 && result.maxDefinedFloat < int32_t(FloatRegisters)
          && result.maxDefinedInt == -1 && result.maxDefinedBool == -1,
          "DXVK D3D9 shader constants exceed supported profile");
        require(result.usedSamplers <= (stage == ShaderStage::Vertex ? 0u : 0xffu),
          "DXVK D3D9 only pixel samplers 0..7 are supported");
        require(stage == ShaderStage::Vertex || result.usedRTs == 1, "DXVK D3D9 only color output 0 is supported");
      });
    slot.object = compiled->shader;
    slot.inputs = compiled->isgn;
    slot.constants = compiled->constants;
    slot.needsConstantCopies = compiled->meta.needsConstantCopies;
    slot.tokenBytes = scan.bytes;
    addResource(m_stats, scan.bytes);
    // Frozen legacy CREATE counter, not the number of actual CPU compiles.
    ++m_stats.shadersCompiled;
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::createDeclaration(uint32_t id, const D3DVERTEXELEMENT9* elements, uint32_t count) {
  try {
    Owner& owner = active();
    require(m_stats.initialized, "DXVK D3D9 is not initialized");
    auto& slot = owner.at(owner.declarations, id);
    require(slot.count == 0 && elements != nullptr && count != 0 && count <= MaxDeclarationElements,
      "DXVK D3D9 declaration outside profile or live replacement");
    uint32_t extent=0;
    require(d9::ValidDeclaration(elements,count,extent),"DXVK D3D9 declaration outside single-stream contract");
    std::copy(elements, elements + count, slot.elements.begin());
    slot.count = count;
    addResource(m_stats, uint64_t(count) * sizeof(D3DVERTEXELEMENT9));
    ++m_stats.declarationsCreated;
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::clear(uint32_t color, float depth, uint32_t flags) {
  d9::ClearRegions command;command.color=color;command.depth=depth;command.flags=flags;command.count=1;
  command.regions[0]={0,0,m_stats.width,m_stats.height};clearRegions(command);
}

void D3D9Backend::clearRegions(const d9::ClearRegions& command) {
  requireProfile(m_stats.initialized&&command.count&&command.count<=d9::MaxClearRegions
    &&command.flags&&(command.flags&~uint32_t(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER))==0
    &&std::isfinite(command.depth)&&command.depth>=0&&command.depth<=1,"DXVK D3D9 clear domain invalid");
  for(uint32_t i=0;i<command.count;++i)
    requireProfile(d9::ValidClearRegion(command.regions[i],m_stats.width,m_stats.height),
      "DXVK D3D9 clear rectangle outside target");
  try {
    Owner& owner = active();
    owner.begin(m_stats);
    owner.bindTargets();
    const auto clearView=[&](const Rc<DxvkImageView>& view,VkImageAspectFlags aspect,VkClearValue value) {
      for(uint32_t i=0;i<command.count;++i) {
        const auto& r=command.regions[i];
        if(!r.x&&!r.y&&r.width==m_stats.width&&r.height==m_stats.height)
          owner.context->clearRenderTarget(view,aspect,value,0);
        else owner.context->clearImageView(view,{int32_t(r.x),int32_t(r.y),0},{r.width,r.height,1},aspect,value);
      }
    };
    if (command.flags & D3DCLEAR_TARGET) {
      const auto color=command.color;
      VkClearValue value{};
      value.color.float32[0] = float((color >> 16) & 255) / 255;
      value.color.float32[1] = float((color >> 8) & 255) / 255;
      value.color.float32[2] = float(color & 255) / 255;
      value.color.float32[3] = float((color >> 24) & 255) / 255;
      clearView(owner.colorView,VK_IMAGE_ASPECT_COLOR_BIT,value);
    }
    if (command.flags & D3DCLEAR_ZBUFFER) {
      VkClearValue value{};
      value.depthStencil.depth = command.depth;
      clearView(owner.depthView,VK_IMAGE_ASPECT_DEPTH_BIT,value);
    }
    owner.submit(m_stats, true);
    ++m_stats.clears;
  } catch (...) { quarantine(); throw; }
}

struct D3D9Backend::PreparedDraw {
  std::array<DxvkVertexInput, 32> attributes{};
  uint32_t extent = 0, indexCount = 0;
  DxvkRasterizerState raster{};
  DxvkMultisampleState multisample{};
  DxvkDepthStencilState depth{};
  DxvkBlendMode blend{};
  uint32_t inputCount=0;
  bool nullInput=false;
  D3D9FFShaderKeyVS fixedVS;
  D3D9FFShaderKeyFS fixedPS;
};

D3D9Backend::PreparedDraw D3D9Backend::prepareDraw(const DrawState& state,const d9::FixedFunctionState* fixed) {
  Owner& owner = active();
  // Owner/thread/initialization errors are internal terminal failures, not
  // unsupported draw inputs. Keep them outside the recoverable category.
  require(m_stats.initialized, "DXVK D3D9 is not initialized");
  requireProfile(d9::ValidDrawTopology(state.primitiveType, state.primitiveCount),
    "DXVK D3D9 topology or primitive count outside profile");
  requireProfile(state.vertexBuffer != 0 && state.vertexBuffer <= BufferSlots
    && state.indexBuffer <= BufferSlots
    // Null textures are valid for programmable shaders too. The original
    // DXSO SpecSamplerNull path supplies native null-sampler values; a
    // nonzero resource still requires its exact live/uploaded identity.
    && state.texture <= TextureSlots
    && (state.vertexShader != 0||fixed) && state.vertexShader <= ShaderSlots
    && (state.pixelShader != 0||fixed) && state.pixelShader <= ShaderSlots
    && state.declaration != 0 && state.declaration <= DeclarationSlots,
    "DXVK D3D9 draw resource ID outside typed profile");
  auto& vb = owner.at(owner.buffers, state.vertexBuffer);
  auto* ib = state.indexBuffer ? &owner.at(owner.buffers, state.indexBuffer) : nullptr;
  auto* texture = state.texture?&owner.at(owner.textures, state.texture):nullptr;
  uint32_t textureMask=texture?1u:0u;
  for(uint32_t i=0;i<state.additionalTextures.size();++i) {
    const auto id=state.additionalTextures[i];
    requireProfile(id<=TextureSlots,"additional texture ID outside typed profile");
    if(!id)continue;
    const auto& value=owner.at(owner.textures,id);
    requireProfile(value.image!=nullptr&&value.uploaded,"additional texture missing or uninitialized");
    textureMask|=1u<<(i+1);
  }
  auto* vs = state.vertexShader?&owner.at(owner.vertexShaders, state.vertexShader):nullptr;
  auto* ps = state.pixelShader?&owner.at(owner.pixelShaders, state.pixelShader):nullptr;
  auto& decl = owner.at(owner.declarations, state.declaration);
  requireProfile(vb.object != nullptr && !vb.index16 && (!ib||(ib->object!=nullptr&&ib->index16))
    && (!texture||(texture->image != nullptr && texture->uploaded))
    && (!vs||vs->object != nullptr) && (!ps||ps->object != nullptr) && decl.count != 0,
    "DXVK D3D9 draw has missing or wrong-type resources");
  require(decl.count <= decl.elements.size() && (!ib||ib->initializedPrefix <= ib->indexBytes.size()),
    "DXVK D3D9 stored declaration or index shadow invariant violated");
  requireProfile(state.stride != 0 && state.stride <= 2048 && state.streamOffset <= vb.size,
    "DXVK D3D9 stream parameters outside profile");
  std::array<DxvkVertexInput,32> attributes{};
  const auto& inputs=vs?vs->inputs:GetFixedFunctionIsgn();
  requireProfile(inputs.elemCount != 0 && inputs.elemCount <= attributes.size(),
    "DXVK D3D9 vertex shader input count outside profile");
  uint32_t extent = 0;
  bool nullInput=false;
  for (uint32_t i = 0; i < inputs.elemCount; ++i) {
    const auto& input = inputs.elems[i];
    bool found = false;
    for (uint32_t j = 0; j < decl.count; ++j) {
      const auto& element = decl.elements[j];
      const auto usage=element.Usage==D3DDECLUSAGE_POSITIONT?D3DDECLUSAGE_POSITION:element.Usage;
      if (uint32_t(input.semantic.usage) == usage && input.semantic.usageIndex == element.UsageIndex) {
        DxvkVertexAttribute attribute{};
        // Production D3D9 BindInputLayout indexes the compiler ISGN, not the
        // original v# register or author declaration order.
        attribute.location = i;
        attribute.binding = 0;
        attribute.format = declarationFormat(element.Type);
        attribute.offset = element.Offset;
        attributes[i] = DxvkVertexInput(attribute);
        extent = std::max(extent, uint32_t(element.Offset) + declarationSize(element.Type));
        found = true;
        break;
      }
    }
    if(!found) {
      requireProfile(!vs,"DXVK D3D9 programmable declaration missing a required shader semantic");
      DxvkVertexAttribute attribute{};attribute.location=i;attribute.binding=1;
      attribute.format=VK_FORMAT_R32G32B32A32_SFLOAT;attribute.offset=0;
      attributes[i]=DxvkVertexInput(attribute);nullInput=true;
    }
  }
  requireProfile(extent <= state.stride, "DXVK D3D9 declaration exceeds vertex stride");
  const uint64_t indexCount = d9::DrawElementCount(state.primitiveType, state.primitiveCount);
  const uint64_t firstIndexByte = uint64_t(state.startIndex) * 2;
  requireProfile(indexCount <= UINT32_MAX && (!ib||(firstIndexByte <= ib->initializedPrefix
    && indexCount * 2 <= uint64_t(ib->initializedPrefix) - firstIndexByte)),
    "DXVK D3D9 draw indices outside initialized IB");
  requireProfile(state.numVertices != 0 && uint64_t(state.minVertexIndex) + state.numVertices <= UINT32_MAX,
    "DXVK D3D9 declared vertex range invalid");
  if(!ib) {
    requireProfile(state.baseVertexIndex==0&&state.minVertexIndex==0&&state.numVertices==indexCount
      &&uint64_t(state.startIndex)+indexCount<=UINT32_MAX
      &&uint64_t(state.streamOffset)+(uint64_t(state.startIndex)+indexCount-1)*state.stride+extent<=vb.initializedPrefix,
      "DXVK D3D9 nonindexed fetch outside initialized VB");
  }
  for (uint64_t i = 0; ib && i < indexCount; ++i) {
    uint16_t index = 0;
    std::memcpy(&index, ib->indexBytes.data() + size_t(firstIndexByte + i * 2), 2);
    requireProfile(index >= state.minVertexIndex && uint64_t(index) < uint64_t(state.minVertexIndex) + state.numVertices,
      "DXVK D3D9 index outside declared min/num vertices");
    const int64_t vertex = int64_t(state.baseVertexIndex) + index;
    requireProfile(vertex >= 0 && uint64_t(state.streamOffset) + uint64_t(vertex) * state.stride + extent <= vb.initializedPrefix,
      "DXVK D3D9 vertex fetch outside initialized VB");
  }
  const auto& vp = state.viewport;
  requireProfile(vp.Width != 0 && vp.Height != 0 && vp.X <= owner.width && vp.Y <= owner.height
    && vp.Width <= owner.width - vp.X && vp.Height <= owner.height - vp.Y
    && std::isfinite(vp.MinZ) && std::isfinite(vp.MaxZ) && vp.MinZ >= 0 && vp.MaxZ <= 1 && vp.MaxZ >= vp.MinZ,
    "DXVK D3D9 viewport outside offscreen target");
  const auto& rs = state.render;
  requireProfile((rs.zEnable == D3DZB_FALSE || rs.zEnable == D3DZB_TRUE) && rs.zWriteEnable <= 1
    && rs.zFunc >= D3DCMP_NEVER && rs.zFunc <= D3DCMP_ALWAYS && rs.alphaBlendEnable <= 1 && rs.colorWriteMask <= 15,
    "DXVK D3D9 render state outside profile");
  requireProfile(rs.cullMode >= D3DCULL_NONE && rs.cullMode <= D3DCULL_CCW
    && rs.srcBlend >= D3DBLEND_ZERO && rs.srcBlend <= D3DBLEND_SRCALPHASAT
    && rs.destBlend >= D3DBLEND_ZERO && rs.destBlend <= D3DBLEND_SRCALPHASAT
    && rs.blendOp >= D3DBLENDOP_ADD && rs.blendOp <= D3DBLENDOP_MAX,
    "DXVK D3D9 cull or blend enum outside profile");
  DxvkRasterizerState raster{};
  raster.setCullMode(cullMode(rs.cullMode));
  raster.setDepthClip(true);
  raster.setFrontFace(VK_FRONT_FACE_CLOCKWISE);
  raster.setPolygonMode(VK_POLYGON_MODE_FILL);
  raster.setFlatShading(false);
  raster.setSampleCount(VK_SAMPLE_COUNT_1_BIT);
  DxvkMultisampleState multisample{};
  multisample.setSampleMask(0xffff);
  multisample.setAlphaToCoverage(false);
  DxvkDepthStencilState depthState{};
  depthState.setDepthTest(rs.zEnable != D3DZB_FALSE);
  depthState.setDepthWrite(rs.zWriteEnable != 0);
  depthState.setDepthCompareOp(static_cast<VkCompareOp>(rs.zFunc - D3DCMP_NEVER));
  depthState.setStencilTest(false);
  depthState.normalize();
  DxvkBlendMode blend{};
  blend.setBlendEnable(rs.alphaBlendEnable != 0);
  blend.setColorOp(blendFactor(rs.srcBlend, false), blendFactor(rs.destBlend, false), blendOp(rs.blendOp));
  blend.setAlphaOp(blendFactor(rs.srcBlend, true), blendFactor(rs.destBlend, true), blendOp(rs.blendOp));
  if(owner.effects.get(D3DRS_SEPARATEALPHABLENDENABLE))
    blend.setAlphaOp(blendFactor(owner.effects.get(D3DRS_SRCBLENDALPHA),true),
      blendFactor(owner.effects.get(D3DRS_DESTBLENDALPHA),true),blendOp(owner.effects.get(D3DRS_BLENDOPALPHA)));
  if(owner.opaque) {
    // Native X8R8G8B8 render targets: destination alpha reads as one.
    const auto opaqueFactor=[](VkBlendFactor factor) {
      if(factor==VK_BLEND_FACTOR_DST_ALPHA)return VK_BLEND_FACTOR_ONE;
      if(factor==VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA)return VK_BLEND_FACTOR_ZERO;
      return factor;
    };
    blend.setColorOp(opaqueFactor(blend.colorSrcFactor()),opaqueFactor(blend.colorDstFactor()),blend.colorBlendOp());
    blend.setAlphaOp(opaqueFactor(blend.alphaSrcFactor()),opaqueFactor(blend.alphaDstFactor()),blend.alphaBlendOp());
  }
  blend.setWriteMask(rs.colorWriteMask);
  blend.normalize();
  // Reject non-finite externally supplied constants before GPU access.
  for (float value : state.vertexFloatConstants)
    requireProfile(std::isfinite(value), "DXVK D3D9 nonfinite vertex constant");
  for (float value : state.pixelFloatConstants)
    requireProfile(std::isfinite(value), "DXVK D3D9 nonfinite pixel constant");
  PreparedDraw result{attributes,extent,uint32_t(indexCount),raster,multisample,depthState,blend,inputs.elemCount,nullInput,{},{}};
  if(fixed) {
    requireProfile(d9::ValidFixedFunction(*fixed),"DXVK D3D9 fixed state outside implemented domain");
    const FixedDeclarationView declaration{decl.elements.data(),decl.count};
    const FixedKeyView facts(*fixed,declaration,textureMask);
    result.fixedVS=BuildFixedFunctionVSKey(facts,false,D3D9FF_VertexBlendMode_Disabled,false);
    result.fixedPS=BuildFixedFunctionFSKey(facts,false);
  }
  return result;
}

void D3D9Backend::recordDraw(const DrawState& state, const PreparedDraw& prepared,
    uint32_t constantSlot, bool batched, const DrawState* previous,const d9::FixedFunctionState* fixed) {
  Owner& owner = *m_owner;
  const auto& vb = owner.at(owner.buffers, state.vertexBuffer);
  const auto* vs = state.vertexShader?&owner.at(owner.vertexShaders,state.vertexShader):nullptr;
  const auto* ps = state.pixelShader?&owner.at(owner.pixelShaders,state.pixelShader):nullptr;
  const auto& vp = state.viewport;
  Rc<DxvkBuffer> vertex = batched ? owner.batchVertexConstants : owner.vertexConstants;
  Rc<DxvkBuffer> pixel = batched ? owner.batchPixelConstants : owner.pixelConstants;
  uint32_t vo = batched ? constantSlot * owner.batchVertexStride : 0;
  uint32_t po = batched ? constantSlot * owner.batchPixelStride : 0;
  if(owner.streamMode) {
    if(vs){vo=owner.ringSlice(owner.streamVertex,m_stats);vertex=owner.streamVertex.buffer;}
    if(ps){po=owner.ringSlice(owner.streamPixel,m_stats);pixel=owner.streamPixel.buffer;}
  }
  {
    WaitTimer timer(m_stats.admissionProfile ? &m_stats.admissionConstantNs : nullptr);
    if(vs)owner.fillConstants(vertex, VertexLayout, state.vertexFloatConstants, *vs, vo);
    if(ps)owner.fillConstants(pixel, PixelLayout, state.pixelFloatConstants, *ps, po);
    if(!vs) {
      auto target=owner.fixedVertexConstants;uint32_t offset=0;
      if(owner.streamMode){offset=owner.ringSlice(owner.streamFixedVertex,m_stats);target=owner.streamFixedVertex.buffer;}
      FillFixedVertex(target->mapPtr(offset),*fixed,prepared.fixedVS,vp,state.render.zEnable!=D3DZB_FALSE);
      owner.context->bindUniformBuffer(VK_SHADER_STAGE_VERTEX_BIT,
        computeResourceSlotId(DxsoProgramType::VertexShader,DxsoBindingType::ConstantBuffer,VSFixedFunction),
        DxvkBufferSlice(target,offset,sizeof(D3D9FixedFunctionVS)));
    }
    if(!ps) {
      auto target=owner.fixedPixelConstants;uint32_t offset=0;
      if(owner.streamMode){offset=owner.ringSlice(owner.streamFixedPixel,m_stats);target=owner.streamFixedPixel.buffer;}
      FillFixedPixel(target->mapPtr(offset),*fixed,prepared.fixedPS);
      owner.context->bindUniformBuffer(VK_SHADER_STAGE_FRAGMENT_BIT,
        computeResourceSlotId(DxsoProgramType::PixelShader,DxsoBindingType::ConstantBuffer,PSFixedFunction),
        DxvkBufferSlice(target,offset,sizeof(D3D9FixedFunctionPS)));
    }
  }
  // Quick UBO and optimized pipeline constants use the same native packing.
  // Changed state gets fresh original DXVK storage, just like native
  // D3D9ConstantBuffer::AllocSlice. Never overwrite an in-flight mapped UBO
  // and never add a GPU wait for a CPU state setter.
  const auto bindBegin=m_stats.admissionProfile?Clock::now():Clock::time_point{};
  D3D9SpecializationInfo spec;
  const auto& effects=owner.effects;
  spec.set<SpecAlphaCompareOp>(effects.get(D3DRS_ALPHATESTENABLE)
    ?effects.get(D3DRS_ALPHAFUNC)-D3DCMP_NEVER:VK_COMPARE_OP_ALWAYS);
  spec.set<SpecAlphaPrecisionBits>(0u); // current target is BGRA8, native 8-bit alpha policy
  const bool fog=effects.get(D3DRS_FOGENABLE)!=0;
  const uint32_t pixelFog=fog?effects.get(D3DRS_FOGTABLEMODE):D3DFOG_NONE;
  spec.set<SpecFogEnabled>(fog);
  spec.set<SpecPixelFogMode>(pixelFog);
  spec.set<SpecVertexFogMode>(fog&&!pixelFog?effects.get(D3DRS_FOGVERTEXMODE):D3DFOG_NONE);
  uint32_t textureMask=state.texture?1u:0u;
  for(uint32_t i=0;i<state.additionalTextures.size();++i)
    if(state.additionalTextures[i])textureMask|=1u<<(i+1);
  spec.set<SpecSamplerNull>(0xffffu^textureMask);
  if(!ps)UpdateFixedFunctionSpecialization(spec,prepared.fixedPS,
      fixed->render[d9::FFIndex(d9::FFRenderStates,D3DRS_SPECULARENABLE)]);
  if(spec.data!=owner.specialization.data) {
    auto storage=owner.specConstants->allocateStorage();
    std::memcpy(storage->mapPtr(),spec.data.data(),spec.UBOSize);
    owner.context->invalidateBuffer(owner.specConstants,std::move(storage));
    for(uint32_t i=0;i<spec.data.size();++i)
      owner.context->setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS,i,spec.data[i]);
    owner.specialization=spec;
  }
  D3D9RenderStateInfo renderInfo{};
  const auto fogColor=FixedColor(effects.get(D3DRS_FOGCOLOR));
  std::copy_n(fogColor.data,3,renderInfo.fogColor.begin());
  const float fogStart=d9::Float(effects.get(D3DRS_FOGSTART));
  renderInfo.fogEnd=d9::Float(effects.get(D3DRS_FOGEND));
  renderInfo.fogScale=1.0f/(renderInfo.fogEnd-fogStart);
  renderInfo.fogDensity=d9::Float(effects.get(D3DRS_FOGDENSITY));
  renderInfo.alphaRef=effects.get(D3DRS_ALPHAREF)&0xff;
  owner.context->pushData(ShaderStages,0,sizeof(renderInfo),&renderInfo);
  owner.context->bindUniformBuffer(VK_SHADER_STAGE_VERTEX_BIT,
    computeResourceSlotId(DxsoProgramType::VertexShader, DxsoBindingType::ConstantBuffer, VSConstantBuffer),
    DxvkBufferSlice(vertex, vo, VertexLayout.totalSize()));
  owner.context->bindUniformBuffer(VK_SHADER_STAGE_FRAGMENT_BIT,
    computeResourceSlotId(DxsoProgramType::PixelShader, DxsoBindingType::ConstantBuffer, PSConstantBuffer),
    DxvkBufferSlice(pixel, po, PixelLayout.totalSize()));
  const auto update=BatchStateChanges(state,m_stats.batchStateReuse?previous:nullptr);
  m_stats.redundantStateBindingsSkipped+=unsigned(!update.targets)+unsigned(!update.layout)
    +unsigned(!update.assembly)+unsigned(!update.raster)+unsigned(!update.multisample)
    +unsigned(!update.depth)+unsigned(!update.blend);
  if(update.targets)owner.bindTargets();
  owner.context->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(vs?Rc<DxvkShader>(vs->object):owner.fixedVertexShader->GetShader());
  owner.context->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(ps?Rc<DxvkShader>(ps->object):owner.fixedPixelShader->GetShader());
  {
    WaitTimer timer(m_stats.admissionProfile ? &m_stats.admissionSamplerNs : nullptr);
    for(uint32_t i=0;i<d9::PixelTextureStages;++i) {
      const auto id=i?state.additionalTextures[i-1]:state.texture;
      const auto* texture=id?&owner.at(owner.textures,id):nullptr;
      const auto& values=i?owner.additionalSamplers[i-1].values:owner.samplerState.values;
      auto& sampler=owner.samplers[i];
      if(texture||!i) {
        const auto key=BuildD3D9SamplerKey(D3D9SamplerInfo(values),false,
          texture&&texture->layout.levels>1,false,*owner.d3d9Options);
        if(sampler==nullptr||!sampler->key().eq(key))sampler=owner.device->createSampler(key);
      }
      owner.context->bindResourceSampler(VK_SHADER_STAGE_FRAGMENT_BIT,PixelSamplerSlot+i,Rc<DxvkSampler>(sampler));
      const bool srgb=values[D3DSAMP_SRGBTEXTURE]!=0;
      owner.context->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT,PixelSamplerSlot+i,
        texture?Rc<DxvkImageView>(srgb&&texture->srgbView!=nullptr?texture->srgbView:texture->view):Rc<DxvkImageView>());
    }
  }
  owner.context->bindVertexBuffer(0, DxvkBufferSlice(vb.object, state.streamOffset, vb.size - state.streamOffset), state.stride);
  if(state.indexBuffer)owner.context->bindIndexBuffer(DxvkBufferSlice(owner.at(owner.buffers,state.indexBuffer).object), VK_INDEX_TYPE_UINT16);
  DxvkVertexBinding vertexBinding{0, prepared.extent, VK_VERTEX_INPUT_RATE_VERTEX, 0};
  DxvkVertexInput bindings[2]={DxvkVertexInput(vertexBinding),DxvkVertexInput(DxvkVertexBinding{1,16,VK_VERTEX_INPUT_RATE_VERTEX,0})};
  if(prepared.nullInput)owner.context->bindVertexBuffer(1,DxvkBufferSlice(owner.nullVertex),0);
  if(update.layout)owner.context->setInputLayout(prepared.inputCount, prepared.attributes.data(),prepared.nullInput?2:1,bindings);
  if(update.assembly)owner.context->setInputAssemblyState(DxvkInputAssemblyState(topology(state.primitiveType), false));
  if(update.raster)owner.context->setRasterizerState(prepared.raster);
  // Native BindDepthBias; DXVK enables the bias only when it is nonzero.
  DxvkDepthBias depthBias{};
  depthBias.depthBiasConstant=d9::Float(effects.get(D3DRS_DEPTHBIAS))*owner.depthBiasScale;
  owner.context->setDepthBiasRepresentation(owner.depthBiasRepresentation);
  owner.context->setDepthBias(depthBias);
  if(update.multisample)owner.context->setMultisampleState(prepared.multisample);
  if(update.depth)owner.context->setDepthStencilState(prepared.depth);
  if(update.blend)owner.context->setBlendMode(0, prepared.blend);
  DxvkViewport viewport{};
  const float zBias = vp.MinZ >= 0.5f ? 0 : 0.001f;
  viewport.viewport = {float(vp.X) + 0.5f, float(vp.Y + vp.Height) + 0.5f, float(vp.Width), -float(vp.Height),
    vp.MinZ, std::min(1.0f, std::max(vp.MaxZ, vp.MinZ + zBias))};
  viewport.scissor = {{int32_t(vp.X), int32_t(vp.Y)}, {vp.Width, vp.Height}};
  if(effects.get(D3DRS_SCISSORTESTENABLE)) {
    const auto& r=effects.scissor;
    const auto x=std::clamp<int64_t>(r[0],vp.X,uint64_t(vp.X)+vp.Width);
    const auto y=std::clamp<int64_t>(r[1],vp.Y,uint64_t(vp.Y)+vp.Height);
    const auto right=std::clamp<int64_t>(r[2],x,uint64_t(vp.X)+vp.Width);
    const auto bottom=std::clamp<int64_t>(r[3],y,uint64_t(vp.Y)+vp.Height);
    viewport.scissor={{int32_t(x),int32_t(y)},{uint32_t(right-x),uint32_t(bottom-y)}};
  }
  owner.context->setViewports(1, &viewport);
  if(m_stats.admissionProfile)
    m_stats.admissionBindNs+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-bindBegin).count());
  // The DXVK draw commits dirty descriptors, pipeline and dynamic state.
  WaitTimer drawTimer(m_stats.admissionProfile ? &m_stats.admissionDrawNs : nullptr);
  if(state.indexBuffer) {
    const VkDrawIndexedIndirectCommand command = {prepared.indexCount, 1, state.startIndex, state.baseVertexIndex, 0};
    owner.context->drawIndexed(1, &command);
  } else {
    const VkDrawIndirectCommand command={prepared.indexCount,1,state.startIndex,0};
    owner.context->draw(1,&command);
  }
}

void D3D9Backend::draw(const DrawState& state) {
  drawBatch(&state, 1);
}

void D3D9Backend::drawFixed(const DrawState& state,const d9::FixedFunctionState& fixed) {
  drawFixedBatch(&state,&fixed,1);
}

void D3D9Backend::drawFixedBatch(const DrawState* states,const d9::FixedFunctionState* fixed,uint32_t count) {
  try {
    Owner& owner=active();
    requireProfile(states&&fixed&&count&&count<=MaxFixedDrawBatch,"DXVK D3D9 invalid fixed batch extent");
    std::array<PreparedDraw,MaxFixedDrawBatch> prepared{};
    // As drawBatch: every entry passes before any shader creation, recording,
    // ring slice or counter. A bad final draw leaves no rendered prefix.
    {
      WaitTimer timer(m_stats.admissionProfile ? &m_stats.admissionPrepareNs : nullptr);
      for(uint32_t i=0;i<count;++i) {
        prepared[i]=prepareDraw(states[i],&fixed[i]);
        requireProfile(!states[i].vertexShader||!states[i].pixelShader,"fixed draw needs a fixed stage");
      }
    }
    WaitTimer recordTimer(m_stats.admissionProfile ? &m_stats.admissionRecordNs : nullptr);
    canAdd(m_stats.draws,count);
    canAdd(m_stats.drawBatches,1);
    if(count>1)canAdd(m_stats.batchedDraws,count);
    if(owner.streamMode){canAdd(m_stats.streamDraws,count);canAdd(m_stats.streamFixedDraws,count);}
    try {
      if(owner.streamMode) {
        for(uint32_t i=0;i<count;++i) {
          if(!states[i].vertexShader&&!owner.fixedVertexShader)
            owner.fixedVertexShader=std::make_unique<D3D9FFShader>(owner.device,DxsoProgramType::VertexShader);
          if(!states[i].pixelShader&&!owner.fixedPixelShader)
            owner.fixedPixelShader=std::make_unique<D3D9FFShader>(owner.device,DxsoProgramType::PixelShader);
        }
        if(owner.streamListDraws>MaxStreamDraws-count){owner.flushStream(m_stats);++m_stats.streamCapSubmissions;}
        owner.beginStream();
        // Each draw takes its own FF ring slices, so the whole batch shares one list.
        for(uint32_t i=0;i<count;++i)recordDraw(states[i],prepared[i],0,false,i?&states[i-1]:nullptr,&fixed[i]);
        owner.streamListDraws+=count;m_stats.streamDraws+=count;m_stats.streamFixedDraws+=count;
      } else for(uint32_t i=0;i<count;++i) {
        // The single coherent FF UBO is rewritten per draw: keep one GPU
        // drain per draw exactly as the unbatched path, only the IPC is shared.
        owner.settle(m_stats);
        if(!states[i].vertexShader&&!owner.fixedVertexShader) {
          owner.fixedVertexShader=std::make_unique<D3D9FFShader>(owner.device,DxsoProgramType::VertexShader);
          owner.fixedVertexConstants=owner.coherentBuffer(sizeof(D3D9FixedFunctionVS),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_UNIFORM_READ_BIT,"x64-d3d9-fixed-vs");
        }
        if(!states[i].pixelShader&&!owner.fixedPixelShader) {
          owner.fixedPixelShader=std::make_unique<D3D9FFShader>(owner.device,DxsoProgramType::PixelShader);
          owner.fixedPixelConstants=owner.coherentBuffer(sizeof(D3D9FixedFunctionPS),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_UNIFORM_READ_BIT,"x64-d3d9-fixed-ps");
        }
        owner.begin(m_stats);recordDraw(states[i],prepared[i],0,false,nullptr,&fixed[i]);owner.submit(m_stats,true);
      }
      m_stats.draws+=count;++m_stats.drawBatches;
      if(count>1)m_stats.batchedDraws+=count;
      for(uint32_t i=0;i<count;++i)if(!states[i].indexBuffer)++m_stats.nonIndexedDraws;
      m_stats.largestDrawBatch=std::max(m_stats.largestDrawBatch,count);
    } catch(...) {quarantine();throw;}
  } catch(const ProfileRejected&) {throw;}
  catch(...) {quarantine();throw;}
}

void D3D9Backend::drawBatch(const DrawState* states, uint32_t count) {
  try {
    Owner& owner = active();
    requireProfile(states && count && count <= MaxDrawBatch, "DXVK D3D9 invalid batch extent");
    std::array<PreparedDraw, MaxDrawBatch> prepared{};
    // No command recording, buffer allocation/writes or counters until ALL
    // entries pass. A bad final draw cannot leave a rendered prefix behind.
    {
      WaitTimer timer(m_stats.admissionProfile ? &m_stats.admissionPrepareNs : nullptr);
      for (uint32_t i = 0; i < count; ++i) prepared[i] = prepareDraw(states[i]);
    }
    WaitTimer recordTimer(m_stats.admissionProfile ? &m_stats.admissionRecordNs : nullptr);
    canAdd(m_stats.draws, count);
    canAdd(m_stats.drawBatches, 1);
    if (count > 1) canAdd(m_stats.batchedDraws, count);
    if (owner.streamMode) canAdd(m_stats.streamDraws, count);
    try {
      if (owner.streamMode) {
        if (owner.streamListDraws > MaxStreamDraws - count) {
          owner.flushStream(m_stats);
          ++m_stats.streamCapSubmissions;
        }
        owner.beginStream();
        for (uint32_t i = 0; i < count; ++i) recordDraw(states[i], prepared[i], 0, false, i?&states[i-1]:nullptr);
        owner.streamListDraws += count;
        m_stats.streamDraws += count;
      } else if (owner.flightMode) {
        auto& flight = owner.beginDrawFlight(m_stats);
        for (uint32_t i = 0; i < count; ++i) recordDraw(states[i], prepared[i], i, true,i?&states[i-1]:nullptr);
        owner.submitDrawFlight(m_stats, flight);
      } else {
        owner.settle(m_stats);
        if (count > 1 && owner.batchVertexConstants == nullptr) {
          const auto uniform = [&](uint32_t bytes, const char* name) {
            return owner.coherentBuffer(bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ShaderStages,
              VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT, name);
          };
          owner.batchVertexConstants = uniform(owner.batchVertexStride * MaxDrawBatch, "x64-d3d9-batch-vs");
          owner.batchPixelConstants = uniform(owner.batchPixelStride * MaxDrawBatch, "x64-d3d9-batch-ps");
          m_stats.batchConstantBytes = uint64_t(owner.batchVertexStride + owner.batchPixelStride) * MaxDrawBatch;
        }
        owner.begin(m_stats);
        for (uint32_t i = 0; i < count; ++i) recordDraw(states[i], prepared[i], i, count > 1,i?&states[i-1]:nullptr);
        owner.submit(m_stats, false);
      }
      m_stats.draws += count;
      for(uint32_t i=0;i<count;++i)if(!states[i].indexBuffer)++m_stats.nonIndexedDraws;
      ++m_stats.drawBatches;
      if (count > 1) m_stats.batchedDraws += count;
      m_stats.largestDrawBatch = std::max(m_stats.largestDrawBatch, count);
    } catch (...) { quarantine(); throw; }
  } catch (const ProfileRejected&) { throw; }
    catch (...) { quarantine(); throw; }
}

void D3D9Backend::flushDraws() {
  try {
    auto& owner=active();
    owner.flushRecordedGroup(m_stats);
    owner.flushStream(m_stats);
  } catch(...) { quarantine();throw; }
}
bool D3D9Backend::hasPendingDraws() const {
  return m_owner && m_owner->recording();
}

std::vector<uint8_t> D3D9Backend::readback(uint32_t first, uint32_t rows) {
  try {
    Owner& owner = active();
    require(m_stats.initialized, "DXVK D3D9 is not initialized");
    require(rows != 0 && first < owner.height && rows <= owner.height - first
      && uint64_t(owner.width) * rows * 4 <= ReadbackBandBytes, "DXVK D3D9 readback band outside target");
    owner.begin(m_stats);
    owner.context->copyImageToBuffer(owner.readback, 0, 0, 0, VK_FORMAT_B8G8R8A8_UNORM,
      owner.color, ColorLayer, {0, int32_t(first), 0}, {owner.width, rows, 1});
    owner.submit(m_stats, true);
    const auto* bytes = static_cast<const uint8_t*>(owner.readback->mapPtr(0));
    require(bytes != nullptr, "DXVK D3D9 readback mapping unavailable");
    std::vector<uint8_t> result(bytes, bytes + size_t(owner.width) * rows * 4);
    ++m_stats.readbacks;
    return result;
  } catch (...) { quarantine(); throw; }
}

std::vector<uint8_t> D3D9Backend::readPresented(uint32_t first, uint32_t rows) {
  try {
    Owner& owner = active();
    requireProfile(m_stats.initialized && owner.diagnosticPresentedReadback,
      "DXVK D3D9 presented readback is not enabled");
    requireProfile(m_stats.presentedFrame != 0 && owner.presentedCaptured,
      "DXVK D3D9 no successful diagnostic Present has been captured at this extent");
    requireProfile(rows != 0 && first < owner.height && rows <= owner.height - first,
      "DXVK D3D9 presented readback rows outside target");
    require(owner.presentedBytes.size() == owner.presentedSize()
      && m_stats.presentedFrame == owner.lastPresentFrame
      && m_stats.presentedFrame == m_stats.presentGpuCompleted
      && m_stats.presentedFrame == m_stats.presentRetired,
      "DXVK D3D9 presented readback has incomplete frame ownership");
    owner.healthy();
    if (!first) canAdd(m_stats.presentedReadbacks, 1);
    const size_t pitch = size_t(owner.width) * 4;
    std::vector<uint8_t> result(owner.presentedBytes.begin() + first * pitch,
      owner.presentedBytes.begin() + (size_t(first) + rows) * pitch);
    if (!first) ++m_stats.presentedReadbacks;
    return result;
  } catch (const ProfileRejected&) { throw; }
    catch (...) { quarantine(); throw; }
}

uint64_t D3D9Backend::issueEvent() {
  try {
    Owner& owner = active();
    require(m_stats.initialized, "DXVK D3D9 is not initialized");
    owner.flushRecordedGroup(m_stats);
    owner.flushStream(m_stats);
    // Each prior command has its own production timeline signal. An event is
    // a stream milestone, not the packet's CPU acknowledgement.
    require(owner.lastSerial != 0, "DXVK D3D9 event has no submitted timeline");
    owner.lastEvent = owner.lastSerial;
    ++m_stats.queries;
    if (m_stats.reuseWaitProfile) {
      if (m_stats.reuseWaitSampleCount < m_stats.reuseWaitSamples.size()) {
        auto& sample = m_stats.reuseWaitSamples[m_stats.reuseWaitSampleCount++];
        sample = { m_stats.queries, m_stats.draws, m_stats.reuseWait };
      } else {
        ++m_stats.reuseWaitSamplesDropped;
      }
    }
    return owner.lastEvent;
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::waitEvent(uint64_t serial) {
  try {
    Owner& owner = active();
    require(m_stats.initialized && serial != 0 && serial <= owner.lastEvent,
      "DXVK D3D9 wait event serial was not issued");
    owner.healthy();
    const VkSemaphore semaphore = owner.completion->handle();
    VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wait.semaphoreCount = 1; wait.pSemaphores = &semaphore; wait.pValues = &serial;
    const auto result = owner.device->vkd()->vkWaitSemaphores(owner.device->handle(), &wait, GpuWaitNs);
    if (result != VK_SUCCESS) {
      owner.device->notifyDeviceErrorFromDriverResult(result);
      throw std::runtime_error("DXVK D3D9 event completion wait failed");
    }
    // The original GPU semaphore, not CPU admission, proves the event. Poll
    // may retire resources only under its existing, separate CPU/use checks.
    require(owner.pollTimeline(m_stats) >= serial, "DXVK D3D9 completed event regressed");
  } catch (...) { quarantine(); throw; }
}

bool D3D9Backend::eventComplete(uint64_t serial) {
  try {
    Owner& owner = active();
    require(m_stats.initialized && serial != 0 && serial <= owner.lastEvent,
      "DXVK D3D9 event serial was not issued");
    owner.healthy();
    return owner.pollTimeline(m_stats) >= serial;
  } catch (...) { quarantine(); throw; }
}

HRESULT D3D9Backend::present() noexcept {
  try {
    Owner& owner = active(Admission::WindowFaultCleanup);
    require(m_stats.initialized, "DXVK D3D9 is not initialized");
    canAdd(m_stats.presentAttempts, 1);
    ++m_stats.presentAttempts;
    if (owner.presenter == nullptr)
      return E_NOTIMPL;

    bool resized = false;
    try {
      owner.validateWindow(&resized);
    } catch (...) {
      m_stats.windowPreflightFailed = true;
      if (!owner.canCleanWindowFault(m_stats))
        throw;
      const auto cleanupStart = Clock::now();
      const uint64_t cleanupDeadline = GetTickCount64() + 5000;
      std::fprintf(stderr, "D3D9 window preflight: quiescent GPU cleanup begin\n");
      std::fflush(stderr);
      owner.shutdown(m_stats, cleanupDeadline, true);
      m_owner.reset();
      m_stats.faultCleanupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - cleanupStart).count();
      m_stats.faultCleanupCompleted = true;
      // normalTeardown remains false; this device can never be used again.
      return D3DERR_DEVICELOST;
    }
    if (resized) {
      // Nothing acquired or recorded: the frame is dropped until Reset.
      canAdd(m_stats.presentsSkipped, 1);
      ++m_stats.presentsSkipped;
      return HRESULT(d9::PresentSkipped);
    }
    owner.healthy();
    owner.settle(m_stats);
    owner.waitPresentRetired(m_stats);
    require(owner.acquiredImage == nullptr && owner.acquiredView == nullptr,
      "DXVK D3D9 WSI image is still owned by an earlier present");
    canAdd(owner.lastPresentFrame, 1);
    canAdd(m_stats.acquiredImages, 1);
    canAdd(m_stats.presentQueued, 1);
    canAdd(m_stats.presentApiSucceeded, 1);
    const uint64_t frame = owner.lastPresentFrame + 1;

    PresenterSync sync{};
    // Production Presenter can wait inside the driver while acquiring. The
    // external watchdog bounds that wait; do not claim this call is cancellable.
    const VkResult acquired = owner.presenter->acquireNextImage(sync, owner.acquiredImage);
    m_stats.lastAcquireVkResult = static_cast<int32_t>(acquired);
    require(acquired == VK_SUCCESS && owner.acquiredImage != nullptr,
      "DXVK D3D9 WSI acquire was not strictly successful");
    ++m_stats.acquiredImages;
    const auto& imageInfo = owner.acquiredImage->info();
    require(imageInfo.extent.width == owner.width && imageInfo.extent.height == owner.height
      && imageInfo.extent.depth == 1 && imageInfo.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      "DXVK D3D9 WSI acquired image differs from the finite profile");
    require(sync.acquire != VK_NULL_HANDLE && sync.present != VK_NULL_HANDLE,
      "DXVK D3D9 WSI synchronization is incomplete");
    owner.acquiredView = owner.acquiredImage->createView(viewKey(imageInfo.format,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));

    owner.begin(m_stats);
    // A CPU deferred Clear is not made real by a submission/timeline alone.
    // The blitter records raw commands outside DxvkContext, so explicitly
    // materialize/synchronize this exact source before handing it over. Merely
    // setting shared=true misses clears on images already in default layout.
    owner.context->releaseExternalResource(owner.color, owner.color->info().layout);
    const auto external = owner.context->beginExternalRendering();
    const VkRect2D rect = {{0, 0}, {owner.width, owner.height}};
    owner.blitter->present(external, owner.acquiredView, rect, owner.colorSampleView, rect);
    if (owner.diagnosticPresentedReadback)
      owner.copyAcquiredImage(external);
    // The raw blit only reads the source without changing its layout. Return
    // its access dependency to the original context before later draw/clear;
    // command-list lifetime tracking alone is not that dependency.
    owner.context->acquireExternalResource(owner.color, owner.color->info().layout);
    owner.context->synchronizeWsi(sync);
    owner.submit(m_stats, false);

    // A distinct owner-resident status covers present, separate from command
    // submission/timeline completion. Queue order is the production DXVK order.
    owner.presentSubmission.result.store(VK_NOT_READY, std::memory_order_release);
    m_stats.lastPresentVkResult = static_cast<int32_t>(VK_NOT_READY);
    owner.lastPresentFrame = frame;
    owner.device->presentImage(owner.presenter, nullptr, frame, &owner.presentSubmission);
    ++m_stats.presentQueued;
    const auto deadline = Clock::now() + CpuWait;
    for (;;) {
      const VkResult result = owner.presentSubmission.result.load(std::memory_order_acquire);
      if (result != VK_NOT_READY) {
        m_stats.lastPresentVkResult = static_cast<int32_t>(result);
        require(result == VK_SUCCESS, "DXVK D3D9 WSI present API was not strictly successful");
        break;
      }
      owner.healthy();
      require(Clock::now() < deadline, "DXVK D3D9 WSI present result did not settle in 5 seconds");
      Sleep(1);
    }
    ++m_stats.presentApiSucceeded;
    owner.settle(m_stats);
    m_stats.presentGpuCompleted = frame;
    owner.waitPresentRetired(m_stats);
    // A resize after the queued present only affects the next frame.
    bool resizedAfter = false;
    owner.validateWindow(&resizedAfter);
    owner.healthy();
    if (owner.diagnosticPresentedReadback) {
      const auto* bytes = static_cast<const uint8_t*>(owner.presentedReadback->mapPtr(0));
      require(bytes != nullptr && owner.presentedBytes.size() == owner.presentedSize(),
        "DXVK D3D9 presented readback mapping unavailable");
      // Only publish after successful Present plus the existing GPU/resource/
      // frame checks. The bytes were copied BEFORE Present; no swapchain read
      // occurs after handing the image to WSI. A later failure quarantines the
      // owner, so neither this snapshot nor an old frame can then be returned.
      std::memcpy(owner.presentedBytes.data(), bytes, owner.presentedSize());
      m_stats.presentedFrame = frame;
      owner.presentedCaptured = true;
    }
    owner.acquiredView = nullptr;
    owner.acquiredImage = nullptr;
    return D3D_OK;
  } catch (...) {
    // Never release a queue-owned status, acquired swapchain image, blitter or
    // Presenter after uncertain submission/driver state. No fake success.
    quarantine();
    return D3DERR_DEVICELOST;
  }
}

void D3D9Backend::destroy(ResourceType type, uint32_t id) {
  try {
    Owner& owner = active();
    require(m_stats.initialized, "DXVK D3D9 is not initialized");
    owner.settle(m_stats);
    owner.waitPresentRetired(m_stats);
    // Remove context-held references as well as the author's typed slot. The
    // next draw supplies a complete state packet and rebinds its own resources.
    owner.unbindAuthorResources();
    switch (type) {
      case ResourceType::VertexBuffer:
      case ResourceType::IndexBuffer: {
        auto& slot = owner.liveBuffer(id);
        require(slot.index16 == (type == ResourceType::IndexBuffer), "DXVK D3D9 destroy buffer type mismatch");
        removeResource(m_stats, slot.size);
        slot = Owner::Buffer{};
      } break;
      case ResourceType::Texture: {
        auto& slot = owner.at(owner.textures, id);
        require(slot.image != nullptr, "DXVK D3D9 destroy texture is not live");
        removeResource(m_stats,slot.layout.bytes);
        slot = Owner::Texture{};
      } break;
      case ResourceType::VertexShader:
      case ResourceType::PixelShader: {
        auto& slot = type == ResourceType::VertexShader ? owner.at(owner.vertexShaders, id) : owner.at(owner.pixelShaders, id);
        require(slot.object != nullptr, "DXVK D3D9 destroy shader is not live");
        removeResource(m_stats, slot.tokenBytes);
        slot = Owner::Shader{};
      } break;
      case ResourceType::Declaration: {
        auto& slot = owner.at(owner.declarations, id);
        require(slot.count != 0, "DXVK D3D9 destroy declaration is not live");
        removeResource(m_stats, uint64_t(slot.count) * sizeof(D3DVERTEXELEMENT9));
        slot = Owner::Declaration{};
      } break;
      default: throw std::runtime_error("DXVK D3D9 destroy unknown resource type");
    }
  } catch (...) { quarantine(); throw; }
}

void D3D9Backend::finish() {
  const uint64_t finishDeadline = GetTickCount64() + 5000;
  try {
    Owner& owner = active();
    if (owner.surfaceTarget)
      owner.validateWindow(); // A faulted owned HWND is never normalTeardown.
    require(m_stats.initialized && m_stats.liveResources == 0 && m_stats.liveLogicalBytes == 0,
      "DXVK D3D9 finish requires all author resources destroyed");
    owner.settle(m_stats);
    owner.waitPresentRetired(m_stats);
    owner.healthy();
    require(m_stats.submitted == m_stats.gpuCompleted && m_stats.gpuCompleted == m_stats.retired,
      "DXVK D3D9 finish has unfinished GPU/resource milestones");
    require(m_stats.presentQueued == m_stats.presentApiSucceeded
      && m_stats.presentApiSucceeded == m_stats.presentGpuCompleted
      && m_stats.presentGpuCompleted == m_stats.presentRetired,
      "DXVK D3D9 finish has unfinished WSI milestones");
    owner.shutdown(m_stats, finishDeadline);
    m_owner.reset();
    m_stats.normalTeardown = true;
  } catch (...) { quarantine(); throw; }
}

D3D9Backend::ShaderCacheStats D3D9Backend::diagnosticShaderCache() const {
  require(m_owner != nullptr && m_owner->thread == GetCurrentThreadId()
    && m_stats.initialized && m_owner->shaderModules != nullptr,
    "DXVK D3D9 shader cache diagnostic outside live owner");
  const auto source = m_owner->shaderModules->stats();
  ShaderCacheStats result;
  result.requests = source.requests;
  result.hits = source.hits;
  result.compileAttempts = source.compileAttempts;
  result.compilesSucceeded = source.compilesSucceeded;
  result.registrations = source.registrations;
  result.raceLosers = source.raceLosers;
  result.validationFailures = source.validationFailures;
  result.registrationFailures = source.registrationFailures;
  result.readyEntries = source.readyEntries;
  result.failedEntries = source.failedEntries;
  result.keyBytes = source.keyBytes;
  result.keyCapacityBytes = source.keyCapacityBytes;
  result.constantBytes = source.constantBytes;
  result.constantCapacityBytes = source.constantCapacityBytes;
  for (size_t i = 0; i < ShaderSlots; ++i) {
    result.vertexCookies[i] = DxvkShader::getCookie(m_owner->vertexShaders[i].object);
    result.pixelCookies[i] = DxvkShader::getCookie(m_owner->pixelShaders[i].object);
  }
  return result;
}

d9::memory::HostSample D3D9Backend::diagnosticMemory() const {
  using namespace d9::memory;
  require(m_owner != nullptr && m_owner->thread == GetCurrentThreadId()
    && m_stats.initialized, "DXVK D3D9 diagnostic outside live owner");
  const auto& owner=*m_owner;HostSample result;
  result.logicalResources=m_stats.liveResources;result.logicalBytes=m_stats.liveLogicalBytes;
  result.submitted=m_stats.submitted;result.completed=m_stats.gpuCompleted;result.retired=m_stats.retired;
  result.draws=m_stats.draws;result.uploads=m_stats.uploads;result.readbacks=m_stats.readbacks;
  result.ownerInlineBytes=sizeof(Owner);
  // Enumerate ALL storage slots. Filtering by object!=nullptr would conceal a
  // faulty destroy that cleared logical identity but retained vector capacity.
  for(const auto& b:owner.buffers) {
    const bool live=b.object!=nullptr;
    result.cpu.observe(b.index16?Kind::IndexBuffer:Kind::VertexBuffer,live,
      live?b.size:0,b.indexBytes.size(),b.indexBytes.capacity());
  }
  for(const auto& t:owner.textures)if(t.image!=nullptr)
    result.cpu.add(Kind::Texture,t.layout.bytes,0,0);
  const auto shaders=[&](const auto& slots,Kind kind){for(const auto& s:slots) {
    const bool live=s.object!=nullptr;
    result.cpu.observe(kind,live,live?s.tokenBytes:0,s.constants.size()*sizeof(DxsoDefinedConstant),
      s.constants.capacity()*sizeof(DxsoDefinedConstant));}};
  shaders(owner.vertexShaders,Kind::VertexShader);shaders(owner.pixelShaders,Kind::PixelShader);
  for(const auto& d:owner.declarations)if(d.count)
    result.cpu.add(Kind::Declaration,uint64_t(d.count)*sizeof(D3DVERTEXELEMENT9),0,0);
  const bool presentedLive=owner.diagnosticPresentedReadback;
  result.cpu.observe(Kind::PresentedReadback,presentedLive,presentedLive?owner.presentedBytes.size():0,
    owner.presentedBytes.size(),owner.presentedBytes.capacity());
  if(presentedLive&&(owner.presentedReadback==nullptr||owner.presentedBytes.size()!=owner.presentedSize()))
    result.cpu.valid=false;
  return result;
}

} // namespace warvk::host::dxvkcore
