#pragma once

#include "../../../dxso/dxso_reader.h"
#include <cstdint>
#include <d3d9.h>
#include <vector>

namespace dxvk {
namespace war3 {
namespace reimpl {

class War3ShaderPatcher {
public:
  // Analyzes and potentially patches the vertex shader bytecode.
  // Returns true if patched, and updates outBytecode/outSize.
  // If no patch needed, returns false.
  static bool PatchVertexShader(const DWORD *pFunction,
                                std::vector<DWORD> &outBytecode);

  static void SetShaderInstanced(IDirect3DVertexShader9 *shader,
                                 bool instanced);
  static bool IsShaderInstanced(IDirect3DVertexShader9 *shader);

private:
  static bool IsStandardUnitShader(const uint32_t *tokens, size_t count);
  // static void GenerateInstancedShader(...);
};

} // namespace reimpl
} // namespace war3
} // namespace dxvk
