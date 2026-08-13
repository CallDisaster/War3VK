#version 460

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

// 阴影可见性预计算（当前帧）
//
// 说明：
// - 该 pass 只输出方向光（太阳）阴影可见性 vis（0..1），不负责调色/点光源/描边。
// - Shadow TAA 将在 receiver 中对 vis 做重投影与时域混合，降低 Alpha-Test 阴影闪烁。

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 1) uniform texture2DArray s_depth;
layout(set = 1, binding = 2) uniform texture2DArray s_shadow;
layout(set = 1, binding = 11) uniform texture2DArray s_casterMask;

layout(set = 1, binding = 3, scalar, row_major)
uniform ShadowData {
  mat4 u_view;
  mat4 u_invViewProj;
  mat4 u_lightViewProj[4];
  vec4 u_splitFar;  // view-space z
  vec4 u_params;    // x=shadowStrength, y=pcfRadius, z=invShadowRes, w=cascadeCount
  vec4 u_params2;   // x=receiverBias, y=cascadeBlendRange, z=debugMode, w=pointLightsEnabled
  vec4 u_params3;   // x=视口宽倒数, y=视口高倒数, z=pcssEnable, w=pcssSearchRadius(texel)
  vec4 u_params4;   // x=pcssMinRadius(texel), y=pcssMaxRadius(texel), z=pcssDepthScale, w=cascadeBiasScale
  vec4 u_sunDir;    // xyz=direction, w=unused
  vec4 u_params5;   // x=normalBiasScale, y=rimIntensity, z=rimPower, w=receiverMode
  vec4 u_params6;   // x=pcfKernel, y=pcfRotateMode, z=pcssSearchKernel, w=pcfCascadeRadiusScale
  vec4 u_viewport;  // x=vpX, y=vpY, z=vpW, w=vpH
  vec4 u_viewportZ; // x=minZ, y=maxZ, z=S1 terrain mask enabled, w=mask epsilon
  mat4 u_prevViewProj;
  vec4 u_taaParams; // x=taaEnabled, y=blendFactor, z=neighborClamp, w=hasHistory/hasPrev
} ubo;

layout(push_constant, scalar)
uniform push_block {
  uint p_colorSampler;
  uint p_rawShadowSampler;
  uint p_compareShadowSampler;
  // 0=nearest comparison, 1=hardware comparison-linear,
  // 2=manual compare-first 2x2 fallback.
  uint p_shadowCompareMode;
};

layout(location = 0) out float o_vis;

bool validFloat(float v) {
  return (v == v) && abs(v) < 1.0e20;
}

bool validVec2(vec2 v) {
  return validFloat(v.x) && validFloat(v.y);
}

bool validVec3(vec3 v) {
  return validFloat(v.x) && validFloat(v.y) && validFloat(v.z);
}

bool validVec4(vec4 v) {
  return validFloat(v.x) && validFloat(v.y) &&
         validFloat(v.z) && validFloat(v.w);
}

// These values become texture offsets, array-loop bounds, and (after the
// proof below) integer cascade/kernel selectors. Keep the proof shared with
// the direct receiver so a malformed UBO always means "no directional
// shadow", never an undefined float-to-int conversion or sample offset.
bool directionalCsmParamsValid() {
  return validFloat(ubo.u_params.w) &&
         ubo.u_params.w >= 1.0 && ubo.u_params.w <= 4.0 &&
         validFloat(ubo.u_params.z) && ubo.u_params.z > 0.0 &&
         validFloat(ubo.u_params6.x) &&
         ubo.u_params6.x >= 0.0 && ubo.u_params6.x <= 3.0 &&
         validFloat(ubo.u_params6.z) &&
         ubo.u_params6.z >= 0.0 && ubo.u_params6.z <= 1.0;
}

float shadowMapDepth(uint cascadeIndex, vec2 uv) {
  return texture(
    sampler2DArray(s_shadow, s_samplers[nonuniformEXT(p_rawShadowSampler)]),
    vec3(uv, float(cascadeIndex))).r;
}

float manualShadowCompareLinear2x2(uint cascadeIndex, vec2 uv,
                                   float refDepth) {
  ivec3 extent = textureSize(
    sampler2DArray(
      s_shadow, s_samplers[nonuniformEXT(p_rawShadowSampler)]),
    0);
  if (any(lessThanEqual(extent, ivec3(0))))
    return 1.0;

  int layer = clamp(int(cascadeIndex), 0, extent.z - 1);
  vec2 texelPosition = uv * vec2(extent.xy) - vec2(0.5);
  ivec2 base = ivec2(floor(texelPosition));
  vec2 weight = fract(texelPosition);
  ivec2 p00 = clamp(base, ivec2(0), extent.xy - ivec2(1));
  ivec2 p10 = clamp(base + ivec2(1, 0), ivec2(0),
                    extent.xy - ivec2(1));
  ivec2 p01 = clamp(base + ivec2(0, 1), ivec2(0),
                    extent.xy - ivec2(1));
  ivec2 p11 = clamp(base + ivec2(1, 1), ivec2(0),
                    extent.xy - ivec2(1));
  float v00 = refDepth <= texelFetch(
      sampler2DArray(
        s_shadow, s_samplers[nonuniformEXT(p_rawShadowSampler)]),
      ivec3(p00, layer), 0).r
      ? 1.0 : 0.0;
  float v10 = refDepth <= texelFetch(
      sampler2DArray(
        s_shadow, s_samplers[nonuniformEXT(p_rawShadowSampler)]),
      ivec3(p10, layer), 0).r
      ? 1.0 : 0.0;
  float v01 = refDepth <= texelFetch(
      sampler2DArray(
        s_shadow, s_samplers[nonuniformEXT(p_rawShadowSampler)]),
      ivec3(p01, layer), 0).r
      ? 1.0 : 0.0;
  float v11 = refDepth <= texelFetch(
      sampler2DArray(
        s_shadow, s_samplers[nonuniformEXT(p_rawShadowSampler)]),
      ivec3(p11, layer), 0).r
      ? 1.0 : 0.0;
  return mix(mix(v00, v10, weight.x),
             mix(v01, v11, weight.x), weight.y);
}

float shadowCompare(uint cascadeIndex, vec2 uv, float refDepth) {
  if (p_shadowCompareMode == 2u)
    return manualShadowCompareLinear2x2(cascadeIndex, uv, refDepth);
  return texture(
    sampler2DArrayShadow(
      s_shadow,
      s_samplers[nonuniformEXT(p_compareShadowSampler)]),
    vec4(uv, float(cascadeIndex), refDepth));
}

float casterMaskValue(uint cascadeIndex, vec2 uv) {
  if (ubo.u_viewportZ.z <= 0.5)
    return 0.0;
  return texture(
    sampler2DArray(s_casterMask,
                   s_samplers[nonuniformEXT(p_rawShadowSampler)]),
    vec3(uv, float(cascadeIndex))).r;
}

bool isTerrainMaskedOccluder(uint cascadeIndex, vec2 uv, float refDepth) {
  if (ubo.u_viewportZ.z <= 0.5)
    return false;
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return false;

  float blockerDepth = shadowMapDepth(cascadeIndex, uv);
  float eps = max(ubo.u_viewportZ.w, 0.0);
  if (refDepth <= blockerDepth + eps)
    return false;
  return casterMaskValue(cascadeIndex, uv) > 0.5;
}

const vec2 kPoisson16[16] = vec2[](
  vec2(-0.94201624, -0.39906216),
  vec2( 0.94201624,  0.39906216),
  vec2( 0.94558609, -0.76890725),
  vec2(-0.94558609,  0.76890725),
  vec2(-0.09418410, -0.92938870),
  vec2( 0.09418410,  0.92938870),
  vec2( 0.34495938,  0.29387760),
  vec2(-0.34495938, -0.29387760),
  vec2(-0.91588581,  0.45771432),
  vec2( 0.91588581, -0.45771432),
  vec2(-0.81544232, -0.87912464),
  vec2( 0.81544232,  0.87912464),
  vec2(-0.38277543,  0.27676845),
  vec2( 0.38277543, -0.27676845),
  vec2( 0.97484398,  0.75648379),
  vec2(-0.97484398, -0.75648379)
);

const vec2 kPoisson25[25] = vec2[](
  vec2(-0.978698, -0.088412),
  vec2(-0.826476,  0.623303),
  vec2(-0.695914, -0.675318),
  vec2(-0.243678,  0.914799),
  vec2(-0.073406, -0.879112),
  vec2( 0.265552, -0.421003),
  vec2( 0.347605,  0.172336),
  vec2( 0.850872,  0.325923),
  vec2( 0.980188, -0.256911),
  vec2( 0.489165, -0.732877),
  vec2(-0.382158, -0.159902),
  vec2(-0.143106,  0.196586),
  vec2( 0.087301,  0.520475),
  vec2( 0.179114, -0.156230),
  vec2( 0.256418,  0.873281),
  vec2(-0.408780,  0.551319),
  vec2(-0.782120, -0.272922),
  vec2(-0.625204,  0.111715),
  vec2( 0.413259, -0.411552),
  vec2( 0.912811,  0.002185),
  vec2( 0.480792,  0.642580),
  vec2(-0.177945, -0.632366),
  vec2(-0.701787, -0.511294),
  vec2( 0.020200, -0.310701),
  vec2( 0.693711, -0.211191)
);

vec2 rotateVec2(vec2 v, vec2 rot) {
  return vec2(v.x * rot.x - v.y * rot.y, v.x * rot.y + v.y * rot.x);
}

float computeCascadeBiasScale(int cascadeIndex, int cascadeCount, float scaleParam) {
  float t = (cascadeCount > 1) ? float(cascadeIndex) / float(cascadeCount - 1) : 0.0;
  // 与 receiver.frag 保持一致：远级联偏置放大收敛，减少脚底阴影被“抬离”。
  float target = 1.0 + 2.0 * t;
  float k = clamp(scaleParam, 0.0, 1.0);
  return mix(1.0, target, k);
}

float computeCascadePcfRadius(float baseRadiusTexel, int cascadeIndex, int cascadeCount, float scaleParam) {
  float scale = computeCascadeBiasScale(cascadeIndex, cascadeCount, scaleParam);
  return baseRadiusTexel / max(scale, 1e-6);
}

bool computeReceiverPlaneDepthGradient(vec4 lightClip, mat4 lightViewProj,
                                       vec3 worldDx, vec3 worldDy,
                                       out vec2 gradient) {
  gradient = vec2(0.0);
  if (!validVec4(lightClip) || abs(lightClip.w) < 1.0e-6 ||
      !validVec3(worldDx) || !validVec3(worldDy))
    return false;

  vec4 lightDx = vec4(worldDx, 0.0) * lightViewProj;
  vec4 lightDy = vec4(worldDy, 0.0) * lightViewProj;
  if (!validVec4(lightDx) || !validVec4(lightDy))
    return false;

  float invW = 1.0 / lightClip.w;
  vec3 ndc = lightClip.xyz * invW;
  if (!validVec3(ndc))
    return false;
  vec3 ndcDx = (lightDx.xyz - ndc * lightDx.w) * invW;
  vec3 ndcDy = (lightDy.xyz - ndc * lightDy.w) * invW;
  if (!validVec3(ndcDx) || !validVec3(ndcDy))
    return false;

  vec2 uvDx = vec2(0.5 * ndcDx.x, -0.5 * ndcDx.y);
  vec2 uvDy = vec2(0.5 * ndcDy.x, -0.5 * ndcDy.y);
  float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
  if (!validFloat(determinant) || abs(determinant) < 1.0e-10)
    return false;

  gradient = vec2(
      (ndcDx.z * uvDy.y - uvDx.y * ndcDy.z) / determinant,
      (uvDx.x * ndcDy.z - ndcDx.z * uvDy.x) / determinant);
  return validFloat(gradient.x) && validFloat(gradient.y);
}

bool receiverPlaneKernelValid(vec2 gradient, float maxAbsOffsetUv) {
  if (!validFloat(gradient.x) || !validFloat(gradient.y) ||
      !validFloat(maxAbsOffsetUv) || maxAbsOffsetUv < 0.0)
    return false;
  const float kMaxReceiverPlaneDepthDelta = 0.0025;
  float worstDepthDelta =
      (abs(gradient.x) + abs(gradient.y)) * maxAbsOffsetUv;
  return validFloat(worstDepthDelta) &&
         worstDepthDelta <= kMaxReceiverPlaneDepthDelta;
}

float receiverPlaneTapReference(float centerReference, vec2 tapOffsetUv,
                                vec2 gradient, bool kernelValid) {
  float reference = kernelValid
      ? centerReference + dot(gradient, tapOffsetUv)
      : centerReference;
  return clamp(reference, 0.0, 1.0);
}

// Keep the raw blocker search and comparison-sampled final PCF bit-for-bit
// aligned with the direct receiver. The prepass is an alternate source for
// TAA, not an alternate CSM filtering model.
float computePcssRadius(uint cascadeIndex, vec2 uv, float refDepth,
                        vec2 receiverPlaneGradient,
                        bool receiverPlaneValid) {
  float radius = validFloat(ubo.u_params.y)
      ? max(ubo.u_params.y, 0.0) : 0.0;
  bool pcssEnabled = validFloat(ubo.u_params3.z) &&
      ubo.u_params3.z > 0.5;
  if (!pcssEnabled)
    return radius;

  float invRes = ubo.u_params.z;
  float searchRadius = max(ubo.u_params3.w, 0.0);
  float minRadius = max(ubo.u_params4.x, 0.0);
  float maxRadius = max(ubo.u_params4.y, minRadius);
  float depthScale = max(ubo.u_params4.z, 0.0);
  if (!validFloat(ubo.u_params6.z) || ubo.u_params6.z < 0.0 ||
      ubo.u_params6.z > 1.0)
    return radius;
  int searchExtent = ubo.u_params6.z > 0.5 ? 2 : 1;
  if (!validFloat(invRes) || invRes <= 0.0 ||
      !validFloat(searchRadius) || !validFloat(minRadius) ||
      !validFloat(maxRadius) || !validFloat(depthScale))
    return radius;

  float sum = 0.0;
  float count = 0.0;
  bool blockerPlaneValid = receiverPlaneValid && receiverPlaneKernelValid(
      receiverPlaneGradient,
      float(searchExtent) * searchRadius * invRes);
  for (int y = -searchExtent; y <= searchExtent; y++) {
    for (int x = -searchExtent; x <= searchExtent; x++) {
      vec2 offset = vec2(float(x), float(y)) * searchRadius * invRes;
      vec2 tapUv = uv + offset;
      if (tapUv.x < 0.0 || tapUv.x > 1.0 ||
          tapUv.y < 0.0 || tapUv.y > 1.0)
        continue;
      float tapRef = receiverPlaneTapReference(
          refDepth, offset, receiverPlaneGradient, blockerPlaneValid);
      float depth = shadowMapDepth(cascadeIndex, tapUv);
      if (depth < tapRef) {
        sum += depth;
        count += 1.0;
      }
    }
  }
  if (count <= 0.0)
    return minRadius;
  float penumbra = (refDepth - sum / count) * depthScale;
  return clamp(minRadius + penumbra, minRadius, maxRadius);
}

float sampleShadowGrid(uint cascadeIndex, vec2 uv, float refDepth,
                       float radiusTexel, int gridRadius,
                       vec2 receiverPlaneGradient,
                       bool receiverPlaneValid) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = ubo.u_params.z;
  float radius = max(radiusTexel, 0.0);
  float sum = 0.0;
  float count = 0.0;
  bool kernelPlaneValid = receiverPlaneValid && receiverPlaneKernelValid(
      receiverPlaneGradient, float(gridRadius) * radius * invRes);

  for (int y = -gridRadius; y <= gridRadius; y++) {
    for (int x = -gridRadius; x <= gridRadius; x++) {
      vec2 o = vec2(float(x), float(y)) * radius * invRes;
      vec2 tapUv = uv + o;
      // 避免边界采样把 clamp-to-edge 变成伪阴影
      if (tapUv.x < 0.0 || tapUv.x > 1.0 || tapUv.y < 0.0 || tapUv.y > 1.0)
        sum += 1.0;
      else
        sum += shadowCompare(
            cascadeIndex, tapUv,
            receiverPlaneTapReference(
                refDepth, o, receiverPlaneGradient, kernelPlaneValid));
      count += 1.0;
    }
  }
  return sum / max(count, 1.0);
}

float sampleShadowPoisson16(uint cascadeIndex, vec2 uv, float refDepth,
                            float radiusTexel, vec2 rot,
                            vec2 receiverPlaneGradient,
                            bool receiverPlaneValid) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = ubo.u_params.z;
  float radius = max(radiusTexel, 0.0);
  float sum = 0.0;
  bool kernelPlaneValid = receiverPlaneValid && receiverPlaneKernelValid(
      receiverPlaneGradient, radius * invRes);

  for (int i = 0; i < 16; i++) {
    vec2 d = kPoisson16[i];
    vec2 o = rotateVec2(d, rot) * radius * invRes;
    vec2 tapUv = uv + o;
    if (tapUv.x < 0.0 || tapUv.x > 1.0 || tapUv.y < 0.0 || tapUv.y > 1.0)
      sum += 1.0;
    else
      sum += shadowCompare(
          cascadeIndex, tapUv,
          receiverPlaneTapReference(
              refDepth, o, receiverPlaneGradient, kernelPlaneValid));
  }
  return sum * (1.0 / 16.0);
}

float sampleShadowPoisson25(uint cascadeIndex, vec2 uv, float refDepth,
                            float radiusTexel, vec2 rot,
                            vec2 receiverPlaneGradient,
                            bool receiverPlaneValid) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = ubo.u_params.z;
  float radius = max(radiusTexel, 0.0);
  float sum = 0.0;
  bool kernelPlaneValid = receiverPlaneValid && receiverPlaneKernelValid(
      receiverPlaneGradient, radius * invRes);

  for (int i = 0; i < 25; i++) {
    vec2 d = kPoisson25[i];
    vec2 o = rotateVec2(d, rot) * radius * invRes;
    vec2 tapUv = uv + o;
    if (tapUv.x < 0.0 || tapUv.x > 1.0 || tapUv.y < 0.0 || tapUv.y > 1.0)
      sum += 1.0;
    else
      sum += shadowCompare(
          cascadeIndex, tapUv,
          receiverPlaneTapReference(
              refDepth, o, receiverPlaneGradient, kernelPlaneValid));
  }
  return sum * (1.0 / 25.0);
}

float sampleShadowPcf(uint cascadeIndex, vec2 uv, float refDepth,
                      float radiusTexel, vec2 rot,
                      vec2 receiverPlaneGradient,
                      bool receiverPlaneValid) {
  float kernel = ubo.u_params6.x;
  if (!validFloat(kernel) || kernel < 0.0 || kernel > 3.0)
    return 1.0;
  if (kernel >= 0.5 && kernel < 1.5)
    return sampleShadowGrid(cascadeIndex, uv, refDepth, radiusTexel, 2,
                            receiverPlaneGradient, receiverPlaneValid);
  if (kernel >= 1.5 && kernel < 2.5)
    return sampleShadowPoisson16(cascadeIndex, uv, refDepth, radiusTexel, rot,
                                 receiverPlaneGradient, receiverPlaneValid);
  if (kernel >= 2.5)
    return sampleShadowPoisson25(cascadeIndex, uv, refDepth, radiusTexel, rot,
                                 receiverPlaneGradient, receiverPlaneValid);
  return sampleShadowGrid(cascadeIndex, uv, refDepth, radiusTexel, 1,
                          receiverPlaneGradient, receiverPlaneValid);
}

// TAA 专用快速 2x2 采样（4 次纹理读取）
float sampleShadowFast4(uint cascadeIndex, vec2 uv, float refDepth) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = ubo.u_params.z;
  float sum = 0.0;
  sum += shadowCompare(cascadeIndex, uv + vec2(-0.5, -0.5) * invRes, refDepth);
  sum += shadowCompare(cascadeIndex, uv + vec2( 0.5, -0.5) * invRes, refDepth);
  sum += shadowCompare(cascadeIndex, uv + vec2(-0.5,  0.5) * invRes, refDepth);
  sum += shadowCompare(cascadeIndex, uv + vec2( 0.5,  0.5) * invRes, refDepth);
  return sum * 0.25;
}

float sampleShadowCross5(uint cascadeIndex, vec2 uv, float refDepth, float radiusTexel) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = ubo.u_params.z;
  float radius = max(radiusTexel, 0.75) * invRes;
  float sum = shadowCompare(cascadeIndex, uv, refDepth);
  sum += shadowCompare(cascadeIndex, uv + vec2( radius, 0.0), refDepth);
  sum += shadowCompare(cascadeIndex, uv + vec2(-radius, 0.0), refDepth);
  sum += shadowCompare(cascadeIndex, uv + vec2(0.0,  radius), refDepth);
  sum += shadowCompare(cascadeIndex, uv + vec2(0.0, -radius), refDepth);
  return sum * 0.2;
}

vec3 computeViewNormal(vec3 viewPos, vec3 viewDx, vec3 viewDy) {
  // Use derivatives for stable slope estimation on steep walls and props.
  vec3 normV_raw = cross(viewDy, viewDx);
  float n2 = dot(normV_raw, normV_raw);
  vec3 normV =
      (n2 > 1e-14) ? (normV_raw * inversesqrt(n2)) : vec3(0.0, 0.0, 1.0);

  vec3 viewDirV = normalize(-viewPos);
  if (dot(normV, viewDirV) < 0.0)
    normV = -normV;

  return normV;
}

vec3 computeWorldUpInView() {
  vec3 worldUpV = (vec4(0.0, 0.0, 1.0, 0.0) * ubo.u_view).xyz;
  float upLen2 = dot(worldUpV, worldUpV);
  return (upLen2 > 1e-12)
      ? (worldUpV * inversesqrt(upLen2))
      : vec3(0.0, 1.0, 0.0);
}

float computeWallReceiverFactor(vec3 normV) {
  float upDot = abs(dot(normV, computeWorldUpInView()));
  return smoothstep(0.15, 0.75, 1.0 - upDot);
}

float computeReceiverGrazingFactor(vec3 normV, vec3 viewPos) {
  float viewLen2 = dot(viewPos, viewPos);
  if (viewLen2 <= 1e-8)
    return 0.0;

  vec3 viewDirV = -viewPos * inversesqrt(viewLen2);
  float facing = abs(dot(normV, viewDirV));
  return 1.0 - smoothstep(0.25, 0.65, facing);
}

float computeLightGrazingFactor(vec3 normV, vec3 lightDirV) {
  float ndotl = abs(dot(normV, lightDirV));
  return 1.0 - smoothstep(0.25, 0.72, ndotl);
}

float computeWallStabilityFactor(vec3 normV, vec3 viewPos, vec3 lightDirV) {
  float wallFactor = computeWallReceiverFactor(normV);
  float viewFactor = computeReceiverGrazingFactor(normV, viewPos);
  float lightFactor = computeLightGrazingFactor(normV, lightDirV);
  return wallFactor * max(viewFactor, lightFactor);
}

float computeWallFilterWeight(float wallFactor, float wallStabilityFactor,
                              float lightGrazingFactor,
                              float viewGrazingFactor) {
  float receiverWeight = smoothstep(0.18, 0.60, wallFactor);
  float grazingWeight = smoothstep(
      0.04, 0.40,
      max(wallStabilityFactor,
          max(lightGrazingFactor, viewGrazingFactor)));
  return clamp(receiverWeight * grazingWeight, 0.0, 1.0);
}

float computeShadowVisibility(vec3 worldPos, vec3 worldDx, vec3 worldDy,
                              float viewDepth, float biasExtra, vec2 rot,
                              float wallFilterWeight) {
  const bool diagnoseCsm = validFloat(ubo.u_params2.z) &&
      ubo.u_params2.z >= 8.5 && ubo.u_params2.z < 9.5;
  if (!validVec3(worldPos) || !validVec3(worldDx) || !validVec3(worldDy) ||
      !validFloat(viewDepth) || !validFloat(biasExtra) ||
      !validFloat(wallFilterWeight))
    return diagnoseCsm ? 0.0 : 1.0;
  if (!directionalCsmParamsValid() ||
      !validFloat(ubo.u_params2.x) ||
      !validFloat(ubo.u_params2.y) || !validFloat(ubo.u_params4.w) ||
      !validFloat(ubo.u_params6.w))
    return diagnoseCsm ? 0.0 : 1.0;
  int cascadeCount = int(ubo.u_params.w);

  float splits[4];
  splits[0] = ubo.u_splitFar.x;
  splits[1] = ubo.u_splitFar.y;
  splits[2] = ubo.u_splitFar.z;
  splits[3] = ubo.u_splitFar.w;
  for (int i = 0; i < cascadeCount; i++) {
    if (!validFloat(splits[i]))
      return diagnoseCsm ? 0.0 : 1.0;
  }

  int c0 = cascadeCount - 1;
  for (int i = 0; i < cascadeCount; i++) {
    if (viewDepth <= splits[i]) {
      c0 = i;
      break;
    }
  }

  float baseBias = max(ubo.u_params2.x, 0.0) + max(biasExtra, 0.0);
  float cascadeBiasScale = max(ubo.u_params4.w, 0.0);
  float pcfCascadeRadiusScale = max(ubo.u_params6.w, 0.0);

  vec4 p = vec4(worldPos, 1.0);

  // The prepass is an alternate source for TAA, not an alternate CSM model:
  // it must keep the same cascade blend as DirectInline.
  vec4 l0 = p * ubo.u_lightViewProj[c0];
  if (!validVec4(l0) || l0.w <= 0.0)
    return diagnoseCsm ? 0.05 : 1.0;
  vec3 n0 = l0.xyz / l0.w;
  if (!validVec3(n0) || n0.z < 0.0 || n0.z > 1.0)
    return diagnoseCsm ? 0.15 : 1.0;

  vec2 uv0 = n0.xy * 0.5 + 0.5;
  uv0.y = 1.0 - uv0.y;
  if (uv0.x < 0.0 || uv0.x > 1.0 || uv0.y < 0.0 || uv0.y > 1.0)
    return diagnoseCsm ? 0.25 : 1.0;
  float bias0 = baseBias * computeCascadeBiasScale(c0, cascadeCount, cascadeBiasScale);
  // 与 receiver.frag 一致：越界直接全亮会造成接触阴影突然断裂。
  float ref0 = clamp(n0.z - bias0, 0.0, 1.0);
  vec2 receiverPlaneGradient0 = vec2(0.0);
  bool receiverPlaneValid0 = computeReceiverPlaneDepthGradient(
      l0, ubo.u_lightViewProj[c0], worldDx, worldDy,
      receiverPlaneGradient0);
  if (isTerrainMaskedOccluder(uint(c0), uv0, ref0))
    return diagnoseCsm ? 0.35 : 1.0;

  if (diagnoseCsm) {
    float blockerDepth = shadowMapDepth(uint(c0), uv0);
    if (blockerDepth >= 0.99999)
      return 0.45;
    return ref0 <= blockerDepth ? 0.55 : 0.65;
  }

  float radius0 = computePcssRadius(
      uint(c0), uv0, ref0, receiverPlaneGradient0, receiverPlaneValid0);
  radius0 = computeCascadePcfRadius(radius0, c0, cascadeCount, pcfCascadeRadiusScale);
  radius0 = mix(radius0, max(radius0, 1.50), wallFilterWeight);
  float vis0 = sampleShadowPcf(
      uint(c0), uv0, ref0, radius0, rot,
      receiverPlaneGradient0, receiverPlaneValid0);

  // Match receiver-side cascade blending. Without this, the TAA source texture
  // has hard split transitions that history cannot fully hide during camera
  // motion.
  float blendRange = max(ubo.u_params2.y, 0.0);
  if (blendRange > 0.0 && c0 < cascadeCount - 1) {
    float far0 = splits[c0];
    float t = clamp((viewDepth - (far0 - blendRange)) / blendRange, 0.0, 1.0);
    float w = t * t * (3.0 - 2.0 * t);
    if (w > 1e-6) {
      int c1 = c0 + 1;
      vec4 l1 = p * ubo.u_lightViewProj[c1];
      if (validVec4(l1) && l1.w > 0.0) {
        vec3 n1 = l1.xyz / l1.w;
        if (validVec3(n1) && n1.z >= 0.0 && n1.z <= 1.0) {
          vec2 uv1 = n1.xy * 0.5 + 0.5;
          uv1.y = 1.0 - uv1.y;
          if (uv1.x < 0.0 || uv1.x > 1.0 ||
              uv1.y < 0.0 || uv1.y > 1.0)
            return vis0;
          float bias1 = baseBias * computeCascadeBiasScale(c1, cascadeCount, cascadeBiasScale);
          float ref1 = clamp(n1.z - bias1, 0.0, 1.0);
          vec2 receiverPlaneGradient1 = vec2(0.0);
          bool receiverPlaneValid1 = computeReceiverPlaneDepthGradient(
              l1, ubo.u_lightViewProj[c1], worldDx, worldDy,
              receiverPlaneGradient1);
          float radius1 = computePcssRadius(
              uint(c1), uv1, ref1,
              receiverPlaneGradient1, receiverPlaneValid1);
          radius1 = computeCascadePcfRadius(radius1, c1, cascadeCount, pcfCascadeRadiusScale);
          radius1 = mix(radius1, max(radius1, 1.50), wallFilterWeight);
          float vis1 = 1.0;
          if (!isTerrainMaskedOccluder(uint(c1), uv1, ref1)) {
            vis1 = sampleShadowPcf(
                uint(c1), uv1, ref1, radius1, rot,
                receiverPlaneGradient1, receiverPlaneValid1);
          }
          return mix(vis0, vis1, w);
        }
      }
    }
  }

  return vis0;
}

void main() {
  ivec2 pix = ivec2(gl_FragCoord.xy);
  int layer = 0;

  // Depth is required for world position reconstruction
  float depth = texelFetch(
    sampler2DArray(s_depth, s_samplers[nonuniformEXT(p_colorSampler)]),
    ivec3(pix, layer),
    0).r;

  // Derivatives are undefined in non-uniform control flow. Build finite,
  // branchless-safe reconstruction candidates before *any* per-pixel return,
  // then keep the visibility pass fail-soft below. Invalid data contributes a
  // zero derivative candidate and never reaches a sample coordinate, array
  // index, or float-to-int conversion.
  vec2 rawVpMin = ubo.u_viewport.xy;
  vec2 rawVpSize = ubo.u_viewport.zw;
  bool viewportValid = validVec2(rawVpMin) && validVec2(rawVpSize);
  vec2 vpMin = viewportValid ? rawVpMin : vec2(0.0);
  vec2 vpSize = max(viewportValid ? rawVpSize : vec2(1.0), vec2(1.0));
  bool depthValid = validFloat(depth);
  float safeDepth = depthValid ? depth : 0.0;
  float minZ = ubo.u_viewportZ.x;
  float maxZ = ubo.u_viewportZ.y;
  bool depthRangeValid = validFloat(minZ) && validFloat(maxZ);
  float safeMinZ = validFloat(minZ) ? minZ : 0.0;
  float safeMaxZ = validFloat(maxZ) ? maxZ : 1.0;
  float depthSpan = safeMaxZ - safeMinZ;
  bool normalizedDepth = depthRangeValid && abs(depthSpan) > 1e-6;
  float safeDepthSpan = normalizedDepth ? depthSpan : 1.0;
  float depthNCandidate = normalizedDepth
      ? (safeDepth - safeMinZ) / safeDepthSpan
      : safeDepth;
  depthNCandidate = clamp(depthNCandidate, 0.0, 1.0);
  vec2 uvVpCandidate = (vec2(pix) - vpMin) / vpSize;
  vec4 clipCandidate = vec4(
      uvVpCandidate.x * 2.0 - 1.0,
      1.0 - uvVpCandidate.y * 2.0,
      depthNCandidate, 1.0);
  vec4 worldHCandidate = clipCandidate * ubo.u_invViewProj;
  bool worldHCandidateValid = validVec4(worldHCandidate) &&
      abs(worldHCandidate.w) >= 1e-6;
  float safeWorldW = worldHCandidateValid ? worldHCandidate.w : 1.0;
  vec3 worldPosCandidate = worldHCandidateValid
      ? worldHCandidate.xyz / safeWorldW
      : vec3(0.0);
  bool worldPosCandidateValid = worldHCandidateValid &&
      validVec3(worldPosCandidate);
  vec3 safeWorldPos = worldPosCandidateValid
      ? worldPosCandidate : vec3(0.0);
  vec4 viewHCandidate = vec4(safeWorldPos, 1.0) * ubo.u_view;
  bool viewHCandidateValid = validVec4(viewHCandidate);
  vec3 viewPosCandidate = viewHCandidateValid
      ? viewHCandidate.xyz : vec3(0.0);
  float viewDepthCandidate = viewHCandidateValid
      ? abs(viewHCandidate.z) : 0.0;
  bool viewDepthCandidateValid = viewHCandidateValid &&
      validFloat(viewDepthCandidate);
  float safeFarSplitForDepth = validFloat(ubo.u_splitFar.w)
      ? max(ubo.u_splitFar.w, 1e-4) : 1e-4;
  float linearDepthCandidate = clamp(
      viewDepthCandidate / safeFarSplitForDepth, 0.0, 1.0);
  vec3 worldDx = dFdx(safeWorldPos);
  vec3 worldDy = dFdy(safeWorldPos);
  vec3 viewDx = dFdx(viewPosCandidate);
  vec3 viewDy = dFdy(viewPosCandidate);
  float linearDepthDx = dFdx(linearDepthCandidate);
  float linearDepthDy = dFdy(linearDepthCandidate);

  // 仅对主世界 viewport 区域做阴影处理，避免影响 UI/空白区域
  if (!depthValid || !viewportValid || !depthRangeValid ||
      float(pix.x) < vpMin.x || float(pix.y) < vpMin.y ||
      float(pix.x) >= (vpMin.x + vpSize.x) ||
      float(pix.y) >= (vpMin.y + vpSize.y)) {
    o_vis = 1.0;
    return;
  }

  float depthN = depthNCandidate;

  // 以 viewport 为基准重建 NDC（与 receiver 保持一致）
  vec2 uvVp = uvVpCandidate;
  vec4 clip = clipCandidate;

  vec4 worldH = worldHCandidate;
  if (!worldHCandidateValid) {
    o_vis = 1.0;
    return;
  }
  vec3 worldPos = worldPosCandidate;
  if (!worldPosCandidateValid) {
    o_vis = 1.0;
    return;
  }

  vec4 viewH = viewHCandidate;
  if (!viewHCandidateValid) {
    o_vis = 1.0;
    return;
  }
  float viewDepth = viewDepthCandidate;
  if (!viewDepthCandidateValid) {
    o_vis = 1.0;
    return;
  }
  vec3 viewPos = viewPosCandidate;

  float receiverMode = ubo.u_params5.w;
  float normalBiasScale = max(ubo.u_params5.x, 0.0);
  float biasExtra = 0.0;
  bool needNormal = true;
  vec3 normV = vec3(0.0, 0.0, 1.0);
  if (needNormal) {
    normV = computeViewNormal(viewPos, viewDx, viewDy);
  }
  vec3 lightDirV = vec3(0.0, 0.0, 1.0);
  if (needNormal) {
    lightDirV = normalize((vec4(ubo.u_sunDir.xyz, 0.0) * ubo.u_view).xyz);
  }
  float viewGrazingFactor = needNormal
      ? computeReceiverGrazingFactor(normV, viewPos)
      : 0.0;
  float lightGrazingFactor = needNormal
      ? computeLightGrazingFactor(normV, lightDirV)
      : 0.0;
  float wallStabilityFactor = needNormal
      ? computeWallStabilityFactor(normV, viewPos, lightDirV)
      : 0.0;
  float wallFactor = needNormal ? computeWallReceiverFactor(normV) : 0.0;
  float wallFilterWeight = computeWallFilterWeight(
      wallFactor, wallStabilityFactor, lightGrazingFactor,
      viewGrazingFactor);
  bool directionalCsmValid = directionalCsmParamsValid();
  if (directionalCsmValid && receiverMode > 0.5 &&
      normalBiasScale > 0.0) {
    float ndotl = abs(dot(normV, lightDirV));

    // Keep non-zero bias in far cascades to prevent wall-striping acne.
    float depthRatio = clamp(viewDepth / max(ubo.u_splitFar.w, 1e-4), 0.0, 1.0);
    const float minFarWeight = 0.35;
    float normalBiasWeight = mix(1.0, minFarWeight, depthRatio * depthRatio);
    float wallBiasDampen = mix(1.0, 0.45, wallStabilityFactor);
    wallBiasDampen = mix(
        wallBiasDampen, min(wallBiasDampen, 0.40), wallFilterWeight);
    float finalNormalScale = normalBiasScale * normalBiasWeight * wallBiasDampen;

    biasExtra = finalNormalScale * (1.0 - ndotl);
    if (receiverMode > 1.5) {
      float slope = sqrt(max(1.0 - ndotl * ndotl, 0.0));
      biasExtra += finalNormalScale * 0.35 * slope;
    }

    // Cap extra bias to avoid wall-contact shadow loss (Peter-Panning).
    float baseReceiverBias = max(ubo.u_params2.x, 0.0);
    float texelBiasFloor = 2.5 * ubo.u_params.z;
    float extraBiasMax = max(baseReceiverBias * 0.75, texelBiasFloor);
    float wallBiasCap = max(baseReceiverBias * 0.65, texelBiasFloor * 1.25);
    extraBiasMax = mix(extraBiasMax, wallBiasCap, wallStabilityFactor);
    float stableBiasCap =
        max(baseReceiverBias * 0.55, texelBiasFloor * 1.35);
    extraBiasMax = mix(
        extraBiasMax, min(extraBiasMax, stableBiasCap), wallFilterWeight);
    biasExtra = clamp(biasExtra, 0.0, extraBiasMax);
  }

  vec2 pcfRot = vec2(1.0, 0.0);
  // Direct and prepass share one deterministic, zero-centroid kernel. A
  // periodic rotation field in either source becomes visible edge crawl.

  o_vis = computeShadowVisibility(
      worldPos, worldDx, worldDy, viewDepth, biasExtra, pcfRot,
      wallFilterWeight);
}
