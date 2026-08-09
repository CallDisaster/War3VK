#include "../war3_owned_image_layout.h"

#include <cassert>

int main() {
  using namespace dxvk::war3::render;

  War3OwnedImageLayoutState state;
  const auto first = state.plan(
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      VK_ACCESS_2_TRANSFER_WRITE_BIT);
  assert(first.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
  assert(first.srcStages == VK_PIPELINE_STAGE_2_NONE);
  assert(first.srcAccess == VK_ACCESS_2_NONE);
  state.commit(first);

  const auto sampled = state.plan(
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_SHADER_READ_BIT);
  assert(sampled.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  assert(sampled.srcStages == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
  assert(sampled.srcAccess == VK_ACCESS_2_TRANSFER_WRITE_BIT);
  state.commit(sampled);
  assert(state.layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  assert(state.transitionSerial() == 2u);

  const auto attachment = state.plan(
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  assert(attachment.oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  assert(attachment.srcStages == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
  assert(attachment.srcAccess == VK_ACCESS_2_SHADER_READ_BIT);

  const VkImageSubresourceRange range = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
  const auto barrier = MakeWar3OwnedImageBarrier(
      attachment, VK_NULL_HANDLE, range);
  assert(barrier.oldLayout == attachment.oldLayout);
  assert(barrier.newLayout == attachment.newLayout);
  assert(barrier.srcStageMask == attachment.srcStages);
  assert(barrier.dstAccessMask == attachment.dstAccess);
  assert(barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
  assert(barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);

  state.reset();
  const auto recreated = state.plan(
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  assert(recreated.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
  assert(recreated.srcStages == VK_PIPELINE_STAGE_2_NONE);
  assert(recreated.srcAccess == VK_ACCESS_2_NONE);
  assert(state.transitionSerial() == 0u);
  return 0;
}
