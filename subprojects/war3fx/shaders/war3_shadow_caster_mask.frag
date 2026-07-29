#version 460

#extension GL_EXT_scalar_block_layout : require

layout(location = 0) in vec2 v_uv;
layout(location = 0) out float o_casterMask;

layout(push_constant, scalar)
uniform push_block {
  mat4 p_mvp;
  uint p_paletteOffset;
  uint p_blendCount;
  uint p_flags;
  float p_alphaRef;
  uint p_samplerIndex;
  float p_terrainDepthBias;
};

void main() {
  // 1 = the nearest caster written to the depth map is S1 terrain.
  o_casterMask = ((p_flags & 0x10u) != 0u) ? 1.0 : 0.0;
}
