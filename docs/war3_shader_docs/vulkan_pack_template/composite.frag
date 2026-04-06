#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];

layout(set = 1, binding = 0, std140) uniform War3PackUBO {
  mat4 u_view;
  mat4 u_proj;
  mat4 u_invView;
  mat4 u_invProj;
  vec3 u_cameraPos;
  float u_gameTime;
  vec2 u_resolution;
  float u_frameTime;
  float u_totalTime;
} ubo;

layout(set = 1, binding = 1) uniform texture2D u_colorTex;
layout(set = 1, binding = 2) uniform texture2D u_depthTex;
layout(set = 1, binding = 3) uniform texture2D u_tex0;
layout(set = 1, binding = 4) uniform texture2D u_tex1;
layout(set = 1, binding = 5) uniform texture2D u_tex2;
layout(set = 1, binding = 6) uniform texture2D u_tex3;
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

vec4 sampleColor(vec2 uv) {
  return texture(sampler2D(u_colorTex, s_samplers[nonuniformEXT(pc.u_samplerColor)]), uv);
}

void main() {
  // [健壮性] 确保 UV 在有效范围内
  vec2 uv = clamp(i_pos, vec2(0.0), vec2(1.0));
  vec4 color = sampleColor(uv);

  float strength = clamp(pc.u_params0.x, 0.0, 1.0);
  float vignette = smoothstep(0.9, 0.3, distance(uv, vec2(0.5)));
  vec3 graded = mix(color.rgb, color.rgb * vec3(1.0, 0.95, 0.9), strength);
  float vignetteMix = mix(1.0, vignette, clamp(pc.u_params0.y, 0.0, 1.0));

  o_color = vec4(graded * vignetteMix, color.a);
}
