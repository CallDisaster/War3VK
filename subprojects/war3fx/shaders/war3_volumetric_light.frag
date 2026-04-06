#version 460

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2DArray s_color;
layout(set = 1, binding = 1) uniform texture2DArray s_depth;
layout(set = 1, binding = 2) uniform texture2DArray s_shadow;

layout(set = 1, binding = 3, scalar, row_major)
uniform CsmData {
  mat4 u_view;
  mat4 u_invViewProj;
  mat4 u_lightViewProj[4];
  vec4 u_splitFar;
  // x=receiverBias y=invShadowRes z=cascadeCount w=pcfRadius
  vec4 u_params;
  // x=cascadeBlendRange y=heightFogBase z=heightFogFalloff w=heightFogStrength
  vec4 u_params2;
  // xyz=sunDir（从太阳指向地面）
  vec4 u_sunDir;
  // xyz=cameraPos
  vec4 u_cameraPos;
  // xyz=worldUp
  vec4 u_worldUp;
} csm;

layout(push_constant, scalar, row_major)
uniform push_block {
  uint p_colorSampler;
  uint p_depthSampler;
  uint p_shadowSampler;
  uint p_flags;

  // x=intensity y=decay z=density w=weight
  vec4 p_params0;
  // x=anisotropy(y映射) y=fadeNear z=fadeFar w=maxRayDistanceScale
  vec4 p_params1;
  // x=sampleCount y=maxWorldDistance z=froxelNearDistance w=shadowStrengthScale
  vec4 p_params2;
  // rgb=sunColor w=sunIntensity
  vec4 p_sunColorScale;
  vec4 p_viewport;   // x=vpX, y=vpY, z=vpW, w=vpH
  vec4 p_viewportZ;  // x=minZ, y=maxZ
  vec4 p_rtSize;     // x=rtW y=rtH
} pc;

layout(location = 0) out vec4 o_color;

float saturate(float v) {
  return clamp(v, 0.0, 1.0);
}

vec2 localToAbsUv(vec2 uvLocal, vec2 vpMin, vec2 vpSize, vec2 texSize) {
  vec2 pixel = vpMin + uvLocal * vpSize;
  return (pixel + vec2(0.5)) / max(texSize, vec2(1.0));
}

float fetchDepthNFromAbs(vec2 uvAbs) {
  float depth = texture(
    sampler2DArray(s_depth, s_samplers[nonuniformEXT(pc.p_depthSampler)]),
    vec3(uvAbs, 0.0)).r;
  float denom = max(pc.p_viewportZ.y - pc.p_viewportZ.x, 1e-6);
  return saturate((depth - pc.p_viewportZ.x) / denom);
}

float fetchDepthNFromLocal(vec2 uvLocal, vec2 vpMin, vec2 vpSize, vec2 texSize) {
  return fetchDepthNFromAbs(localToAbsUv(uvLocal, vpMin, vpSize, texSize));
}

vec3 reconstructWorldPos(vec2 uvLocal, float depthN) {
  vec4 clip = vec4(uvLocal.x * 2.0 - 1.0, 1.0 - uvLocal.y * 2.0, depthN, 1.0);
  vec4 worldH = clip * csm.u_invViewProj;
  float invW = (abs(worldH.w) > 1e-6) ? (1.0 / worldH.w) : 1.0;
  return worldH.xyz * invW;
}

float shadowMapDepth(uint cascadeIndex, vec2 uv) {
  return texture(
    sampler2DArray(s_shadow, s_samplers[nonuniformEXT(pc.p_shadowSampler)]),
    vec3(uv, float(cascadeIndex))).r;
}

float shadowCompare(uint cascadeIndex, vec2 uv, float refDepth) {
  float d = shadowMapDepth(cascadeIndex, uv);
  return (refDepth <= d) ? 1.0 : 0.0;
}

float sampleShadowPcf3x3(uint cascadeIndex, vec2 uv, float refDepth, float radiusTexel) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = csm.u_params.y;
  float radius = max(radiusTexel, 0.0);
  float sum = 0.0;
  float count = 0.0;

  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      vec2 o = vec2(float(x), float(y)) * radius * invRes;
      vec2 tapUv = uv + o;
      if (tapUv.x < 0.0 || tapUv.x > 1.0 || tapUv.y < 0.0 || tapUv.y > 1.0)
        sum += 1.0;
      else
        sum += shadowCompare(cascadeIndex, tapUv, refDepth);
      count += 1.0;
    }
  }

  return sum / max(count, 1.0);
}

float computeShadowVisibility(vec3 worldPos, float viewDepth, bool flipUvX, bool flipUvY, bool flipViewDepth, out float validMask) {
  validMask = 0.0;
  int cascadeCount = clamp(int(csm.u_params.z + 0.5), 1, 4);
  float splits[4];
  splits[0] = csm.u_splitFar.x;
  splits[1] = csm.u_splitFar.y;
  splits[2] = csm.u_splitFar.z;
  splits[3] = csm.u_splitFar.w;

  // 默认使用 abs(viewDepth) 兼容 RH/LH；调试开关可回退到原始带符号深度。
  float vDepth = flipViewDepth ? viewDepth : abs(viewDepth);
  int c0 = cascadeCount - 1;
  for (int i = 0; i < cascadeCount; i++) {
    if (vDepth <= splits[i]) {
      c0 = i;
      break;
    }
  }

  vec4 p = vec4(worldPos, 1.0);
  vec4 l0 = p * csm.u_lightViewProj[c0];
  if (abs(l0.w) <= 1e-6)
    return 1.0;
  vec3 n0 = l0.xyz / l0.w;
  if (n0.z < 0.0 || n0.z > 1.0)
    return 1.0;
  vec2 uv0 = n0.xy * 0.5 + 0.5;
  uv0.y = 1.0 - uv0.y;
  if (flipUvX)
    uv0.x = 1.0 - uv0.x;
  if (flipUvY)
    uv0.y = 1.0 - uv0.y;
  // 体积雾不把“阴影图覆盖区外”当作可见光源，避免角落聚光与拉丝。
  if (uv0.x < 0.0 || uv0.x > 1.0 || uv0.y < 0.0 || uv0.y > 1.0)
    return 1.0;
  validMask = 1.0;

  float baseBias = max(csm.u_params.x, 0.0);
  float ref0 = clamp(n0.z - baseBias, 0.0, 1.0);
  float radius0 = max(csm.u_params.w, 0.0);
  float vis0 = sampleShadowPcf3x3(uint(c0), uv0, ref0, radius0);

  float blendRange = max(csm.u_params2.x, 0.0);
  if (blendRange <= 0.0 || c0 >= cascadeCount - 1)
    return vis0;

  float far0 = splits[c0];
  float t = clamp((vDepth - (far0 - blendRange)) / blendRange, 0.0, 1.0);
  float w = t * t * (3.0 - 2.0 * t);
  if (w <= 1e-6)
    return vis0;

  int c1 = c0 + 1;
  vec4 l1 = p * csm.u_lightViewProj[c1];
  if (abs(l1.w) <= 1e-6)
    return vis0;
  vec3 n1 = l1.xyz / l1.w;
  if (n1.z < 0.0 || n1.z > 1.0)
    return vis0;
  vec2 uv1 = n1.xy * 0.5 + 0.5;
  uv1.y = 1.0 - uv1.y;
  if (flipUvX)
    uv1.x = 1.0 - uv1.x;
  if (flipUvY)
    uv1.y = 1.0 - uv1.y;

  float ref1 = clamp(n1.z - baseBias, 0.0, 1.0);
  float vis1 = sampleShadowPcf3x3(uint(c1), uv1, ref1, radius0);
  return mix(vis0, vis1, w);
}

float hgPhase(float mu, float g) {
  float gg = g * g;
  float denom = max(1.0 + gg - 2.0 * g * mu, 1e-3);
  // Henyey-Greenstein 相函数（归一化项用 1/(4pi)）
  return (1.0 - gg) / (12.5663706 * pow(denom, 1.5));
}

void main() {
  ivec2 pix = ivec2(gl_FragCoord.xy);
  vec4 base = texelFetch(
    sampler2DArray(s_color, s_samplers[nonuniformEXT(pc.p_colorSampler)]),
    ivec3(pix, 0),
    0);

  vec2 texSize = max(pc.p_rtSize.xy, vec2(1.0));
  vec2 vpMin = pc.p_viewport.xy;
  vec2 vpSize = max(pc.p_viewport.zw, vec2(1.0));
  vec2 fpix = vec2(pix) + vec2(0.5);

  if (fpix.x < vpMin.x || fpix.y < vpMin.y ||
      fpix.x >= (vpMin.x + vpSize.x) || fpix.y >= (vpMin.y + vpSize.y)) {
    o_color = base;
    return;
  }

  float intensity = max(pc.p_params0.x, 0.0);
  float sunIntensity = max(pc.p_sunColorScale.w, 0.0);
  if (intensity <= 1e-6 || sunIntensity <= 1e-6) {
    o_color = base;
    return;
  }

  vec2 uvLocal = (fpix - vpMin) / vpSize;
  float depthN = fetchDepthNFromLocal(uvLocal, vpMin, vpSize, texSize);
  if (depthN >= 0.9999) {
    // 天空像素直接跳过，避免“天空整屏发白”。
    o_color = base;
    return;
  }

  const uint flags = pc.p_flags;
  const bool flipUvX = (flags & 0x1u) != 0u;
  const bool flipUvY = (flags & 0x2u) != 0u;
  const bool flipSun = (flags & 0x4u) != 0u;
  const bool disableNearFade = (flags & 0x8u) != 0u;
  const bool flipViewDepth = (flags & 0x10u) != 0u;

  vec3 worldPos = reconstructWorldPos(uvLocal, depthN);
  vec3 cameraPos = csm.u_cameraPos.xyz;
  vec3 rayVec = worldPos - cameraPos;
  float rayLen = length(rayVec);
  if (rayLen <= 1e-4) {
    o_color = base;
    return;
  }

  int sampleCount = int(clamp(pc.p_params2.x, 4.0, 96.0));
  float maxDistScale = clamp(pc.p_params1.w, 0.05, 2.0);
  float maxWorldDistance = max(pc.p_params2.y * maxDistScale, 25.0);
  // 积分终点限制到当前像素表面，避免“穿透表面继续累加”导致局部爆亮。
  float marchLength = min(rayLen, maxWorldDistance);
  if (marchLength <= 1e-4) {
    o_color = base;
    return;
  }

  vec3 rayDir = rayVec / rayLen;
  vec3 sunDir = csm.u_sunDir.xyz;
  float sunLen = length(sunDir);
  if (sunLen <= 1e-6)
    sunDir = vec3(-0.3, -0.2, -1.0);
  else
    sunDir /= sunLen;
  if (flipSun)
    sunDir = -sunDir;

  // 将 UI 参数映射到稳定范围，减少“视角一变就爆亮/消失”的问题。
  float anisotropyG = clamp((pc.p_params1.x - 0.5) * 0.6, -0.05, 0.35);
  float mu = dot(-sunDir, -rayDir);
  float phase = mix(0.07957747, hgPhase(mu, anisotropyG), 0.45);

  float density = max(pc.p_params0.z, 0.0);
  float scatterWeight = max(pc.p_params0.w, 0.0);
  float decay = clamp(pc.p_params0.y, 0.70, 0.999);
  // decay 越小，消光越强；density 放大总体散射密度（按归一化光程）。
  float sigmaT = mix(0.05, 2.2, saturate(1.0 - decay)) * max(density, 1e-3);

  // Froxel 分层：沿视线做对数深度切片，近处更密，远处更稀。
  float froxelNear = clamp(pc.p_params2.z, 0.1, max(marchLength - 0.1, 0.1));
  if (froxelNear >= marchLength)
    froxelNear = max(0.1, marchLength * 0.2);
  float logNear = log2(froxelNear + 1.0);
  float logFar = log2(marchLength + 1.0);

  float heightFogBase = csm.u_params2.y;
  float heightFogFalloff = max(csm.u_params2.z, 1e-5);
  float heightFogStrength = clamp(csm.u_params2.w, 0.0, 2.0);

  float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) *
                       43758.5453) - 0.5;
  jitter *= 0.9;

  float transmittance = 1.0;
  float accumLit = 0.0;
  float accumShadow = 0.0;
  vec3 worldUp = csm.u_worldUp.xyz;
  float upLen = length(worldUp);
  if (upLen <= 1e-6)
    worldUp = vec3(0.0, 0.0, 1.0);
  else
    worldUp /= upLen;

  for (int i = 0; i < 96; i++) {
    if (i >= sampleCount)
      break;

    float s0 = float(i) / float(sampleCount);
    float s1 = float(i + 1) / float(sampleCount);
    float d0 = exp2(mix(logNear, logFar, s0)) - 1.0;
    float d1 = exp2(mix(logNear, logFar, s1)) - 1.0;
    float ds = max(d1 - d0, 0.0);
    if (ds <= 1e-5)
      continue;

    float dMid = 0.5 * (d0 + d1);
    dMid += jitter * ds;
    dMid = clamp(dMid, froxelNear, marchLength);

    vec3 samplePos = cameraPos + rayDir * dMid;
    vec3 sampleViewPos = (vec4(samplePos, 1.0) * csm.u_view).xyz;
    float sampleViewDepth = sampleViewPos.z;

    float shadowValid = 0.0;
    float shadowVis = computeShadowVisibility(
      samplePos, sampleViewDepth, flipUvX, flipUvY, flipViewDepth, shadowValid);
    if (shadowValid <= 1e-5)
      continue;

    float heightCoord = dot(samplePos, worldUp);
    float heightTerm =
      exp(-max(heightCoord - heightFogBase, 0.0) * heightFogFalloff);
    float localDensityMul = mix(1.0, heightTerm, heightFogStrength);
    localDensityMul = clamp(localDensityMul, 0.05, 2.5);

    float dsN = ds / max(marchLength, 1.0);
    float stepScatter =
      shadowVis * shadowValid * phase * scatterWeight * dsN * localDensityMul;
    float stepOcclusion =
      (1.0 - shadowVis) * shadowValid * phase * scatterWeight * dsN * localDensityMul;
    accumLit += transmittance * stepScatter;
    accumShadow += transmittance * stepOcclusion;

    float stepTrans = exp(-sigmaT * dsN * localDensityMul);
    transmittance *= stepTrans;
    if (transmittance <= 1e-4)
      break;
  }

  float fadeNear = clamp(pc.p_params1.y, 0.0, 0.95);
  float fadeFar = clamp(pc.p_params1.z, fadeNear + 0.01, 1.0);
  // 使用线性视深做衰减，避免透视深度导致“俯仰变化时忽明忽暗”。
  float pixelViewDepth = abs((vec4(worldPos, 1.0) * csm.u_view).z);
  float depthNorm = saturate(pixelViewDepth / max(csm.u_splitFar.w, 1.0));
  float depthFade = smoothstep(fadeNear, fadeFar, depthNorm);
  if (!disableNearFade)
    depthFade = mix(0.70, 1.0, depthFade);
  else
    depthFade = 1.0;

  float shafts = accumLit * intensity * sunIntensity * depthFade *
                 max(pc.p_params2.w, 0.0);
  shafts = min(shafts, 2.0);
  float shadowColumns = accumShadow * intensity * depthFade *
                        max(pc.p_params2.w, 0.0);
  shadowColumns = min(shadowColumns, 1.2);

  vec3 outColor = base.rgb + shafts * pc.p_sunColorScale.rgb;
  // 给遮挡区一点体积阴影对比，形成“柱状阴影”感。
  float columnAtten = clamp(1.0 - shadowColumns * 0.35, 0.40, 1.0);
  outColor *= columnAtten;
  o_color = vec4(outColor, base.a);
}
