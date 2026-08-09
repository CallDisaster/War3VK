#pragma once

#include "../../../vulkan/vulkan_loader.h"

#include <functional>
#include <utility>

namespace dxvk {

  class DxvkDevice;

  namespace war3::render {

    /**
     * \brief Command-list-owned Vulkan pipeline
     *
     * War3's built-in pipeline helpers return raw VkPipeline handles.  A raw
     * handle cannot be destroyed while any submitted command buffer still
     * references it.  This owner is deliberately small enough to be retained
     * by DxvkCommandList::track: dropping a cache entry only drops the cache's
     * reference, while the last in-flight command list performs destruction
     * after GPU completion.
     */
    class War3TrackedVkPipeline final : public RcObject {

    public:

      using DestroyProc = std::function<void (VkPipeline)>;

      War3TrackedVkPipeline(VkPipeline handle, DestroyProc destroy)
      : m_handle(handle), m_destroy(std::move(destroy)) { }

      ~War3TrackedVkPipeline() {
        const VkPipeline handle = std::exchange(m_handle, VK_NULL_HANDLE);
        if (handle != VK_NULL_HANDLE && m_destroy)
          m_destroy(handle);
      }

      VkPipeline handle() const {
        return m_handle;
      }

    private:

      VkPipeline m_handle = VK_NULL_HANDLE;
      DestroyProc m_destroy;

    };

    /**
     * Adopts a pipeline created by DxvkDevice::createBuiltInGraphicsPipeline.
     * The returned owner keeps the Vulkan device alive until destruction.
     */
    Rc<War3TrackedVkPipeline> AdoptWar3TrackedVkPipeline(
      const Rc<DxvkDevice>& device,
            VkPipeline     pipeline);

  }

}
