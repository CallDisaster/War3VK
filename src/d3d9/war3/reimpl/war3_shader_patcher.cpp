#include "war3_shader_patcher.h"
#include "../../d3d9_shader.h"
#include "../../d3d9_war3_debug.h"

namespace dxvk {
namespace war3 {
namespace reimpl {

// Helper to extract register type
// Type bits: 0-2 come from token[28:30], 3-4 come from token[11:12]
static uint32_t GetRegisterType(uint32_t token) {
  return ((token & 0x70000000) >> 28) | ((token & 0x00001800) >> 8);
}

// Helper to extract register number
static uint32_t GetRegisterNum(uint32_t token) { return token & 0x000007ff; }

// Helper to modify register token
static uint32_t ModifyRegisterToken(uint32_t token, uint32_t newType,
                                    uint32_t newNum) {
  uint32_t maskType = 0x70001800;
  uint32_t maskNum = 0x000007ff;
  uint32_t cleanToken = token & ~(maskType | maskNum);

  uint32_t valType = (newType & 0x7) << 28;
  uint32_t valTypeH = ((newType >> 3) & 0x3) << 11;

  return cleanToken | valType | valTypeH | (newNum & maskNum);
}

// Helper to get opcode length (simplified for VS 2.0/3.0)
static uint32_t GetInstructionLength(uint32_t token, uint32_t majorVer) {
  DxsoOpcode opcode = static_cast<DxsoOpcode>(token & 0xFFFF);
  if (opcode == DxsoOpcode::Comment) {
    return (token & 0x7fff0000) >> 16;
  }
  if (opcode == DxsoOpcode::End)
    return 0;
  if (majorVer >= 2) {
    return (token & 0x0f000000) >> 24;
  }
  return 0;
}

bool War3ShaderPatcher::PatchVertexShader(const DWORD *pOriginalBytecode,
                                          std::vector<DWORD> &outBytecode) {

  const uint32_t *tokens =
      reinterpret_cast<const uint32_t *>(pOriginalBytecode);

  // Calculate size by looking for End token (0x0000FFFF)
  size_t tokenCount = 0;
  while (true) {
    // Safety check just in case
    if (tokenCount > 65536)
      return false;

    if ((tokens[tokenCount] & 0xFFFF) == 0xFFFF) {
      tokenCount++; // Include End token
      break;
    }
    tokenCount++;
  }

  if (tokenCount < 2)
    return false;

  uint32_t versionToken = tokens[0];
  uint32_t majorVer = (versionToken >> 8) & 0xFF;

  // Only patch VS 2.0+
  if (majorVer < 2)
    return false;

  if (!IsStandardUnitShader(
          reinterpret_cast<const uint32_t *>(pOriginalBytecode), tokenCount)) {
    return false;
  }

  static bool s_logged = false;
  if (!s_logged) {
    WAR3_RENDER_LOG("[ShaderPatcher] Patching Vertex Shader for Instancing!\n");
    s_logged = true;
  }

  // Start building new bytecode
  outBytecode.clear();
  outBytecode.reserve(tokenCount + 25); // Reserve extra space for new dcls

  outBytecode.push_back(versionToken);

  // Helper to add simple Dcl instruction for VS 2.0
  auto AddDcl = [&](uint32_t usage, uint32_t usageIndex, uint32_t regNum) {
    outBytecode.push_back(0x0200005F); // Dcl Opcode (Len 2)

    // Usage (0-4 bits), UsageIndex (16-19 bits)
    uint32_t semantic = (usage & 0xF) | ((usageIndex & 0xF) << 16);
    outBytecode.push_back(semantic);

    // Dest Register: Type Input(1), Num(regNum), Mask(0xF)
    // Type 1 -> 0x10000000
    // Mask F -> 0x000F0000
    uint32_t dest = 0x100F0000 | (regNum & 0x7FF);
    outBytecode.push_back(dest);
  };

  // Insert Dcls for v8-v13 (TexCoord 1-6)
  // v8-v11: WorldMatrix rows (4x float4 = 64 bytes)
  // v12: Color (float4 = 16 bytes)
  // v13: Extra (float4, .x = teamColorIndex, .yzw = padding)
  AddDcl(5, 1, 8);
  AddDcl(5, 2, 9);
  AddDcl(5, 3, 10);
  AddDcl(5, 4, 11);
  AddDcl(5, 5, 12);
  AddDcl(5, 6, 13); // TEXCOORD6 -> v13, teamColorIndex

  // Copy loop
  size_t i = 1; // Start after Version
  while (i < tokenCount) {
    uint32_t token = tokens[i];
    DxsoOpcode opcode = static_cast<DxsoOpcode>(token & 0xFFFF);

    if (opcode == DxsoOpcode::End) {
      outBytecode.push_back(token);
      break;
    }

    uint32_t len = GetInstructionLength(token, majorVer);
    if (len == 0 && opcode != DxsoOpcode::End) {
      outBytecode.push_back(token); // Fallback
      i++;
      continue;
    }

    // Copy Opcode token
    outBytecode.push_back(token);

    // Process arguments
    for (size_t k = 0; k < len; k++) {
      uint32_t argToken = tokens[i + 1 + k];
      uint32_t regType = GetRegisterType(argToken);
      uint32_t regNum = GetRegisterNum(argToken);

      if (regType == 2 /* Const */) {
        if (regNum >= 14 && regNum <= 17) {
          // Map 14->8 .. 17->11
          argToken =
              ModifyRegisterToken(argToken, 1 /* Input */, regNum - 14 + 8);
        } else if (regNum == 11) {
          // Map 11->12
          argToken = ModifyRegisterToken(argToken, 1 /* Input */, 12);
        }
      }
      outBytecode.push_back(argToken);
    }

    i += (1 + len);
  }

  return true;
}

bool War3ShaderPatcher::IsStandardUnitShader(const uint32_t *tokens,
                                             size_t count) {
  if (count < 2)
    return false;
  uint32_t versionToken = tokens[0];
  uint32_t majorVer = (versionToken >> 8) & 0xFF;
  if (majorVer < 2)
    return false;

  bool usesC14_17 = false;
  bool usesC4_10 = false;
  bool usesC11 = false;

  size_t i = 1;
  while (i < count) {
    uint32_t token = tokens[i];
    DxsoOpcode opcode = static_cast<DxsoOpcode>(token & 0xFFFF);
    if (opcode == DxsoOpcode::End)
      break;

    uint32_t len = GetInstructionLength(token, majorVer);
    if (len == 0) {
      i++;
      continue;
    }

    for (size_t k = 0; k < len; k++) {
      uint32_t argToken = tokens[i + 1 + k];
      uint32_t regType = GetRegisterType(argToken);
      uint32_t regNum = GetRegisterNum(argToken);

      if (regType == 2 /* Const */) {
        if (regNum >= 14 && regNum <= 17)
          usesC14_17 = true;
        if (regNum >= 4 && regNum <= 10)
          usesC4_10 = true;
        if (regNum == 11)
          usesC11 = true;
      }
    }
    i += (1 + len);
  }

  return usesC14_17 && usesC11 && !usesC4_10;
}

void War3ShaderPatcher::SetShaderInstanced(IDirect3DVertexShader9 *shader,
                                           bool instanced) {
  if (!shader)
    return;
  // Safe cast to D3D9VertexShader because we are in DXVK backend
  auto *vs = static_cast<D3D9VertexShader *>(shader);
  vs->SetInstanced(instanced);
}

bool War3ShaderPatcher::IsShaderInstanced(IDirect3DVertexShader9 *shader) {
  if (!shader)
    return false;
  auto *vs = static_cast<D3D9VertexShader *>(shader);
  return vs->IsInstanced();
}

} // namespace reimpl
} // namespace war3
} // namespace dxvk
