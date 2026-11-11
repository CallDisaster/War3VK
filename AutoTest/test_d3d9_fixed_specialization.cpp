// Differential oracle frozen from the native pre-extraction UpdateFixedFunctionPS.
// The production helper is compiled with real key/spec types in BOTH widths.
#include "../src/d3d9/d3d9_fixed_function_keys.h"
#include <cstdio>
#include <cstring>
using namespace dxvk;
static bool Reference(D3D9SpecializationInfo& m_specInfo,const D3D9FFShaderKeyFS& key,uint32_t specular) {
  struct { std::array<uint32_t,256> renderStates{}; } m_state;
  m_state.renderStates[D3DRS_SPECULARENABLE]=specular;
    // Spec constants...
    uint32_t activeTextureStageCount;
    for (activeTextureStageCount = 0;
         activeTextureStageCount < caps::TextureStageCount;
         activeTextureStageCount++) {
      auto &stage = key.Stages[activeTextureStageCount].Contents;
      if (stage.ColorOp == D3DTOP_DISABLE)
        break;
    }

    const auto repackArg = [](uint32_t arg) {
      return (arg & 0b111u) | ((arg & 0b110000u) >> 1u);
    };

    uint32_t lastActiveTextureStage = std::max(activeTextureStageCount, 1u) -
                                      1u; // Subtract 1 to make it fit 3 bits
    bool dirty =
        m_specInfo.set<D3D9SpecConstantId::SpecFFLastActiveTextureStage>(
            lastActiveTextureStage);
    dirty |= m_specInfo.set<D3D9SpecConstantId::SpecFFGlobalSpecularEnabled>(
        m_state.renderStates[D3DRS_SPECULARENABLE]);
    constexpr uint32_t perTextureStageSpecConsts =
        static_cast<uint32_t>(D3D9SpecConstantId::SpecFFTextureStage1ColorOp) -
        static_cast<uint32_t>(D3D9SpecConstantId::SpecFFTextureStage0ColorOp);
    for (uint32_t i = 0; i < 4; i++) {
      if (i <= activeTextureStageCount) {
        dirty |=
            m_specInfo.set(static_cast<D3D9SpecConstantId>(
                               D3D9SpecConstantId::SpecFFTextureStage0ColorOp +
                               perTextureStageSpecConsts * i),
                           key.Stages[i].Contents.ColorOp);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ColorArg1 +
                perTextureStageSpecConsts * i),
            repackArg(key.Stages[i].Contents.ColorArg1));
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ColorArg2 +
                perTextureStageSpecConsts * i),
            repackArg(key.Stages[i].Contents.ColorArg2));
        dirty |=
            m_specInfo.set(static_cast<D3D9SpecConstantId>(
                               D3D9SpecConstantId::SpecFFTextureStage0AlphaOp +
                               perTextureStageSpecConsts * i),
                           key.Stages[i].Contents.AlphaOp);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0AlphaArg1 +
                perTextureStageSpecConsts * i),
            repackArg(key.Stages[i].Contents.AlphaArg1));
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0AlphaArg2 +
                perTextureStageSpecConsts * i),
            repackArg(key.Stages[i].Contents.AlphaArg2));
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ResultIsTemp +
                perTextureStageSpecConsts * i),
            key.Stages[i].Contents.ResultIsTemp);
      } else {
        dirty |=
            m_specInfo.set(static_cast<D3D9SpecConstantId>(
                               D3D9SpecConstantId::SpecFFTextureStage0ColorOp +
                               perTextureStageSpecConsts * i),
                           0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ColorArg1 +
                perTextureStageSpecConsts * i),
            0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ColorArg2 +
                perTextureStageSpecConsts * i),
            0);
        dirty |=
            m_specInfo.set(static_cast<D3D9SpecConstantId>(
                               D3D9SpecConstantId::SpecFFTextureStage0AlphaOp +
                               perTextureStageSpecConsts * i),
                           0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0AlphaArg1 +
                perTextureStageSpecConsts * i),
            0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0AlphaArg2 +
                perTextureStageSpecConsts * i),
            0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ResultIsTemp +
                perTextureStageSpecConsts * i),
            0);
      }
    }

  return dirty;
}
int main() {
  uint32_t seed=0x4a519932u,checks=0;
  auto next=[&](){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
  for(uint32_t n=0;n<100000;++n) {
    D3D9FFShaderKeyFS key{};
    auto* bytes=reinterpret_cast<unsigned char*>(&key);
    for(size_t i=0;i<sizeof(key);++i)bytes[i]=uint8_t(next());
    for(uint32_t i=0;i<8;++i)key.Stages[i].Contents.ColorOp=i<n%9?D3DTOP_MODULATE:D3DTOP_DISABLE;
    D3D9SpecializationInfo expected,actual;
    for(auto& word:expected.data)word=next();actual=expected;
    const uint32_t specular=next()&1;
    bool x=Reference(expected,key,specular),y=UpdateFixedFunctionSpecialization(actual,key,specular);
    if(x!=y||actual.data!=expected.data)return 1;
    ++checks;
    if(UpdateFixedFunctionSpecialization(actual,key,specular))return 2;
    ++checks;
  }
  std::printf("fixed specialization checks=%u failures=0 bits=%zu\n",checks,sizeof(void*)*8);
}
