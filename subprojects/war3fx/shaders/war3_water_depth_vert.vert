#version 460

#extension GL_EXT_scalar_block_layout : require

// The water's surface depth: the game's water draws replayed with the camera,
// depth only (d3d9_war3_shadow.cpp, renderWaterDepth). The water is drawn in
// world space; the push constant is world * view * projection, row vectors,
// D3D clip space (the pass flips the viewport as the game's own draws are).
layout(location = 0) in vec3 in_pos;

layout(push_constant, scalar, row_major)
uniform push_block {
  mat4 p_worldViewProj;
};

void main() {
  gl_Position = vec4(in_pos, 1.0) * p_worldViewProj;
}
