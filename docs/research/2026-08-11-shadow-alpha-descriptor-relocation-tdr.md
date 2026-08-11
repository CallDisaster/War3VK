# Shadow alpha descriptor relocation and TDR evidence

## Evidence

Two `VK_EXT_device_fault` incidents reported `READ_INVALID` addresses inside
16 MiB and 32 MiB allocations that the binding recorder had already observed
as unbound before the failing driver call. Both incidents occurred during the
WarVK shadow/receiver frame and had no Arena overflow or partial transaction.
A 643-second isolated `life_and_death_tdr` run with only DXVK memory defrag
disabled completed without a new device loss or GPU event. This A/B is strong
localization evidence, not by itself a release workaround.

`War3ShadowCasterDraw` retained the logical `DxvkImageView`, but CSM, terrain
mask, point shadow and outline replay bound a by-value `DxvkDescriptor` copied
at capture time. DXVK increments the image version when backing storage is
replaced; `DxvkImageView::getDescriptor()` then lazily recreates the view for
the current backing. The copied descriptor cannot participate in that update.
Replay therefore could bind the old `VkImageView` while tracking the current
logical image allocation, allowing the old backing to retire before the GPU
sampled it.

## Contract

- The capture-time descriptor remains diagnostic identity only.
- Every replay transaction resolves the descriptor from the retained
  `DxvkImageView` before the first draw/clear and rejects a missing current
  image view fail-closed.
- Directional depth and terrain-mask replay use the same prepared current
  descriptor. Point-shadow faces and both outline modes use descriptors sealed
  by their existing preflight phase.
- The logical image is still tracked for read access, so the current backing
  selected by `getDescriptor()` remains owned through GPU completion.
- Disabling memory defrag is an A/B instrument only and is not the production
  fix.

## Primary references

- Khronos Vulkan resource lifetime rules:
  https://docs.vulkan.org/spec/latest/chapters/resources.html
- Khronos Vulkan memory binding and allocation rules:
  https://docs.vulkan.org/spec/latest/chapters/memory.html
- Upstream DXVK memory-defrag configuration description:
  https://github.com/doitsujin/dxvk/blob/master/dxvk.conf

Offline contracts and build success cannot replace a default-defrag physical
TDR stress run. The candidate must be retested with memory defrag enabled.
