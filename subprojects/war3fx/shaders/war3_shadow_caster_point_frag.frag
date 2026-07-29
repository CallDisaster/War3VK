#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 1) uniform texture2D u_alphaTex;

layout(push_constant) uniform push_block {
  mat4 p_mvp;
  uint p_paletteOffset;
  uint p_blendCount;
  uint p_flags;
  float p_alphaRef;
  uint p_samplerIndex;
  float p_terrainDepthBias;
  uint p_pad0;
  uint p_pad1;
  vec4 p_pointLightPosRange;
};

float hash12(vec2 p) {
  return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
  if ((p_flags & 0x4u) != 0u) {
    bool hashed = (p_flags & 0x8u) != 0u;
    float alpha = texture(
      sampler2D(u_alphaTex, s_samplers[nonuniformEXT(p_samplerIndex)]),
      v_uv).a;

    if (hashed) {
      float hashWidth = 0.06 + 0.05 * (1.0 - p_alphaRef);
      float delta = alpha - p_alphaRef;
      if (delta <= -hashWidth) {
        discard;
      } else if (delta < hashWidth) {
        float t = (delta + hashWidth) / (2.0 * hashWidth);
        ivec2 texSize = textureSize(
          sampler2D(u_alphaTex, s_samplers[nonuniformEXT(p_samplerIndex)]),
          0);
        vec2 texel = floor(fract(v_uv) * max(vec2(texSize), vec2(1.0)));
        vec2 hashP = texel + vec2(float(p_samplerIndex) * 17.0, 0.0);
        if (t < hash12(hashP))
          discard;
      }
    } else if (alpha < p_alphaRef) {
      discard;
    }
  }

  // Point shadows require radial cube depth. This shader is never used by
  // directional CSM, so gl_FragDepth is written on every surviving path.
  float faceDepth = 1.0 / max(gl_FragCoord.w, 1.0e-8);
  float resolution = max(float(p_pad0), 1.0);
  vec2 faceNdc = gl_FragCoord.xy * (2.0 / resolution) - vec2(1.0);
  float radialDepth = faceDepth * length(vec3(faceNdc, 1.0));
  float range = max(p_pointLightPosRange.w, 1.0);
  gl_FragDepth = clamp(radialDepth / range, 0.0, 1.0);
}
