# TDR P0: War3-owned image layout state

## Problem

Several War3 render passes used `DxvkImageView::getLayout()` as if it were the
image's current Vulkan layout.  That value is the view's preferred descriptor
layout.  It does not prove which layout the image is currently in, and it also
caused recreated images to use a read-only layout as the `oldLayout` of their
first barrier instead of `VK_IMAGE_LAYOUT_UNDEFINED`.

Motion-vector and TAA-current views also declared only sampled usage even
though the same views are attached as color render targets.

## Change

`War3OwnedImageLayoutState` is a value-only state machine for images whose
creation and barriers are owned by War3 passes.  A recreated image starts at
`UNDEFINED`, with source stage/access `NONE/0`.  Every recorded transition is
committed both to this state and to `DxvkImage::trackLayout` for the exact
subresource range.  The helper always uses ignored queue-family indices.

The state is wired into the default paths for:

- directional and volume-sun CSM depth images and the terrain caster mask;
- Shadow Receiver color/depth copies, motion vectors, TAA current and both
  history images;
- FXAA/SMAA input copies, intermediate images and lookup-table uploads;
- SSAO and volumetric-light copies/intermediates/fallback cube;
- ShaderPack input, ping-pong, uploaded and fallback images.

Caller-owned D3D9 color/depth targets are not assigned a parallel War3 state.
Their current subresource layout is read from `DxvkImage`, temporarily changed,
and restored.  ShaderPack now also restores caller depth on pipeline-creation
failure and does not transition it when no pass is active.

Motion-vector and TAA-current image views now include
`VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` in addition to sampled usage.

## Deliberate exclusions

- `d3d9_war3_shadow_outline.cpp` is handled by the separate outline P0 work.
- Point-shadow cube state was converted by the bounded follow-up documented in
  `2026-08-09-tdr-p0-point-shadow-image-layout.md`; it uses independent face
  state rather than sharing the CSM state blindly.
- This change does not alter resource retirement or publication fences.

## Verification boundary

A Win32 runnable covers first-use `UNDEFINED/NONE/0`, subsequent transitions,
reset semantics, barrier fields and ignored queue-family indices.  Static and
build verification completed as follows:

- all 78 static-test scripts passed;
- all 21 Win32 Meson runnable tests passed;
- a clean Win32 DLL build completed and `ninja -C build32 -n` reported no work;
- `git diff --check` passed;
- `d3d9.dll` is 34,128,214 bytes with SHA-256
  `0244F8B5716695E31F644D360893BBC4DFCC4111DEFA0CC11648D155EA440915`.

No DLL was deployed and no game process was started by this branch.
