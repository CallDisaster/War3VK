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
  uint p_shadowSampler;
};

layout(location = 0) out float o_vis;

float shadowMapDepth(uint cascadeIndex, vec2 uv) {
  return texture(
    sampler2DArray(s_shadow, s_samplers[nonuniformEXT(p_shadowSampler)]),
    vec3(uv, float(cascadeIndex))).r;
}

float shadowCompare(uint cascadeIndex, vec2 uv, float refDepth) {
  float d = shadowMapDepth(cascadeIndex, uv);
  return (refDepth <= d) ? 1.0 : 0.0;
}

float casterMaskValue(uint cascadeIndex, vec2 uv) {
  if (ubo.u_viewportZ.z <= 0.5)
    return 0.0;
  return texture(
    sampler2DArray(s_casterMask, s_samplers[nonuniformEXT(p_shadowSampler)]),
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
  vec2( 0.94558609, -0.76890725),
  vec2(-0.09418410, -0.92938870),
  vec2( 0.34495938,  0.29387760),
  vec2(-0.91588581,  0.45771432),
  vec2(-0.81544232, -0.87912464),
  vec2(-0.38277543,  0.27676845),
  vec2( 0.97484398,  0.75648379),
  vec2( 0.44323325, -0.97511554),
  vec2( 0.53742981, -0.47373420),
  vec2(-0.26496911, -0.41893023),
  vec2( 0.79197514,  0.19090188),
  vec2(-0.24188840,  0.99706507),
  vec2(-0.81409955,  0.91437590),
  vec2( 0.19984126,  0.78641367),
  vec2( 0.14383161, -0.14100790)
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

float sampleShadowGrid(uint cascadeIndex, vec2 uv, float refDepth, float radiusTexel, int gridRadius) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = ubo.u_params.z;
  float radius = max(radiusTexel, 0.0);
  float sum = 0.0;
  float count = 0.0;

  for (int y = -gridRadius; y <= gridRadius; y++) {
    for (int x = -gridRadius; x <= gridRadius; x++) {
      vec2 o = vec2(float(x), float(y)) * radius * invRes;
      vec2 tapUv = uv + o;
      // 避免边界采样把 clamp-to-edge 变成伪阴影
      if (tapUv.x < 0.0 || tapUv.x > 1.0 || tapUv.y < 0.0 || tapUv.y > 1.0)
        sum += 1.0;
      else
        sum += shadowCompare(cascadeIndex, tapUv, refDepth);
      count += 1.0;
    }
  }
  return sum / max(count, 1.0);
}

float sampleShadowPoisson16(uint cascadeIndex, vec2 uv, float refDepth, float radiusTexel, vec2 rot) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = ubo.u_params.z;
  float radius = max(radiusTexel, 0.0);
  float sum = 0.0;

  for (int i = 0; i < 16; i++) {
    vec2 d = kPoisson16[i];
    vec2 o = rotateVec2(d, rot) * radius * invRes;
    vec2 tapUv = uv + o;
    if (tapUv.x < 0.0 || tapUv.x > 1.0 || tapUv.y < 0.0 || tapUv.y > 1.0)
      sum += 1.0;
    else
      sum += shadowCompare(cascadeIndex, tapUv, refDepth);
  }
  return sum * (1.0 / 16.0);
}

float sampleShadowPoisson25(uint cascadeIndex, vec2 uv, float refDepth, float radiusTexel, vec2 rot) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = ubo.u_params.z;
  float radius = max(radiusTexel, 0.0);
  float sum = 0.0;

  for (int i = 0; i < 25; i++) {
    vec2 d = kPoisson25[i];
    vec2 o = rotateVec2(d, rot) * radius * invRes;
    vec2 tapUv = uv + o;
    if (tapUv.x < 0.0 || tapUv.x > 1.0 || tapUv.y < 0.0 || tapUv.y > 1.0)
      sum += 1.0;
    else
      sum += shadowCompare(cascadeIndex, tapUv, refDepth);
  }
  return sum * (1.0 / 25.0);
}

float sampleShadowPcf(uint cascadeIndex, vec2 uv, float refDepth, float radiusTexel, vec2 rot) {
  int kernel = int(ubo.u_params6.x + 0.5);
  if (kernel == 1)
    return sampleShadowGrid(cascadeIndex, uv, refDepth, radiusTexel, 2);
  if (kernel == 2)
    return sampleShadowPoisson16(cascadeIndex, uv, refDepth, radiusTexel, rot);
  if (kernel == 3)
    return sampleShadowPoisson25(cascadeIndex, uv, refDepth, radiusTexel, rot);
  return sampleShadowGrid(cascadeIndex, uv, refDepth, radiusTexel, 1);
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

vec3 computeViewNormal(vec3 viewPos) {
  // Use derivatives for stable slope estimation on steep walls and props.
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

float sampleShadowStableWall(uint cascadeIndex, vec2 uv, float refDepth, float radiusTexel) {
  float invRes = max(ubo.u_params.z, 1e-6);
  vec2 snappedUv = (floor(uv / invRes) + 0.5) * invRes;
  float stableRadius = max(radiusTexel, 1.75);
  return sampleShadowGrid(cascadeIndex, snappedUv, refDepth, stableRadius, 2);
}

float computeShadowVisibility(vec3 worldPos, float viewDepth, float biasExtra, vec2 rot, bool stableWallPath) {
  const bool diagnoseCsm = int(ubo.u_params2.z + 0.5) == 9;
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
  float cascadeBiasScale = max(ubo.u_params4.w, 0.0);
  float pcfCascadeRadiusScale = max(ubo.u_params6.w, 0.0);

  vec4 p = vec4(worldPos, 1.0);

  // Sample primary cascade only (skip cascade blending for TAA - temporal filter smooths it)
  vec4 l0 = p * ubo.u_lightViewProj[c0];
  if (l0.w <= 0.0)
    return diagnoseCsm ? 0.05 : 1.0;
  vec3 n0 = l0.xyz / l0.w;
  if (n0.z < 0.0 || n0.z > 1.0)
    return diagnoseCsm ? 0.15 : 1.0;

  vec2 uv0 = n0.xy * 0.5 + 0.5;
  uv0.y = 1.0 - uv0.y;
  if (diagnoseCsm &&
      (uv0.x < 0.0 || uv0.x > 1.0 || uv0.y < 0.0 || uv0.y > 1.0))
    return 0.25;
  float bias0 = baseBias * computeCascadeBiasScale(c0, cascadeCount, cascadeBiasScale);
  // 与 receiver.frag 一致：越界直接全亮会造成接触阴影突然断裂。
  float ref0 = clamp(n0.z - bias0, 0.0, 1.0);
  if (isTerrainMaskedOccluder(uint(c0), uv0, ref0))
    return diagnoseCsm ? 0.35 : 1.0;

  if (diagnoseCsm) {
    float blockerDepth = shadowMapDepth(uint(c0), uv0);
    if (blockerDepth >= 0.99999)
      return 0.45;
    return ref0 <= blockerDepth ? 0.55 : 0.65;
  }

  float radius0 = max(ubo.u_params.y, 0.0);
  radius0 = computeCascadePcfRadius(radius0, c0, cascadeCount, pcfCascadeRadiusScale);

  // The visibility source must preserve the receiver's cascade contract.
  // Stable walls previously returned here and hard-switched at splitFar.
  float vis0 = stableWallPath
      ? sampleShadowStableWall(uint(c0), uv0, ref0, radius0)
      : sampleShadowPcf(uint(c0), uv0, ref0, radius0, rot);

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
      if (l1.w > 0.0) {
        vec3 n1 = l1.xyz / l1.w;
        if (n1.z >= 0.0 && n1.z <= 1.0) {
          vec2 uv1 = n1.xy * 0.5 + 0.5;
          uv1.y = 1.0 - uv1.y;
          float bias1 = baseBias * computeCascadeBiasScale(c1, cascadeCount, cascadeBiasScale);
          float ref1 = clamp(n1.z - bias1, 0.0, 1.0);
          float radius1 = max(ubo.u_params.y, 0.0);
          radius1 = computeCascadePcfRadius(radius1, c1, cascadeCount, pcfCascadeRadiusScale);
          float vis1 = 1.0;
          if (!isTerrainMaskedOccluder(uint(c1), uv1, ref1)) {
            // Stable-wall filtering is used on both cascades before blending;
            // never mix its snapped grid with the generic PCF family.
            vis1 = stableWallPath
                ? sampleShadowStableWall(uint(c1), uv1, ref1, radius1)
                : sampleShadowPcf(uint(c1), uv1, ref1, radius1, rot);
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

  // 仅对主世界 viewport 区域做阴影处理，避免影响 UI/空白区域
  vec2 vpMin  = ubo.u_viewport.xy;
  vec2 vpSize = max(ubo.u_viewport.zw, vec2(1.0));
  if (float(pix.x) < vpMin.x || float(pix.y) < vpMin.y ||
      float(pix.x) >= (vpMin.x + vpSize.x) ||
      float(pix.y) >= (vpMin.y + vpSize.y)) {
    o_vis = 1.0;
    return;
  }

  float minZ = ubo.u_viewportZ.x;
  float maxZ = ubo.u_viewportZ.y;
  float depthN = depth;
  if (abs(maxZ - minZ) > 1e-6) {
    depthN = (depth - minZ) / (maxZ - minZ);
  }
  depthN = clamp(depthN, 0.0, 1.0);

  // 以 viewport 为基准重建 NDC（与 receiver 保持一致）
  vec2 uvVp = (vec2(pix) - vpMin) / vpSize;
  vec4 clip = vec4(uvVp.x * 2.0 - 1.0, 1.0 - uvVp.y * 2.0, depthN, 1.0);

  vec4 worldH = clip * ubo.u_invViewProj;
  if (abs(worldH.w) < 1e-6) {
    o_vis = 1.0;
    return;
  }
  vec3 worldPos = worldH.xyz / worldH.w;

  vec4 viewH = vec4(worldPos, 1.0) * ubo.u_view;
  float viewDepth = abs(viewH.z);
  vec3 viewPos = viewH.xyz;

  float receiverMode = ubo.u_params5.w;
  float normalBiasScale = max(ubo.u_params5.x, 0.0);
  float biasExtra = 0.0;
  bool needNormal = true;
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
  bool stableWallPath =
      wallFactor > 0.28 &&
      (wallStabilityFactor > 0.08 || lightGrazingFactor > 0.08 ||
       viewGrazingFactor > 0.20);
  if (receiverMode > 0.5 && normalBiasScale > 0.0) {
    float ndotl = abs(dot(normV, lightDirV));

    // Keep non-zero bias in far cascades to prevent wall-striping acne.
    float depthRatio = clamp(viewDepth / max(ubo.u_splitFar.w, 1e-4), 0.0, 1.0);
    const float minFarWeight = 0.35;
    float normalBiasWeight = mix(1.0, minFarWeight, depthRatio * depthRatio);
    float wallBiasDampen = mix(1.0, 0.45, wallStabilityFactor);
    if (stableWallPath)
      wallBiasDampen = min(wallBiasDampen, 0.40);
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
    if (stableWallPath)
      extraBiasMax = min(extraBiasMax, max(baseReceiverBias * 0.55, texelBiasFloor * 1.35));
    biasExtra = clamp(biasExtra, 0.0, extraBiasMax);
  }

  vec2 pcfRot = vec2(1.0, 0.0);
  // This pass feeds Shadow TAA. Any per-pixel rotated Poisson pattern becomes
  // part of the temporal input and shows up as edge crawl, so keep the current
  // visibility source deterministic. Non-TAA receiver sampling may still rotate.

  o_vis = computeShadowVisibility(worldPos, viewDepth, biasExtra, pcfRot, stableWallPath);
}
