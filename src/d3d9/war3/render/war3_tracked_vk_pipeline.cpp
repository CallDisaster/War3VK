#include "war3_tracked_vk_pipeline.h"

#include "../../../dxvk/dxvk_device.h"

namespace dxvk::war3::render {

  Rc<War3TrackedVkPipeline> AdoptWar3TrackedVkPipeline(
    const Rc<DxvkDevice>& device,
          VkPipeline     pipeline) {
    if (!device || pipeline == VK_NULL_HANDLE)
      return nullptr;

    // Capturing the device Rc is intentional: a command list may release its
    // final pipeline owner during device teardown, and vkDestroyPipeline still
    // needs a live device dispatch table at that point.
    return new War3TrackedVkPipeline(
      pipeline,
      [device] (VkPipeline handle) {
        auto vk = device->vkd();
        vk->vkDestroyPipeline(vk->device(), handle, nullptr);
      });
  }

}
