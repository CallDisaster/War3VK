#version 460

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2DArray s_color;
layout(set = 1, binding = 1) uniform texture2DArray s_depth;

layout(push_constant, scalar, row_major)
uniform push_block {
  uint  p_colorSampler;
  uint  p_depthSampler;
  float p_radiusPx;
  float p_strength;
  float p_bias;
  float p_power;
  mat4  p_invViewProj;
  vec4  p_viewport;   // x=vpX, y=vpY, z=vpW, w=vpH
  vec4  p_viewportZ;  // x=minZ, y=maxZ
  vec4  p_fade;       // x=fadeNear, y=fadeFar
} pc;

layout(location = 0) out vec4 o_color;

float fetchDepthN(ivec2 pix, int layer) {
  float depth = texelFetch(
    sampler2DArray(s_depth, s_samplers[nonuniformEXT(pc.p_depthSampler)]),
    ivec3(pix, layer),
    0).r;

  float minZ = pc.p_viewportZ.x;
  float maxZ = pc.p_viewportZ.y;
  float depthN = depth;
  float denom = max(maxZ - minZ, 1e-6);
  depthN = clamp((depth - minZ) / denom, 0.0, 1.0);
  return depthN;
}

vec3 reconstructWorld(vec2 uv, float depthN) {
  vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depthN, 1.0);
  vec4 worldH = clip * pc.p_invViewProj;
  float w = worldH.w;
  if (abs(w) < 1e-6)
    return vec3(0.0);
  return worldH.xyz / w;
}

float linearDepthN(vec2 uv, float depthN) {
  vec3 nearW = reconstructWorld(uv, 0.0);
  vec3 farW = reconstructWorld(uv, 1.0);
  vec3 posW = reconstructWorld(uv, depthN);
  vec3 ray = farW - nearW;
  float denom = dot(ray, ray);
  if (denom < 1e-8)
    return depthN;
  float t = dot(posW - nearW, ray) / denom;
  return clamp(t, 0.0, 1.0);
}

void main() {
  ivec2 pix = ivec2(gl_FragCoord.xy);
  int layer = 0;

  vec4 col = texelFetch(
    sampler2DArray(s_color, s_samplers[nonuniformEXT(pc.p_colorSampler)]),
    ivec3(pix, layer),
    0);

  vec2 vpMin = pc.p_viewport.xy;
  vec2 vpSize = max(pc.p_viewport.zw, vec2(1.0));
  if (float(pix.x) < vpMin.x || float(pix.y) < vpMin.y ||
      float(pix.x) >= (vpMin.x + vpSize.x) ||
      float(pix.y) >= (vpMin.y + vpSize.y)) {
    o_color = col;
    return;
  }

  vec2 uvCenter = (vec2(pix) - vpMin) / vpSize;
  float depthN = fetchDepthN(pix, layer);
  float depthLin = linearDepthN(uvCenter, depthN);
  float depthScale = mix(1.0, 6.0, depthLin * depthLin);

  const ivec2 kOffsets[8] = ivec2[](
    ivec2( 1,  0), ivec2(-1,  0),
    ivec2( 0,  1), ivec2( 0, -1),
    ivec2( 1,  1), ivec2(-1,  1),
    ivec2( 1, -1), ivec2(-1, -1)
  );

  ivec2 vpMinI = ivec2(vpMin);
  ivec2 vpMaxI = ivec2(vpMin + vpSize) - ivec2(1, 1);

  float radius = max(pc.p_radiusPx, 1.0);
  float occlusion = 0.0;
  for (int i = 0; i < 8; i++) {
    ivec2 sp = pix + ivec2(round(vec2(kOffsets[i]) * radius));
    sp = clamp(sp, vpMinI, vpMaxI);
    vec2 uvSp = (vec2(sp) - vpMin) / vpSize;
    float d = fetchDepthN(sp, layer);
    float dLin = linearDepthN(uvSp, d);
    float delta = (depthLin - dLin) * depthScale;
    occlusion += step(pc.p_bias, delta);
  }

  float fade = 1.0 - smoothstep(pc.p_fade.x, pc.p_fade.y, depthLin);
  occlusion *= clamp(fade, 0.0, 1.0);

  float ao = 1.0 - (occlusion / 8.0) * pc.p_strength;
  ao = clamp(ao, 0.0, 1.0);
  ao = pow(ao, max(pc.p_power, 0.25));

  o_color = vec4(col.rgb * ao, col.a);
}
