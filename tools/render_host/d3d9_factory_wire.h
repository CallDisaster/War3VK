#pragma once
#include "d3d9_wire.h"
#include <d3d9.h>
#include <cmath>
namespace warvk::host::d9 {
// Explicit named scalar fields, not memcpy of a native ABI object.
constexpr uint32_t FactoryCapsBytes=304, FactoryIdentifierBytes=1104;
enum class FactoryQuery:uint32_t { Caps=1,Identifier,DeviceType,Format,Multisample,DepthMatch,Conversion,ModeCount,EnumMode,DisplayMode };
inline uint32_t FactoryArguments(FactoryQuery query) {
  switch(query) {
    case FactoryQuery::Caps:case FactoryQuery::Identifier:case FactoryQuery::ModeCount:return 2;
    case FactoryQuery::DeviceType:case FactoryQuery::Multisample:case FactoryQuery::DepthMatch:return 5;
    case FactoryQuery::Format:return 6;
    case FactoryQuery::Conversion:return 4;
    case FactoryQuery::EnumMode:return 3;
    case FactoryQuery::DisplayMode:return 1;
  }return 0;
}
inline uint32_t FactoryResultBytes(FactoryQuery query) {
  switch(query) {
    case FactoryQuery::Caps:return FactoryCapsBytes;
    case FactoryQuery::Identifier:return FactoryIdentifierBytes;
    case FactoryQuery::Multisample:case FactoryQuery::ModeCount:return 4;
    case FactoryQuery::EnumMode:case FactoryQuery::DisplayMode:return 16;
    default:return 0;
  }
}
inline bool ValidFactoryQuery(const Header& h) {
  const auto used=FactoryArguments(FactoryQuery(h.args[0]));
  if(h.op!=Op::FactoryQuery||h.id||h.generation||h.payloadBytes||!used)return false;
  for(size_t i=used;i<h.args.size();++i)if(h.args[i])return false;
  return true;
}
inline void EncodeCaps(const D3DCAPS9& c,uint8_t* p) {
  size_t at=0;
  P32(p+at,uint32_t(c.DeviceType));at+=4;
  P32(p+at,uint32_t(c.AdapterOrdinal));at+=4;
  P32(p+at,uint32_t(c.Caps));at+=4;
  P32(p+at,uint32_t(c.Caps2));at+=4;
  P32(p+at,uint32_t(c.Caps3));at+=4;
  P32(p+at,uint32_t(c.PresentationIntervals));at+=4;
  P32(p+at,uint32_t(c.CursorCaps));at+=4;
  P32(p+at,uint32_t(c.DevCaps));at+=4;
  P32(p+at,uint32_t(c.PrimitiveMiscCaps));at+=4;
  P32(p+at,uint32_t(c.RasterCaps));at+=4;
  P32(p+at,uint32_t(c.ZCmpCaps));at+=4;
  P32(p+at,uint32_t(c.SrcBlendCaps));at+=4;
  P32(p+at,uint32_t(c.DestBlendCaps));at+=4;
  P32(p+at,uint32_t(c.AlphaCmpCaps));at+=4;
  P32(p+at,uint32_t(c.ShadeCaps));at+=4;
  P32(p+at,uint32_t(c.TextureCaps));at+=4;
  P32(p+at,uint32_t(c.TextureFilterCaps));at+=4;
  P32(p+at,uint32_t(c.CubeTextureFilterCaps));at+=4;
  P32(p+at,uint32_t(c.VolumeTextureFilterCaps));at+=4;
  P32(p+at,uint32_t(c.TextureAddressCaps));at+=4;
  P32(p+at,uint32_t(c.VolumeTextureAddressCaps));at+=4;
  P32(p+at,uint32_t(c.LineCaps));at+=4;
  P32(p+at,uint32_t(c.MaxTextureWidth));at+=4;
  P32(p+at,uint32_t(c.MaxTextureHeight));at+=4;
  P32(p+at,uint32_t(c.MaxVolumeExtent));at+=4;
  P32(p+at,uint32_t(c.MaxTextureRepeat));at+=4;
  P32(p+at,uint32_t(c.MaxTextureAspectRatio));at+=4;
  P32(p+at,uint32_t(c.MaxAnisotropy));at+=4;
  P32(p+at,uint32_t(c.StencilCaps));at+=4;
  P32(p+at,uint32_t(c.FVFCaps));at+=4;
  P32(p+at,uint32_t(c.TextureOpCaps));at+=4;
  P32(p+at,uint32_t(c.MaxTextureBlendStages));at+=4;
  P32(p+at,uint32_t(c.MaxSimultaneousTextures));at+=4;
  P32(p+at,uint32_t(c.VertexProcessingCaps));at+=4;
  P32(p+at,uint32_t(c.MaxActiveLights));at+=4;
  P32(p+at,uint32_t(c.MaxUserClipPlanes));at+=4;
  P32(p+at,uint32_t(c.MaxVertexBlendMatrices));at+=4;
  P32(p+at,uint32_t(c.MaxVertexBlendMatrixIndex));at+=4;
  P32(p+at,uint32_t(c.MaxPrimitiveCount));at+=4;
  P32(p+at,uint32_t(c.MaxVertexIndex));at+=4;
  P32(p+at,uint32_t(c.MaxStreams));at+=4;
  P32(p+at,uint32_t(c.MaxStreamStride));at+=4;
  P32(p+at,uint32_t(c.VertexShaderVersion));at+=4;
  P32(p+at,uint32_t(c.MaxVertexShaderConst));at+=4;
  P32(p+at,uint32_t(c.PixelShaderVersion));at+=4;
  P32(p+at,uint32_t(c.DevCaps2));at+=4;
  P32(p+at,uint32_t(c.Reserved5));at+=4;
  P32(p+at,uint32_t(c.MasterAdapterOrdinal));at+=4;
  P32(p+at,uint32_t(c.AdapterOrdinalInGroup));at+=4;
  P32(p+at,uint32_t(c.NumberOfAdaptersInGroup));at+=4;
  P32(p+at,uint32_t(c.DeclTypes));at+=4;
  P32(p+at,uint32_t(c.NumSimultaneousRTs));at+=4;
  P32(p+at,uint32_t(c.StretchRectFilterCaps));at+=4;
  P32(p+at,uint32_t(c.VS20Caps.Caps));at+=4;
  P32(p+at,uint32_t(c.VS20Caps.DynamicFlowControlDepth));at+=4;
  P32(p+at,uint32_t(c.VS20Caps.NumTemps));at+=4;
  P32(p+at,uint32_t(c.VS20Caps.StaticFlowControlDepth));at+=4;
  P32(p+at,uint32_t(c.PS20Caps.Caps));at+=4;
  P32(p+at,uint32_t(c.PS20Caps.DynamicFlowControlDepth));at+=4;
  P32(p+at,uint32_t(c.PS20Caps.NumTemps));at+=4;
  P32(p+at,uint32_t(c.PS20Caps.StaticFlowControlDepth));at+=4;
  P32(p+at,uint32_t(c.PS20Caps.NumInstructionSlots));at+=4;
  P32(p+at,uint32_t(c.VertexTextureFilterCaps));at+=4;
  P32(p+at,uint32_t(c.MaxVShaderInstructionsExecuted));at+=4;
  P32(p+at,uint32_t(c.MaxPShaderInstructionsExecuted));at+=4;
  P32(p+at,uint32_t(c.MaxVertexShader30InstructionSlots));at+=4;
  P32(p+at,uint32_t(c.MaxPixelShader30InstructionSlots));at+=4;
  P32(p+at,FloatBits(c.MaxVertexW));at+=4;
  P32(p+at,FloatBits(c.GuardBandLeft));at+=4;
  P32(p+at,FloatBits(c.GuardBandTop));at+=4;
  P32(p+at,FloatBits(c.GuardBandRight));at+=4;
  P32(p+at,FloatBits(c.GuardBandBottom));at+=4;
  P32(p+at,FloatBits(c.ExtentsAdjust));at+=4;
  P32(p+at,FloatBits(c.MaxPointSize));at+=4;
  P32(p+at,FloatBits(c.PixelShader1xMaxValue));at+=4;
  P32(p+at,FloatBits(c.MaxNpatchTessellationLevel));at+=4;
  (void)at;
}
inline bool DecodeCaps(const uint8_t* p,uint32_t bytes,D3DCAPS9& result) {
  if(!p||bytes!=FactoryCapsBytes)return false;
  D3DCAPS9 c{};size_t at=0;
  c.DeviceType=D3DDEVTYPE(U32(p+at));at+=4;
  c.AdapterOrdinal=decltype(c.AdapterOrdinal)(U32(p+at));at+=4;
  c.Caps=decltype(c.Caps)(U32(p+at));at+=4;
  c.Caps2=decltype(c.Caps2)(U32(p+at));at+=4;
  c.Caps3=decltype(c.Caps3)(U32(p+at));at+=4;
  c.PresentationIntervals=decltype(c.PresentationIntervals)(U32(p+at));at+=4;
  c.CursorCaps=decltype(c.CursorCaps)(U32(p+at));at+=4;
  c.DevCaps=decltype(c.DevCaps)(U32(p+at));at+=4;
  c.PrimitiveMiscCaps=decltype(c.PrimitiveMiscCaps)(U32(p+at));at+=4;
  c.RasterCaps=decltype(c.RasterCaps)(U32(p+at));at+=4;
  c.ZCmpCaps=decltype(c.ZCmpCaps)(U32(p+at));at+=4;
  c.SrcBlendCaps=decltype(c.SrcBlendCaps)(U32(p+at));at+=4;
  c.DestBlendCaps=decltype(c.DestBlendCaps)(U32(p+at));at+=4;
  c.AlphaCmpCaps=decltype(c.AlphaCmpCaps)(U32(p+at));at+=4;
  c.ShadeCaps=decltype(c.ShadeCaps)(U32(p+at));at+=4;
  c.TextureCaps=decltype(c.TextureCaps)(U32(p+at));at+=4;
  c.TextureFilterCaps=decltype(c.TextureFilterCaps)(U32(p+at));at+=4;
  c.CubeTextureFilterCaps=decltype(c.CubeTextureFilterCaps)(U32(p+at));at+=4;
  c.VolumeTextureFilterCaps=decltype(c.VolumeTextureFilterCaps)(U32(p+at));at+=4;
  c.TextureAddressCaps=decltype(c.TextureAddressCaps)(U32(p+at));at+=4;
  c.VolumeTextureAddressCaps=decltype(c.VolumeTextureAddressCaps)(U32(p+at));at+=4;
  c.LineCaps=decltype(c.LineCaps)(U32(p+at));at+=4;
  c.MaxTextureWidth=decltype(c.MaxTextureWidth)(U32(p+at));at+=4;
  c.MaxTextureHeight=decltype(c.MaxTextureHeight)(U32(p+at));at+=4;
  c.MaxVolumeExtent=decltype(c.MaxVolumeExtent)(U32(p+at));at+=4;
  c.MaxTextureRepeat=decltype(c.MaxTextureRepeat)(U32(p+at));at+=4;
  c.MaxTextureAspectRatio=decltype(c.MaxTextureAspectRatio)(U32(p+at));at+=4;
  c.MaxAnisotropy=decltype(c.MaxAnisotropy)(U32(p+at));at+=4;
  c.StencilCaps=decltype(c.StencilCaps)(U32(p+at));at+=4;
  c.FVFCaps=decltype(c.FVFCaps)(U32(p+at));at+=4;
  c.TextureOpCaps=decltype(c.TextureOpCaps)(U32(p+at));at+=4;
  c.MaxTextureBlendStages=decltype(c.MaxTextureBlendStages)(U32(p+at));at+=4;
  c.MaxSimultaneousTextures=decltype(c.MaxSimultaneousTextures)(U32(p+at));at+=4;
  c.VertexProcessingCaps=decltype(c.VertexProcessingCaps)(U32(p+at));at+=4;
  c.MaxActiveLights=decltype(c.MaxActiveLights)(U32(p+at));at+=4;
  c.MaxUserClipPlanes=decltype(c.MaxUserClipPlanes)(U32(p+at));at+=4;
  c.MaxVertexBlendMatrices=decltype(c.MaxVertexBlendMatrices)(U32(p+at));at+=4;
  c.MaxVertexBlendMatrixIndex=decltype(c.MaxVertexBlendMatrixIndex)(U32(p+at));at+=4;
  c.MaxPrimitiveCount=decltype(c.MaxPrimitiveCount)(U32(p+at));at+=4;
  c.MaxVertexIndex=decltype(c.MaxVertexIndex)(U32(p+at));at+=4;
  c.MaxStreams=decltype(c.MaxStreams)(U32(p+at));at+=4;
  c.MaxStreamStride=decltype(c.MaxStreamStride)(U32(p+at));at+=4;
  c.VertexShaderVersion=decltype(c.VertexShaderVersion)(U32(p+at));at+=4;
  c.MaxVertexShaderConst=decltype(c.MaxVertexShaderConst)(U32(p+at));at+=4;
  c.PixelShaderVersion=decltype(c.PixelShaderVersion)(U32(p+at));at+=4;
  c.DevCaps2=decltype(c.DevCaps2)(U32(p+at));at+=4;
  c.Reserved5=decltype(c.Reserved5)(U32(p+at));at+=4;
  c.MasterAdapterOrdinal=decltype(c.MasterAdapterOrdinal)(U32(p+at));at+=4;
  c.AdapterOrdinalInGroup=decltype(c.AdapterOrdinalInGroup)(U32(p+at));at+=4;
  c.NumberOfAdaptersInGroup=decltype(c.NumberOfAdaptersInGroup)(U32(p+at));at+=4;
  c.DeclTypes=decltype(c.DeclTypes)(U32(p+at));at+=4;
  c.NumSimultaneousRTs=decltype(c.NumSimultaneousRTs)(U32(p+at));at+=4;
  c.StretchRectFilterCaps=decltype(c.StretchRectFilterCaps)(U32(p+at));at+=4;
  c.VS20Caps.Caps=decltype(c.VS20Caps.Caps)(U32(p+at));at+=4;
  c.VS20Caps.DynamicFlowControlDepth=decltype(c.VS20Caps.DynamicFlowControlDepth)(U32(p+at));at+=4;
  c.VS20Caps.NumTemps=decltype(c.VS20Caps.NumTemps)(U32(p+at));at+=4;
  c.VS20Caps.StaticFlowControlDepth=decltype(c.VS20Caps.StaticFlowControlDepth)(U32(p+at));at+=4;
  c.PS20Caps.Caps=decltype(c.PS20Caps.Caps)(U32(p+at));at+=4;
  c.PS20Caps.DynamicFlowControlDepth=decltype(c.PS20Caps.DynamicFlowControlDepth)(U32(p+at));at+=4;
  c.PS20Caps.NumTemps=decltype(c.PS20Caps.NumTemps)(U32(p+at));at+=4;
  c.PS20Caps.StaticFlowControlDepth=decltype(c.PS20Caps.StaticFlowControlDepth)(U32(p+at));at+=4;
  c.PS20Caps.NumInstructionSlots=decltype(c.PS20Caps.NumInstructionSlots)(U32(p+at));at+=4;
  c.VertexTextureFilterCaps=decltype(c.VertexTextureFilterCaps)(U32(p+at));at+=4;
  c.MaxVShaderInstructionsExecuted=decltype(c.MaxVShaderInstructionsExecuted)(U32(p+at));at+=4;
  c.MaxPShaderInstructionsExecuted=decltype(c.MaxPShaderInstructionsExecuted)(U32(p+at));at+=4;
  c.MaxVertexShader30InstructionSlots=decltype(c.MaxVertexShader30InstructionSlots)(U32(p+at));at+=4;
  c.MaxPixelShader30InstructionSlots=decltype(c.MaxPixelShader30InstructionSlots)(U32(p+at));at+=4;
  c.MaxVertexW=Float(U32(p+at));at+=4;if(!std::isfinite(c.MaxVertexW))return false;
  c.GuardBandLeft=Float(U32(p+at));at+=4;if(!std::isfinite(c.GuardBandLeft))return false;
  c.GuardBandTop=Float(U32(p+at));at+=4;if(!std::isfinite(c.GuardBandTop))return false;
  c.GuardBandRight=Float(U32(p+at));at+=4;if(!std::isfinite(c.GuardBandRight))return false;
  c.GuardBandBottom=Float(U32(p+at));at+=4;if(!std::isfinite(c.GuardBandBottom))return false;
  c.ExtentsAdjust=Float(U32(p+at));at+=4;if(!std::isfinite(c.ExtentsAdjust))return false;
  c.MaxPointSize=Float(U32(p+at));at+=4;if(!std::isfinite(c.MaxPointSize))return false;
  c.PixelShader1xMaxValue=Float(U32(p+at));at+=4;if(!std::isfinite(c.PixelShader1xMaxValue))return false;
  c.MaxNpatchTessellationLevel=Float(U32(p+at));at+=4;if(!std::isfinite(c.MaxNpatchTessellationLevel))return false;
  if(c.DeviceType!=D3DDEVTYPE_HAL||c.AdapterOrdinal||c.Reserved5)return false;
  result=c;return true;
}
inline void EncodeIdentifier(const D3DADAPTER_IDENTIFIER9& id,uint8_t* p) {
  std::memcpy(p,id.Driver,512);std::memcpy(p+512,id.Description,512);std::memcpy(p+1024,id.DeviceName,32);
  P64(p+1056,uint64_t(id.DriverVersion.QuadPart));
  P32(p+1064,id.VendorId);P32(p+1068,id.DeviceId);P32(p+1072,id.SubSysId);P32(p+1076,id.Revision);
  P32(p+1080,id.DeviceIdentifier.Data1);
  ipc::WriteLe(p+1084,id.DeviceIdentifier.Data2,2);ipc::WriteLe(p+1086,id.DeviceIdentifier.Data3,2);
  std::memcpy(p+1088,id.DeviceIdentifier.Data4,8);P32(p+1096,id.WHQLLevel);P32(p+1100,0);
}
inline bool DecodeIdentifier(const uint8_t* p,uint32_t bytes,D3DADAPTER_IDENTIFIER9& result) {
  if(!p||bytes!=FactoryIdentifierBytes||U32(p+1100)||!std::memchr(p,0,512)
    ||!std::memchr(p+512,0,512)||!std::memchr(p+1024,0,32))return false;
  D3DADAPTER_IDENTIFIER9 id{};
  std::memcpy(id.Driver,p,512);std::memcpy(id.Description,p+512,512);std::memcpy(id.DeviceName,p+1024,32);
  id.DriverVersion.QuadPart=U64(p+1056);id.VendorId=U32(p+1064);id.DeviceId=U32(p+1068);
  id.SubSysId=U32(p+1072);id.Revision=U32(p+1076);id.DeviceIdentifier.Data1=U32(p+1080);
  id.DeviceIdentifier.Data2=WORD(ipc::ReadLe(p+1084,2));id.DeviceIdentifier.Data3=WORD(ipc::ReadLe(p+1086,2));
  std::memcpy(id.DeviceIdentifier.Data4,p+1088,8);id.WHQLLevel=U32(p+1096);result=id;return true;
}
} // namespace warvk::host::d9

