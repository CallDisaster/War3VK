#include "d3d9_shader.h"

#include "d3d9_caps.h"
#include "d3d9_device.h"
#include "d3d9_util.h"

namespace dxvk {

  D3D9CommonShader::D3D9CommonShader() {}

  D3D9CommonShader::D3D9CommonShader(const DxsoCompiledModule& module)
  : m_isgn(module.isgn), m_usedSamplers(module.usedSamplers),
    m_usedRTs(module.usedRTs), m_textureTypes(module.textureTypes),
    m_info(module.info), m_meta(module.meta), m_constants(module.constants),
    m_maxDefinedFloatConst(module.maxDefinedFloat),
    m_maxDefinedIntConst(module.maxDefinedInt),
    m_maxDefinedBoolConst(module.maxDefinedBool), m_shader(module.shader) {
    // Preserve the existing wrapper ABI and D3D9 VS sampler numbering.
    if (m_info.shaderStage() == VK_SHADER_STAGE_VERTEX_BIT)
      m_usedSamplers <<= FirstVSSamplerSlot;
  }


  void D3D9ShaderModuleSet::GetShaderModule(
            D3D9DeviceEx*         pDevice,
            D3D9CommonShader*     pShaderModule,
            uint32_t*             pLength,
            VkShaderStageFlagBits ShaderStage,
      const DxsoModuleInfo*       pDxbcModuleInfo,
      const void*                 pShaderBytecode) {
    DxsoReader reader(
      reinterpret_cast<const char*>(pShaderBytecode));

    DxsoModule module(reader);

    if (unlikely(module.info().shaderStage() != ShaderStage))
      throw DxvkError("GetShaderModule: Bytecode does not match shader stage");

    auto* options = pDevice->GetOptions();
    const bool d3d8Compat = pDevice->IsD3D8Compatible();
    const bool allowSm3InD3D8 = options->allowSm3InD3D8;

    const uint32_t majorVersion = module.info().majorVersion();
    const uint32_t minorVersion = module.info().minorVersion();

    // Vertex shader version checks
    if (ShaderStage == VK_SHADER_STAGE_VERTEX_BIT) {
      // Late fixed-function capable hardware exposed support for VS 1.1
      const uint32_t shaderModelVS = (d3d8Compat && !allowSm3InD3D8)
        ? 1u
        : std::max(1u, options->shaderModel);

      if (unlikely(majorVersion > shaderModelVS
               || (majorVersion == 1 && minorVersion > 1)
               // Skip checking the SM2 minor version, as it has a 2_x mode apparently
               || (majorVersion == 3 && minorVersion != 0))) {
        throw DxvkError(str::format("GetShaderModule: Unsupported VS version ", majorVersion, ".", minorVersion));
      }
    // Pixel shader version checks
    } else if (ShaderStage == VK_SHADER_STAGE_FRAGMENT_BIT) {
      const uint32_t shaderModelPS = (d3d8Compat && !allowSm3InD3D8)
        ? std::min(1u, options->shaderModel)
        : options->shaderModel;

      if (unlikely(majorVersion > shaderModelPS
               || (majorVersion == 1 && minorVersion > 4)
               // Skip checking the SM2 minor version, as it has a 2_x mode apparently
               || (majorVersion == 3 && minorVersion != 0))) {
        throw DxvkError(str::format("GetShaderModule: Unsupported PS version ", majorVersion, ".", minorVersion));
      }
    } else {
      throw DxvkError("GetShaderModule: Unsupported shader stage");
    }

    DxsoAnalysisInfo info = module.analyze();
    *pLength = info.bytecodeByteLength;

    const DxsoShaderModuleDomain domain { pDevice->GetDXVKDevice().ptr(),
      *pDxbcModuleInfo, pDevice->GetVertexConstantLayout(), pDevice->GetPixelConstantLayout() };
    DxsoShaderModuleCache* cache;
    {
      std::unique_lock<dxvk::mutex> lock(m_mutex);
      if (!m_modules) {
        // The cache is owned by this device, so this raw registration target
        // cannot outlive it and does not introduce an Rc ownership cycle.
        auto* device = pDevice->GetDXVKDevice().ptr();
        m_modules = std::make_unique<DxsoShaderModuleCache>(domain,
          [device](const Rc<DxvkShader>& shader) { device->registerShader(shader); });
      }
      cache = m_modules.get();
    }
    const std::string& dumpPath = options->shaderDumpPath;
    const auto beforeCompile = [&](const std::string& name) {
      Logger::debug(str::format("Compiling shader ", name));
      if (dumpPath.empty())
        return;
      DxsoReader dumpReader(reinterpret_cast<const char*>(pShaderBytecode));
      dumpReader.store(std::ofstream(str::topath(str::format(dumpPath, "/", name, ".dxso").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc), info.bytecodeByteLength);
      char comment[2048];
      Com<ID3DBlob> blob;
      if (SUCCEEDED(DisassembleShader(pShaderBytecode, TRUE, comment, &blob))) {
        std::ofstream out(str::topath(str::format(dumpPath, "/", name, ".dxso.dis").c_str()).c_str(),
          std::ios_base::binary | std::ios_base::trunc);
        out.write(reinterpret_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
      }
    };
    const auto validate = [&](const DxsoCompiledModule& compiled, bool newlyCompiled) {
    if (newlyCompiled && !dumpPath.empty()) {
      std::ofstream dumpStream(str::topath(str::format(dumpPath, "/", compiled.shader->debugName(), ".spv").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc);
      compiled.shader->dump(dumpStream);
    }
    const int32_t maxFloatConstantIndex = compiled.maxDefinedFloat;
    const int32_t maxIntConstantIndex = compiled.maxDefinedInt;
    const int32_t maxBoolConstantIndex = compiled.maxDefinedBool;

    // Vertex shader specific validations. These validations are not
    // performed on SWVP devices or on MIXED devices, even if
    // SetSoftwareVertexProcessing(FALSE) is used to disable SWVP mode.
    if (!pDevice->CanSWVP() && ShaderStage == VK_SHADER_STAGE_VERTEX_BIT) {
      // Validate the float constant value advertised in pCaps->MaxFloatConstantsVS for HWVP.
      if (unlikely(maxFloatConstantIndex > static_cast<int32_t>(caps::MaxFloatConstantsVS - 1)))
        throw DxvkError(str::format("GetShaderModule: Invalid VS float constant index ", maxFloatConstantIndex));
      // Validate the integer constant value advertised in pCaps->MaxOtherConstants for HWVP.
      if (unlikely(maxIntConstantIndex > static_cast<int32_t>(caps::MaxOtherConstants - 1)))
        throw DxvkError(str::format("GetShaderModule: Invalid VS int constant index ", maxIntConstantIndex));
      // Validate the bool constant value advertised in pCaps->MaxOtherConstants for HWVP.
      if (unlikely(maxBoolConstantIndex > static_cast<int32_t>(caps::MaxOtherConstants - 1)))
        throw DxvkError(str::format("GetShaderModule: Invalid VS bool constant index ", maxBoolConstantIndex));
    // Pixel shader specific validations.
    } else if (ShaderStage == VK_SHADER_STAGE_FRAGMENT_BIT) {
      const bool isSM2XOrNewer = majorVersion == 3 || (majorVersion == 2 && minorVersion != 0);
      // Pixel shader model version 2_x has the same limits here as version 2_0
      const uint32_t maxFloatConstantsPS = majorVersion == 3 ? caps::MaxSM3FloatConstantsPS :
                                           majorVersion == 2 ? caps::MaxSM2FloatConstantsPS :
                                           caps::MaxSM1FloatConstantsPS;
      // Validate the float constant value coresponding to the supported shader model version.
      if (unlikely(!pDevice->CanSWVP() && maxFloatConstantIndex > static_cast<int32_t>(maxFloatConstantsPS - 1)))
        throw DxvkError(str::format("GetShaderModule: Invalid PS float constant index ", maxFloatConstantIndex));
      // Pixel shaders below version 2_x can not use integer constants, not even in SWVP/MIXED mode
      if (unlikely(!isSM2XOrNewer && maxIntConstantIndex != -1))
        throw DxvkError("GetShaderModule: Invalid use of PS int constant");
      // Validate the integer constant value advertised in pCaps->MaxOtherConstants for HWVP.
      else if (unlikely(isSM2XOrNewer && !pDevice->CanSWVP() &&
                        maxIntConstantIndex > static_cast<int32_t>(caps::MaxOtherConstants - 1)))
        throw DxvkError(str::format("GetShaderModule: Invalid PS int constant index ", maxIntConstantIndex));
      // Pixel shaders below version 2_x can not use bool constants, not even in SWVP/MIXED mode
      if (unlikely(!isSM2XOrNewer && maxBoolConstantIndex != -1))
        throw DxvkError("GetShaderModule: Invalid use of PS bool constant");
      // Validate the bool constant value advertised in pCaps->MaxOtherConstants for HWVP.
      else if (unlikely(isSM2XOrNewer && !pDevice->CanSWVP() &&
                        maxBoolConstantIndex > static_cast<int32_t>(caps::MaxOtherConstants - 1)))
        throw DxvkError(str::format("GetShaderModule: Invalid PS bool constant index ", maxBoolConstantIndex));
    }
    };
    const auto compiled = cache->getOrCompile(domain, ShaderStage, pShaderBytecode,
      info.bytecodeByteLength, validate, beforeCompile);
    *pShaderModule = D3D9CommonShader(*compiled);
  }

}
