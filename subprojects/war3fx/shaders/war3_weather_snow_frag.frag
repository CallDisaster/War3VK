#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

#include "war3_weather_density.glsl"

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2DArray s_depth;

layout(push_constant, scalar, row_major) uniform SnowPush {
  mat4 viewProj;
  vec4 regionXY;
  vec4 heightTime;
  vec4 windAnchor;
  uint seed;
  uint depthSampler;
  float cameraZ;
  uint packedMedia;
} pc;

layout(location = 0) in vec2 v_corner;
layout(location = 1) in vec3 v_world;
layout(location = 2) in float v_alpha;
layout(location = 0) out vec4 o_color;

float particleTransmittance() {
  vec3 eye = vec3(pc.windAnchor.zw, pc.cameraZ);
  vec3 delta = v_world - eye;
  float distanceWorld = length(delta);
  float feather = float(pc.packedMedia & 255u) * (0.5 / 255.0);
  vec4 regionMin = vec4(pc.regionXY.xy, pc.heightTime.x, feather);
  vec4 regionMax = vec4(pc.regionXY.zw, pc.heightTime.y, 0.0);
  vec4 controls = vec4(float((pc.packedMedia >> 8) & 255u) / 255.0,
                       float((pc.packedMedia >> 16) & 255u) / 255.0,
                       float((pc.packedMedia >> 24) & 255u) / 255.0, 1.0);
  vec4 motion = vec4(pc.windAnchor.xy, pc.heightTime.z, 0.0);
  if (distanceWorld <= 1.0e-5)
    return 1.0;
  vec3 ray = delta / distanceWorld;
  vec2 interval = vec2(0.0, distanceWorld);
  if (!war3WeatherRayInterval(eye, ray, regionMin.xyz, regionMax.xyz, interval))
    return 1.0;
  float tau = 0.0;
  // Integrate only eye->flake. Using integrated scene-surface fog would apply
  // too much extinction to near flakes and double-fog opaque scene geometry.
  for (int i = 0; i < 4; ++i) {
    vec3 samplePosition = eye + ray * mix(interval.x, interval.y,
                                         (float(i) + 0.5) / 4.0);
    tau += war3WeatherSigma(samplePosition, regionMin, regionMax,
                           controls, motion);
  }
  return exp(-clamp(tau * (interval.y - interval.x) * 0.25, 0.0, 12.0));
}

void main() {
  // Derivatives are evaluated before any per-fragment rejection.
  float featherWidth = max(fwidth(dot(v_corner, v_corner)), 0.02);
  float depthSoftness = max(fwidth(gl_FragCoord.z) * 2.0, 0.00003);
  float radial = 1.0 - smoothstep(0.5, 1.0 + featherWidth,
                                  dot(v_corner, v_corner));
  if (v_alpha <= 0.0001 || radial <= 0.0001)
    discard;
  float sceneDepth = texelFetch(
      sampler2DArray(s_depth,
          s_samplers[nonuniformEXT(pc.depthSampler & 65535u)]),
      ivec3(ivec2(gl_FragCoord.xy), 0), 0).r;
  float separation = sceneDepth - gl_FragCoord.z;
  if ((pc.depthSampler & 0x80000000u) != 0u)
    separation = -separation;
  if (separation <= 0.0)
    discard;
  float intersectionFade = smoothstep(0.0, depthSoftness, separation);
  float alpha = clamp(v_alpha * radial * intersectionFade *
                      particleTransmittance(), 0.0, 0.75);
  // Premultiplied surface-like snow. It does not add radiance irrespective of
  // opacity, and the blend state preserves the destination alpha channel.
  vec3 snowColor = vec3(0.83, 0.88, 0.94);
  o_color = vec4(snowColor * alpha, alpha);
}
