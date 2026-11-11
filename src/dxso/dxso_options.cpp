#include "dxso_options.h"

namespace dxvk {

  DxsoOptions::DxsoOptions() {}

  DxsoOptions::DxsoOptions(const Rc<DxvkDevice>& device, const D3D9Options& options,
                           uint32_t vertexFloatConstantBytes) {

    const Rc<DxvkAdapter> adapter = device->adapter();

    const DxvkDeviceFeatures& devFeatures = device->features();
    const DxvkDeviceInfo& devInfo = adapter->deviceProperties();

    // Apply shader-related options
    strictConstantCopies = options.strictConstantCopies;

    strictPow            = options.strictPow;
    d3d9FloatEmulation   = options.d3d9FloatEmulation;

    invariantPosition    = options.invariantPosition;

    forceSamplerTypeSpecConstants = options.forceSamplerTypeSpecConstants;
    forceSampleRateShading = options.forceSampleRateShading;

    vertexFloatConstantBufferAsSSBO = vertexFloatConstantBytes > devInfo.core.properties.limits.maxUniformBufferRange;

    robustness2Supported = devFeatures.extRobustness2.robustBufferAccess2;

    sincosEmulation     = device->getShaderCompileOptions().flags.test(DxvkShaderCompileFlag::LowerSinCos);
  }

}
