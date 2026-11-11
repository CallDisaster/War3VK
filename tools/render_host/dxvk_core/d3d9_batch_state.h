#pragma once
#include "d3d9_backend.h"

namespace warvk::host::dxvkcore {
// Only adjacent, fully prevalidated draws within ONE drawBatch may share these
// settings. No upload/destroy/reset/clear/present can occur between them. The
// first draw always restores every setting, even on a continued command list.
// Constants, resource bindings, viewport and every draw's validation remain
// independent. No bytewise comparison of padded structs or persistent cache.
struct BatchStateUpdates {
  bool targets=true,layout=true,assembly=true,raster=true,multisample=true,depth=true,blend=true;
};
inline BatchStateUpdates BatchStateChanges(const D3D9Backend::DrawState& current,
    const D3D9Backend::DrawState* previous) {
  if(!previous)return {};
  const auto& a=current.render;const auto& b=previous->render;
  return {false,current.vertexShader!=previous->vertexShader||current.declaration!=previous->declaration,
    current.primitiveType!=previous->primitiveType,a.cullMode!=b.cullMode,false,
    a.zEnable!=b.zEnable||a.zWriteEnable!=b.zWriteEnable||a.zFunc!=b.zFunc,
    a.alphaBlendEnable!=b.alphaBlendEnable||a.srcBlend!=b.srcBlend||a.destBlend!=b.destBlend
      ||a.blendOp!=b.blendOp||a.colorWriteMask!=b.colorWriteMask};
}
} // namespace warvk::host::dxvkcore
