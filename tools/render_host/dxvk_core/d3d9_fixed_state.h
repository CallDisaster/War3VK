#pragma once
#include "../d3d9_ff_state.h"
#include "../../../src/d3d9/d3d9_fixed_function_keys.h"

namespace warvk::host::dxvkcore {
// Adapter for the SAME production key builder. These pointers are stack-local
// views owned by the x64 render thread, never serialized or retained.
struct FixedDeclarationView {
  const D3DVERTEXELEMENT9* elements;uint32_t count;
  bool TestFlag(dxvk::D3D9VertexDeclFlag flag) const {
    for(uint32_t i=0;i<count;++i) {
      const auto& e=elements[i];
      switch(flag) {
        case dxvk::D3D9VertexDeclFlag::HasPositionT:if(e.Usage==D3DDECLUSAGE_POSITIONT)return true;break;
        case dxvk::D3D9VertexDeclFlag::HasColor0:if(e.Usage==D3DDECLUSAGE_COLOR&&!e.UsageIndex)return true;break;
        case dxvk::D3D9VertexDeclFlag::HasColor1:if(e.Usage==D3DDECLUSAGE_COLOR&&e.UsageIndex==1)return true;break;
        case dxvk::D3D9VertexDeclFlag::HasPointSize:if(e.Usage==D3DDECLUSAGE_PSIZE)return true;break;
        case dxvk::D3D9VertexDeclFlag::HasFog:if(e.Usage==D3DDECLUSAGE_FOG)return true;break;
        default:break;
      }
    }return false;
  }
  uint32_t GetTexcoordMask() const {
    uint32_t mask=0;
    for(uint32_t i=0;i<count;++i) {
      const auto& e=elements[i];if(e.Usage!=D3DDECLUSAGE_TEXCOORD||e.UsageIndex>=8)continue;
      const auto n=e.Type<=D3DDECLTYPE_FLOAT4?e.Type+1:
        e.Type==D3DDECLTYPE_SHORT2||e.Type==D3DDECLTYPE_SHORT2N||e.Type==D3DDECLTYPE_USHORT2N||e.Type==D3DDECLTYPE_FLOAT16_2?2:4;
      mask|=n<<(e.UsageIndex*3);
    }return mask;
  }
};
struct FixedKeyView {
  const FixedDeclarationView* vertexDecl;
  std::array<DWORD,256> renderStates{};
  std::array<std::array<DWORD,32>,8> textureStages{};
  std::array<const void*,8> textures{};
  std::array<uint32_t,8> enabledLightIndices{};
  FixedKeyView(const d9::FixedFunctionState& state,const FixedDeclarationView& decl,uint32_t textureMask):vertexDecl(&decl) {
    for(size_t i=0;i<d9::FFRenderStates.size();++i)renderStates[d9::FFRenderStates[i]]=state.render[i];
    for(size_t s=0;s<8;++s)for(size_t i=0;i<d9::FFStageStates.size();++i)
      textureStages[s][dxvk::RemapTextureStageStateType(d9::FFStageStates[i])]=state.stages[s][i];
    for(uint32_t i=0;i<8;++i)textures[i]=(textureMask&(1u<<i))?&state:nullptr;
    for(uint32_t i=0;i<8;++i)enabledLightIndices[i]=i<state.lightCount?i:UINT32_MAX;
  }
};
inline dxvk::Vector4 FixedColor(DWORD c) {
  return dxvk::Vector4(float((c>>16)&255)/255,float((c>>8)&255)/255,float(c&255)/255,float(c>>24)/255);
}
// The two matrix inverses of the vertex block depend only on the world and view matrices, which the game
// changes far less often than it draws. A bitwise-equal input gives the same output, so a block filled
// from this cache is byte-identical to one filled by FillFixedFunctionTransforms.
struct FixedTransformCache {
  dxvk::Matrix4 world,view,worldView,normal,inverseView;
  bool viewValid=false,worldValid=false;
};
inline void FillFixedVertex(void* mapped,const d9::FixedFunctionState& state,
    const dxvk::D3D9FFShaderKeyVS& key,const D3DVIEWPORT9& vp,bool zTest,FixedTransformCache* cache=nullptr) {
  using namespace dxvk;
  // Same mapped uniform layout and transform convention as UpdateFixedFunctionVS.
  std::memset(mapped,0,sizeof(D3D9FixedFunctionVS));
  auto* data=reinterpret_cast<D3D9FixedFunctionVS*>(mapped);
  const Matrix4 world(state.transforms[0].m),view(state.transforms[1].m),projection(state.transforms[2].m);
  std::array<Matrix4,8> textures;
  for(unsigned i=0;i<8;++i)textures[i]=Matrix4(state.transforms[i+3].m);
  D3D9ViewportInfo viewport;
  viewport.inverseExtent=Vector4(2.0f/float(vp.Width),-2.0f/float(vp.Height),zTest?1.0f:0.0f,1.0f);
  viewport.inverseOffset=Vector4(-float(vp.X),-float(vp.Y),0,0)*viewport.inverseExtent+Vector4(-1,1,0,0);
  if(cache) {
    if(!cache->viewValid||std::memcmp(&cache->view,&view,sizeof(Matrix4))) {
      cache->view=view;cache->inverseView=transpose(inverse(view));cache->viewValid=true;cache->worldValid=false;
    }
    if(!cache->worldValid||std::memcmp(&cache->world,&world,sizeof(Matrix4))) {
      cache->world=world;cache->worldView=view*world;cache->normal=inverse(cache->worldView);cache->worldValid=true;
    }
    data->WorldView=cache->worldView;data->NormalMatrix=cache->normal;data->InverseView=cache->inverseView;
    data->Projection=projection;
    for(uint32_t i=0;i<data->TexcoordMatrices.size();++i)data->TexcoordMatrices[i]=textures[i];
    data->ViewportInfo=viewport;
  } else FillFixedFunctionTransforms(data,world,view,projection,textures.data(),viewport);
  data->GlobalAmbient=FixedColor(state.render[d9::FFIndex(d9::FFRenderStates,D3DRS_AMBIENT)]);
  for(uint32_t i=0;i<state.lightCount;++i)data->Lights[i]=D3D9Light(state.lights[i],view);
  data->Material=state.material;data->TweenFactor=0;data->Key=key.Data;
}
inline void FillFixedPixel(void* mapped,const d9::FixedFunctionState& state,const dxvk::D3D9FFShaderKeyFS& key) {
  dxvk::D3D9FixedFunctionPS data;
  data.textureFactor=FixedColor(state.render[d9::FFIndex(d9::FFRenderStates,D3DRS_TEXTUREFACTOR)]);
  data.Key=key;std::memcpy(mapped,&data,sizeof(data));
}
}
