#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];

layout(set = 1, binding = 1) uniform texture2D u_colorTex;
layout(set = 1, binding = 7) uniform texture2D u_prevPass;

layout(push_constant) uniform War3PackPush {
  uint u_samplerColor;
  uint u_samplerDepth;
  uint u_samplerTex0;
  uint u_samplerTex1;
  uint u_samplerTex2;
  uint u_samplerTex3;
  uint u_samplerPrev;
  uint u_samplerPad;
  vec4 u_params0;
  vec4 u_params1;
  vec4 u_params2;
  vec4 u_params3;
} pc;

layout(location = 0) in vec2 i_pos;
layout(location = 0) out vec4 o_color;

void main() {
  // [健壮性] 确保 UV 在有效范围内
  vec2 uv = clamp(i_pos, vec2(0.0), vec2(1.0));
  vec4 color = texture(sampler2D(u_prevPass, s_samplers[nonuniformEXT(pc.u_samplerPrev)]), uv);
  o_color = color;
}

