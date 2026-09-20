// The method under test is generated verbatim from dxvk_memory.cpp. Vulkan and
// backing allocation are substitutes; this is NOT a driver/GPU validation test.
#include "../src/dxvk/dxvk_buffer_allocation_guard.h"
#include "../src/util/util_string.h"
#include "../src/util/util_likely.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <memory>
#include <iostream>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); ++checks; } while (0)
static unsigned checks = 0;
namespace dxvk {
template<class T> using Rc = std::shared_ptr<T>;
enum class DxvkAllocationMode { NoDedicated, NoAllocation, NoDeviceMemory, NoFallback };
enum class DxvkAllocationFlag { OwnsImage };
template<class T> struct Flags {
  unsigned bits = 0;
  bool test(T f) const { return bits & (1u << unsigned(f)); }
  void set(T f) { bits |= 1u << unsigned(f); }
  bool isClear() const { return bits == 0; }
};
struct DxvkAllocationInfo {
  Flags<DxvkAllocationMode> mode;
  VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  VkExternalMemoryHandleTypeFlagBits handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
};
struct Probe {
  int created = 0, destroyed = 0, allocated = 0, sparseAlive = 0;
  VkResult createResult = VK_SUCCESS, bindResult = VK_SUCCESS;
  bool dedicated = false, metadata = false, nullAllocation = false;
  bool allocationThrows = false, sparseThrows = false, handleThrows = false;
} probe;
struct FakeVk {
  VkDevice device() { return VK_NULL_HANDLE; }
  VkResult vkCreateImage(VkDevice, const VkImageCreateInfo*, const void*, VkImage* out) {
    if (probe.createResult != VK_SUCCESS) return probe.createResult;
    ++probe.created; *out = (VkImage)(uintptr_t)1; return VK_SUCCESS;
  }
  void vkDestroyImage(VkDevice, VkImage, const void*) { ++probe.destroyed; }
  void vkGetImageMemoryRequirements2(VkDevice, const VkImageMemoryRequirementsInfo2*, VkMemoryRequirements2* r) {
    r->memoryRequirements = {4096, 256, 1};
    auto* d = static_cast<VkMemoryDedicatedRequirements*>(r->pNext);
    d->prefersDedicatedAllocation = d->requiresDedicatedAllocation = probe.dedicated;
  }
  VkResult vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return probe.bindResult; }
};
struct FakeDevice {
  FakeVk vk;
  struct { struct { VkPhysicalDeviceProperties properties{}; } core; } props;
  FakeVk* vkd() { return &vk; }
  const auto& properties() { return props; }
  void notifyDeviceErrorFromDriverResult(VkResult) { }
};
struct DxvkSparsePageTable {
  DxvkSparsePageTable(FakeDevice*, const VkImageCreateInfo&, VkImage) {
    if (probe.sparseThrows) throw DxvkError("sparse-injected");
    ++probe.sparseAlive;
  }
  ~DxvkSparsePageTable() { --probe.sparseAlive; }
  struct Properties { unsigned metadataPageCount; };
  Properties getProperties() { return {probe.metadata ? 1u : 0u}; }
};
struct DxvkResourceAllocation {
  Flags<DxvkAllocationFlag> m_flags;
  VkImage m_image = VK_NULL_HANDLE;
  VkDeviceMemory m_memory = (VkDeviceMemory)(uintptr_t)1;
  uint64_t m_address = 0;
  DxvkSparsePageTable* m_sparsePageTable = nullptr;
  ~DxvkResourceAllocation() {
    if (m_flags.test(DxvkAllocationFlag::OwnsImage)) ++probe.destroyed;
    delete m_sparsePageTable;
  }
  void initKmtHandles(VkExternalMemoryHandleTypeFlagBits) {
    if (probe.handleThrows) throw DxvkError("handle-injected");
  }
};
struct DxvkPageAllocator { static constexpr uint64_t ChunkAddressMask = 0xffff; };
constexpr uint64_t SparseMemoryPageSize = 65536;
class DxvkMemoryAllocator {
public:
  FakeDevice* m_device;
  explicit DxvkMemoryAllocator(FakeDevice* d) : m_device(d) { }
  uint32_t getMemoryTypeMask(VkMemoryPropertyFlags) { return 0; }
  Rc<DxvkResourceAllocation> allocateMemory(const VkMemoryRequirements&, const DxvkAllocationInfo&) {
    ++probe.allocated;
    if (probe.allocationThrows) throw DxvkError("allocation-injected");
    if (probe.nullAllocation) return nullptr;
    return std::make_shared<DxvkResourceAllocation>();
  }
  Rc<DxvkResourceAllocation> allocateDedicatedMemory(const VkMemoryRequirements& r,
      const DxvkAllocationInfo& a, const void*) { return allocateMemory(r, a); }
  Rc<DxvkResourceAllocation> createAllocation(DxvkSparsePageTable* table, const DxvkAllocationInfo&) {
    if (probe.allocationThrows) throw DxvkError("allocation-injected");
    if (probe.nullAllocation) return nullptr;
    auto a = std::make_shared<DxvkResourceAllocation>(); a->m_sparsePageTable = table;
    a->m_memory = VK_NULL_HANDLE; return a;
  }
  void logMemoryError(const VkMemoryRequirements&) { }
  void logMemoryStats() { }
  Rc<DxvkResourceAllocation> createImageResource(const VkImageCreateInfo&, const DxvkAllocationInfo&, const void*);
};
#include "image_allocation_probe.inc"
}

int main() {
  try {
    using namespace dxvk;
    FakeDevice device; DxvkMemoryAllocator allocator(&device);
    for (unsigned mode = 0; mode != 13; ++mode) {
      probe = {};
      VkImageCreateInfo info{}; info.tiling = VK_IMAGE_TILING_OPTIMAL;
      DxvkAllocationInfo allocation;
      if (mode == 1 || mode == 3) probe.dedicated = true;
      if (mode == 2 || mode == 3) probe.nullAllocation = true;
      if (mode == 4 || mode == 5 || mode == 7) probe.allocationThrows = true;
      if (mode == 5) probe.dedicated = true;
      if (mode == 6 || mode == 7 || mode == 8) info.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
      if (mode == 6) probe.sparseThrows = true;
      if (mode == 7) probe.metadata = true;
      if (mode == 9) probe.bindResult = VK_ERROR_DEVICE_LOST;
      if (mode == 10) {
        probe.handleThrows = true; allocation.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
      }
      if (mode == 11 || mode == 12) {
        info.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
        probe.allocationThrows = mode == 11;
        probe.nullAllocation = mode == 12;
      }
      bool threw = false;
      try {
        auto result = allocator.createImageResource(info, allocation, nullptr);
        CHECK(bool(result) == !probe.nullAllocation);
        if (result) CHECK(probe.destroyed == 0);
      } catch (const DxvkError& e) {
        threw = true;
        if (probe.allocationThrows) CHECK(e.message() == "allocation-injected");
      }
      CHECK(threw == (mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 9 || mode == 10 || mode == 11));
      CHECK(probe.created == 1); CHECK(probe.destroyed == 1); CHECK(probe.sparseAlive == 0);
    }
    probe = {}; probe.createResult = VK_ERROR_DEVICE_LOST;
    bool failed = false;
    try { allocator.createImageResource({}, {}, nullptr); } catch (const DxvkError&) { failed = true; }
    CHECK(failed); CHECK(probe.created == 0 && probe.destroyed == 0);
    // Production constructor helper: null/assign exceptions never publish;
    // imported storage has no external-image destruction authority.
    for (int failure = 0; failure != 3; ++failure) {
      unsigned published = 0; std::shared_ptr<int> storage;
      try {
        DxvkPublishInitialImageStorage(failure == 0 ? std::shared_ptr<int>() : std::make_shared<int>(1),
          [&](auto&& p) { if (failure == 1) throw DxvkError("assign"); storage = std::move(p); },
          [&] { ++published; });
      } catch (const DxvkError&) { }
      CHECK(published == (failure == 2 ? 1u : 0u));
    }
    std::cout << "image production-method checks=" << checks << " failures=0 (fake Vulkan)\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
