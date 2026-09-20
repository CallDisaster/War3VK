// 方向光阴影采样/接收公共实现：由 war3_shadow_receiver.frag 与
// war3_shadow_visibility.frag 通过 #include 共享。
//
// 本文件只收录相对 A 树 09-16 整理仍无实质漂移的函数（函数体以本树为准）。
// 漂移函数（shadowMapDepth / shadowCompare / casterMaskValue / kPoisson16 /
// sampleShadowGrid / sampleShadowPoisson16 / sampleShadowPoisson25 /
// sampleShadowPcf / computeViewNormal）留在各自 shader 内，不强行合并。
//
// 分段 include 保持原定义顺序，避免 SPIR-V 因函数重排而变化。
// WAR3_SHADOW_COMMON_PART=1  isTerrainMaskedOccluder
// WAR3_SHADOW_COMMON_PART=2  kPoisson25 / rotateVec2 / cascade helpers
// WAR3_SHADOW_COMMON_PART=3  wall/grazing helpers
// 依赖 include 方已声明：s_samplers、s_shadow、s_casterMask、
// ShadowData ubo（含 u_params/u_params6/u_view/u_viewportZ）以及
// shadowMapDepth / casterMaskValue。
#ifndef WAR3_SHADOW_COMMON_PART
#error "war3_shadow_common.glsl requires WAR3_SHADOW_COMMON_PART"
#endif

#if WAR3_SHADOW_COMMON_PART == 1
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
#elif WAR3_SHADOW_COMMON_PART == 2
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
#elif WAR3_SHADOW_COMMON_PART == 3
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
#endif
