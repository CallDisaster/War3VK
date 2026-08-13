#pragma once

namespace war3shader::policy {

  // Loading raw SPIR-V from disk is a developer diagnostic capability, not a
  // release feature.  There is intentionally no environment-variable path:
  // only a separately compiled developer binary may opt in.
#if defined(WARVK_ENABLE_RAW_SHADERPACK_DEV) && \
    WARVK_ENABLE_RAW_SHADERPACK_DEV
  inline constexpr bool kRawShaderPackEnabled = true;
#else
  inline constexpr bool kRawShaderPackEnabled = false;
#endif

  inline constexpr const char* kReleaseDisabledMessage =
      "Raw SPIR-V ShaderPack loading is disabled in release builds";

}
