#version 460

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2DArray s_color;
layout(set = 1, binding = 1) uniform texture2DArray s_effect;
layout(set = 1, binding = 2) uniform texture2DArray s_depth;

layout(push_constant, scalar)
uniform push_block {
  uint p_colorSampler;
  uint p_effectSampler;
  uint p_depthSampler;
  uint p_pad1;
  vec4 p_rtSize; // x=fullW y=fullH z=effectW w=effectH
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

vec4 depthAwareUpsample(vec2 uv, ivec2 fullSize, ivec2 effectSize,
                        float centerDepth) {
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
    deltas[i] = abs(guideDepth - centerDepth);
    if (deltas[i] < bestDelta) {
      bestDelta = deltas[i];
      bestIndex = i;
    }

    float wx = offsets[i].x == 0 ? (1.0 - f.x) : f.x;
    float wy = offsets[i].y == 0 ? (1.0 - f.y) : f.y;
    spatial[i] = max(wx * wy, 1e-4);
  }

  // 先找深度最接近的 low-res 样本，再相对它衰减其余样本。
  // 这比固定 raw-depth 阈值更能兼容 War3 不同投影/相机高度。
  float depthScale = max(2e-5, bestDelta * 2.0 + 2e-5);

  // Among depth-compatible candidates, use the spatially nearest one as the
  // volumetric-edge reference. On flat terrain all four depth deltas can tie;
  // always using array element zero in that case made a thin column crawl or
  // wash across an entire 4x4 full-resolution footprint as the camera moved.
  int referenceIndex = bestIndex;
  float referenceSpatial = -1.0;
  float referenceDepthTolerance = max(2e-5, bestDelta * 0.25 + 2e-5);
  for (int i = 0; i < 4; i++) {
    if (deltas[i] <= bestDelta + referenceDepthTolerance &&
        spatial[i] > referenceSpatial) {
      referenceIndex = i;
      referenceSpatial = spatial[i];
    }
  }

  vec4 sumEffect = vec4(0.0);
  float weightSum = 0.0;
  for (int i = 0; i < 4; i++) {
    float relativeDelta = max(deltas[i] - bestDelta, 0.0);
    float depthWeight = exp2(-4.0 * relativeDelta / depthScale);
    float guideWeight = spatial[i] * depthWeight;
    // RGB scattering and alpha transmittance are the low-resolution solution,
    // not a range guide for themselves. Using either signal as its own guide
    // preserved quarter-resolution shadow texels as block staircases. The
    // independent full-resolution scene depth is the joint-bilateral guide:
    // geometry discontinuities remain sealed, while volumetric boundaries on
    // one continuous surface receive bilinear coverage AA.
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
  vec4 effect = depthAwareUpsample(uv, fullSize, effectSize, centerDepth);

  float scatterPeak = max(max(effect.r, effect.g), effect.b);
  if (scatterPeak <= 1e-7 && effect.a >= 0.999999) {
    o_color = base;
    return;
  }

  // 单次散射合成：先应用 Beer-Lambert T，再将散射映射进当前 LDR
  // headroom。按 headroom 做指数响应等价于每个通道上的 soft shoulder：
  // 小信号近似线性，强信号渐近 1，不会在最大档突然硬截成白片。
  // Transmittance includes physical extinction plus the integrator's bounded
  // 24% true-CSM column-readability term.
  float transmittance = clamp(effect.a, exp(-1.5), 1.0);
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
