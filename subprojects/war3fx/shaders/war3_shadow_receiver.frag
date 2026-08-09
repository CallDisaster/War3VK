#version 460

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2DArray s_color;
layout(set = 1, binding = 1) uniform texture2DArray s_depth;
layout(set = 1, binding = 2) uniform texture2DArray s_shadow;
layout(set = 1, binding = 5) uniform textureCubeArray s_pointShadow;  // Point Light CubeArray Shadow
// Shadow TAA 相关资源（屏幕空间 2D）
layout(set = 1, binding = 7) uniform texture2D s_shadowCurrent;
layout(set = 1, binding = 8) uniform texture2D s_motionVector;
layout(set = 1, binding = 9) uniform texture2D s_shadowHistory;
layout(set = 1, binding = 10, rg16f) uniform image2D s_shadowHistoryWrite;
layout(set = 1, binding = 11) uniform texture2DArray s_casterMask;
layout(set = 1, binding = 12) uniform texture2DArray s_pointContactVisibility;
layout(set = 1, binding = 13) uniform texture2D s_pointContactHiz;

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
  // x=0 DirectInline, 1 PrepassCurrentOnly, 2 TemporalCurrentOnly,
  //   3 TemporalHistory
  // y=current-frame weight, z=neighbor clamp, w=readable history contract
  vec4 u_taaParams;
  mat4 u_proj;       // current view -> clip, used by software contact rays
  vec4 u_pointRayParams;  // x=enabled y=strength z=maxDistance w=thickness
  vec4 u_pointRayParams2; // x=steps y=startOffset z=maxLights w=A1 layerCount
  // x=projection-derived far clear raw, y=known, z=raw depth quantum
  vec4 u_depthContract;
} ubo;

struct PointLight {
    vec4 pos;   // xyz, w=range
    vec4 color; // rgb, w=intensity
    // x=authored shadow intensity; yzw=per-frame view-space position
    vec4 params;
};

layout(set = 1, binding = 4, scalar)
uniform LightBlock {
    uint u_count;
    uint u_pad[3];  // Align to 16 bytes (match C++ side)
    PointLight u_lights[16];
} lights;

const uint POINT_SHADOW_MAX_LIGHTS = 4u;

struct PointShadowLightData {
    vec4 lightPos;  // xyz=position, w=range
    float bias;
    float enabled;
    float shadowIntensity;
    float pad0;
};

// Point Shadow Data
layout(set = 1, binding = 6, scalar)
uniform PointShadowBlock {
    uint u_lightCount;
    uint u_debugLightIndex;
    uint u_samplerIndex;
    uint u_pad2;
    // x=pcfNear y=pcfFar z=texelBiasScale w=rangeFadeStart
    vec4 u_filterParams;
    PointShadowLightData u_lights[POINT_SHADOW_MAX_LIGHTS];
} pointShadow;

layout(location = 0) in  vec2 i_pos;
layout(location = 0) out vec4 o_color;

layout(push_constant, scalar)
uniform push_block {
  uint p_colorSampler;
  uint p_rawShadowSampler;
  uint p_compareShadowSampler;
  // 0=nearest comparison, 1=hardware comparison-linear,
  // 2=manual compare-first 2x2 fallback.
  uint p_shadowCompareMode;
};

bool validFloat(float v) {
  return (v == v) && abs(v) < 1.0e20;
}

bool validVec3(vec3 v) {
  return validFloat(v.x) && validFloat(v.y) && validFloat(v.z);
}

bool validVec4(vec4 v) {
  return validFloat(v.x) && validFloat(v.y) &&
         validFloat(v.z) && validFloat(v.w);
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
  // 远级联偏置放大过大时容易出现“接触阴影丢失（脚底缺阴影）”。
  // 将最大倍率从 4.0（1+3*t）收敛到 3.0（1+2*t），减轻远级联 Peter-Panning。
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
  vec3 ndcDx = (lightDx.xyz - ndc * lightDx.w) * invW;
  vec3 ndcDy = (lightDy.xyz - ndc * lightDy.w) * invW;
  if (!validVec3(ndcDx) || !validVec3(ndcDy))
    return false;

  // The shadow texture flips NDC Y, so its differential must be flipped too.
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
  // Reject discontinuities and degenerate reconstructed planes as one whole
  // kernel. 0.25% of normalized cascade depth is deliberately conservative;
  // a rejected proof uses the historical centre reference for every tap.
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
  int kernel = int(ubo.u_params6.x + 0.5);
  if (kernel == 1)
    return sampleShadowGrid(cascadeIndex, uv, refDepth, radiusTexel, 2,
                            receiverPlaneGradient, receiverPlaneValid);
  if (kernel == 2)
    return sampleShadowPoisson16(cascadeIndex, uv, refDepth, radiusTexel, rot,
                                 receiverPlaneGradient, receiverPlaneValid);
  if (kernel == 3)
    return sampleShadowPoisson25(cascadeIndex, uv, refDepth, radiusTexel, rot,
                                 receiverPlaneGradient, receiverPlaneValid);
  return sampleShadowGrid(cascadeIndex, uv, refDepth, radiusTexel, 1,
                          receiverPlaneGradient, receiverPlaneValid);
}

vec3 computeViewNormal(vec3 viewPos) {
  // Use local derivatives in view-space to avoid depth-neighbor reconstruction
  // instability on steep walls/decorations.
  vec3 dX = dFdx(viewPos);
  vec3 dY = dFdy(viewPos);
  vec3 normV_raw = cross(dY, dX);
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
  int cascadeCount = clamp(int(ubo.u_params.w), 1, 4);

  float splits[4];
  splits[0] = ubo.u_splitFar.x;
  splits[1] = ubo.u_splitFar.y;
  splits[2] = ubo.u_splitFar.z;
  splits[3] = ubo.u_splitFar.w;

  int c0 = cascadeCount - 1;
  for (int i = 0; i < cascadeCount; i++) {
    if (viewDepth <= splits[i]) {
      c0 = i;
      break;
    }
  }

  float baseBias = max(ubo.u_params2.x, 0.0) + max(biasExtra, 0.0);
  float blendRange = max(ubo.u_params2.y, 0.0);
  bool pcssEnabled = (ubo.u_params3.z > 0.5);
  float pcssSearchRadius = max(ubo.u_params3.w, 0.0);
  float pcssMinRadius = max(ubo.u_params4.x, 0.0);
  float pcssMaxRadius = max(ubo.u_params4.y, pcssMinRadius);
  float pcssDepthScale = max(ubo.u_params4.z, 0.0);
  float cascadeBiasScale = max(ubo.u_params4.w, 0.0);
  float pcfCascadeRadiusScale = max(ubo.u_params6.w, 0.0);
  int pcssSearchKernel = int(ubo.u_params6.z + 0.5);
  int searchRadius = (pcssSearchKernel > 0) ? 2 : 1;

  vec4 p = vec4(worldPos, 1.0);

  // Sample primary cascade
  vec4 l0 = p * ubo.u_lightViewProj[c0];
  // Outside the light clip volume: treat as fully lit
  if (l0.w <= 0.0)
    return 1.0;
  vec3 n0 = l0.xyz / l0.w;
  if (n0.z < 0.0 || n0.z > 1.0)
    return 1.0;
  // Shadow map is rendered with a negative viewport height (DXVK/D3D style),
  // which flips NDC-Y. Vulkan texture coordinates use top-left origin, so
  // convert NDC->UV with an explicit Y flip.
  vec2 uv0 = n0.xy * 0.5 + 0.5;
  uv0.y = 1.0 - uv0.y;
  if (uv0.x < 0.0 || uv0.x > 1.0 || uv0.y < 0.0 || uv0.y > 1.0)
    return 1.0;
  float bias0 = baseBias * computeCascadeBiasScale(c0, cascadeCount, cascadeBiasScale);
  // 关键修复：
  // 之前 refDepth<0 直接返回全亮，会在高视角/远级联下把接触阴影“截掉一块”。
  // 这里改为 clamp，避免底部阴影突然消失。
  float ref0 = clamp(n0.z - bias0, 0.0, 1.0);
  vec2 receiverPlaneGradient0 = vec2(0.0);
  bool receiverPlaneValid0 = computeReceiverPlaneDepthGradient(
      l0, ubo.u_lightViewProj[c0], worldDx, worldDy,
      receiverPlaneGradient0);
  if (isTerrainMaskedOccluder(uint(c0), uv0, ref0))
    return 1.0;

  float radius0 = max(ubo.u_params.y, 0.0);
  if (pcssEnabled) {
    float sum = 0.0;
    float cnt = 0.0;
    float invRes = ubo.u_params.z;
    bool blockerPlaneValid = receiverPlaneValid0 &&
        receiverPlaneKernelValid(
            receiverPlaneGradient0,
            float(searchRadius) * pcssSearchRadius * invRes);
    for (int y = -searchRadius; y <= searchRadius; y++) {
      for (int x = -searchRadius; x <= searchRadius; x++) {
        vec2 o = vec2(float(x), float(y)) * pcssSearchRadius * invRes;
        float d = shadowMapDepth(uint(c0), uv0 + o);
        float tapRef = receiverPlaneTapReference(
            ref0, o, receiverPlaneGradient0, blockerPlaneValid);
        if (d < tapRef) {
          sum += d;
          cnt += 1.0;
        }
      }
    }
    if (cnt > 0.0) {
      float avgBlocker = sum / cnt;
      float penumbra = (ref0 - avgBlocker) * pcssDepthScale;
      radius0 = clamp(pcssMinRadius + penumbra, pcssMinRadius, pcssMaxRadius);
    } else {
      radius0 = pcssMinRadius;
    }
  }
  radius0 = computeCascadePcfRadius(radius0, c0, cascadeCount, pcfCascadeRadiusScale);
  // Wall stabilization is a continuous radius adjustment on the same PCF
  // kernel. Snapping the receiver UV or switching filter families introduces
  // whole-texel jumps as camera/sun motion crosses a classification boundary.
  radius0 = mix(radius0, max(radius0, 1.50), wallFilterWeight);
  float vis0 = sampleShadowPcf(
      uint(c0), uv0, ref0, radius0, rot,
      receiverPlaneGradient0, receiverPlaneValid0);

  // Blend into next cascade to hide seams
  if (blendRange > 0.0 && c0 < cascadeCount - 1) {
    float far0 = splits[c0];
    float t = clamp((viewDepth - (far0 - blendRange)) / blendRange, 0.0, 1.0);
    float w = t * t * (3.0 - 2.0 * t);

    int c1 = c0 + 1;
    vec4 l1 = p * ubo.u_lightViewProj[c1];
    if (l1.w <= 0.0)
      return vis0;
    vec3 n1 = l1.xyz / l1.w;
    if (n1.z < 0.0 || n1.z > 1.0)
      return vis0;
    vec2 uv1 = n1.xy * 0.5 + 0.5;
    uv1.y = 1.0 - uv1.y;
    if (uv1.x < 0.0 || uv1.x > 1.0 || uv1.y < 0.0 || uv1.y > 1.0)
      return vis0;
    float bias1 = baseBias * computeCascadeBiasScale(c1, cascadeCount, cascadeBiasScale);
    float ref1 = clamp(n1.z - bias1, 0.0, 1.0);
    vec2 receiverPlaneGradient1 = vec2(0.0);
    bool receiverPlaneValid1 = computeReceiverPlaneDepthGradient(
        l1, ubo.u_lightViewProj[c1], worldDx, worldDy,
        receiverPlaneGradient1);
    float radius1 = max(ubo.u_params.y, 0.0);
    if (pcssEnabled) {
      float sum = 0.0;
      float cnt = 0.0;
      float invRes = ubo.u_params.z;
      bool blockerPlaneValid = receiverPlaneValid1 &&
          receiverPlaneKernelValid(
              receiverPlaneGradient1,
              float(searchRadius) * pcssSearchRadius * invRes);
      for (int y = -searchRadius; y <= searchRadius; y++) {
        for (int x = -searchRadius; x <= searchRadius; x++) {
          vec2 o = vec2(float(x), float(y)) * pcssSearchRadius * invRes;
          float d = shadowMapDepth(uint(c1), uv1 + o);
          float tapRef = receiverPlaneTapReference(
              ref1, o, receiverPlaneGradient1, blockerPlaneValid);
          if (d < tapRef) {
            sum += d;
            cnt += 1.0;
          }
        }
      }
      if (cnt > 0.0) {
        float avgBlocker = sum / cnt;
        float penumbra = (ref1 - avgBlocker) * pcssDepthScale;
        radius1 = clamp(pcssMinRadius + penumbra, pcssMinRadius, pcssMaxRadius);
      } else {
        radius1 = pcssMinRadius;
      }
    }
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

  return vis0;
}

// A nearest-filtered cubemap lookup does not return the depth located on the
// continuous input ray. It returns the depth stored at one quantized face
// texel. Reconstruct that texel's centre ray so receiver-plane depth and the
// fetched caster depth use the exact same radial direction.
vec3 pointCubeNearestTexelRay(vec3 direction, float cubeResolution) {
  vec3 axis = abs(direction);
  float major = 0.0;
  vec2 faceSt = vec2(0.0);
  int face = 0;

  // Match the Vulkan cube face table. Z > Y > X is deterministic for exact
  // ties; the reconstructed centre ray lies strictly inside the selected face
  // so the subsequent texture lookup cannot choose a different tie outcome.
  if (axis.z >= axis.y && axis.z >= axis.x) {
    major = max(axis.z, 1.0e-12);
    if (direction.z >= 0.0) {
      face = 4;
      faceSt = vec2(direction.x, -direction.y) / major;
    } else {
      face = 5;
      faceSt = vec2(-direction.x, -direction.y) / major;
    }
  } else if (axis.y >= axis.x) {
    major = max(axis.y, 1.0e-12);
    if (direction.y >= 0.0) {
      face = 2;
      faceSt = vec2(direction.x, direction.z) / major;
    } else {
      face = 3;
      faceSt = vec2(direction.x, -direction.z) / major;
    }
  } else {
    major = max(axis.x, 1.0e-12);
    if (direction.x >= 0.0) {
      face = 0;
      faceSt = vec2(-direction.z, -direction.y) / major;
    } else {
      face = 1;
      faceSt = vec2(direction.z, -direction.y) / major;
    }
  }

  cubeResolution = max(cubeResolution, 1.0);
  vec2 uv = clamp(faceSt * 0.5 + vec2(0.5), vec2(0.0), vec2(1.0));
  vec2 texel = clamp(floor(uv * cubeResolution), vec2(0.0),
                     vec2(cubeResolution - 1.0));
  vec2 texelSt = ((texel + vec2(0.5)) / cubeResolution) * 2.0 - 1.0;

  if (face == 0)
    return vec3(1.0, -texelSt.y, -texelSt.x);
  if (face == 1)
    return vec3(-1.0, -texelSt.y, texelSt.x);
  if (face == 2)
    return vec3(texelSt.x, 1.0, texelSt.y);
  if (face == 3)
    return vec3(texelSt.x, -1.0, -texelSt.y);
  if (face == 4)
    return vec3(texelSt.x, -texelSt.y, 1.0);
  return vec3(-texelSt.x, -texelSt.y, -1.0);
}

float samplePointShadowPcf(uint lightIndex, vec3 dir, float currentDist,
                           float shadowRange, float biasBase,
                           vec3 receiverNormalWorld,
                           float receiverNormalConfidence) {
  float dirLenSq = dot(dir, dir);
  if (!validFloat(dirLenSq) || dirLenSq <= 1.0e-12 ||
      !validFloat(currentDist) || !validFloat(shadowRange) ||
      currentDist < 0.0 || shadowRange <= 1.0e-4)
    return 1.0;

  vec3 dirN = dir * inversesqrt(dirLenSq);
  vec3 refUp = (abs(dirN.y) < 0.99) ? vec3(0.0, 1.0, 0.0)
                                     : vec3(1.0, 0.0, 0.0);
  vec3 tangent = normalize(cross(refUp, dirN));
  vec3 bitangent = normalize(cross(dirN, tangent));

  float distRatio = clamp(currentDist / max(shadowRange, 1e-4), 0.0, 1.0);
  float cubeResolution = max(float(textureSize(
    samplerCubeArray(s_pointShadow,
      s_samplers[nonuniformEXT(pointShadow.u_samplerIndex)]),
    0).x), 1.0);
  // A cube texel subtends 2/R only at a face centre. Reusing that centre
  // footprint at edges/corners over-blurs PCF and inflates world-space bias by
  // as much as ~2.1x. The major-axis factor is a conservative scalar
  // approximation to the face Jacobian: exact at the centre and much closer
  // at seams while keeping the circular kernel orientation-independent.
  float cubeMajor = max(max(abs(dirN.x), abs(dirN.y)), abs(dirN.z));
  float texelAngle = (2.0 * cubeMajor) / cubeResolution;
  float pcfNear = clamp(pointShadow.u_filterParams.x, 0.0, 4.0);
  float pcfFar = clamp(pointShadow.u_filterParams.y, pcfNear, 6.0);
  float pcfRadius = texelAngle * mix(pcfNear, pcfFar, distRatio);

  // Zero-centred concentric disk. The previous random set put most taps near
  // the outer rim (mean r^2 ~= 0.85), which over-blurred a nominal one-texel
  // kernel and could shift a thin blocker edge. These paired rings integrate
  // the texel footprint without a directional centroid bias (mean r^2 ~= 0.47).
  const vec2 taps[16] = vec2[](
    vec2( 0.250000,  0.000000), vec2( 0.000000,  0.250000),
    vec2(-0.250000,  0.000000), vec2( 0.000000, -0.250000),
    vec2( 0.461940,  0.191342), vec2(-0.191342,  0.461940),
    vec2(-0.461940, -0.191342), vec2( 0.191342, -0.461940),
    vec2( 0.530330,  0.530330), vec2(-0.530330,  0.530330),
    vec2(-0.530330, -0.530330), vec2( 0.530330, -0.530330),
    vec2( 0.382683,  0.923880), vec2(-0.923880,  0.382683),
    vec2(-0.382683, -0.923880), vec2( 0.923880, -0.382683)
  );

  // Point shadow caster writes linear distance / range to the depth cube.
  // Keep the receiver in the same domain; the older perspective-depth reverse
  // formula made the cube map look permanently lit on many face/orientation cases.
  float currentDepth = clamp(currentDist / max(shadowRange, 1e-4), 0.0, 1.0);
  float texelWorld = texelAngle * currentDist;
  float texelBiasScale = clamp(pointShadow.u_filterParams.z, 0.0, 1.0);
  // Keep a small residual depth bias for quantization. When a reconstructed
  // receiver plane is trustworthy, every tap compares against the exact
  // intersection on the quantized cube-texel ray. If that proof is unavailable
  // (most often at grazing surfaces and silhouettes), use one conservative,
  // bounded slope fallback for the entire kernel. Mixing exact plane taps with
  // uncorrected centre-depth taps creates coherent moire bands.
  float biasWorld = max(biasBase, 0.0) +
                    texelWorld * texelBiasScale;
  float biasDepth = clamp(biasWorld / max(shadowRange, 1.0), 0.0, 0.01);
  float normalLenSq = dot(receiverNormalWorld, receiverNormalWorld);
  bool receiverNormalValid =
      validFloat(normalLenSq) && normalLenSq > 1.0e-8 &&
      receiverNormalConfidence > 0.20;
  vec3 receiverNormal = receiverNormalValid
      ? receiverNormalWorld * inversesqrt(normalLenSq)
      : -dirN;
  if (dot(receiverNormal, dirN) > 0.0)
    receiverNormal = -receiverNormal;
  float receiverCosine = receiverNormalValid
      ? clamp(-dot(receiverNormal, dirN), 0.0, 1.0)
      : 0.0;
  float receiverCosineSafe = max(receiverCosine, 0.20);
  float receiverSlope = receiverNormalValid
      ? sqrt(max(1.0 - receiverCosineSafe * receiverCosineSafe, 0.0)) /
            receiverCosineSafe
      : 4.0;
  float fallbackSlopeScale = 1.0 + 0.75 * min(receiverSlope, 4.0);
  float fallbackBiasWorld = max(biasBase, 0.0) +
                            texelWorld * texelBiasScale * fallbackSlopeScale;
  float fallbackBiasDepth = clamp(
      fallbackBiasWorld / max(shadowRange, 1.0), 0.0, 0.01);
  float planeNumerator = dot(receiverNormal, dir);
  bool receiverPlaneValid = receiverNormalValid &&
      validFloat(planeNumerator) &&
      planeNumerator < -0.12 * max(currentDist, 1.0e-4);
  float visible = 0.0;

  for (int i = 0; i < 16; ++i) {
    // Cube lookup is homogeneous in its xyz coordinate and this texture has a
    // single mip. Normalizing every tap therefore cannot change the sampled
    // direction, but costs 16 reciprocal-square-roots per shaded light.
    vec3 sampleDir =
      dirN + (tangent * taps[i].x + bitangent * taps[i].y) * pcfRadius;
    vec3 texelRay = pointCubeNearestTexelRay(
        sampleDir, cubeResolution);
    float receiverDepth = currentDepth;
    float tapBiasDepth = receiverPlaneValid ? biasDepth : fallbackBiasDepth;
    if (receiverPlaneValid) {
      float texelRayLenSq = dot(texelRay, texelRay);
      float planeDenominator = dot(receiverNormal, texelRay);
      if (validFloat(texelRayLenSq) && texelRayLenSq > 1.0e-8 &&
          validFloat(planeDenominator) && planeDenominator < -0.08) {
        // The nearest lookup and receiver reference must name the same cube
        // texel-centre ray. Using the continuous tap here creates a signed,
        // quantized depth residual that becomes coherent grazing-angle bands.
        float texelRayLength = sqrt(texelRayLenSq);
        float receiverPlaneDistance =
            planeNumerator * texelRayLength / planeDenominator;
        if (validFloat(receiverPlaneDistance) &&
            receiverPlaneDistance >= 0.0) {
          receiverDepth = clamp(
              receiverPlaneDistance / max(shadowRange, 1.0e-4), 0.0, 1.0);
        } else {
          // Exact-plane mode is kernel-wide. A numerically invalid tap must
          // fail soft instead of switching this tap to a different reference
          // domain and reintroducing a regular band pattern.
          visible += 1.0;
          continue;
        }
      } else {
        visible += 1.0;
        continue;
      }
    }
    float storedDepth = texture(
      samplerCubeArray(s_pointShadow,
        s_samplers[nonuniformEXT(pointShadow.u_samplerIndex)]),
      vec4(texelRay, float(lightIndex))).r;

    visible += (receiverDepth > storedDepth + tapBiasDepth) ? 0.0 : 1.0;
  }

  return visible * (1.0 / 16.0);
}

// Reconstruct one exact integer depth pixel. Point-light normals use this
// explicit four-neighbour path instead of derivatives of depth-reconstructed
// positions: derivatives after divergent background/clear early-outs are not
// defined strongly enough at a model silhouette and were the source of the
// bright stair-step edge.
bool reconstructPointSceneViewPixel(ivec2 samplePix, out vec3 sceneView) {
  vec2 vpMin = ubo.u_viewport.xy;
  vec2 vpSize = max(ubo.u_viewport.zw, vec2(1.0));
  ivec2 depthExtent = textureSize(
      sampler2DArray(s_depth,
        s_samplers[nonuniformEXT(p_colorSampler)]), 0).xy;
  if (any(lessThanEqual(depthExtent, ivec2(0))))
    return false;
  if (samplePix.x < int(vpMin.x) || samplePix.y < int(vpMin.y) ||
      float(samplePix.x) >= vpMin.x + vpSize.x ||
      float(samplePix.y) >= vpMin.y + vpSize.y ||
      any(lessThan(samplePix, ivec2(0))) ||
      any(greaterThanEqual(samplePix, depthExtent)))
    return false;

  float rawDepth = texelFetch(
      sampler2DArray(s_depth,
        s_samplers[nonuniformEXT(p_colorSampler)]),
      ivec3(samplePix, 0), 0).r;
  if (!validFloat(rawDepth))
    return false;

  float minZ = ubo.u_viewportZ.x;
  float maxZ = ubo.u_viewportZ.y;
  float zLo = min(minZ, maxZ);
  float zHi = max(minZ, maxZ);
  if (rawDepth < zLo - 1.0e-5 || rawDepth > zHi + 1.0e-5)
    return false;
  // A0 is optional detail. If the projection's far endpoint is ambiguous,
  // disable it rather than interpreting clear pixels as blockers.
  if (ubo.u_depthContract.y <= 0.5)
    return false;
  float clearEpsilon = max(0.5 * ubo.u_depthContract.z, 1.0e-7);
  if (abs(rawDepth - ubo.u_depthContract.x) <= clearEpsilon)
    return false;

  float depthN = rawDepth;
  if (abs(maxZ - minZ) > 1.0e-6)
    depthN = (rawDepth - minZ) / (maxZ - minZ);
  depthN = clamp(depthN, 0.0, 1.0);

  vec2 uvVp = (vec2(samplePix) - vpMin) / vpSize;
  vec4 clip = vec4(uvVp.x * 2.0 - 1.0,
                   1.0 - uvVp.y * 2.0,
                   depthN, 1.0);
  vec4 worldH = clip * ubo.u_invViewProj;
  if (!validVec4(worldH) || abs(worldH.w) < 1.0e-6)
    return false;

  vec4 sceneViewH = vec4(worldH.xyz / worldH.w, 1.0) * ubo.u_view;
  if (!validVec4(sceneViewH))
    return false;
  sceneView = sceneViewH.xyz;
  return validVec3(sceneView);
}

// Reconstruct the visible scene position at a viewport-relative UV. This uses
// the same integer-pixel/DXVK half-texel convention as the main receiver.
bool reconstructPointRaySceneView(vec2 uvVp, out vec3 sceneView) {
  if (!validFloat(uvVp.x) || !validFloat(uvVp.y) ||
      any(lessThanEqual(uvVp, vec2(0.0))) ||
      any(greaterThanEqual(uvVp, vec2(1.0))))
    return false;
  vec2 vpMin = ubo.u_viewport.xy;
  vec2 vpSize = max(ubo.u_viewport.zw, vec2(1.0));
  return reconstructPointSceneViewPixel(
      ivec2(vpMin + uvVp * vpSize), sceneView);
}

bool pointNeighbourTangent(vec3 centerView, ivec2 neighbourPixel,
                           float directionSign, float pixelFootprint,
                           out vec3 tangent, out float confidence) {
  tangent = vec3(0.0);
  confidence = 0.0;
  vec3 neighbourView;
  if (!reconstructPointSceneViewPixel(neighbourPixel, neighbourView))
    return false;

  vec3 delta = (neighbourView - centerView) * directionSign;
  float deltaLength = length(delta);
  float depthDelta = abs(abs(neighbourView.z) - abs(centerView.z));
  float depthGuard = max(pixelFootprint * 6.0,
                         abs(centerView.z) * 0.0015 + 0.5);
  float lengthGuard = max(pixelFootprint * 12.0,
                          abs(centerView.z) * 0.0030 + 1.0);
  if (!validVec3(delta) || !validFloat(deltaLength) ||
      !validFloat(depthDelta) || deltaLength <= 1.0e-5 ||
      depthDelta >= depthGuard || deltaLength >= lengthGuard)
    return false;

  float depthConfidence = 1.0 - smoothstep(
      depthGuard * 0.35, depthGuard, depthDelta);
  float lengthConfidence = 1.0 - smoothstep(
      lengthGuard * 0.45, lengthGuard, deltaLength);
  tangent = delta;
  confidence = clamp(min(depthConfidence, lengthConfidence), 0.0, 1.0);
  return confidence > 1.0e-4;
}

vec3 computePointLightViewNormal(vec3 centerView, out float confidence) {
  confidence = 0.0;
  float viewLength2 = dot(centerView, centerView);
  vec3 viewFacing = viewLength2 > 1.0e-10
      ? -centerView * inversesqrt(viewLength2)
      : vec3(0.0, 0.0, 1.0);

  vec2 vpSize = max(ubo.u_viewport.zw, vec2(1.0));
  vec4 centerClip = vec4(centerView, 1.0) * ubo.u_proj;
  float clipW = abs(centerClip.w);
  float projX = abs(ubo.u_proj[0][0]);
  float projY = abs(ubo.u_proj[1][1]);
  if (!validFloat(clipW) || !validFloat(projX) || !validFloat(projY) ||
      clipW <= 1.0e-6 || projX <= 1.0e-6 || projY <= 1.0e-6)
    return viewFacing;
  vec2 footprint = vec2(
      2.0 * clipW / (vpSize.x * projX),
      2.0 * clipW / (vpSize.y * projY));
  if (!validFloat(footprint.x) || !validFloat(footprint.y))
    return viewFacing;
  footprint = max(footprint, vec2(1.0e-4));

  ivec2 centerPixel = ivec2(gl_FragCoord.xy);
  vec3 dxLeft, dxRight, dyUp, dyDown;
  float cxLeft, cxRight, cyUp, cyDown;
  bool hasLeft = pointNeighbourTangent(
      centerView, centerPixel + ivec2(-1, 0), -1.0, footprint.x,
      dxLeft, cxLeft);
  bool hasRight = pointNeighbourTangent(
      centerView, centerPixel + ivec2(1, 0), 1.0, footprint.x,
      dxRight, cxRight);
  bool hasUp = pointNeighbourTangent(
      centerView, centerPixel + ivec2(0, -1), -1.0, footprint.y,
      dyUp, cyUp);
  bool hasDown = pointNeighbourTangent(
      centerView, centerPixel + ivec2(0, 1), 1.0, footprint.y,
      dyDown, cyDown);

  if ((!hasLeft && !hasRight) || (!hasUp && !hasDown))
    return viewFacing;
  vec3 dX = hasLeft && (!hasRight || cxLeft >= cxRight)
      ? dxLeft : dxRight;
  vec3 dY = hasUp && (!hasDown || cyUp >= cyDown)
      ? dyUp : dyDown;
  float confidenceX = hasLeft && (!hasRight || cxLeft >= cxRight)
      ? cxLeft : cxRight;
  float confidenceY = hasUp && (!hasDown || cyUp >= cyDown)
      ? cyUp : cyDown;

  vec3 normalRaw = cross(dY, dX);
  float normalLength2 = dot(normalRaw, normalRaw);
  if (!validFloat(normalLength2) || normalLength2 <= 1.0e-14)
    return viewFacing;
  vec3 reconstructed = normalRaw * inversesqrt(normalLength2);
  if (dot(reconstructed, viewFacing) < 0.0)
    reconstructed = -reconstructed;
  confidence = clamp(min(confidenceX, confidenceY), 0.0, 1.0);
  // viewFacing only resolves the hemisphere of a valid reconstructed normal.
  // It is never blended into point-light shading: doing so turns missing
  // neighbours at a thin silhouette into an artificial maximum-NdotV rim.
  return reconstructed;
}

// Software-simulated point-light ray tracing (screen-space contact shadow).
// It walks a short ray from the visible receiver toward the light, projects
// each view-space sample back to the current depth buffer, and detects a
// conservative depth crossing. Missing/off-screen information is fail-soft:
// visibility remains 1 and the cube shadow, when available, stays authoritative.
float tracePointContactRay(uint lightIndex, vec3 receiverView,
                           vec3 normalView, vec3 lightView) {
  if (ubo.u_pointRayParams.x <= 0.5)
    return 1.0;

  // A1 resolve: depth-guided bilinear reconstruction from the four surrounding
  // half-resolution texels. Half texel h owns full pixels 2h+[0,1], so for a
  // full pixel index p its continuous half coordinate is
  // (p+0.5)/2-0.5 = gl_FragCoord/2-0.5. Unknown/mixed/depth-mismatched taps
  // contribute zero occlusion and are deliberately not renormalized; missing
  // evidence can only move the result toward fully lit and never falls through
  // to A0.
  uint hizLayerCount = uint(clamp(ubo.u_pointRayParams2.w + 0.5, 0.0, 2.0));
  if (lightIndex < hizLayerCount) {
    ivec2 halfExtent = textureSize(s_pointContactHiz, 0);
    float receiverDepth = abs(receiverView.z);
    if (any(lessThanEqual(halfExtent, ivec2(0))) ||
        !validFloat(receiverDepth))
      return 1.0;

    vec2 halfCoord = gl_FragCoord.xy * 0.5 - vec2(0.5);
    ivec2 halfBase = ivec2(floor(halfCoord));
    vec2 halfFraction = fract(halfCoord);
    ivec2 anchorPixel = clamp(ivec2(gl_FragCoord.xy) / 2,
                              ivec2(0), halfExtent - ivec2(1));
    float depthTolerance = max(1.0, receiverDepth * 0.002);
    float trustedOcclusion = 0.0;
    bool anchorTrusted = false;

    for (int oy = 0; oy < 2; ++oy) {
      float spatialY = oy == 0 ? 1.0 - halfFraction.y : halfFraction.y;
      for (int ox = 0; ox < 2; ++ox) {
        float spatialX = ox == 0 ? 1.0 - halfFraction.x : halfFraction.x;
        float spatialWeight = spatialX * spatialY;
        if (spatialWeight <= 0.0)
          continue;
        // Clamp-to-edge preserves a constant visibility field at the screen
        // boundary. The repeated tap keeps its original bilinear weight; it is
        // not new evidence and still has to pass the same confidence/depth gate.
        ivec2 halfPixel = clamp(halfBase + ivec2(ox, oy),
                                ivec2(0), halfExtent - ivec2(1));

        vec2 visibilityConfidence = texelFetch(
            s_pointContactVisibility,
            ivec3(halfPixel, int(lightIndex)), 0).rg;
        vec2 encodedDepth = texelFetch(s_pointContactHiz, halfPixel, 0).rg;
        if (!validFloat(visibilityConfidence.x) ||
            !validFloat(visibilityConfidence.y) ||
            visibilityConfidence.x < 0.0 || visibilityConfidence.x > 1.0 ||
            visibilityConfidence.y < 0.0 || visibilityConfidence.y > 1.0 ||
            !validFloat(encodedDepth.x) || !validFloat(encodedDepth.y) ||
            encodedDepth.x <= 0.0 || encodedDepth.y < encodedDepth.x)
          continue;

        float representativeDepth = 0.5 * (encodedDepth.x + encodedDepth.y);
        float depthDelta = abs(receiverDepth - representativeDepth);
        if (!validFloat(representativeDepth) ||
            !validFloat(depthDelta) || depthDelta >= depthTolerance)
          continue;

        float depthWeight = 1.0 - smoothstep(
            0.5 * depthTolerance, depthTolerance, depthDelta);
        float confidence = visibilityConfidence.y *
                           clamp(depthWeight, 0.0, 1.0);
        float occlusion = 1.0 - visibilityConfidence.x;
        if (all(equal(halfPixel, anchorPixel)) && confidence > 0.0)
          anchorTrusted = true;
        trustedOcclusion += spatialWeight * confidence * occlusion;
      }
    }

    if (!anchorTrusted)
      return 1.0;
    return clamp(1.0 - trustedOcclusion, 0.0, 1.0);
  }

  vec3 toLight = lightView - receiverView;
  float lightDistance = length(toLight);
  if (!validFloat(lightDistance) || lightDistance <= 1.0e-4)
    return 1.0;

  float maxDistance = clamp(ubo.u_pointRayParams.z, 32.0, 2400.0);
  float startOffset = clamp(ubo.u_pointRayParams2.y, 1.0, 96.0);
  float traceDistance = min(lightDistance, maxDistance);
  if (traceDistance <= startOffset * 1.5)
    return 1.0;

  vec3 rayDir = toLight / lightDistance;
  // Move primarily along the light ray to leave the receiver surface. A small
  // oriented normal term reduces grazing self hits without detaching shadows.
  float normalSign = dot(normalView, rayDir) >= 0.0 ? 1.0 : -1.0;
  vec3 rayOrigin = receiverView + rayDir * startOffset +
      normalView * (normalSign * min(startOffset * 0.20, 3.0));
  float marchDistance = max(traceDistance - startOffset, 1.0);
  int stepCount = clamp(int(ubo.u_pointRayParams2.x + 0.5), 4, 32);
  float depthBias = max(1.0, startOffset * 0.35);
  float thickness = max(clamp(ubo.u_pointRayParams.w, 1.0, 160.0),
                        depthBias * 2.25);

  // Stable interleaved gradient noise breaks up regular banding. It is not
  // frame-dependent, so this optional path does not add temporal shimmer.
  float jitter = fract(52.9829189 * fract(
      dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  float hitConfidence = 0.0;

  for (int stepIndex = 0; stepIndex < 32; ++stepIndex) {
    if (stepIndex >= stepCount)
      break;

    float stepT = (float(stepIndex) + 0.35 + jitter * 0.30) /
                  float(stepCount);
    vec3 raySampleView = rayOrigin + rayDir * (stepT * marchDistance);
    vec4 rayClip = vec4(raySampleView, 1.0) * ubo.u_proj;
    if (!validVec4(rayClip) || abs(rayClip.w) < 1.0e-6)
      break;

    vec3 rayNdc = rayClip.xyz / rayClip.w;
    if (!validVec3(rayNdc) || rayNdc.z < 0.0 || rayNdc.z > 1.0)
      break;

    vec2 rayUv = vec2(rayNdc.x * 0.5 + 0.5,
                      0.5 - rayNdc.y * 0.5);
    vec3 sceneView;
    if (!reconstructPointRaySceneView(rayUv, sceneView))
      break;

    float rayDepth = abs(raySampleView.z);
    float sceneDepth = abs(sceneView.z);
    if (!validFloat(rayDepth) || !validFloat(sceneDepth))
      continue;

    float depthDelta = rayDepth - sceneDepth;
    float enter = smoothstep(depthBias, depthBias * 1.75, depthDelta);
    float leave = 1.0 - smoothstep(thickness * 0.75, thickness,
                                   depthDelta);
    float edgeDistance = min(min(rayUv.x, rayUv.y),
                             min(1.0 - rayUv.x, 1.0 - rayUv.y));
    float edgeConfidence = smoothstep(0.0, 0.035, edgeDistance);
    hitConfidence = max(hitConfidence, enter * leave * edgeConfidence);
    if (hitConfidence > 0.985)
      break;
  }

  return clamp(1.0 - hitConfidence, 0.0, 1.0);
}

void main() {
  // Important:
  // The command buffer dynamic viewport/scissor at the insertion point may not cover
  // the full render target (e.g. UI/world sub-rects). If we sample by interpolated UV
  // (i_pos), the copied image can get rescaled into the current viewport.
  //
  // To ensure a 1:1 readback of the current render target regardless of viewport,
  // sample using framebuffer pixel coordinates.
  ivec2 pix = ivec2(gl_FragCoord.xy);
  int layer = 0;

  vec4 col = texelFetch(
    sampler2DArray(s_color, s_samplers[nonuniformEXT(p_colorSampler)]),
    ivec3(pix, layer),
    0);

  // Depth is required for world position reconstruction
  float depth = texelFetch(
    sampler2DArray(s_depth, s_samplers[nonuniformEXT(p_colorSampler)]),
    ivec3(pix, layer),
    0).r;
  if (!validFloat(depth)) {
    o_color = col;
    return;
  }

  // 仅对主世界 viewport 区域做阴影处理，避免影响 UI/空白区域（例如底部面板）。
  vec2 vpMin  = ubo.u_viewport.xy;
  vec2 vpSize = max(ubo.u_viewport.zw, vec2(1.0));
  if (float(pix.x) < vpMin.x || float(pix.y) < vpMin.y ||
      float(pix.x) >= (vpMin.x + vpSize.x) ||
      float(pix.y) >= (vpMin.y + vpSize.y)) {
    o_color = col;
    return;
  }

  float minZ = ubo.u_viewportZ.x;
  float maxZ = ubo.u_viewportZ.y;
  float zLo = min(minZ, maxZ);
  float zHi = max(minZ, maxZ);
  if (depth < zLo - 1e-5 || depth > zHi + 1e-5) {
    o_color = col;
    return;
  }
  // Projection-derived far endpoint handles reversed projection and reversed
  // viewport MinZ/MaxZ independently. If it cannot be inferred, disable this
  // optional receiver pass rather than guessing that zHi is the clear value.
  if (ubo.u_depthContract.y <= 0.5) {
    o_color = col;
    return;
  }
  float clearRaw = ubo.u_depthContract.x;
  float clearEpsilon = max(0.5 * ubo.u_depthContract.z, 1.0e-7);
  if (abs(depth - clearRaw) <= clearEpsilon) {
    o_color = col;
    return;
  }
  float depthN = depth;
  if (abs(maxZ - minZ) > 1e-6) {
    depthN = (depth - minZ) / (maxZ - minZ);
  }
  depthN = clamp(depthN, 0.0, 1.0);

  // 注意：
  // 不要用“depth 接近 1”直接判定为背景/天空盒。
  // 在 D16/D24 深度或远景场景下，地形/模型的深度可能被量化到非常接近 1，
  // 若在这里提前 return，会出现“地形网格一半有阴影一半没阴影”的三角形分裂伪影。

  int debugMode = int(ubo.u_params2.z);
  if (debugMode == 3) {
    o_color = vec4(vec3(depthN), 1.0);
    return;
  }

  // Shadow TAA Debug: Motion Vector
  if (debugMode == 4) {
    vec2 mv = texelFetch(
      sampler2D(s_motionVector, s_samplers[nonuniformEXT(p_colorSampler)]),
      pix,
      0).xy;
    // 以 10x 放大，便于观察；R/G 表示方向，B 表示幅度
    vec2 rg = clamp(mv * 10.0 + 0.5, 0.0, 1.0);
    float b = clamp(length(mv) * 20.0, 0.0, 1.0);
    o_color = vec4(rg, b, 1.0);
    return;
  }

  // Shadow TAA Debug: Shadow History（直接显示历史纹理内容）
  if (debugMode == 5) {
    float h = 0.0;
    bool hasHistory = (ubo.u_taaParams.w > 0.5);
    if (hasHistory) {
      h = texelFetch(
        sampler2D(s_shadowHistory, s_samplers[nonuniformEXT(p_colorSampler)]),
        pix,
        0).r;
    }
    o_color = vec4(vec3(h), 1.0);
    return;
  }

  // Shadow TAA Debug: pre-temporal current-frame visibility. Comparing this
  // view against mode 5 (history) and mode 2 (final factor) distinguishes
  // unstable CSM input from stale temporal accumulation.
  if (debugMode == 7) {
    float currentVis = texelFetch(
      sampler2D(s_shadowCurrent, s_samplers[nonuniformEXT(p_colorSampler)]),
      pix,
      0).r;
    o_color = vec4(vec3(currentVis), 1.0);
    return;
  }
  if (debugMode == 8) {
    float currentVis = texelFetch(
      sampler2D(s_shadowCurrent, s_samplers[nonuniformEXT(p_colorSampler)]),
      pix,
      0).r;
    float shadowMask = clamp(1.0 - currentVis, 0.0, 1.0);
    o_color = vec4(mix(col.rgb, vec3(1.0, 0.05, 0.02),
                       shadowMask * 0.82),
                   col.a);
    return;
  }
  if (debugMode == 9) {
    float code = texelFetch(
      sampler2D(s_shadowCurrent, s_samplers[nonuniformEXT(p_colorSampler)]),
      pix,
      0).r;
    vec3 diagnosisColor = vec3(0.15);
    if (code < 0.10)
      diagnosisColor = vec3(1.0, 0.0, 1.0);   // invalid light W
    else if (code < 0.20)
      diagnosisColor = vec3(1.0, 0.25, 0.0);  // light depth outside [0,1]
    else if (code < 0.30)
      diagnosisColor = vec3(1.0, 0.9, 0.0);   // outside cascade XY
    else if (code < 0.40)
      diagnosisColor = vec3(0.0, 1.0, 1.0);   // suppressed by S1 mask
    else if (code < 0.50)
      diagnosisColor = vec3(0.05, 0.25, 1.0); // clear shadow-map texel
    else if (code < 0.60)
      diagnosisColor = vec3(0.1, 0.9, 0.15);  // occupied but compare is lit
    else
      diagnosisColor = vec3(1.0, 0.05, 0.02); // occupied and shadowed
    o_color = vec4(mix(col.rgb, diagnosisColor, 0.78), col.a);
    return;
  }

  float strength = clamp(ubo.u_params.x, 0.0, 1.0);
  if (debugMode == 0 && strength <= 1e-4 && lights.u_count == 0u) {
    o_color = col;
    return;
  }

  // 以 viewport 为基准重建 NDC，避免 viewport != RT 时产生“拉伸/乱飞”
  //
  // 重要（D3D9 half-texel / DXVK viewport 规则）：
  // DXVK 在绑定 VkViewport 时会对 (x,y) 加 0.5（见 d3d9_device.cpp: BindViewportAndScissor），
  // 从而使 D3D9 的 NDC (-1/+1) 精确落在像素中心。
  // 因此这里必须使用“整数像素坐标”（pix），而不是 (pix + 0.5)：
  // - 若再加一次 0.5，会产生半像素偏移，导致深度重建的 worldPos 偏斜，
  //   典型表现为：阴影抽搐/地形网格沿对角线出现三角形分裂伪影。
  vec2 uvVp = (vec2(pix) - vpMin) / vpSize;

  // D3D-style NDC:
  // - X: [-1..1]
  // - Y: +1 at top (Vulkan's default is +1 at bottom)
  // - Z: [0..1]
  vec4 clip = vec4(uvVp.x * 2.0 - 1.0, 1.0 - uvVp.y * 2.0, depthN, 1.0);

  vec4 worldH = clip * ubo.u_invViewProj;
  // If W is invalid, keep original color to avoid NaNs and screen-space tearing.
  if (!validVec4(worldH) || abs(worldH.w) < 1e-6) {
    o_color = col;
    return;
  }
  vec3 worldPos = worldH.xyz / worldH.w;
  if (!validVec3(worldPos)) {
    o_color = col;
    return;
  }

  if (debugMode == 6) {
    float pointVis = 1.0;
    float activeMask = 0.0;
    uint debugLight = min(pointShadow.u_debugLightIndex,
                          POINT_SHADOW_MAX_LIGHTS - 1u);
    if (debugLight < pointShadow.u_lightCount &&
        pointShadow.u_lights[debugLight].enabled > 0.5) {
      PointShadowLightData ps = pointShadow.u_lights[debugLight];
      vec3 lightToFrag = worldPos - ps.lightPos.xyz;
      float currentDist = length(lightToFrag);
      float shadowRange = max(ps.lightPos.w, 1.0);
      if (currentDist < shadowRange * 0.999) {
        vec4 debugViewH = vec4(worldPos, 1.0) * ubo.u_view;
        if (validVec4(debugViewH)) {
          float pointNormalConfidence = 0.0;
          vec3 pointNormV = computePointLightViewNormal(
              debugViewH.xyz, pointNormalConfidence);
          float normalTrust = smoothstep(
              0.0, 0.35, clamp(pointNormalConfidence, 0.0, 1.0));
          vec3 pointNormW = pointNormV * transpose(mat3(ubo.u_view));
          pointVis = samplePointShadowPcf(
              debugLight, lightToFrag, currentDist, shadowRange, ps.bias,
              pointNormW, normalTrust);
          activeMask = 1.0;
        }
      }
    }
    o_color = (activeMask > 0.5)
        ? vec4(vec3(pointVis), 1.0)
        : vec4(0.18, 0.0, 0.0, 1.0);
    return;
  }

  vec4 viewH = vec4(worldPos, 1.0) * ubo.u_view;
  if (!validVec4(viewH)) {
    o_color = col;
    return;
  }
  // 兼容 RH/LH：部分投影会让“前方深度”为 -Z，这里取绝对值用于级联选择与过渡。
  float viewDepth = abs(viewH.z);
  float farSplit = max(max(ubo.u_splitFar.x, ubo.u_splitFar.y),
                       max(ubo.u_splitFar.z, ubo.u_splitFar.w));
  if (!validFloat(viewDepth)) {
    o_color = col;
    return;
  }
  bool pointLightsEnabled = ubo.u_params2.w > 0.5 && lights.u_count > 0u;
  bool sunShadowCoverage =
      validFloat(farSplit) && farSplit > 1e-4 &&
      viewDepth <= farSplit + max(64.0, farSplit * 0.05);
  // The receiver pass also owns point lights. CSM coverage ending must only
  // disable the sun shadow; returning here used to hard-cut point lighting and
  // its cube shadow at the directional shadow distance.
  if (!sunShadowCoverage) {
    strength = 0.0;
    if (!pointLightsEnabled && debugMode == 0) {
      o_color = col;
      return;
    }
  }

  // Debug outputs
  if (debugMode == 1) {
    // Cascade visualization
    int cascadeCount = clamp(int(ubo.u_params.w), 1, 4);
    float splits[4];
    splits[0] = ubo.u_splitFar.x;
    splits[1] = ubo.u_splitFar.y;
    splits[2] = ubo.u_splitFar.z;
    splits[3] = ubo.u_splitFar.w;
    int c = cascadeCount - 1;
    for (int i = 0; i < cascadeCount; i++) {
      if (viewDepth <= splits[i]) {
        c = i;
        break;
      }
    }
    vec3 colors[4];
    colors[0] = vec3(1.0, 0.2, 0.2);
    colors[1] = vec3(0.2, 1.0, 0.2);
    colors[2] = vec3(0.2, 0.4, 1.0);
    colors[3] = vec3(1.0, 1.0, 0.2);
    o_color = vec4(colors[c], 1.0);
    return;
  }

  float taaMode = ubo.u_taaParams.x;
  bool prepassEnabled = (taaMode > 0.5);
  bool temporalRequested = (taaMode > 1.5);
  bool temporalHasHistory = (taaMode > 2.5);
  float receiverMode = ubo.u_params5.w;
  float normalBiasScale = max(ubo.u_params5.x, 0.0);
  float rimIntensity = max(ubo.u_params5.y, 0.0);
  float rimPower = max(ubo.u_params5.z, 0.1);
  float biasExtra = 0.0;
  vec3 viewPos = viewH.xyz;
  bool needNormal = strength > 1e-4 || pointLightsEnabled ||
                    receiverMode > 0.5 || rimIntensity > 1e-4;
  vec3 normV = vec3(0.0, 0.0, 1.0);
  if (needNormal) {
    normV = computeViewNormal(viewPos);
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
  bool stableWallCurrentOnly = wallFilterWeight > 0.50;
  if (!prepassEnabled && receiverMode > 0.5 && normalBiasScale > 0.0) {
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

  // DirectInline uses a deterministic, zero-centroid paired kernel. A
  // periodic world-coordinate rotation field turns smooth sun motion into
  // visible stripes, and true spatiotemporal noise is only valid after the
  // temporal history contract is complete.
  vec2 pcfRot = vec2(1.0, 0.0);

  float vis = 1.0;
  if (debugMode == 2 || strength > 1e-4) {
    float currVis = 1.0;

    if (prepassEnabled) {
      // PrepassCurrentOnly and Temporal consume exactly the same current-frame
      // visibility source. Temporal A/B therefore changes only history use.
      currVis = texelFetch(
        sampler2D(s_shadowCurrent, s_samplers[nonuniformEXT(p_colorSampler)]),
        pix,
        0).r;
    } else {
      vec3 worldDx = dFdx(worldPos);
      vec3 worldDy = dFdy(worldPos);
      currVis = computeShadowVisibility(
          worldPos, worldDx, worldDy, viewDepth, biasExtra, pcfRot,
          wallFilterWeight);
    }

    vis = validFloat(currVis) ? clamp(currVis, 0.0, 1.0) : 1.0;

    // Shadow TAA：对 vis 做重投影与时域混合（主要用于 Alpha-Test 阴影稳定）
    if (temporalRequested && !stableWallCurrentOnly) {
      float blend = clamp(ubo.u_taaParams.y, 0.0, 1.0);
      bool clampEnabled = (ubo.u_taaParams.z > 0.5);
      bool hasHistory = temporalHasHistory &&
                        (ubo.u_taaParams.w > 0.5);
      float currLinearDepth =
          clamp(viewDepth / max(ubo.u_splitFar.w, 1e-4), 0.0, 1.0);

      if (hasHistory) {
        vec2 uvVp = (vec2(pix) + 0.5 - vpMin) / vpSize;
        vec2 mv = texelFetch(
          sampler2D(s_motionVector, s_samplers[nonuniformEXT(p_colorSampler)]),
          pix,
          0).xy;

        // Clarity-first history weighting. Motion and half-shadow edges always
        // increase the current-frame contribution; they never drive it below
        // the configured 0.12/0.20/0.30 candidate weight.
        float mvLen = length(mv);
        float motionScale = clamp(mvLen * 8.0, 0.0, 1.0);
        float edgeFactor =
            1.0 - smoothstep(0.12, 0.42, abs(currVis - 0.5));
        float adaptiveBlend =
            max(blend, mix(blend, max(blend, 0.30), motionScale));
        adaptiveBlend =
            max(adaptiveBlend, mix(blend, max(blend, 0.24), edgeFactor));

        // historyUv = uv - mv（mv = curr - prev）
        vec2 historyUv = uvVp - mv;
        vec2 histPixF = vpMin + historyUv * vpSize;
        ivec2 histPix = ivec2(histPixF);

        bool validHistory =
            (histPixF.x >= vpMin.x) && (histPixF.y >= vpMin.y) &&
            (histPixF.x < (vpMin.x + vpSize.x)) &&
            (histPixF.y < (vpMin.y + vpSize.y));

        float histVis = currVis;
        float histDepth = currLinearDepth;
        if (validHistory) {
          vec2 historyTexSize = vec2(textureSize(
            sampler2D(s_shadowHistory, s_samplers[nonuniformEXT(p_colorSampler)]),
            0));
          vec2 historyTexUv =
            (histPixF + vec2(0.5)) / max(historyTexSize, vec2(1.0));
          vec2 histSample = texture(
            sampler2D(s_shadowHistory, s_samplers[nonuniformEXT(p_colorSampler)]),
            historyTexUv).rg;
          float rawHistVis = histSample.r;
          histVis = rawHistVis;
          histDepth = histSample.g;

          // Reject disocclusion and geometry changes. The derivative-aware
          // tolerance remains tight on flat receivers while allowing depth
          // quantization across sloped terrain.
          float depthDerivative =
              max(abs(dFdx(currLinearDepth)), abs(dFdy(currLinearDepth)));
          float depthTolerance =
              max(0.0015, depthDerivative * 2.5 + mvLen * 0.01);
          validHistory =
              validFloat(rawHistVis) && validFloat(histDepth) &&
              abs(histDepth - currLinearDepth) <= depthTolerance;

          // Variance clipping (mu +/- gamma*sigma) follows the temporal-AA
          // neighbourhood rectification model. It reuses the same 3x3 current
          // visibility fetch budget as the old min/max clamp, but rejects an
          // isolated stale history extreme without accepting the full local
          // range. Anchor the interval to currVis so a one-pixel leaf/cutout
          // shadow cannot be erased merely because its eight neighbours differ.
          float historyRectification = 0.0;
          if (clampEnabled) {
            vec2 visibilityMoments = vec2(0.0);
            float neighborhoodSamples = 0.0;

            for (int oy = -1; oy <= 1; oy++) {
              for (int ox = -1; ox <= 1; ox++) {
                ivec2 np = pix + ivec2(ox, oy);
                if (float(np.x) >= vpMin.x && float(np.y) >= vpMin.y &&
                    float(np.x) < (vpMin.x + vpSize.x) &&
                    float(np.y) < (vpMin.y + vpSize.y)) {
                  float n = texelFetch(
                    sampler2D(s_shadowCurrent, s_samplers[nonuniformEXT(p_colorSampler)]),
                    np,
                    0).r;
                  n = validFloat(n) ? clamp(n, 0.0, 1.0) : currVis;
                  visibilityMoments += vec2(n, n * n);
                  neighborhoodSamples += 1.0;
                }
              }
            }
            float invSamples = 1.0 / max(neighborhoodSamples, 1.0);
            float meanVisibility = visibilityMoments.x * invSamples;
            float variance = max(
                visibilityMoments.y * invSamples -
                meanVisibility * meanVisibility,
                0.0);
            float sigma = sqrt(variance);
            // The survey-supported gamma range is roughly 0.75..1.25. Keep
            // flat receivers strict and give half-shadow PCF edges more room.
            float gamma = mix(0.85, 1.10, edgeFactor);
            float clampPad = mix(0.006, 0.020, edgeFactor);
            float varianceLower =
                min(currVis, meanVisibility - gamma * sigma) - clampPad;
            float varianceUpper =
                max(currVis, meanVisibility + gamma * sigma) + clampPad;
            histVis = clamp(rawHistVis,
                            max(varianceLower, 0.0),
                            min(varianceUpper, 1.0));
            historyRectification = abs(rawHistVis - histVis);
          }

          if (validHistory) {
            // A large visibility disagreement is a reactive event (moving,
            // hidden or newly visible caster). Bias sharply toward current so
            // a disappearing shadow cannot leave a long temporal trail.
            float historyDisagreement = abs(rawHistVis - currVis);
            float reactive = max(
                smoothstep(0.08, 0.35, historyDisagreement),
                smoothstep(0.01, 0.20, historyRectification));
            adaptiveBlend =
                mix(adaptiveBlend, 1.0, reactive * 0.90);
            vis = mix(histVis, currVis, adaptiveBlend);
            vis = validFloat(vis) ? clamp(vis, 0.0, 1.0) : currVis;
          } else {
            vis = currVis;
          }
        } else {
          vis = currVis;
        }
      } else {
        // Contract invalidation/recovery frame: current only.
        vis = currVis;
      }

      imageStore(s_shadowHistoryWrite, pix,
                 vec4(vis, currLinearDepth, 0.0, 0.0));
    } else if (temporalRequested) {
      // 墙面/掠视角 receiver 直接使用当前帧 PCF，避免 history 在高梯度面上
      // 形成“波浪式流动”。
      float currLinearDepth =
          clamp(viewDepth / max(ubo.u_splitFar.w, 1e-4), 0.0, 1.0);
      imageStore(s_shadowHistoryWrite, pix,
                 vec4(currVis, currLinearDepth, 0.0, 0.0));
    }

    if (debugMode == 2) {
      o_color = vec4(vec3(vis), 1.0);
      return;
    }
  }

  vis = validFloat(vis) ? clamp(vis, 0.0, 1.0) : 1.0;
  float mul = 1.0 - strength + strength * vis;
  vec3 baseColor = col.rgb * mul;
  vec3 rimAdd = vec3(0.0);
  if (rimIntensity > 1e-4 && needNormal) {
    vec3 viewDirV = normalize(-viewPos);
    float ndv = clamp(dot(normV, viewDirV), 0.0, 1.0);
    float rim = pow(1.0 - ndv, rimPower);
    rimAdd = baseColor * (rimIntensity * rim);
  }

  if (!pointLightsEnabled) {
    o_color = vec4(baseColor + rimAdd, col.a);
    return;
  }

  uint lightCount = min(lights.u_count, 16u);

  vec3 accumLight = vec3(0.0);
  const float pointLightEnergyScale = 0.78;
  vec3 pointNormV = normV;
  bool pointNormalReady = false;
  float pointNormalConfidence = 0.0;

  for (uint i = 0; i < lightCount; i++) {
      float lRange = lights.u_lights[i].pos.w;
      vec3 lColor = lights.u_lights[i].color.rgb;
      float lIntensity = lights.u_lights[i].color.w;
      // Uniform across the frame and precomputed by the producer. The old
      // expression transformed the same world-space light once for every
      // full-resolution pixel and every active light.
      vec3 lPosV = lights.u_lights[i].params.yzw;

      vec3 L = lPosV - viewPos;
      float distSq = dot(L, L);
      // Most full-screen receiver pixels are outside a local light. Reject in
      // squared distance so those pixels do not pay a square root per light.
      // Positive/ordered comparisons retain the old fail-soft behavior for
      // negative or NaN producer data.
      if (lRange > 0.0 && distSq >= 0.0 &&
          distSq < lRange * lRange) {
          float dist = sqrt(max(distSq, 0.0));
          // War3 world units are large; raw inverse-square makes normal point
          // lights nearly invisible, then explodes into aliasing when boosted.
          // Use range-normalized falloff so light authoring remains predictable.
          float r = max(lRange, 1e-3);
          float x = clamp(dist / r, 0.0, 1.0);
          float window = max(1.0 - x * x, 0.0);
          window *= window; // smoothstep-like 收口
          float rangeFalloff = 1.0 / (1.0 + x * x * 6.0);
          float atten = window * rangeFalloff * lIntensity;

          vec3 Ldir = L / max(dist, 1e-6);
          if (!pointNormalReady) {
              pointNormV = computePointLightViewNormal(
                  viewPos, pointNormalConfidence);
              pointNormalReady = true;
          }
          // Point lights are direct emitters. Keep a strict Lambert terminator:
          // view-facing normal blends, half-Lambert wrap and a non-zero floor
          // illuminate back faces and make a local light look like ambient fill.
          float normalTrust = smoothstep(
              0.0, 0.35, clamp(pointNormalConfidence, 0.0, 1.0));
          float pointReceiverCosine = max(dot(pointNormV, Ldir), 0.0);
          float nFactor = pointReceiverCosine * normalTrust;
          if (nFactor <= 1.0e-4)
              continue;

          // u_view is an orthonormal world->view transform under the row-vector
          // convention used by this pass. Its transpose maps the reconstructed
          // view normal back to the cube map's world-space direction domain.
          vec3 pointNormW = pointNormV * transpose(mat3(ubo.u_view));

          float pointShadowFactor = 1.0;
          if (i < pointShadow.u_lightCount) {
              PointShadowLightData ps = pointShadow.u_lights[i];
              if (ps.enabled > 0.5) {
                  vec3 lightToFrag = worldPos - ps.lightPos.xyz;
                  float currentDist = length(lightToFrag);
                  float shadowRange = max(ps.lightPos.w, 1.0);
                  float shadowStrength = clamp(ps.shadowIntensity, 0.0, 1.0);

                  if (currentDist < shadowRange) {
                      // Fade the shadow term before the finite-radius light
                      // window reaches zero. This keeps the optional shadow
                      // from visibly snapping to fully lit at the range edge.
                      float rangeFadeStart =
                          clamp(pointShadow.u_filterParams.w, 0.50, 0.98);
                      // Use the cube's own distance/range domain. Today it is
                      // identical to the direct-light range, but keeping the
                      // transition local to the shadow contract prevents a
                      // stale/mismatched range from reintroducing a hard edge.
                      float shadowDistRatio =
                          clamp(currentDist / shadowRange, 0.0, 1.0);
                      float shadowRangeFade =
                          1.0 - smoothstep(rangeFadeStart, 1.0,
                                           shadowDistRatio);
                      if (shadowRangeFade > 1.0e-4) {
                          float visPoint = samplePointShadowPcf(
                              i, lightToFrag, currentDist, shadowRange,
                              ps.bias, pointNormW, normalTrust);
                          pointShadowFactor = mix(
                              1.0, visPoint,
                              shadowStrength * shadowRangeFade);
                      }
                  }
              }
          }

          uint pointRayMaxLights = uint(clamp(
              ubo.u_pointRayParams2.z + 0.5, 1.0, 2.0));
          if (ubo.u_pointRayParams.x > 0.5 && i < pointRayMaxLights) {
              float contactStrength =
                  clamp(ubo.u_pointRayParams.y, 0.0, 1.0) *
                  clamp(lights.u_lights[i].params.x, 0.0, 1.0);
              if (contactStrength > 1.0e-4) {
                  float contactVisibility =
                      tracePointContactRay(i, viewPos, pointNormV, lPosV);
                  // The software ray is a short-range detail supplement. It may
                  // only add occlusion; a complete cube map remains the fallback
                  // and cannot be accidentally brightened by missing screen data.
                  float contactFactor = mix(1.0, contactVisibility,
                                            contactStrength);
                  pointShadowFactor = min(pointShadowFactor, contactFactor);
              }
          }

          float lightTerm = nFactor * atten * pointShadowFactor;
          accumLight += max(col.rgb, vec3(0.0)) * lColor *
                        (lightTerm * pointLightEnergyScale);
      }
  }

  // Keep light transport separated: the sun visibility only modulates the sun
  // base, while each point shadow only modulates its own direct contribution.
  vec3 finalColor = baseColor + accumLight + rimAdd;

  o_color = vec4(finalColor, col.a);
}
