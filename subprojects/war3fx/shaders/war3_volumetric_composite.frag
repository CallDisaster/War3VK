#version 460

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2DArray s_color;
layout(set = 1, binding = 1) uniform texture2DArray s_effect;
layout(set = 1, binding = 2) uniform texture2DArray s_depth;
layout(set = 1, binding = 3) uniform texture2DArray s_shadowGuide;

layout(set = 1, binding = 4, scalar, row_major)
uniform CsmData {
  mat4 u_view;
  mat4 u_invViewProj;
  mat4 u_lightViewProj[4];
  vec4 u_splitFar;
  vec4 u_params;
  vec4 u_params2;
  vec4 u_sunDir;
  vec4 u_cameraPos;
  vec4 u_worldUp;
  vec4 u_volumeSunParams;
} csm;

layout(push_constant, scalar)
uniform push_block {
  uint p_colorSampler;
  uint p_effectSampler;
  uint p_depthSampler;
  uint p_guideSampler;
  vec4 p_rtSize; // x=fullW y=fullH z=effectW w=effectH
  vec4 p_guideSize; // x=guideW y=guideH z=enabled w=reserved
  vec4 p_viewport; // x,y,width,height
  vec4 p_viewportZ; // minZ,maxZ,maxReadabilityAtten,readabilityScale
} pc;

layout(location = 0) out vec4 o_color;

bool finiteVec4(vec4 v) {
  return !any(isnan(v)) && !any(isinf(v));
}

float fetchDepth(ivec2 pixel, ivec2 fullSize) {
  ivec2 p = clamp(pixel, ivec2(0), fullSize - ivec2(1));
  return texelFetch(
    sampler2DArray(s_depth, s_samplers[nonuniformEXT(pc.p_depthSampler)]),
    ivec3(p, 0), 0).r;
}

vec4 fetchEffect(ivec2 pixel, ivec2 effectSize) {
  ivec2 p = clamp(pixel, ivec2(0), effectSize - ivec2(1));
  vec4 value = texelFetch(
    sampler2DArray(s_effect, s_samplers[nonuniformEXT(pc.p_effectSampler)]),
    ivec3(p, 0), 0);
  return finiteVec4(value) ? value : vec4(0.0, 0.0, 0.0, 1.0);
}

float fetchGuide(vec2 uv) {
  if (pc.p_guideSize.z < 0.5)
    return 0.0;
  float value = textureLod(
    sampler2DArray(s_shadowGuide,
      s_samplers[nonuniformEXT(pc.p_guideSampler)]),
    vec3(clamp(uv, vec2(0.0), vec2(1.0)), 0.0), 0.0).r;
  return (isnan(value) || isinf(value)) ? 0.0 : clamp(value, 0.0, 1.0);
}

bool fetchWorldPosition(ivec2 pixel, ivec2 fullSize, out vec3 worldPos) {
  ivec2 p = clamp(pixel, ivec2(0), fullSize - ivec2(1));
  vec2 viewportMin = pc.p_viewport.xy;
  vec2 viewportSize = max(pc.p_viewport.zw, vec2(1.0));
  vec2 local = (vec2(p) - viewportMin) / viewportSize;
  if (any(lessThan(local, vec2(-1.0e-4))) ||
      any(greaterThan(local, vec2(1.0001))))
    return false;
  float rawDepth = fetchDepth(p, fullSize);
  float depthRange = pc.p_viewportZ.y - pc.p_viewportZ.x;
  if (isnan(rawDepth) || isinf(rawDepth) || abs(depthRange) <= 1.0e-6)
    return false;
  float depthN = clamp(
    (rawDepth - pc.p_viewportZ.x) / depthRange, 0.0, 1.0);
  vec4 clip = vec4(local.x * 2.0 - 1.0,
                   1.0 - local.y * 2.0, depthN, 1.0);
  vec4 worldH = clip * csm.u_invViewProj;
  if (!finiteVec4(worldH) || abs(worldH.w) <= 1.0e-6)
    return false;
  worldPos = worldH.xyz / worldH.w;
  return !any(isnan(worldPos)) && !any(isinf(worldPos));
}

bool reconstructReceiverPlane(ivec2 pixel, ivec2 fullSize,
                              out vec3 centerWorld, out vec3 normal) {
  vec3 leftWorld;
  vec3 rightWorld;
  vec3 upWorld;
  vec3 downWorld;
  if (!fetchWorldPosition(pixel, fullSize, centerWorld) ||
      !fetchWorldPosition(pixel + ivec2(-1, 0), fullSize, leftWorld) ||
      !fetchWorldPosition(pixel + ivec2(1, 0), fullSize, rightWorld) ||
      !fetchWorldPosition(pixel + ivec2(0, -1), fullSize, upWorld) ||
      !fetchWorldPosition(pixel + ivec2(0, 1), fullSize, downWorld))
    return false;
  vec3 tangentX = rightWorld - leftWorld;
  vec3 tangentY = downWorld - upWorld;
  normal = cross(tangentX, tangentY);
  float lengthSquared = dot(normal, normal);
  if (isnan(lengthSquared) || isinf(lengthSquared) ||
      lengthSquared <= 1.0e-10)
    return false;
  normal *= inversesqrt(lengthSquared);
  return true;
}

vec4 depthAwareUpsample(vec2 uv, ivec2 fullSize, ivec2 effectSize,
                        float centerDepth, bool planeValid, vec3 centerWorld,
                        vec3 receiverNormal, float centerGuide) {
  if (all(equal(fullSize, effectSize))) {
    ivec2 p = clamp(ivec2(uv * vec2(effectSize)), ivec2(0),
                    effectSize - ivec2(1));
    return fetchEffect(p, effectSize);
  }

  vec2 lowCoord = uv * vec2(effectSize) - vec2(0.5);
  ivec2 lowBase = ivec2(floor(lowCoord));
  vec2 f = fract(lowCoord);

  const ivec2 offsets[4] = ivec2[4](
    ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));
  vec4 values[4];
  float deltas[4];
  float shadowGuides[4];
  float spatial[4];
  float bestDelta = 1e30;
  int bestIndex = 0;

  for (int i = 0; i < 4; i++) {
    ivec2 lowPixel = clamp(lowBase + offsets[i], ivec2(0),
                           effectSize - ivec2(1));
    values[i] = fetchEffect(lowPixel, effectSize);

    vec2 guideUv = (vec2(lowPixel) + vec2(0.5)) / vec2(effectSize);
    ivec2 guidePixel = clamp(ivec2(guideUv * vec2(fullSize)), ivec2(0),
                             fullSize - ivec2(1));
    float guideDepth = fetchDepth(guidePixel, fullSize);
    vec3 sampleWorld;
    if (planeValid && fetchWorldPosition(
          guidePixel, fullSize, sampleWorld)) {
      deltas[i] = abs(dot(sampleWorld - centerWorld, receiverNormal));
    } else {
      deltas[i] = abs(guideDepth - centerDepth);
    }
    shadowGuides[i] = fetchGuide(guideUv);
    if (deltas[i] < bestDelta) {
      bestDelta = deltas[i];
      bestIndex = i;
    }

    float wx = offsets[i].x == 0 ? (1.0 - f.x) : f.x;
    float wy = offsets[i].y == 0 ? (1.0 - f.y) : f.y;
    spatial[i] = max(wx * wy, 1e-4);
  }

  // A receiver-plane range metric remains near zero across a flat surface at
  // low camera pitch. Raw hardware depth is retained only as a fail-soft path
  // when neighborhood reconstruction is invalid.
  float centerDistance = length(centerWorld - csm.u_cameraPos.xyz);
  float depthScale = planeValid
    ? max(0.75, centerDistance * 0.0005)
    : max(2e-5, bestDelta * 2.0 + 2e-5);

  vec4 sumEffect = vec4(0.0);
  float weightSum = 0.0;
  for (int i = 0; i < 4; i++) {
    float relativeDelta = max(deltas[i] - bestDelta, 0.0);
    float depthWeight = exp2(-4.0 * relativeDelta / depthScale);
    float shadowWeight = pc.p_guideSize.z > 0.5
      ? exp2(-6.0 * abs(shadowGuides[i] - centerGuide))
      : 1.0;
    float guideWeight = spatial[i] * depthWeight * shadowWeight;
    // The range inputs are independent full-resolution receiver geometry and
    // the separately integrated higher-resolution directional guide. The
    // low-resolution RGBA solution never guides itself.
    vec4 resolved = vec4(
      max(values[i].rgb, vec3(0.0)),
      clamp(values[i].a, exp(-1.0), 1.0));
    sumEffect += resolved * guideWeight;
    weightSum += guideWeight;
  }

  if (weightSum <= 1e-6)
    return values[bestIndex];
  return sumEffect / weightSum;
}

void main() {
  ivec2 pix = ivec2(gl_FragCoord.xy);
  ivec2 fullSize = max(ivec2(pc.p_rtSize.xy + vec2(0.5)), ivec2(1));
  ivec2 effectSize = max(ivec2(pc.p_rtSize.zw + vec2(0.5)), ivec2(1));
  vec2 uv = (vec2(pix) + vec2(0.5)) / vec2(fullSize);

  vec4 base = texelFetch(
    sampler2DArray(s_color, s_samplers[nonuniformEXT(pc.p_colorSampler)]),
    ivec3(pix, 0), 0);
  float centerDepth = fetchDepth(pix, fullSize);
  vec3 centerWorld = vec3(0.0);
  vec3 receiverNormal = vec3(0.0, 0.0, 1.0);
  bool planeValid = reconstructReceiverPlane(
    pix, fullSize, centerWorld, receiverNormal);
  float centerGuide = fetchGuide(uv);
  vec4 effect = depthAwareUpsample(
    uv, fullSize, effectSize, centerDepth, planeValid,
    centerWorld, receiverNormal, centerGuide);

  // This term must be resolved before the no-effect early-out. Clear air can
  // legitimately have no scattering/extinction while a true CSM shadow guide
  // still needs to reveal the caster-to-receiver shadow column.
  float readabilityAttenuation = min(
    centerGuide * max(pc.p_viewportZ.w, 0.0),
    max(pc.p_viewportZ.z, 0.0));
  float scatterPeak = max(max(effect.r, effect.g), effect.b);
  if (scatterPeak <= 1e-7 && effect.a >= 0.999999 &&
      readabilityAttenuation <= 1e-7) {
    o_color = base;
    return;
  }

  // 单次散射合成：先应用 Beer-Lambert T，再将散射映射进当前 LDR
  // headroom。按 headroom 做指数响应等价于每个通道上的 soft shoulder：
  // 小信号近似线性，强信号渐近 1，不会在最大档突然硬截成白片。
  // Apply the bounded true-CSM readability term only after the independent
  // directional guide has been reconstructed at the output pixel. Small
  // fractional coverage remains linear and cannot darken an entire 1/4 texel
  // before upsampling.
  float transmittance = clamp(
    effect.a * (1.0 - readabilityAttenuation), exp(-1.5), 1.0);
  vec3 scattering = max(effect.rgb, vec3(0.0));
  vec3 attenuatedBase = base.rgb * transmittance;
  vec3 headroom = max(
    vec3(1.0) - clamp(attenuatedBase, vec3(0.0), vec3(1.0)), vec3(0.0));
  vec3 safeHeadroom = max(headroom, vec3(1e-4));
  vec3 acceptedScattering =
    headroom * (vec3(1.0) - exp(-scattering / safeHeadroom));
  vec3 color = attenuatedBase + acceptedScattering;
  if (any(isnan(color)) || any(isinf(color)))
    color = base.rgb;

  // Safety clamp only; the exponential shoulder above performs the actual
  // highlight compression without altering the no-effect path.
  color = clamp(color, vec3(0.0), vec3(1.0));

  o_color = vec4(color, base.a);
}
