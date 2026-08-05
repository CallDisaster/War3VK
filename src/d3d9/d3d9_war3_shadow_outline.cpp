#include "d3d9_war3_shadow.h"
#include "d3d9_shader.h"
#include "d3d9_war3_debug.h"
#include "d3d9_war3_hook.h"

#include "war3/shader/war3_shader_manager.h"

#include <war3_fullscreen_vert.h>
#include <war3_outline_edge.h>
#include <war3_outline_expand_vert.h>
#include <war3_unit_outline.h>

#include <algorithm>
#include <array>

namespace dxvk {

namespace {

// 与 shadow caster shader push constants 保持一致。
struct ShadowCasterPushConstants {
  Matrix4 mvp;
  uint32_t paletteOffset;
  uint32_t blendCount;
  uint32_t flags;
  float alphaRef;
  uint32_t samplerIndex;
  float terrainDepthBias;
  uint32_t padding[2];
  float outlineColor[4];
};

struct OutlineEdgePushConstants {
  uint32_t visibleSampler;
  uint32_t allSampler;
  float invWidth;
  float invHeight;
  float widthPx;
  uint32_t showVisible;
  uint32_t showOccluded;
  float color[4];
};

} // namespace
void War3ShadowReceiverPass::renderUnitOutlineScreenSpace(
    const Rc<DxvkCommandList> &ctx, const War3PipelineInput &input) {
  if (!War3RenderState::HasOutlineHandles()) {
    return;
  }

  // 统计需要渲染的单位数量（仅描边表内的单位）
  uint32_t unitCount = 0;
  for (const auto &caster : input.scene.shadowCasters) {
    // 说明：描边目标以 batchHandle 匹配为准，不再强依赖
    // batchTag==WorldObjects。 原因：部分对象会在 SelectionOverlay/Decorations
    // 等 WorldGroup Tag 下绘制， 但仍需要支持描边；同时 terrain
    // 等通常没有有效句柄，不会误命中。
    if (War3RenderState::IsOutlineHandle(caster.batchHandle)) {
      unitCount++;
    }
  }

  if (unitCount == 0) {
    static bool s_loggedNoMatch = false;
    if (!s_loggedNoMatch) {
      s_loggedNoMatch = true;
      WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: outline handles present "
                      "but no draw match (handles=%u)\n",
                      War3RenderState::GetOutlineHandleCount());
    }
    if (!war3dbg::RenderLogEnabled()) {
      static uint32_t s_outlineNoMatchLogs = 0;
      if (s_outlineNoMatchLogs++ < 3) {
        war3dbg::Print("DXVK_Outline: no draw match (handles=%u, casters=%u)\n",
                       War3RenderState::GetOutlineHandleCount(),
                       static_cast<uint32_t>(input.scene.shadowCasters.size()));
      }
    }
    return;
  }

  War3RenderSettings defaultSettings = {};
  const War3RenderSettings *settings =
      input.settings ? input.settings : &defaultSettings;
  const War3OccludedOutlineSettings &outline = settings->occludedOutline;

  const auto &colorView = input.colorView;
  const auto &depthView = input.depthView;
  if (!colorView || !depthView) {
    return;
  }

  VkExtent3D extent = colorView->mipLevelExtent(0u);
  VkFormat colorFormat = colorView->image()->info().format;
  // VkFormat depthFormat = depthView->image()->info().format; // Unused for MRT
  // manual compare

  ensureOutlineMaskResources(extent);
  if (!m_outlineMaskVisibleView || !m_outlineMaskAllView) {
    return;
  }

  if (!m_outlineMaskLayout) {
    m_outlineMaskLayout = createOutlineMaskPipelineLayout();
  }

  // MRT Render Logic
  auto drawMaskPassMRT = [&]() {
    if (!m_outlineMaskLayout)
      return;

    // Clear values for 2 attachments (Visible, All)
    VkClearValue clearValues[2];
    clearValues[0].color = {0.0f, 0.0f, 0.0f, 0.0f};
    clearValues[1].color = {0.0f, 0.0f, 0.0f, 0.0f};

    VkRenderingAttachmentInfo colorAtt[2];

    // Attachment 0: Visible Mask
    colorAtt[0] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAtt[0].imageView = m_outlineMaskVisibleView->handle();
    colorAtt[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt[0].clearValue = clearValues[0];

    // Attachment 1: All Mask
    colorAtt[1] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAtt[1].imageView = m_outlineMaskAllView->handle();
    colorAtt[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt[1].clearValue = clearValues[1];

    // No Depth Attachment (Manual Compare)

    VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea = {{0, 0}, {extent.width, extent.height}};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 2; // MRT
    renderInfo.pColorAttachments = colorAtt;
    renderInfo.pDepthAttachment = nullptr;

    DxvkResourceBufferInfo paletteBufferInfo =
        ensureShadowMatrixBuffer(ctx, input);
    if (paletteBufferInfo.buffer == VK_NULL_HANDLE)
      return;

    const uint32_t objectBase = m_shadowMatrixObjectBase;
    const uint32_t casterCount =
        static_cast<uint32_t>(input.scene.shadowCasters.size());

    ctx->cmdBeginRendering(&renderInfo);

    VkViewport vp = {};
    vp.x = float(input.scene.worldCamera.viewport.X);
    vp.y = float(input.scene.worldCamera.viewport.Y) +
           float(input.scene.worldCamera.viewport.Height);
    vp.width = float(input.scene.worldCamera.viewport.Width);
    vp.height = -float(input.scene.worldCamera.viewport.Height);
    vp.minDepth = input.scene.worldCamera.viewport.MinZ;
    vp.maxDepth = input.scene.worldCamera.viewport.MaxZ;
    ctx->cmdSetViewport(1, &vp);

    VkRect2D scissor = {{0, 0}, {extent.width, extent.height}};
    ctx->cmdSetScissor(1, &scissor);

    for (uint32_t drawIdx = 0; drawIdx < casterCount; drawIdx++) {
      const auto &draw = input.scene.shadowCasters[drawIdx];
      if (!War3RenderState::IsOutlineHandle(draw.batchHandle)) {
        continue;
      }

      ShadowCasterPipelineKey key = {};
      key.positionFormat = draw.positionFormat;
      key.positionStride = draw.positionStride;
      key.positionOffset = draw.positionOffset;
      key.topology = draw.topology;
      key.alphaTestEnabled = draw.alphaTestEnabled && draw.diffuseTexture &&
                             draw.HasUsableUvBinding();
      if (key.alphaTestEnabled) {
        key.uvFormat = draw.uvFormat;
        key.uvOffset = draw.uvOffset;
        key.uvStride = draw.uvStride;
        key.uvBinding = draw.uvBinding;
      }
      key.outlineMode = 0;

      if (draw.vertexBlendEnabled && draw.vertexBlendCount > 0) {
        key.blendWeightFormat = draw.blendWeightFormat;
        key.blendWeightOffset = draw.blendWeightOffset;
      } else {
        key.blendWeightFormat = VK_FORMAT_UNDEFINED;
        key.blendWeightOffset = 0;
      }

      if (draw.vertexBlendEnabled && draw.vertexBlendIndexed) {
        key.blendIndexFormat = draw.blendIndexFormat;
        key.blendIndexOffset = draw.blendIndexOffset;
      } else {
        key.blendIndexFormat = VK_FORMAT_UNDEFINED;
        key.blendIndexOffset = 0;
      }

      key.blendBinding = draw.blendBinding;
      key.blendStride = draw.blendStride;

      // Use a separate pipeline map for MRT pipelines
      auto it = m_outlineMaskMRTPipelines.find(key);
      VkPipeline pipeline = VK_NULL_HANDLE;

      if (it != m_outlineMaskMRTPipelines.end()) {
        pipeline = it->second;
      } else {
        // Create MRT Pipeline
        auto p = createOutlineMaskPipeline(key);
        pipeline = p.pipeline;
        m_outlineMaskMRTPipelines.insert({key, pipeline});
      }

      if (pipeline == VK_NULL_HANDLE) {
        continue;
      }

      ShadowCasterPushConstants pc = {};
      pc.blendCount = draw.vertexBlendCount;
      pc.flags = 0u;
      pc.mvp = input.scene.worldCamera.viewProj;

      if (draw.vertexBlendEnabled) {
        pc.flags |= 0x1u;
        if (draw.vertexBlendIndexed)
          pc.flags |= 0x2u;
        pc.paletteOffset = draw.paletteIndex * 256u;
      } else {
        pc.paletteOffset = objectBase + drawIdx;
      }

      if (key.alphaTestEnabled) {
        pc.flags |= 0x4u;
        pc.alphaRef = draw.alphaRef;
        pc.samplerIndex = draw.diffuseSamplerIndex;
      }

      ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                           VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

      // 资源布局固定为 0..4；binding2 继续保持场景深度，3/4 在描边
      // 尚未启用 direct 时绑定有效矩阵 storage 作为失效关闭兜底。
      std::array<DxvkDescriptorWrite, 5> descriptors = {};
      descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      descriptors[0].buffer = paletteBufferInfo;

      if (key.alphaTestEnabled) {
        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = &draw.textureDescriptor;
        ctx->track(draw.diffuseTexture->image(), DxvkAccess::Read);
      } else {
        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = nullptr;
      }

      // [NEW] Bind Scene Depth (m_depthCopyView)
      descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptors[2].descriptor = m_depthCopyView->getDescriptor();
      descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      descriptors[3].buffer = paletteBufferInfo;
      descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      descriptors[4].buffer = paletteBufferInfo;

      ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_outlineMaskLayout,
                         descriptors.size(), descriptors.data(), sizeof(pc),
                         &pc);

      if (draw.positionStorage.ptr() != nullptr)
        ctx->track(draw.positionStorage);
      if (draw.indexStorage.ptr() != nullptr &&
          draw.indexStorage.ptr() != draw.positionStorage.ptr())
        ctx->track(draw.indexStorage);
      if (draw.blendStorage.ptr() != nullptr)
        ctx->track(draw.blendStorage);
      if (key.alphaTestEnabled && draw.uvBinding != 0u &&
          draw.uvStorage.ptr() != nullptr &&
          draw.uvStorage.ptr() != draw.positionStorage.ptr() &&
          draw.uvStorage.ptr() != draw.blendStorage.ptr())
        ctx->track(draw.uvStorage);

      VkBuffer vbs[2];
      VkDeviceSize offsets[2];
      VkDeviceSize sizes[2];
      VkDeviceSize strides[2];
      uint32_t vbCount = 1;

      vbs[0] = draw.positionInfo.buffer;
      offsets[0] = draw.positionInfo.offset;
      sizes[0] = draw.positionInfo.size;
      strides[0] = draw.positionStride;

      if (draw.blendBinding == 1) {
        vbCount = 2;
        vbs[1] = draw.blendInfo.buffer;
        offsets[1] = draw.blendInfo.offset;
        sizes[1] = draw.blendInfo.size;
        strides[1] = draw.blendStride;
      } else if (key.alphaTestEnabled && draw.uvBinding == 1u) {
        vbCount = 2;
        vbs[1] = draw.uvInfo.buffer;
        offsets[1] = draw.uvInfo.offset;
        sizes[1] = draw.uvInfo.size;
        strides[1] = draw.uvStride;
      }

      ctx->cmdBindVertexBuffers(0, vbCount, vbs, offsets, sizes, strides);
      if (key.alphaTestEnabled && draw.uvBinding == 2u) {
        const VkBuffer uvBuffer = draw.uvInfo.buffer;
        const VkDeviceSize uvOffset = draw.uvInfo.offset;
        const VkDeviceSize uvSize = draw.uvInfo.size;
        const VkDeviceSize uvStride = draw.uvStride;
        ctx->cmdBindVertexBuffers(2u, 1u, &uvBuffer, &uvOffset, &uvSize,
                                  &uvStride);
      }

      // [FIX] Restore batchHandle to TLS so D3D9Device::OnDraw can identify the
      // object for filtering
      War3RenderState::SetTlsBatchHandle(draw.batchHandle);

      if (draw.indexed) {
        ctx->cmdBindIndexBuffer2(draw.indexInfo.buffer, draw.indexInfo.offset,
                                 draw.indexInfo.size, draw.indexType);
        ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
                            draw.vertexOffset, 0);
      } else {
        ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);
      }

      // Reset handle to avoid leaking to other draws
      War3RenderState::SetTlsBatchHandle(0);
    }

    ctx->cmdEndRendering();

    // Track Outputs
    ctx->track(m_outlineMaskVisibleView->image(), DxvkAccess::Write);
    ctx->track(m_outlineMaskAllView->image(), DxvkAccess::Write);

    // Track Input Depth
    ctx->track(m_depthCopyView->image(), DxvkAccess::Read);
  };

  drawMaskPassMRT();

  // 遮罩写入后：切回 SHADER_READ
  auto transitionMaskToRead = [&](const Rc<DxvkImageView> &view) {
    VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = view->getLayout();
    barrier.image = view->image()->handle();
    barrier.subresourceRange = view->imageSubresources();

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  };

  transitionMaskToRead(m_outlineMaskVisibleView);
  transitionMaskToRead(m_outlineMaskAllView);

  // ===== 边缘检测并合成到主颜色 =====
  if (!m_outlineEdgeLayout) {
    std::array<DxvkDescriptorSetLayoutBinding, 2> bindings = {
        DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                       VK_SHADER_STAGE_FRAGMENT_BIT),
        DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                       VK_SHADER_STAGE_FRAGMENT_BIT),
    };
    m_outlineEdgeLayout = m_device->createBuiltInPipelineLayout(
        DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_FRAGMENT_BIT,
        sizeof(OutlineEdgePushConstants), bindings.size(), bindings.data());
  }

  OutlineEdgePipelineKey edgeKey = {};
  edgeKey.format = colorFormat;
  VkPipeline edgePipeline = VK_NULL_HANDLE;
  auto edgeIt = m_outlineEdgePipelines.find(edgeKey);
  if (edgeIt != m_outlineEdgePipelines.end()) {
    edgePipeline = edgeIt->second;
  } else {
    VkPipelineColorBlendAttachmentState blendAtt = {};
    blendAtt.blendEnable = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.colorBlendOp = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAtt.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    util::DxvkBuiltInGraphicsState state = {};
    state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
    state.fs = util::DxvkBuiltInShaderStage(war3_outline_edge, nullptr);
    state.colorFormat = colorFormat;
    state.cbAttachment = &blendAtt;

    edgePipeline =
        m_device->createBuiltInGraphicsPipeline(m_outlineEdgeLayout, state);
    m_outlineEdgePipelines.insert({edgeKey, edgePipeline});
  }

  if (edgePipeline == VK_NULL_HANDLE) {
    return;
  }

  if (!outline.showVisible && !outline.showOccluded) {
    static bool s_logNoOutline = false;
    if (!s_logNoOutline) {
      s_logNoOutline = true;
      WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: outline disabled (no "
                      "visible/occluded)\n");
    }
    return;
  }

  VkRenderingAttachmentInfo colorAtt = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  colorAtt.imageView = colorView->handle();
  colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea = {{0, 0}, {extent.width, extent.height}};
  renderInfo.layerCount = 1;
  renderInfo.colorAttachmentCount = 1;
  renderInfo.pColorAttachments = &colorAtt;

  ctx->cmdBeginRendering(&renderInfo);

  VkViewport vp = {};
  vp.x = float(input.scene.worldCamera.viewport.X);
  vp.y = float(input.scene.worldCamera.viewport.Y) +
         float(input.scene.worldCamera.viewport.Height);
  vp.width = float(input.scene.worldCamera.viewport.Width);
  vp.height = -float(input.scene.worldCamera.viewport.Height);
  vp.minDepth = 0.0f;
  vp.maxDepth = 1.0f;
  ctx->cmdSetViewport(1, &vp);

  VkRect2D scissor = {{0, 0}, {extent.width, extent.height}};
  ctx->cmdSetScissor(1, &scissor);

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                       VK_PIPELINE_BIND_POINT_GRAPHICS, edgePipeline);

  std::array<DxvkDescriptorWrite, 2> edgeDescriptors = {};
  edgeDescriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  edgeDescriptors[0].descriptor = m_outlineMaskVisibleView->getDescriptor();
  edgeDescriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  edgeDescriptors[1].descriptor = m_outlineMaskAllView->getDescriptor();

  OutlineEdgePushConstants pc = {};
  pc.visibleSampler = m_shadowSamplerActive->getDescriptor().samplerIndex;
  pc.allSampler = m_shadowSamplerActive->getDescriptor().samplerIndex;
  pc.invWidth = 1.0f / float(extent.width);
  pc.invHeight = 1.0f / float(extent.height);
  pc.widthPx = (std::max)(1.0f, outline.widthPx);
  pc.showVisible = outline.showVisible ? 1u : 0u;
  pc.showOccluded = outline.showOccluded ? 1u : 0u;
  pc.color[0] = outline.colorR;
  pc.color[1] = outline.colorG;
  pc.color[2] = outline.colorB;
  pc.color[3] = outline.colorA;

  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_outlineEdgeLayout,
                     edgeDescriptors.size(), edgeDescriptors.data(), sizeof(pc),
                     &pc);

  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  ctx->track(colorView->image(), DxvkAccess::Write);
  ctx->track(m_outlineMaskVisibleView->image(), DxvkAccess::Read);
  ctx->track(m_outlineMaskAllView->image(), DxvkAccess::Read);
  ctx->track(m_shadowSamplerActive);

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    WAR3_RENDER_LOG(
        "DXVK War3ShadowReceiverPass: outline screen-space enabled units=%u\n",
        unitCount);
  }
}

void War3ShadowReceiverPass::renderUnitOutline(const Rc<DxvkCommandList> &ctx,
                                               const War3PipelineInput &input) {
  if (war3::ShaderManager::get().hasOverride(
          war3shader::RenderStageId::Outline)) {
    // 使用自定义 Outline 材质时，跳过内置描边路径，避免重复绘制
    static bool s_loggedSkip = false;
    if (!s_loggedSkip && War3RenderState::HasOutlineHandles()) {
      s_loggedSkip = true;
      WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: outline override active, "
                      "skip built-in (handles=%u)\n",
                      War3RenderState::GetOutlineHandleCount());
      if (!war3dbg::RenderLogEnabled()) {
        war3dbg::Print(
            "DXVK_Outline: override active, skip built-in (handles=%u)\n",
            War3RenderState::GetOutlineHandleCount());
      }
    }
    return;
  }
  War3RenderSettings defaultSettings = {};
  const War3RenderSettings *settings =
      input.settings ? input.settings : &defaultSettings;
  if (settings->occludedOutline.useScreenSpace) {
    renderUnitOutlineScreenSpace(ctx, input);
    return;
  }

  if (!War3RenderState::HasOutlineHandles()) {
    return;
  }

  // 统计需要渲染的单位数量（仅描边表内的单位）
  uint32_t unitCount = 0;
  for (const auto &caster : input.scene.shadowCasters) {
    if (War3RenderState::IsOutlineHandle(caster.batchHandle)) {
      unitCount++;
    }
  }

  if (unitCount == 0) {
    static bool s_loggedNoMatch = false;
    if (!s_loggedNoMatch) {
      s_loggedNoMatch = true;
      WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: outline handles present "
                      "but no draw match (handles=%u)\n",
                      War3RenderState::GetOutlineHandleCount());
    }
    if (!war3dbg::RenderLogEnabled()) {
      static uint32_t s_outlineNoMatchLogs = 0;
      if (s_outlineNoMatchLogs++ < 3) {
        war3dbg::Print("DXVK_Outline: no draw match (handles=%u, casters=%u)\n",
                       War3RenderState::GetOutlineHandleCount(),
                       static_cast<uint32_t>(input.scene.shadowCasters.size()));
      }
    }
    return;
  }

  static bool s_loggedOutline = false;
  if (!s_loggedOutline) {
    s_loggedOutline = true;
    WAR3_RENDER_LOG(
        "DXVK War3ShadowReceiverPass: renderUnitOutline FIRST_CALL units=%u\n",
        unitCount);
  }

  // 获取描边颜色
  const float outlineR = settings->occludedOutline.colorR;
  const float outlineG = settings->occludedOutline.colorG;
  const float outlineB = settings->occludedOutline.colorB;
  const float outlineA = settings->occludedOutline.colorA;
  const War3OutlineMode outlineMode = settings->occludedOutline.mode;

  // 获取相机 ViewProj
  const Matrix4 &viewProj = input.scene.worldCamera.viewProj;

  // 获取颜色视图和深度视图
  const auto &colorView = input.colorView;
  const auto &depthView = input.depthView;

  if (!colorView || !depthView) {
    return;
  }

  VkExtent3D extent = colorView->mipLevelExtent(0u);
  VkFormat colorFormat = colorView->image()->info().format;
  VkFormat depthFormat = depthView->image()->info().format;

  // 确保管线布局存在
  if (!m_outlineLayout) {
    m_outlineLayout = m_shadowCasterLayout; // 复用 shadow caster 布局
  }

  // 设置渲染区域
  VkRenderingAttachmentInfo colorAtt = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  colorAtt.imageView = colorView->handle();
  colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // 保留现有颜色
  colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingAttachmentInfo depthAtt = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depthAtt.imageView = depthView->handle();
  depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // 使用现有深度（只读）
  depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_NONE; // 不写入深度

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea = {{0, 0}, {extent.width, extent.height}};
  renderInfo.layerCount = 1;
  renderInfo.colorAttachmentCount = 1;
  renderInfo.pColorAttachments = &colorAtt;
  renderInfo.pDepthAttachment = &depthAtt;

  // 确保骨骼调色板 SSBO 已上传（描边可能在阴影关闭时单独启用）
  DxvkResourceBufferInfo paletteBufferInfo =
      ensureShadowMatrixBuffer(ctx, input);
  if (paletteBufferInfo.buffer == VK_NULL_HANDLE) {
    return;
  }

  // 开始渲染
  ctx->cmdBeginRendering(&renderInfo);

  // 设置视口和裁剪
  VkViewport vp = {};
  vp.x = float(input.scene.worldCamera.viewport.X);
  vp.y = float(input.scene.worldCamera.viewport.Y) +
         float(input.scene.worldCamera.viewport.Height);
  vp.width = float(input.scene.worldCamera.viewport.Width);
  vp.height = -float(input.scene.worldCamera.viewport.Height);
  vp.minDepth = 0.0f;
  vp.maxDepth = 1.0f;
  ctx->cmdSetViewport(1, &vp);

  VkRect2D scissor = {{0, 0}, {extent.width, extent.height}};
  ctx->cmdSetScissor(1, &scissor);

  uint32_t drawnUnits = 0;

  // 遍历单位并渲染描边
  for (const auto &draw : input.scene.shadowCasters) {
    // 只渲染描边表命中（batchTag 不再强依赖 WorldObjects，避免遗漏）
    if (!War3RenderState::IsOutlineHandle(draw.batchHandle)) {
      continue;
    }

    // 构建管线 key
    ShadowCasterPipelineKey key = {};
    key.positionFormat = draw.positionFormat;
    key.positionStride = draw.positionStride;
    key.positionOffset = draw.positionOffset;
    key.topology = draw.topology;
    key.alphaTestEnabled = draw.alphaTestEnabled && draw.diffuseTexture &&
                           draw.HasUsableUvBinding();
    if (key.alphaTestEnabled) {
      key.uvFormat = draw.uvFormat;
      key.uvOffset = draw.uvOffset;
      key.uvStride = draw.uvStride;
      key.uvBinding = draw.uvBinding;
    }
    key.outlineMode = static_cast<uint8_t>(outlineMode);

    if (draw.vertexBlendEnabled && draw.vertexBlendCount > 0) {
      key.blendWeightFormat = draw.blendWeightFormat;
      key.blendWeightOffset = draw.blendWeightOffset;
    } else {
      key.blendWeightFormat = VK_FORMAT_UNDEFINED;
      key.blendWeightOffset = 0;
    }

    if (draw.vertexBlendEnabled && draw.vertexBlendIndexed) {
      key.blendIndexFormat = draw.blendIndexFormat;
      key.blendIndexOffset = draw.blendIndexOffset;
    } else {
      key.blendIndexFormat = VK_FORMAT_UNDEFINED;
      key.blendIndexOffset = 0;
    }

    key.blendBinding = draw.blendBinding;
    key.blendStride = draw.blendStride;

    // 查找或创建描边管线
    auto it = m_outlinePipelines.find(key);
    VkPipeline outlinePipeline = VK_NULL_HANDLE;

    if (it != m_outlinePipelines.end()) {
      outlinePipeline = it->second;
    } else {
      // 创建描边管线（与 shadow caster 类似，但深度测试 = GREATER，有颜色输出）
      VkVertexInputBindingDescription bindings[3] = {};
      uint32_t bindingCount = 1;

      bindings[0].binding = 0;
      bindings[0].stride = key.positionStride;
      bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

      if (key.blendBinding == 1) {
        bindings[bindingCount].binding = 1;
        bindings[bindingCount].stride = key.blendStride;
        bindings[bindingCount].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        ++bindingCount;
      }

      if (key.alphaTestEnabled && key.uvBinding != 0u &&
          key.uvBinding != key.blendBinding) {
        bindings[bindingCount].binding = key.uvBinding;
        bindings[bindingCount].stride = key.uvStride;
        bindings[bindingCount].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        ++bindingCount;
      }

      // Alpha-tested skinned outline draws may need position + blend weight +
      // blend index + UV. Keep the outline VI layout capacity in sync with the
      // caster path to avoid corrupting the stack when all four are present.
      std::array<VkVertexInputAttributeDescription, 4> attributes = {};
      uint32_t attributeCount = 0;

      // 位置 (location = 0)
      attributes[attributeCount].location = 0;
      attributes[attributeCount].binding = 0;
      attributes[attributeCount].format = key.positionFormat;
      attributes[attributeCount].offset = key.positionOffset;
      attributeCount++;

      // 混合权重 (location = 1)
      if (key.blendWeightFormat != VK_FORMAT_UNDEFINED) {
        attributes[attributeCount].location = 1;
        attributes[attributeCount].binding = key.blendBinding;
        attributes[attributeCount].format = key.blendWeightFormat;
        attributes[attributeCount].offset = key.blendWeightOffset;
        attributeCount++;
      }

      // 混合索引 (location = 2)
      if (key.blendIndexFormat != VK_FORMAT_UNDEFINED) {
        attributes[attributeCount].location = 2;
        attributes[attributeCount].binding = key.blendBinding;
        attributes[attributeCount].format = key.blendIndexFormat;
        attributes[attributeCount].offset = key.blendIndexOffset;
        attributeCount++;
      }

      // UV (location = 3)
      if (key.alphaTestEnabled && key.uvFormat != VK_FORMAT_UNDEFINED) {
        attributes[attributeCount].location = 3;
        attributes[attributeCount].binding = key.uvBinding;
        attributes[attributeCount].format = key.uvFormat;
        attributes[attributeCount].offset = key.uvOffset;
        attributeCount++;
      }

      VkPipelineVertexInputStateCreateInfo viState = {
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      viState.vertexBindingDescriptionCount = bindingCount;
      viState.pVertexBindingDescriptions = bindings;
      viState.vertexAttributeDescriptionCount = attributeCount;
      viState.pVertexAttributeDescriptions = attributes.data();

      VkPipelineInputAssemblyStateCreateInfo iaState = {
          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      iaState.topology = key.topology;
      iaState.primitiveRestartEnable = VK_FALSE;

      VkPipelineRasterizationStateCreateInfo rsState = {
          VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rsState.depthClampEnable = VK_FALSE;
      rsState.rasterizerDiscardEnable = VK_FALSE;
      rsState.polygonMode = VK_POLYGON_MODE_FILL;
      rsState.cullMode =
          VK_CULL_MODE_FRONT_BIT; // Cull front faces, draw back faces only for
                                  // outline silhouette
      rsState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
      const bool silhouette = outlineMode == War3OutlineMode::Silhouette;
      if (silhouette) {
        // 轮廓模式：轻微正向深度偏移，避免薄片/单面模型整块填充
        rsState.depthBiasEnable = VK_TRUE;
        rsState.depthBiasConstantFactor = 1.0f;
        rsState.depthBiasSlopeFactor = 0.5f;
      } else {
        // 遮挡高亮：强负偏移，防止自遮挡
        rsState.depthBiasEnable = VK_TRUE;
        rsState.depthBiasConstantFactor = -100.0f;
        rsState.depthBiasSlopeFactor = -2.0f;
      }
      rsState.lineWidth = 1.0f;

      // 关键：轮廓模式使用 LESS_OR_EQUAL（只绘制轮廓外扩部分）
      // 被遮挡高亮使用 GREATER（只绘制遮挡区域）
      VkPipelineDepthStencilStateCreateInfo dsState = {
          VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      dsState.depthTestEnable = VK_TRUE;
      dsState.depthWriteEnable = VK_FALSE; // 不写入深度
      dsState.depthCompareOp =
          silhouette ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER;
      dsState.depthBoundsTestEnable = VK_FALSE;
      dsState.stencilTestEnable = VK_FALSE;

      // 颜色混合（Alpha 混合）
      VkPipelineColorBlendAttachmentState blendAtt = {};
      blendAtt.blendEnable = VK_TRUE;
      blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blendAtt.colorBlendOp = VK_BLEND_OP_ADD;
      blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
      blendAtt.colorWriteMask =
          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

      util::DxvkBuiltInGraphicsState state = {};
      state.vs = util::DxvkBuiltInShaderStage(
          war3_outline_expand_vert,
          nullptr); // 使用专用 Outline VS (带剪裁空间扩展)
      state.fs = util::DxvkBuiltInShaderStage(war3_unit_outline,
                                              nullptr); // 使用 Outline FS
      state.colorFormat = colorFormat;
      state.depthFormat = depthFormat;
      state.viState = &viState;
      state.iaState = &iaState;
      state.rsState = &rsState;
      state.dsState = &dsState;
      state.cbAttachment = &blendAtt;

      outlinePipeline =
          m_device->createBuiltInGraphicsPipeline(m_outlineLayout, state);
      m_outlinePipelines.insert({key, outlinePipeline});

      WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: created outline pipeline "
                      "posF=%u topology=%u alpha=%d mode=%u\n",
                      uint32_t(key.positionFormat), uint32_t(key.topology),
                      key.alphaTestEnabled, uint32_t(key.outlineMode));
    }

    if (outlinePipeline == VK_NULL_HANDLE) {
      continue;
    }

    // Push constants
    ShadowCasterPushConstants pc = {};
    pc.paletteOffset = draw.paletteIndex * 256u;
    pc.blendCount = draw.vertexBlendCount;
    pc.flags = 0u;
    pc.outlineColor[0] = outlineR;
    pc.outlineColor[1] = outlineG;
    pc.outlineColor[2] = outlineB;
    pc.outlineColor[3] = outlineA;

    if (draw.vertexBlendEnabled) {
      pc.flags |= 0x1u;
      if (draw.vertexBlendIndexed)
        pc.flags |= 0x2u;
      pc.mvp = viewProj; // 使用相机 ViewProj
    } else {
      pc.mvp = viewProj * draw.worldMatrix;
    }

    if (key.alphaTestEnabled) {
      pc.flags |= 0x4u;
      pc.alphaRef = draw.alphaRef;
      pc.samplerIndex = draw.diffuseSamplerIndex;
    }

    // 绑定管线
    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                         VK_PIPELINE_BIND_POINT_GRAPHICS, outlinePipeline);

    // 绑定资源
    std::array<DxvkDescriptorWrite, 5> descriptors = {};
    descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptors[0].buffer = paletteBufferInfo;
    // Binding 1: Alpha Texture
    if (key.alphaTestEnabled) {
      descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptors[1].descriptor =
          &draw.textureDescriptor; // Need to ensure this is set?
      ctx->track(draw.diffuseTexture->image(), DxvkAccess::Read);
    } else {
      descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptors[1].descriptor = nullptr;
    }
    descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptors[2].descriptor = nullptr;
    descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptors[3].buffer = paletteBufferInfo;
    descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptors[4].buffer = paletteBufferInfo;

    ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_outlineLayout,
                       descriptors.size(), descriptors.data(), sizeof(pc), &pc);

    // 追踪资源
    if (draw.positionStorage.ptr() != nullptr)
      ctx->track(draw.positionStorage);
    if (draw.indexStorage.ptr() != nullptr &&
        draw.indexStorage.ptr() != draw.positionStorage.ptr())
      ctx->track(draw.indexStorage);
    if (draw.blendStorage.ptr() != nullptr)
      ctx->track(draw.blendStorage);
    if (key.alphaTestEnabled && draw.uvBinding != 0u &&
        draw.uvStorage.ptr() != nullptr &&
        draw.uvStorage.ptr() != draw.positionStorage.ptr() &&
        draw.uvStorage.ptr() != draw.blendStorage.ptr())
      ctx->track(draw.uvStorage);

    // 绑定顶点缓冲区
    VkBuffer vbs[2];
    VkDeviceSize offsets[2];
    VkDeviceSize sizes[2];
    VkDeviceSize strides[2];
    uint32_t vbCount = 1;

    vbs[0] = draw.positionInfo.buffer;
    offsets[0] = draw.positionInfo.offset;
    sizes[0] = draw.positionInfo.size;
    strides[0] = draw.positionStride;

    if (draw.blendBinding == 1) {
      vbCount = 2;
      vbs[1] = draw.blendInfo.buffer;
      offsets[1] = draw.blendInfo.offset;
      sizes[1] = draw.blendInfo.size;
      strides[1] = draw.blendStride;
    } else if (key.alphaTestEnabled && draw.uvBinding == 1u) {
      vbCount = 2;
      vbs[1] = draw.uvInfo.buffer;
      offsets[1] = draw.uvInfo.offset;
      sizes[1] = draw.uvInfo.size;
      strides[1] = draw.uvStride;
    }

    ctx->cmdBindVertexBuffers(0, vbCount, vbs, offsets, sizes, strides);
    if (key.alphaTestEnabled && draw.uvBinding == 2u) {
      const VkBuffer uvBuffer = draw.uvInfo.buffer;
      const VkDeviceSize uvOffset = draw.uvInfo.offset;
      const VkDeviceSize uvSize = draw.uvInfo.size;
      const VkDeviceSize uvStride = draw.uvStride;
      ctx->cmdBindVertexBuffers(2u, 1u, &uvBuffer, &uvOffset, &uvSize,
                                &uvStride);
    }

    // 绘制
    // [FIX] Restore batchHandle to TLS
    War3RenderState::SetTlsBatchHandle(draw.batchHandle);

    if (draw.indexed) {
      ctx->cmdBindIndexBuffer2(draw.indexInfo.buffer, draw.indexInfo.offset,
                               draw.indexInfo.size, draw.indexType);
      ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
                          draw.vertexOffset, 0);
    } else {
      ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);
    }

    // Reset handle
    War3RenderState::SetTlsBatchHandle(0);

    drawnUnits++;
  }

  ctx->cmdEndRendering();

  // 资源追踪
  ctx->track(colorView->image(), DxvkAccess::Write);
  ctx->track(depthView->image(), DxvkAccess::Read);

  static uint32_t s_logCount = 0;
  if (s_logCount < 5) {
    s_logCount++;
    WAR3_RENDER_LOG(
        "DXVK War3ShadowReceiverPass: renderUnitOutline drawnUnits=%u\n",
        drawnUnits);
  }
}


} // namespace dxvk
