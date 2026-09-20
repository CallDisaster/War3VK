#pragma once

#include "../util/util_error.h"

#include <utility>

namespace dxvk {

// Only an allocation failure, never device loss or an arbitrary driver error.
// Existing DxvkError handlers still see the diagnostic. Speculative users may
// catch this subtype, but must check device health before any fallback.
class DxvkBufferAllocationError : public DxvkError {
public:
  DxvkBufferAllocationError()
  : DxvkError(std::string("Failed to allocate buffer storage")) { }

  explicit DxvkBufferAllocationError(std::string&& message)
  : DxvkError(std::move(message)) { }
};

// Publish only a completely initialized resource. Failed allocation/assignment
// must not leave a raw resource pointer in the allocator's relocation map.
// The caller owns the storage before and after assignment, including unwind.
template<typename Storage, typename Assign, typename Publish>
void DxvkPublishInitialBufferStorage(
    Storage&& storage, Assign&& assign, Publish&& publish) {
  if (!storage)
    throw DxvkBufferAllocationError();
  assign(std::move(storage));
  publish();
}

// Owns a newly created, not-yet-published Vulkan handle until ownership moves
// to DxvkResourceAllocation. No command can reference it at this point.
template<typename Cleanup>
class DxvkUnboundBufferGuard {
public:
  explicit DxvkUnboundBufferGuard(Cleanup cleanup)
  : m_cleanup(std::move(cleanup)) { }
  DxvkUnboundBufferGuard(const DxvkUnboundBufferGuard&) = delete;
  DxvkUnboundBufferGuard& operator=(const DxvkUnboundBufferGuard&) = delete;
  ~DxvkUnboundBufferGuard() noexcept {
    if (m_owned)
      m_cleanup();
  }
  void release() noexcept { m_owned = false; }
private:
  Cleanup m_cleanup;
  bool m_owned = true;
};

}
