#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace dxvk::war3::render {

// Frozen offline trial candidates. This matcher is NOT a render authorization.
// Callers must additionally prove current mounted content, node mapping,
// lifetime, world scope and evaluated values before any lighting consumption.
struct NativeModelLightShadowCandidate {
  std::string_view path;
  std::string_view sha256;
  uint32_t bytes;
  uint32_t lightObjectId;
};

inline constexpr std::array<NativeModelLightShadowCandidate, 11>
kNativeModelLightShadowCandidates = {{
  {"Doodads\\Cityscape\\Props\\LanternPost\\LanternPost.mdx", "FDF026E90C0543D550186B8A8D568A4C0A21474E8587785E15D1A5FA56F875E8", 7144u, 4u},
  {"Doodads\\LordaeronSummer\\Props\\TorchHuman\\TorchHuman.mdx", "EBBA55E0DD1E7B156C884121C0A35B88120E02F7C3BAA743B1DC06DE5A08A16A", 4343u, 2u},
  {"Doodads\\LordaeronSummer\\Props\\TorchHumanOmni\\TorchHumanOmni.mdx", "25B05840E6B77EE3C34DF00E2A49C7B0214B5E15063C0A4B21EA111BE57EA23C", 4343u, 2u},
  {"Doodads\\LordaeronSummer\\Props\\LanternPost\\LanternPost1.mdx", "945F43D452B5398E61A813E464E8673C4A5D18402FB42A3D8A30171EC3995CF3", 10650u, 8u},
  {"Doodads\\Cityscape\\Structures\\City_LowWall_TallEndCapWithLantern\\City_LowWall_TallEndCapWithLantern.mdx", "D16B129AE1055A891BFF74ABABB2CB91C40C29C31040D83E26500FDA5075B305", 4868u, 1u},
  {"Doodads\\LordaeronSummer\\Props\\brazierOmni\\brazierOmni.mdx", "59EA41E965317FE8664ECE3B37D00F348F79A2BAA38259A448D2C2611A6CFEB9", 5367u, 1u},
  {"Doodads\\Village\\Props\\Village_Lightpost\\Village_Lightpost.mdx", "382203CCBEAB88A9BB10CC1571EC9C24FF548A69303D27A1A5FE12B40FCC79CA", 5876u, 5u},
  {"Doodads\\LordaeronSummer\\Props\\LanternPost\\LanternPost0.mdx", "C6FD6DBDB38653AD1673608EFFC3642A1AE17C5C279CF22F43BA28F828FC033D", 10730u, 8u},
  {"Doodads\\LordaeronSummer\\Props\\TorchHuman\\TorchHuman.mdx", "98D1A8C63A13AC5C6FCD863EB0DF906ED096A6750941DEBC5F05CFE59BE81B6C", 4343u, 2u},
  {"Doodads\\LordaeronSummer\\Props\\brazierOmni\\brazierOmni.mdx", "AE326158DAC1FB880E9C59D7F29387B5BD1A737EB76FAE9F91F668E9E531160B", 5367u, 1u},
  {"Doodads\\Ruins\\Props\\Firepot\\Firepot.mdx", "D05461D94414B833DC1334A8DDF9E66D6825EC110C7C1226619D4A63629F0F59", 15815u, 1u},
}};

inline char NativeLightPathChar(char ch) noexcept {
  if (ch == '/') return '\\';
  if (ch >= 'A' && ch <= 'Z') return char(ch + ('a' - 'A'));
  return ch;
}

inline bool NativeLightSamePath(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (NativeLightPathChar(a[i]) != NativeLightPathChar(b[i])) return false;
  return true;
}

inline const NativeModelLightShadowCandidate* FindNativeLightShadowCandidate(
    std::string_view path, std::string_view sha256, uint32_t bytes,
    uint32_t mdlObjectId) noexcept {
  if (path.empty() || sha256.size() != 64 || !bytes) return nullptr;
  for (const auto& entry : kNativeModelLightShadowCandidates)
    if (entry.bytes == bytes && entry.lightObjectId == mdlObjectId &&
        entry.sha256 == sha256 && NativeLightSamePath(entry.path, path))
      return &entry;
  return nullptr;
}

} // namespace dxvk::war3::render
