#pragma once
#include <d3d9.h>
#include <cstdint>

// D3D9 primitive topology facts shared by the x86 proxy and the x64 host.
// Points stay refused: their size/sprite state is not carried by this profile.
// The census of Game.dll's own calls (2026-10-30) used line strips, triangle
// lists and triangle strips with up to 5,896 primitives per draw.
namespace warvk::host::d9 {
constexpr uint32_t MaxDrawPrimitives=65535;
// Vertices (non-indexed) or indices (indexed) one draw consumes; 0 = refused type.
inline uint64_t DrawElementCount(D3DPRIMITIVETYPE type,uint64_t primitives) {
  switch(type) {
    case D3DPT_LINELIST: return primitives*2;
    case D3DPT_LINESTRIP: return primitives+1;
    case D3DPT_TRIANGLELIST: return primitives*3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return primitives+2;
    default: return 0;
  }
}
inline bool ValidDrawTopology(D3DPRIMITIVETYPE type,uint32_t primitives) {
  return primitives!=0&&primitives<=MaxDrawPrimitives&&DrawElementCount(type,primitives)!=0;
}
} // namespace warvk::host::d9
