#pragma once

#include "../../../vulkan/vulkan_loader.h"

#include <cstdint>

namespace dxvk::war3::render {

/**
 * Value-only state for images created and transitioned directly by War3
 * render passes. DxvkImageView::getLayout() is the view's preferred descriptor
 * layout, not the image's current Vulkan layout, so it must never be used as
 * ownership state.
 */
struct War3OwnedImageLayoutTransition {
  VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkPipelineStageFlags2 srcStages = VK_PIPELINE_STAGE_2_NONE;
  VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
  VkPipelineStageFlags2 dstStages = VK_PIPELINE_STAGE_2_NONE;
  VkAccessFlags2 dstAccess = VK_ACCESS_2_NONE;
};

class War3OwnedImageLayoutState {
public:
  constexpr War3OwnedImageLayoutState() = default;

  constexpr void reset() {
    m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_stages = VK_PIPELINE_STAGE_2_NONE;
    m_access = VK_ACCESS_2_NONE;
    m_transitionSerial = 0u;
  }

  constexpr VkImageLayout layout() const {
    return m_layout;
  }

  constexpr VkPipelineStageFlags2 stages() const {
    return m_stages;
  }

  constexpr VkAccessFlags2 access() const {
    return m_access;
  }

  constexpr uint64_t transitionSerial() const {
    return m_transitionSerial;
  }

  constexpr War3OwnedImageLayoutTransition plan(
      VkImageLayout newLayout,
      VkPipelineStageFlags2 dstStages,
      VkAccessFlags2 dstAccess) const {
    War3OwnedImageLayoutTransition result = {};
    result.oldLayout = m_layout;
    result.newLayout = newLayout;
    result.srcStages = m_layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_PIPELINE_STAGE_2_NONE
        : m_stages;
    result.srcAccess = m_layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_ACCESS_2_NONE
        : m_access;
    result.dstStages = dstStages;
    result.dstAccess = dstAccess;
    return result;
  }

  constexpr void commit(const War3OwnedImageLayoutTransition& transition) {
    m_layout = transition.newLayout;
    m_stages = transition.dstStages;
    m_access = transition.dstAccess;
    ++m_transitionSerial;
  }

private:
  VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkPipelineStageFlags2 m_stages = VK_PIPELINE_STAGE_2_NONE;
  VkAccessFlags2 m_access = VK_ACCESS_2_NONE;
  uint64_t m_transitionSerial = 0u;
};

inline VkImageMemoryBarrier2 MakeWar3OwnedImageBarrier(
    const War3OwnedImageLayoutTransition& transition,
    VkImage image,
    const VkImageSubresourceRange& subresources) {
  VkImageMemoryBarrier2 barrier = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = transition.srcStages;
  barrier.srcAccessMask = transition.srcAccess;
  barrier.dstStageMask = transition.dstStages;
  barrier.dstAccessMask = transition.dstAccess;
  barrier.oldLayout = transition.oldLayout;
  barrier.newLayout = transition.newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = subresources;
  return barrier;
}

template <typename Image>
inline void CommitWar3OwnedImageLayout(
    War3OwnedImageLayoutState& state,
    const War3OwnedImageLayoutTransition& transition,
    Image& image,
    const VkImageSubresourceRange& subresources) {
  state.commit(transition);
  image.trackLayout(subresources, transition.newLayout);
}

} // namespace dxvk::war3::render
