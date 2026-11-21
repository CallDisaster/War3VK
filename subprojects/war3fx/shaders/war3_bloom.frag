#version 460

#extension GL_EXT_nonuniform_qualifier : require

// Bloom and tone mapping (src/d3d9/war3/effects/war3_bloom_executor.cpp).
// One shader for the four passes. The math is the HLSL embedded in
// src/d3d9/war3/render/war3_post_process.cpp, which the earlier D3D9
// implementation compiled with d3dcompiler at run time (its sRGB conversion
// was unconditional: the executor always sets bit 1); the
// uv is the destination texel centre, which is where that HLSL vertex shader's
// half-texel offset lands under DXVK's D3D9 viewport (offset 0.5).

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D s_source;
layout(set = 1, binding = 1) uniform texture2D s_bloom;

layout(push_constant) uniform push_block {
  uint  p_sourceSampler;
  uint  p_bloomSampler;
  uint  p_mode;      // 0 prefilter, 1 downsample, 2 upsample, 3 composite
  uint  p_flags;     // bit 0: ACES tone map, bit 1: sRGB decode/encode
  vec2  p_invTarget; // 1 / render target size
  vec2  p_texel;     // 1 / source size (downsample and upsample)
  float p_threshold;
  float p_softKnee;
  float p_intensity;
  float p_exposure;
};

layout(location = 0) out vec4 o_color;

vec3 sourceAt(vec2 uv) {
  return textureLod(sampler2D(s_source, s_samplers[nonuniformEXT(p_sourceSampler)]), uv, 0.0).rgb;
}

vec3 bloomAt(vec2 uv) {
  return textureLod(sampler2D(s_bloom, s_samplers[nonuniformEXT(p_bloomSampler)]), uv, 0.0).rgb;
}

// HLSL mul(M, v) with M given by rows: one dot product per row.
vec3 acesFitted(vec3 color) {
  color = vec3(dot(vec3(0.59719, 0.35458, 0.04823), color),
               dot(vec3(0.07600, 0.90834, 0.01566), color),
               dot(vec3(0.02840, 0.13383, 0.83777), color));
  vec3 a = color * (color + 0.0245786) - 0.000090537;
  vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
  color = a / b;
  color = vec3(dot(vec3( 1.60475, -0.53108, -0.07367), color),
               dot(vec3(-0.10208,  1.10813, -0.00605), color),
               dot(vec3(-0.00327, -0.07276,  1.07602), color));
  return clamp(color, 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c) {
  return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

vec3 srgbToLinear(vec3 c) {
  return pow(clamp(c, 0.0, 1.0), vec3(2.2));
}

void main() {
  const vec2 uv = gl_FragCoord.xy * p_invTarget;
  const vec2 t = p_texel;
  const bool srgb = (p_flags & 2u) != 0u;

  if (p_mode == 0u) {
    vec3 color = sourceAt(uv);
    if (srgb)
      color = srgbToLinear(color);
    float brightness = max(max(color.r, color.g), color.b);
    float knee = max(1e-4, p_softKnee);
    float soft = clamp((brightness - p_threshold + knee) / (2.0 * knee), 0.0, 1.0);
    float weight = max(brightness - p_threshold, 0.0) / max(brightness, 1e-4);
    weight = max(weight, soft * soft);
    o_color = vec4(color * weight, 1.0);
  } else if (p_mode == 1u) {
    vec3 sum = vec3(0.0);
    sum += sourceAt(uv);
    sum += sourceAt(uv + t * vec2( 1.0,  0.0));
    sum += sourceAt(uv + t * vec2(-1.0,  0.0));
    sum += sourceAt(uv + t * vec2( 0.0,  1.0));
    sum += sourceAt(uv + t * vec2( 0.0, -1.0));
    sum += sourceAt(uv + t * vec2( 1.0,  1.0));
    sum += sourceAt(uv + t * vec2(-1.0,  1.0));
    sum += sourceAt(uv + t * vec2( 1.0, -1.0));
    sum += sourceAt(uv + t * vec2(-1.0, -1.0));
    sum += sourceAt(uv + t * vec2( 2.0,  0.0));
    sum += sourceAt(uv + t * vec2(-2.0,  0.0));
    sum += sourceAt(uv + t * vec2( 0.0,  2.0));
    sum += sourceAt(uv + t * vec2( 0.0, -2.0));
    o_color = vec4(sum / 13.0, 1.0);
  } else if (p_mode == 2u) {
    vec3 sum = vec3(0.0);
    sum += sourceAt(uv + t * vec2(-1.0, -1.0));
    sum += sourceAt(uv + t * vec2( 0.0, -1.0));
    sum += sourceAt(uv + t * vec2( 1.0, -1.0));
    sum += sourceAt(uv + t * vec2(-1.0,  0.0));
    sum += sourceAt(uv);
    sum += sourceAt(uv + t * vec2( 1.0,  0.0));
    sum += sourceAt(uv + t * vec2(-1.0,  1.0));
    sum += sourceAt(uv + t * vec2( 0.0,  1.0));
    sum += sourceAt(uv + t * vec2( 1.0,  1.0));
    o_color = vec4(sum / 9.0, 1.0);
  } else {
    vec3 base = sourceAt(uv);
    vec3 bloom = bloomAt(uv);
    if (srgb)
      base = srgbToLinear(base);
    vec3 color = base + bloom * p_intensity;
    color *= max(p_exposure, 0.0);
    if ((p_flags & 1u) != 0u)
      color = acesFitted(color);
    else
      color = clamp(color, 0.0, 1.0);
    if (srgb)
      color = linearToSrgb(color);
    o_color = vec4(color, 1.0);
  }
}
