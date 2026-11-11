#pragma once
// The original production key rules. Both native and remote owners supply
// resolved local state; no pointer or compiler bitfield is an IPC contract.
#include "d3d9_fixed_function.h"
#include "d3d9_spec_constants.h"
#include <limits>
namespace dxvk {
// The uber shader reads dynamic keys until an optimized pipeline is ready.
// Both paths must describe the same texture operations. Preserve the native
// packing, including the first disabled stage, rather than leaving spec zero.
inline bool UpdateFixedFunctionSpecialization(D3D9SpecializationInfo& spec,
    const D3D9FFShaderKeyFS& key,uint32_t specular) {
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
      spec.set<D3D9SpecConstantId::SpecFFLastActiveTextureStage>(
          lastActiveTextureStage);
  dirty |= spec.set<D3D9SpecConstantId::SpecFFGlobalSpecularEnabled>(
      specular);
  constexpr uint32_t perTextureStageSpecConsts =
      static_cast<uint32_t>(D3D9SpecConstantId::SpecFFTextureStage1ColorOp) -
      static_cast<uint32_t>(D3D9SpecConstantId::SpecFFTextureStage0ColorOp);
  for (uint32_t i = 0; i < 4; i++) {
    if (i <= activeTextureStageCount) {
      dirty |=
          spec.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0ColorOp +
                             perTextureStageSpecConsts * i),
                         key.Stages[i].Contents.ColorOp);
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0ColorArg1 +
              perTextureStageSpecConsts * i),
          repackArg(key.Stages[i].Contents.ColorArg1));
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0ColorArg2 +
              perTextureStageSpecConsts * i),
          repackArg(key.Stages[i].Contents.ColorArg2));
      dirty |=
          spec.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0AlphaOp +
                             perTextureStageSpecConsts * i),
                         key.Stages[i].Contents.AlphaOp);
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0AlphaArg1 +
              perTextureStageSpecConsts * i),
          repackArg(key.Stages[i].Contents.AlphaArg1));
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0AlphaArg2 +
              perTextureStageSpecConsts * i),
          repackArg(key.Stages[i].Contents.AlphaArg2));
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0ResultIsTemp +
              perTextureStageSpecConsts * i),
          key.Stages[i].Contents.ResultIsTemp);
    } else {
      dirty |=
          spec.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0ColorOp +
                             perTextureStageSpecConsts * i),
                         0);
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0ColorArg1 +
              perTextureStageSpecConsts * i),
          0);
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0ColorArg2 +
              perTextureStageSpecConsts * i),
          0);
      dirty |=
          spec.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0AlphaOp +
                             perTextureStageSpecConsts * i),
                         0);
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0AlphaArg1 +
              perTextureStageSpecConsts * i),
          0);
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0AlphaArg2 +
              perTextureStageSpecConsts * i),
          0);
      dirty |= spec.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0ResultIsTemp +
              perTextureStageSpecConsts * i),
          0);
    }
  }
  return dirty;
}
inline void FillFixedFunctionTransforms(D3D9FixedFunctionVS* data,
    const Matrix4& world,const Matrix4& view,const Matrix4& projection,
    const Matrix4* textureMatrices,const D3D9ViewportInfo& viewport) {
  data->WorldView=view*world;
  data->NormalMatrix=inverse(data->WorldView);
  data->InverseView=transpose(inverse(view));
  data->Projection=projection;
  for(uint32_t i=0;i<data->TexcoordMatrices.size();++i)data->TexcoordMatrices[i]=textureMatrices[i];
  data->ViewportInfo=viewport;
}
template<class State>
D3D9FFShaderKeyVS BuildFixedFunctionVSKey(const State& m_state, bool m_nativeLightSplit,
    D3D9FF_VertexBlendMode vertexBlendMode, bool indexedVertexBlend) {
  D3D9FFShaderKeyVS key;
  key.Data.Contents.NativeLightSplit = m_nativeLightSplit;
  key.Data.Contents.VertexHasPositionT =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
  key.Data.Contents.VertexHasColor0 =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor0);
  key.Data.Contents.VertexHasColor1 =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor1);
  key.Data.Contents.VertexHasPointSize =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPointSize);
  key.Data.Contents.VertexHasFog =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasFog);

  bool lighting = m_state.renderStates[D3DRS_LIGHTING] != 0 &&
                  !key.Data.Contents.VertexHasPositionT;
  bool colorVertex = m_state.renderStates[D3DRS_COLORVERTEX] != 0;
  uint32_t mask =
      (lighting && colorVertex)
          ? (key.Data.Contents.VertexHasColor0 ? D3DMCS_COLOR1
                                               : D3DMCS_MATERIAL) |
                (key.Data.Contents.VertexHasColor1 ? D3DMCS_COLOR2
                                                   : D3DMCS_MATERIAL)
          : 0;

  key.Data.Contents.UseLighting = lighting;
  key.Data.Contents.NormalizeNormals =
      m_state.renderStates[D3DRS_NORMALIZENORMALS];
  key.Data.Contents.LocalViewer =
      m_state.renderStates[D3DRS_LOCALVIEWER] && lighting;

  key.Data.Contents.RangeFog = m_state.renderStates[D3DRS_RANGEFOGENABLE];

  key.Data.Contents.DiffuseSource =
      m_state.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] & mask;
  key.Data.Contents.AmbientSource =
      m_state.renderStates[D3DRS_AMBIENTMATERIALSOURCE] & mask;
  key.Data.Contents.SpecularSource =
      m_state.renderStates[D3DRS_SPECULARMATERIALSOURCE] & mask;
  key.Data.Contents.EmissiveSource =
      m_state.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] & mask;

  key.Data.Contents.SpecularEnabled =
      m_state.renderStates[D3DRS_SPECULARENABLE];

  uint32_t lightCount = 0;

  if (key.Data.Contents.UseLighting) {
    for (uint32_t i = 0; i < caps::MaxEnabledLights; i++) {
      if (m_state.enabledLightIndices[i] !=
          std::numeric_limits<uint32_t>::max())
        lightCount++;
    }
  }

  key.Data.Contents.LightCount = lightCount;

  for (uint32_t i = 0; i < caps::MaxTextureBlendStages; i++) {
    uint32_t transformFlags =
        m_state.textureStages[i][DXVK_TSS_TEXTURETRANSFORMFLAGS] &
        ~(D3DTTFF_PROJECTED);
    uint32_t index = m_state.textureStages[i][DXVK_TSS_TEXCOORDINDEX];
    uint32_t indexFlags = (index & TCIMask) >> TCIOffset;

    transformFlags &= 0b111;
    index &= 0b111;

    key.Data.Contents.TransformFlags |= transformFlags << (i * 3);
    key.Data.Contents.TexcoordFlags |= indexFlags << (i * 3);
    key.Data.Contents.TexcoordIndices |= index << (i * 3);
  }

  key.Data.Contents.VertexTexcoordDeclMask =
      m_state.vertexDecl != nullptr ? m_state.vertexDecl->GetTexcoordMask() : 0;

  key.Data.Contents.VertexBlendMode = uint32_t(vertexBlendMode);

  if (vertexBlendMode == D3D9FF_VertexBlendMode_Normal) {
    key.Data.Contents.VertexBlendIndexed = indexedVertexBlend;
    key.Data.Contents.VertexBlendCount =
        m_state.renderStates[D3DRS_VERTEXBLEND] & 0xff;
  }

  key.Data.Contents.VertexClipping =
      m_state.renderStates[D3DRS_CLIPPLANEENABLE] != 0;

  return key;
}
template<class State>
D3D9FFShaderKeyFS BuildFixedFunctionFSKey(const State& m_state, bool m_nativeLightSplit) {
  // Used args for a given operation.
  auto ArgsMask = [](DWORD Op) {
    switch (Op) {
    case D3DTOP_DISABLE:
      return 0b000u; // No Args
    case D3DTOP_SELECTARG1:
    case D3DTOP_PREMODULATE:
      return 0b010u; // Arg 1
    case D3DTOP_SELECTARG2:
      return 0b100u; // Arg 2
    case D3DTOP_MULTIPLYADD:
    case D3DTOP_LERP:
      return 0b111u; // Arg 0, 1, 2
    default:
      return 0b110u; // Arg 1, 2
    }
  };

  D3D9FFShaderKeyFS key;

  uint32_t activeTextureStageCount = 0;
  for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
    auto &stage = key.Stages[i].Contents;
    auto &data = m_state.textureStages[i];

    // Subsequent stages do not occur if this is true.
    if (data[DXVK_TSS_COLOROP] == D3DTOP_DISABLE)
      break;

    // If the stage is invalid (ie. no texture bound),
    // this and all subsequent stages get disabled.
    if (m_state.textures[i] == nullptr) {
      if (((data[DXVK_TSS_COLORARG0] & D3DTA_SELECTMASK) == D3DTA_TEXTURE &&
           (ArgsMask(data[DXVK_TSS_COLOROP]) & (1 << 0u))) ||
          ((data[DXVK_TSS_COLORARG1] & D3DTA_SELECTMASK) == D3DTA_TEXTURE &&
           (ArgsMask(data[DXVK_TSS_COLOROP]) & (1 << 1u))) ||
          ((data[DXVK_TSS_COLORARG2] & D3DTA_SELECTMASK) == D3DTA_TEXTURE &&
           (ArgsMask(data[DXVK_TSS_COLOROP]) & (1 << 2u))))
        break;
    }

    stage.ColorOp = data[DXVK_TSS_COLOROP];
    stage.AlphaOp = data[DXVK_TSS_ALPHAOP];

    stage.ColorArg0 = data[DXVK_TSS_COLORARG0];
    stage.ColorArg1 = data[DXVK_TSS_COLORARG1];
    stage.ColorArg2 = data[DXVK_TSS_COLORARG2];

    stage.AlphaArg0 = data[DXVK_TSS_ALPHAARG0];
    stage.AlphaArg1 = data[DXVK_TSS_ALPHAARG1];
    stage.AlphaArg2 = data[DXVK_TSS_ALPHAARG2];

    stage.ResultIsTemp = data[DXVK_TSS_RESULTARG] == D3DTA_TEMP;

    activeTextureStageCount = i + 1;
  }

  auto &stage0 = key.Stages[0].Contents;

  if (stage0.ResultIsTemp && stage0.ColorOp != D3DTOP_DISABLE &&
      stage0.AlphaOp == D3DTOP_DISABLE) {
    stage0.AlphaOp = D3DTOP_SELECTARG1;
    stage0.AlphaArg1 = D3DTA_DIFFUSE;
  }

  stage0.GlobalSpecularEnable = m_state.renderStates[D3DRS_SPECULARENABLE];
  stage0.NativeLightSplit = m_nativeLightSplit;

  // The last stage *always* writes to current.
  if (activeTextureStageCount >= 1)
    key.Stages[activeTextureStageCount - 1].Contents.ResultIsTemp = false;

  return key;
}
}

