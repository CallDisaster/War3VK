// war3_shaderpack_internal.h - ShaderPack 运行时内部接口
#pragma once

#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_cmdlist.h"

#include <vector>

namespace dxvk {
  struct War3PipelineInput;
}

namespace war3shader::internal {

// 初始化 Vulkan ShaderPack 运行时
void InitShaderPackRuntime(const dxvk::Rc<dxvk::DxvkDevice>& device);

// 每帧执行 ShaderPack（composite/final）
void RunShaderPackPasses(const dxvk::Rc<dxvk::DxvkCommandList>& ctx,
                         const dxvk::War3PipelineInput& input);

// 获取 Vulkan ShadowPack 的 receiver shader（SPIR-V）
const std::vector<uint32_t>* GetShadowReceiverSpirv();

} // namespace war3shader::internal
