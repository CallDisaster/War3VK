#include "gpu_render_backend.h"

#ifndef _WIN32
#error RH2 GPU laboratory requires Windows external NT handles.
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace warvk::host::gpu {
namespace {
constexpr uint64_t FenceTimeoutNs = 5'000'000'000ull;
constexpr VkFormat ImageFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags ImageUsage =
  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr auto MemoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr auto SemaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#if defined(RH2_GPU_VALIDATION) && RH2_GPU_VALIDATION
constexpr bool RequireValidation = true;
#else
constexpr bool RequireValidation = false;
#endif
std::atomic<uint64_t> validationInstances{0}, validationErrors{0}, validationWarnings{0};

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error("RH2 GPU: " + message);
}
void checkGlobal(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    fail(std::string(operation) + " VkResult=" + std::to_string(result));
}
uint64_t checkedImageBytes(uint32_t width, uint32_t height) {
  // Fixed lab limit, not a new shipping memory budget. Bounds the x86 oracle.
  if (!width || !height || width > 4096 || height > 4096)
    fail("image extent outside bounded lab range 1..4096");
  return uint64_t(width) * height * 4;
}
bool nonzero(const uint8_t* bytes, size_t size) {
  for (size_t i = 0; i < size; ++i)
    if (bytes[i]) return true;
  return false;
}

struct NtHandle {
  HANDLE value = nullptr;
  NtHandle() = default;
  ~NtHandle() { if (value) CloseHandle(value); }
  NtHandle(const NtHandle&) = delete;
  NtHandle& operator=(const NtHandle&) = delete;
  void duplicateBorrowed(uint64_t source) {
    if (!source || source > std::numeric_limits<uintptr_t>::max() ||
        source == uint64_t(std::numeric_limits<uintptr_t>::max()))
      fail("invalid/local-pointer-width NT handle");
    HANDLE borrowed = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(source));
    if (!DuplicateHandle(GetCurrentProcess(), borrowed, GetCurrentProcess(),
                         &value, 0, FALSE, DUPLICATE_SAME_ACCESS))
      fail("DuplicateHandle(local import) Win32=" + std::to_string(GetLastError()));
  }
  uint64_t integer() const { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)); }
};

#define RH2_INSTANCE_FUNCTIONS(X) \
  X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties2) \
  X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
  X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceImageFormatProperties2) \
  X(GetPhysicalDeviceExternalSemaphoreProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define RH2_DEVICE_FUNCTIONS(X) \
  X(DestroyDevice) X(GetDeviceQueue) X(CreateCommandPool) X(DestroyCommandPool) \
  X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
  X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(QueueSubmit) \
  X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) \
  X(FreeMemory) X(BindImageMemory) X(GetMemoryWin32HandleKHR) \
  X(CreateSemaphore) X(DestroySemaphore) X(GetSemaphoreWin32HandleKHR) \
  X(ImportSemaphoreWin32HandleKHR) X(CreateBuffer) X(DestroyBuffer) \
  X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
  X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(CmdCopyBuffer) \
  X(CmdPipelineBarrier) X(CmdCopyImageToBuffer) X(CreateImageView) X(DestroyImageView) \
  X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
  X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineLayout) \
  X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline) \
  X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) \
  X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdPushConstants) X(CmdDrawIndexed)

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize allocationBytes = 0;
  bool coherent = false;
};

struct Context {
  HMODULE loader = nullptr;
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessengerEXT = nullptr;
  PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT = nullptr;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool inFlight = false, quarantined = false, fault = false;
  BackendStats counters;
#define RH2_DECLARE(name) PFN_vk##name name = nullptr;
  RH2_INSTANCE_FUNCTIONS(RH2_DECLARE)
  RH2_DEVICE_FUNCTIONS(RH2_DECLARE)
#undef RH2_DECLARE

  ~Context() {
    // Unknown completion cannot be converted to successful retirement. The
    // lab's process watchdog is the containment boundary for a wedged driver.
    if (inFlight || quarantined) return;
    if (device) {
      if (fence && DestroyFence) DestroyFence(device, fence, nullptr);
      if (commandPool && DestroyCommandPool) DestroyCommandPool(device, commandPool, nullptr);
      if (DestroyDevice) DestroyDevice(device, nullptr);
    }
    if (debugMessenger && DestroyDebugUtilsMessengerEXT)
      DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    if (instance && DestroyInstance) DestroyInstance(instance, nullptr);
    if (loader) FreeLibrary(loader);
  }
  void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
      fault = true;
      fail(std::string(operation) + " VkResult=" + std::to_string(result));
    }
  }
  void usable() const {
    if (fault || quarantined || inFlight) fail("faulted or unretired GPU context");
  }
  template<typename T> void loadInstance(T& destination, const char* name) {
    destination = reinterpret_cast<T>(getInstanceProcAddr(instance, name));
    if (!destination) fail(std::string("missing instance entry ") + name);
  }
  template<typename T> void loadDevice(T& destination, const char* name) {
    destination = reinterpret_cast<T>(GetDeviceProcAddr(device, name));
    if (!destination) fail(std::string("missing device entry ") + name);
  }
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT*, void*) noexcept {
    // No pointer back to Context: a quarantined driver instance can outlive its
    // C++ owner after a timed-out submission. Only process-lifetime atomics are
    // safe for those late callbacks (and destruction-time callbacks).
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      validationErrors.fetch_add(1, std::memory_order_relaxed);
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
      validationWarnings.fetch_add(1, std::memory_order_relaxed);
    }
    // Never throw, allocate, log handles, call Vulkan or suppress API calls from
    // the callback. The executable's gate checks the cumulative totals.
    return VK_FALSE;
  }
  void requireValidationSupport() {
    const auto layersFn = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
      getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
    const auto extensionsFn = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
      getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    if (!layersFn || !extensionsFn) fail("validation requested but instance enumeration unavailable");
    uint32_t count = 0;
    checkGlobal(layersFn(&count, nullptr), "enumerate validation layer count");
    std::vector<VkLayerProperties> layers(count);
    checkGlobal(layersFn(&count, layers.data()), "enumerate validation layers");
    if (!std::any_of(layers.begin(), layers.begin() + count, [](const VkLayerProperties& layer) {
          return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        }))
      fail("RH2_GPU_VALIDATION requires VK_LAYER_KHRONOS_validation; no silent disable");
    count = 0;
    checkGlobal(extensionsFn(nullptr, &count, nullptr), "enumerate instance extension count");
    std::vector<VkExtensionProperties> extensions(count);
    checkGlobal(extensionsFn(nullptr, &count, extensions.data()), "enumerate instance extensions");
    if (!std::any_of(extensions.begin(), extensions.begin() + count, [](const VkExtensionProperties& extension) {
          return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        }))
      fail("RH2_GPU_VALIDATION requires VK_EXT_debug_utils; no silent disable");
  }
  void init(uint32_t width, uint32_t height, const ExportedImage* expected) {
    checkedImageBytes(width, height);
    wchar_t system[MAX_PATH + 1]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) fail("GetSystemDirectoryW failed");
    const std::wstring library = std::wstring(system, length) + L"\\vulkan-1.dll";
    loader = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) fail("system Vulkan loader unavailable Win32=" + std::to_string(GetLastError()));
    getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (!getInstanceProcAddr) fail("vkGetInstanceProcAddr unavailable");
    const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    uint32_t version = VK_API_VERSION_1_0;
    if (enumerateVersion) checkGlobal(enumerateVersion(&version), "vkEnumerateInstanceVersion");
    if (version < VK_API_VERSION_1_1) fail("Vulkan 1.1 required, no CPU fallback");
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
      getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance) fail("vkCreateInstance unavailable");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "WarVK RH2 GPU laboratory candidate";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    if (RequireValidation) {
      requireValidationSupport();
      debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
      debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      debug.pfnUserCallback = &Context::validationCallback;
      debug.pUserData = nullptr;
      create.pNext = &debug; // Also captures vkCreateInstance/vkDestroyInstance.
      create.enabledLayerCount = 1; create.ppEnabledLayerNames = &layer;
      create.enabledExtensionCount = 1; create.ppEnabledExtensionNames = &extension;
    }
    checkGlobal(createInstance(&create, nullptr, &instance), "vkCreateInstance");
    if (RequireValidation) {
      counters.validationEnabled = true;
      validationInstances.fetch_add(1, std::memory_order_relaxed);
    }
#define RH2_LOAD_INSTANCE(name) loadInstance(name, "vk" #name);
    RH2_INSTANCE_FUNCTIONS(RH2_LOAD_INSTANCE)
#undef RH2_LOAD_INSTANCE
    if (RequireValidation) {
      loadInstance(CreateDebugUtilsMessengerEXT, "vkCreateDebugUtilsMessengerEXT");
      loadInstance(DestroyDebugUtilsMessengerEXT, "vkDestroyDebugUtilsMessengerEXT");
      checkGlobal(CreateDebugUtilsMessengerEXT(instance, &debug, nullptr, &debugMessenger),
                  "vkCreateDebugUtilsMessengerEXT");
    }
    selectDevice(width, height, expected);
    constexpr const char* extensions[] = {
      VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME
    };
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 2;
    deviceInfo.ppEnabledExtensionNames = extensions;
    checkGlobal(CreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
#define RH2_LOAD_DEVICE(name) loadDevice(name, "vk" #name);
    RH2_DEVICE_FUNCTIONS(RH2_LOAD_DEVICE)
#undef RH2_LOAD_DEVICE
    GetDeviceQueue(device, queueFamily, 0, &queue);
    GetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    check(CreateCommandPool(device, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    check(AllocateCommandBuffers(device, &allocate, &command), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(CreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
  }
  void selectDevice(uint32_t width, uint32_t height, const ExportedImage* expected) {
    uint32_t count = 0;
    checkGlobal(EnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPU count");
    if (!count) fail("no Vulkan physical devices");
    std::vector<VkPhysicalDevice> devices(count);
    checkGlobal(EnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPUs");
    devices.resize(count);
    if (devices.empty()) fail("no Vulkan physical devices in final enumeration");
    std::string refusals;
    bool matchingAdapterFound = false, adapterIdentityUnavailable = false;
    for (auto candidate : devices) {
      VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      properties.pNext = &id;
      GetPhysicalDeviceProperties2(candidate, &properties);
      const std::string label(properties.properties.deviceName);
      auto refuse = [&](const char* why) { refusals += label + ": " + why + "; "; };
      if (!nonzero(id.deviceUUID, VK_UUID_SIZE) || !nonzero(id.driverUUID, VK_UUID_SIZE)) {
        adapterIdentityUnavailable = true;
        refuse("missing device/driver UUID"); continue;
      }
      if (expected && (std::memcmp(expected->deviceUUID.data(), id.deviceUUID, VK_UUID_SIZE) ||
          std::memcmp(expected->driverUUID.data(), id.driverUUID, VK_UUID_SIZE))) {
        refuse("device/driver UUID mismatch"); continue;
      }
      // Remember identity BEFORE testing capabilities. A matching adapter with
      // unsupported image/semaphore/API features is NOT an AdapterMismatch.
      matchingAdapterFound = true;
      if (properties.properties.apiVersion < VK_API_VERSION_1_1) {
        refuse("device Vulkan < 1.1"); continue;
      }
      uint32_t extensionCount = 0;
      checkGlobal(EnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr),
                  "enumerate device extension count");
      std::vector<VkExtensionProperties> extensions(extensionCount);
      checkGlobal(EnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount,
                  extensions.data()), "enumerate device extensions");
      auto has = [&](const char* name) {
        return std::any_of(extensions.begin(), extensions.begin() + extensionCount,
          [&](const VkExtensionProperties& p) { return !std::strcmp(p.extensionName, name); });
      };
      if (!has(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) ||
          !has(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME)) {
        refuse("Win32 external memory/semaphore extensions missing"); continue;
      }
      VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
      external.handleType = MemoryHandleType;
      VkPhysicalDeviceImageFormatInfo2 formatInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
      formatInfo.pNext = &external;
      formatInfo.format = ImageFormat;
      formatInfo.type = VK_IMAGE_TYPE_2D;
      formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      formatInfo.usage = ImageUsage;
      VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
      VkImageFormatProperties2 formatProperties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
      formatProperties.pNext = &externalProperties;
      const VkResult formatResult = GetPhysicalDeviceImageFormatProperties2(candidate, &formatInfo, &formatProperties);
      if (formatResult == VK_ERROR_FORMAT_NOT_SUPPORTED) { refuse("external RGBA8 image unsupported"); continue; }
      checkGlobal(formatResult, "query external image properties");
      const auto& em = externalProperties.externalMemoryProperties;
      const auto requiredImage = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
      if ((em.externalMemoryFeatures & requiredImage) != requiredImage ||
          !(em.compatibleHandleTypes & MemoryHandleType)) {
        refuse("RGBA8 image not both EXPORTABLE/IMPORTABLE"); continue;
      }
      const auto& limits = formatProperties.imageFormatProperties;
      if (limits.maxExtent.width < width || limits.maxExtent.height < height ||
          !(limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
        refuse("external image extent/sample limit"); continue;
      }
      VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
      semaphoreInfo.handleType = SemaphoreHandleType;
      VkExternalSemaphoreProperties semaphoreProperties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
      GetPhysicalDeviceExternalSemaphoreProperties(candidate, &semaphoreInfo, &semaphoreProperties);
      const auto requiredSemaphore = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
      if ((semaphoreProperties.externalSemaphoreFeatures & requiredSemaphore) != requiredSemaphore ||
          !(semaphoreProperties.compatibleHandleTypes & SemaphoreHandleType)) {
        refuse("opaque Win32 binary semaphore not EXPORTABLE/IMPORTABLE"); continue;
      }
      uint32_t queueCount = 0;
      GetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
      std::vector<VkQueueFamilyProperties> queues(queueCount);
      GetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
      bool found = false;
      for (uint32_t i = 0; i < queueCount; ++i) {
        if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
          queueFamily = i; found = true; break;
        }
      }
      if (!found) { refuse("graphics queue missing"); continue; }
      physical = candidate;
      counters.deviceName = label;
      std::copy(id.deviceUUID, id.deviceUUID + VK_UUID_SIZE, counters.deviceUUID.begin());
      std::copy(id.driverUUID, id.driverUUID + VK_UUID_SIZE, counters.driverUUID.begin());
      counters.imageExternalFeatures = em.externalMemoryFeatures;
      counters.semaphoreExternalFeatures = semaphoreProperties.externalSemaphoreFeatures;
      return;
    }
    if (expected && !matchingAdapterFound && !adapterIdentityUnavailable)
      throw AdapterMismatch("RH2 GPU: no deviceUUID+driverUUID match; " + refusals);
    fail("no compatible GPU; " + refusals);
  }
  uint32_t memoryType(uint32_t mask, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const {
    for (unsigned pass = 0; pass < 2; ++pass) {
      const auto wanted = required | (pass ? 0 : preferred);
      for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((mask & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted)
          return i;
    }
    fail("compatible memory type unavailable");
  }
  void createBuffer(Buffer& target, VkDeviceSize bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags required) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(CreateBuffer(device, &info, nullptr, &target.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    GetBufferMemoryRequirements(device, target.buffer, &requirements);
    const uint32_t type = memoryType(requirements.memoryTypeBits, required,
      (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
    check(AllocateMemory(device, &allocate, nullptr, &target.memory), "allocate buffer memory");
    target.allocationBytes = requirements.size;
    target.coherent = bool(memoryProperties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(BindBufferMemory(device, target.buffer, target.memory, 0), "vkBindBufferMemory");
  }
  void destroyBuffer(Buffer& target) noexcept {
    if (target.buffer) DestroyBuffer(device, target.buffer, nullptr);
    if (target.memory) FreeMemory(device, target.memory, nullptr);
    target = {};
  }
  void begin() {
    usable();
    check(ResetFences(device, 1, &fence), "vkResetFences");
    check(ResetCommandBuffer(command, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(BeginCommandBuffer(command, &info), "vkBeginCommandBuffer");
  }
  void end() { check(EndCommandBuffer(command), "vkEndCommandBuffer"); }
  void submitAndWait(VkSemaphore wait, VkPipelineStageFlags stage, VkSemaphore signal) {
    usable();
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    if (wait) { submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &wait; submit.pWaitDstStageMask = &stage; }
    if (signal) { submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &signal; }
    // Preserve uncertainty even if the driver reports an error during submit.
    inFlight = true;
    check(QueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
    ++counters.submitCount;
    ++counters.fenceWaitCount;
    check(WaitForFences(device, 1, &fence, VK_TRUE, FenceTimeoutNs), "vkWaitForFences(5s)");
    inFlight = false;
  }
  void imageBarrier(VkImage image, VkImageLayout oldLayout, uint32_t sourceQueue, uint32_t destQueue,
                    VkAccessFlags sourceAccess, VkAccessFlags destAccess,
                    VkPipelineStageFlags sourceStage, VkPipelineStageFlags destStage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess; barrier.dstAccessMask = destAccess;
    barrier.oldLayout = oldLayout; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = sourceQueue; barrier.dstQueueFamilyIndex = destQueue;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CmdPipelineBarrier(command, sourceStage, destStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
  BackendStats stats() const {
    auto result = counters;
    result.faulted = fault || quarantined;
    result.validationErrors = validationErrors.load(std::memory_order_relaxed);
    result.validationWarnings = validationWarnings.load(std::memory_order_relaxed);
    return result;
  }
};

void createExternalImage(Context& c, uint32_t width, uint32_t height, VkImage& image) {
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external.handleTypes = MemoryHandleType;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.pNext = &external; info.imageType = VK_IMAGE_TYPE_2D; info.format = ImageFormat;
  info.extent = {width, height, 1}; info.mipLevels = 1; info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = ImageUsage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  c.check(c.CreateImage(c.device, &info, nullptr, &image), "create external image");
}
void createExportSemaphore(Context& c, VkSemaphore& semaphore, NtHandle& handle) {
  VkExportSemaphoreWin32HandleInfoKHR win32{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
  win32.dwAccess = GENERIC_ALL;
  VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
  external.pNext = &win32; external.handleTypes = SemaphoreHandleType;
  VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  info.pNext = &external;
  c.check(c.CreateSemaphore(c.device, &info, nullptr, &semaphore), "create export semaphore");
  VkSemaphoreGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
  get.semaphore = semaphore; get.handleType = SemaphoreHandleType;
  c.check(c.GetSemaphoreWin32HandleKHR(c.device, &get, &handle.value), "export NT semaphore");
}
void importSemaphore(Context& c, uint64_t borrowed, VkSemaphore& semaphore) {
  NtHandle handle; handle.duplicateBorrowed(borrowed);
  VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  c.check(c.CreateSemaphore(c.device, &info, nullptr, &semaphore), "create import semaphore");
  VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
  import.semaphore = semaphore; import.handleType = SemaphoreHandleType;
  import.handle = handle.value;
  // Permanent reference import. NT handle ownership is NOT transferred by Vulkan.
  c.check(c.ImportSemaphoreWin32HandleKHR(c.device, &import), "import NT semaphore");
}

} // anonymous namespace

struct Renderer::Impl {
  Context c;
  uint32_t width = 0, height = 0, memoryTypeIndex = 0;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory imageMemory = VK_NULL_HANDLE;
  VkImageView imageView = VK_NULL_HANDLE;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkShaderModule vertexShader = VK_NULL_HANDLE, fragmentShader = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkSemaphore ready = VK_NULL_HANDLE, consumed = VK_NULL_HANDLE;
  NtHandle imageHandle, readyHandle, consumedHandle;
  Buffer geometry, upload;
  bool initialized = false, awaitingConsumer = false;

  ~Impl() {
    if (c.inFlight || awaitingConsumer) { c.quarantined = true; return; }
    if (!c.device) return;
    if (pipeline) c.DestroyPipeline(c.device, pipeline, nullptr);
    if (pipelineLayout) c.DestroyPipelineLayout(c.device, pipelineLayout, nullptr);
    if (vertexShader) c.DestroyShaderModule(c.device, vertexShader, nullptr);
    if (fragmentShader) c.DestroyShaderModule(c.device, fragmentShader, nullptr);
    if (framebuffer) c.DestroyFramebuffer(c.device, framebuffer, nullptr);
    if (renderPass) c.DestroyRenderPass(c.device, renderPass, nullptr);
    if (imageView) c.DestroyImageView(c.device, imageView, nullptr);
    c.destroyBuffer(upload); c.destroyBuffer(geometry);
    if (ready) c.DestroySemaphore(c.device, ready, nullptr);
    if (consumed) c.DestroySemaphore(c.device, consumed, nullptr);
    if (image) c.DestroyImage(c.device, image, nullptr);
    if (imageMemory) c.FreeMemory(c.device, imageMemory, nullptr);
  }
  void init(uint32_t w, uint32_t h, ShaderWords vertex, ShaderWords fragment) {
    width = w; height = h;
    c.init(w, h, nullptr);
    createExternalImage(c, w, h, image);
    VkMemoryRequirements requirements{};
    c.GetImageMemoryRequirements(c.device, image, &requirements);
    memoryTypeIndex = c.memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // Always dedicated on BOTH sides, including implementations reporting
    // DEDICATED_ONLY. Import copies the exact size and memoryTypeIndex.
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    VkExportMemoryWin32HandleInfoKHR win32{VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    win32.pNext = &dedicated; win32.dwAccess = GENERIC_ALL;
    VkExportMemoryAllocateInfo external{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    external.pNext = &win32; external.handleTypes = MemoryHandleType;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = &external; allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memoryTypeIndex;
    c.check(c.AllocateMemory(c.device, &allocate, nullptr, &imageMemory), "allocate export image");
    c.check(c.BindImageMemory(c.device, image, imageMemory, 0), "bind export image");
    c.counters.sharedImageBytes = requirements.size;
    c.counters.ownedDeviceBytes = requirements.size;
    VkMemoryGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    get.memory = imageMemory; get.handleType = MemoryHandleType;
    c.check(c.GetMemoryWin32HandleKHR(c.device, &get, &imageHandle.value), "export NT image memory");
    createExportSemaphore(c, ready, readyHandle);
    createExportSemaphore(c, consumed, consumedHandle);
    createGeometry();
    createPipeline(vertex, fragment);
  }
  void createGeometry() {
    const float positions[] = {-0.65f, -0.65f, 0.65f, -0.65f, 0.65f, 0.65f, -0.65f, 0.65f};
    const uint32_t indices[] = {0, 1, 2, 2, 3, 0};
    constexpr size_t bytes = sizeof(positions) + sizeof(indices);
    c.createBuffer(geometry, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    c.createBuffer(upload, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    c.counters.geometryBytes = geometry.allocationBytes;
    c.counters.geometryPayloadBytes = bytes;
    c.counters.ownedDeviceBytes += geometry.allocationBytes;
    c.counters.uploadStagingPeakBytes = upload.allocationBytes;
    void* mapped = nullptr;
    c.check(c.MapMemory(c.device, upload.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map geometry staging");
    std::memcpy(mapped, positions, sizeof(positions));
    std::memcpy(static_cast<uint8_t*>(mapped) + sizeof(positions), indices, sizeof(indices));
    VkResult flushed = VK_SUCCESS;
    if (!upload.coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = upload.memory; range.size = VK_WHOLE_SIZE;
      flushed = c.FlushMappedMemoryRanges(c.device, 1, &range);
    }
    c.UnmapMemory(c.device, upload.memory);
    c.check(flushed, "flush geometry staging");
    c.begin();
    VkBufferCopy copy{0, 0, bytes};
    c.CmdCopyBuffer(c.command, upload.buffer, geometry.buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = geometry.buffer; barrier.size = VK_WHOLE_SIZE;
    c.CmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
      0, 0, nullptr, 1, &barrier, 0, nullptr);
    c.end(); c.submitAndWait(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    c.destroyBuffer(upload);
  }
  void createShader(ShaderWords words, VkShaderModule& module) {
    if (!words.data || words.words < 5 || words.words > 1024 * 1024 || words.data[0] != 0x07230203u)
      fail("invalid bounded SPIR-V word array");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.words * sizeof(uint32_t); info.pCode = words.data;
    c.check(c.CreateShaderModule(c.device, &info, nullptr, &module), "vkCreateShaderModule");
  }
  void createPipeline(ShaderWords vertex, ShaderWords fragment) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = ImageFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    c.check(c.CreateImageView(c.device, &view, nullptr, &imageView), "vkCreateImageView");
    VkAttachmentDescription attachment{};
    attachment.format = ImageFormat; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1; pass.pAttachments = &attachment;
    pass.subpassCount = 1; pass.pSubpasses = &subpass;
    c.check(c.CreateRenderPass(c.device, &pass, nullptr, &renderPass), "vkCreateRenderPass");
    VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    frame.renderPass = renderPass; frame.attachmentCount = 1; frame.pAttachments = &imageView;
    frame.width = width; frame.height = height; frame.layers = 1;
    c.check(c.CreateFramebuffer(c.device, &frame, nullptr, &framebuffer), "vkCreateFramebuffer");
    createShader(vertex, vertexShader); createShader(fragment, fragmentShader);
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    c.check(c.CreatePipelineLayout(c.device, &layout, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertexShader;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragmentShader;
    const VkVertexInputBindingDescription binding{0, sizeof(float) * 2, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1; vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1; vertexInput.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, float(width), float(height), 0, 1};
    const VkRect2D scissor{{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo viewportInfo{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportInfo.viewportCount = 1; viewportInfo.pViewports = &viewport;
    viewportInfo.scissorCount = 1; viewportInfo.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1; colorBlend.pAttachments = &blend;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly; info.pViewportState = &viewportInfo;
    info.pRasterizationState = &raster; info.pMultisampleState = &samples; info.pColorBlendState = &colorBlend;
    info.layout = pipelineLayout; info.renderPass = renderPass;
    c.check(c.CreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines");
  }
  void render(uint32_t variant) {
    if (variant > 3) fail("frame variant outside 0..3");
    c.begin();
    if (!initialized) {
      c.imageBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    } else {
      c.imageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_EXTERNAL, c.queueFamily,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    VkClearValue clear{};
    clear.color.float32[0] = 0.015625f; clear.color.float32[1] = 0.03125f;
    clear.color.float32[2] = 0.0625f; clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = renderPass; begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, {width, height}};
    begin.clearValueCount = 1; begin.pClearValues = &clear;
    c.CmdBeginRenderPass(c.command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    c.CmdBindPipeline(c.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDeviceSize offset = 0;
    c.CmdBindVertexBuffers(c.command, 0, 1, &geometry.buffer, &offset);
    c.CmdBindIndexBuffer(c.command, geometry.buffer, sizeof(float) * 8, VK_INDEX_TYPE_UINT32);
    c.CmdPushConstants(c.command, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(variant), &variant);
    c.CmdDrawIndexed(c.command, 6, 1, 0, 0, 0);
    c.CmdEndRenderPass(c.command);
    c.imageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, c.queueFamily, VK_QUEUE_FAMILY_EXTERNAL,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    c.end();
    const VkSemaphore wait = awaitingConsumer ? consumed : VK_NULL_HANDLE;
    if (wait) ++c.counters.consumerWaitCount;
    awaitingConsumer = true;
    c.submitAndWait(wait, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, ready);
    initialized = true;
    ++c.counters.drawCount;
  }
  void settle() {
    c.usable();
    if (awaitingConsumer) {
      c.begin();
      // Wait-only submission consumes the imported binary semaphore payload.
      // It does NOT acquire the image: keep EXTERNAL ownership for the next
      // render's GENERAL acquire, irrespective of this per-frame settlement.
      c.end();
      ++c.counters.consumerWaitCount;
      c.submitAndWait(consumed, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_NULL_HANDLE);
      awaitingConsumer = false;
    }
  }
};

struct Importer::Impl {
  Context c;
  uint32_t width = 0, height = 0;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory imageMemory = VK_NULL_HANDLE;
  VkSemaphore ready = VK_NULL_HANDLE, consumed = VK_NULL_HANDLE;
  Buffer readbackBuffer;
  uint64_t pixelBytes = 0;

  ~Impl() {
    if (c.inFlight) { c.quarantined = true; return; }
    if (!c.device) return;
    c.destroyBuffer(readbackBuffer);
    if (ready) c.DestroySemaphore(c.device, ready, nullptr);
    if (consumed) c.DestroySemaphore(c.device, consumed, nullptr);
    if (image) c.DestroyImage(c.device, image, nullptr);
    if (imageMemory) c.FreeMemory(c.device, imageMemory, nullptr);
  }
  void init(const ExportedImage& exported) {
    width = exported.width; height = exported.height;
    pixelBytes = checkedImageBytes(width, height);
    if (exported.format != uint32_t(ImageFormat) || !exported.allocationBytes ||
        exported.allocationBytes > 256ull * 1024 * 1024 ||
        !nonzero(exported.deviceUUID.data(), 16) || !nonzero(exported.driverUUID.data(), 16))
      fail("invalid external image metadata");
    NtHandle memoryHandle; memoryHandle.duplicateBorrowed(exported.imageMemoryHandle);
    c.init(width, height, &exported);
    createExternalImage(c, width, height, image);
    VkMemoryRequirements requirements{};
    c.GetImageMemoryRequirements(c.device, image, &requirements);
    if (exported.memoryTypeIndex >= c.memoryProperties.memoryTypeCount ||
        !(requirements.memoryTypeBits & (1u << exported.memoryTypeIndex)) ||
        requirements.size != exported.allocationBytes)
      fail("external image size/memoryTypeIndex mismatch");
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    import.pNext = &dedicated; import.handleType = MemoryHandleType; import.handle = memoryHandle.value;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = &import; allocate.allocationSize = exported.allocationBytes;
    allocate.memoryTypeIndex = exported.memoryTypeIndex;
    c.check(c.AllocateMemory(c.device, &allocate, nullptr, &imageMemory), "import NT image memory");
    c.check(c.BindImageMemory(c.device, image, imageMemory, 0), "bind imported image");
    // The opaque imported payload is an alias, NOT another physical allocation.
    c.counters.sharedImageBytes = exported.allocationBytes;
    importSemaphore(c, exported.readySemaphoreHandle, ready);
    importSemaphore(c, exported.consumedSemaphoreHandle, consumed);
    c.createBuffer(readbackBuffer, pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    c.counters.readbackBytes = readbackBuffer.allocationBytes;
    c.counters.ownedDeviceBytes = readbackBuffer.allocationBytes;
  }
  std::vector<uint8_t> readback() {
    c.begin();
    // Imported image content/layout comes from the exporting release. It MUST
    // NOT be transitioned from UNDEFINED here (that would discard the frame).
    c.imageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_EXTERNAL, c.queueFamily,
      0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    c.CmdCopyImageToBuffer(c.command, image, VK_IMAGE_LAYOUT_GENERAL, readbackBuffer.buffer, 1, &copy);
    VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostRead.buffer = readbackBuffer.buffer; hostRead.size = VK_WHOLE_SIZE;
    c.CmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 0, nullptr, 1, &hostRead, 0, nullptr);
    c.imageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, c.queueFamily, VK_QUEUE_FAMILY_EXTERNAL,
      VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    c.end();
    c.submitAndWait(ready, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, consumed);
    std::vector<uint8_t> pixels(static_cast<size_t>(pixelBytes));
    void* mapped = nullptr;
    c.check(c.MapMemory(c.device, readbackBuffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map test readback");
    if (!readbackBuffer.coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = readbackBuffer.memory; range.size = VK_WHOLE_SIZE;
      const VkResult result = c.InvalidateMappedMemoryRanges(c.device, 1, &range);
      if (result != VK_SUCCESS) {
        c.UnmapMemory(c.device, readbackBuffer.memory);
        c.check(result, "invalidate test readback");
      }
    }
    std::memcpy(pixels.data(), mapped, pixels.size());
    c.UnmapMemory(c.device, readbackBuffer.memory);
    ++c.counters.readbackCount;
    return pixels;
  }
};

Renderer::Renderer(uint32_t width, uint32_t height, ShaderWords vertex, ShaderWords fragment)
: m_impl(std::make_unique<Impl>()) { m_impl->init(width, height, vertex, fragment); }
Renderer::~Renderer() = default;
ExportedImage Renderer::exportImage() const {
  m_impl->c.usable();
  ExportedImage result;
  result.width = m_impl->width; result.height = m_impl->height;
  result.format = uint32_t(ImageFormat); result.memoryTypeIndex = m_impl->memoryTypeIndex;
  result.allocationBytes = m_impl->c.counters.sharedImageBytes;
  result.deviceUUID = m_impl->c.counters.deviceUUID; result.driverUUID = m_impl->c.counters.driverUUID;
  result.imageMemoryHandle = m_impl->imageHandle.integer();
  result.readySemaphoreHandle = m_impl->readyHandle.integer();
  result.consumedSemaphoreHandle = m_impl->consumedHandle.integer();
  return result;
}
void Renderer::render(uint32_t variant) { m_impl->render(variant); }
void Renderer::settleLastConsumer() { m_impl->settle(); }
BackendStats Renderer::stats() const { return m_impl->c.stats(); }
Importer::Importer(const ExportedImage& image) : m_impl(std::make_unique<Impl>()) { m_impl->init(image); }
Importer::~Importer() = default;
std::vector<uint8_t> Importer::readback() { return m_impl->readback(); }
BackendStats Importer::stats() const { return m_impl->c.stats(); }
ValidationTotals processValidationTotals() noexcept {
  return {validationInstances.load(std::memory_order_relaxed),
          validationErrors.load(std::memory_order_relaxed),
          validationWarnings.load(std::memory_order_relaxed)};
}

} // namespace warvk::host::gpu
