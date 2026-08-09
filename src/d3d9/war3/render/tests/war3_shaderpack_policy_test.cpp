#include "../../../war3_shaderpack_policy.h"

#include <iostream>

int main() {
#if defined(WARVK_EXPECT_RAW_SHADERPACK_DEV) && \
    WARVK_EXPECT_RAW_SHADERPACK_DEV
  if (!war3shader::policy::kRawShaderPackEnabled) {
    std::cerr << "developer policy build did not enable raw ShaderPack\n";
    return 1;
  }
#else
  if (war3shader::policy::kRawShaderPackEnabled) {
    std::cerr << "release/default policy unexpectedly enabled ShaderPack\n";
    return 1;
  }
#endif

  std::cout << "war3_shaderpack_policy_test: PASS\n";
  return 0;
}
