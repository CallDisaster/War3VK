#version 460

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2DArray s_color;
layout(set = 1, binding = 1) uniform texture2DArray s_depth;
layout(set = 1, binding = 2) uniform texture2DArray s_shadow;
layout(set = 1, binding = 5) uniform textureCubeArray s_pointShadow;

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
  // x=volumeSunEnabled y=softRadius z=receiverBias w=invResolution
  // When x>0.5, lightViewProj[0] is the stable battlefield ortho and
  // cascadeCount is 1 — not a camera-relative CSM slice.
  vec4 u_volumeSunParams;
} csm;

struct PointLight {
  vec4 pos;    // xyz=world position, w=range
  vec4 color;  // rgb=color, w=intensity
  // x=cube layer (-1=unshadowed), y=shadow intensity
  vec4 shadow;
};

layout(set = 1, binding = 4, scalar)
uniform LightBlock {
  uint u_count;
  uint u_pointShadowSamplerIndex;
  uint u_pointShadowedLightCount;
  uint u_pad;
  // x=invCubeResolution y=worldBias z=texelBiasScale w=rangeFadeStart
  vec4 u_pointShadowFilter;
  PointLight u_lights[16];
} lights;

struct FogVolume {
  vec4 worldToLocal0;
  vec4 worldToLocal1;
  vec4 worldToLocal2;
  // x=shape (0 sphere, 1 box, 2 cylinder), y=density,
  // z=normalized edge feather, w=reserved.
  vec4 params;
};

layout(set = 1, binding = 6, scalar)
uniform FogVolumeBlock {
  uint u_count;
  uint u_pad0;
  uint u_pad1;
  uint u_pad2;
  FogVolume u_volumes[8];
} fogVolumes;

layout(push_constant, scalar, row_major)
uniform push_block {
  uint p_colorSampler;
  uint p_depthSampler;
  uint p_shadowSampler;
  uint p_flags;

  // x=intensity y=single-scatter persistence z=density w=weight
  vec4 p_params0;
  // x=anisotropy(y映射) y=fadeNear z=fadeFar w=maxRayDistanceScale
  vec4 p_params1;
  // x=sampleCount y=maxWorldDistance z=froxelNearDistance w=shadowStrengthScale
  vec4 p_params2;
  // rgb=sunColor w=sunIntensity
  vec4 p_sunColorScale;
  vec4 p_viewport;   // x=vpX, y=vpY, z=vpW, w=vpH
  // x=minZ y=maxZ z=scene extinction mix w=unshadowed scatter fallback
  vec4 p_viewportZ;
  // x=effect width y=effect height z=far clear raw w=raw depth quantum
  vec4 p_rtSize;
} pc;

layout(location = 0) out vec4 o_color;

void emitNoEffect() {
  // a=1：不压暗底图；rgb=0：不加亮
  o_color = vec4(0.0, 0.0, 0.0, 1.0);
}

float saturate(float v) {
  return clamp(v, 0.0, 1.0);
}

bool finiteFloat(float v) {
  return !isnan(v) && !isinf(v);
}

bool finiteVec3(vec3 v) {
  return !any(isnan(v)) && !any(isinf(v));
}

bool finiteVec4(vec4 v) {
  return !any(isnan(v)) && !any(isinf(v));
}

vec3 fogWorldToLocalPoint(FogVolume volume, vec3 worldPos) {
  vec4 p = vec4(worldPos, 1.0);
  return vec3(dot(volume.worldToLocal0, p),
              dot(volume.worldToLocal1, p),
              dot(volume.worldToLocal2, p));
}

vec3 fogWorldToLocalDirection(FogVolume volume, vec3 worldDir) {
  vec4 d = vec4(worldDir, 0.0);
  return vec3(dot(volume.worldToLocal0, d),
              dot(volume.worldToLocal1, d),
              dot(volume.worldToLocal2, d));
}

bool clipFogSlab(float origin, float direction,
                 inout float rayEnter, inout float rayExit) {
  if (abs(direction) <= 1.0e-8)
    return abs(origin) <= 1.0;
  float t0 = (-1.0 - origin) / direction;
  float t1 = ( 1.0 - origin) / direction;
  if (t0 > t1) {
    float swapValue = t0;
    t0 = t1;
    t1 = swapValue;
  }
  rayEnter = max(rayEnter, t0);
  rayExit = min(rayExit, t1);
  return rayExit > rayEnter + 1.0e-6;
}

bool intersectFogVolume(FogVolume volume, vec3 cameraPos, vec3 rayDir,
                        float intervalStart, float intervalEnd,
                        out vec2 rayInterval) {
  rayInterval = vec2(1.0, 0.0);
  if (!finiteVec4(volume.worldToLocal0) ||
      !finiteVec4(volume.worldToLocal1) ||
      !finiteVec4(volume.worldToLocal2) || !finiteVec4(volume.params))
    return false;

  vec3 origin = fogWorldToLocalPoint(volume, cameraPos);
  vec3 direction = fogWorldToLocalDirection(volume, rayDir);
  if (!finiteVec3(origin) || !finiteVec3(direction))
    return false;

  int shape = int(floor(volume.params.x + 0.5));
  float rayEnter = intervalStart;
  float rayExit = intervalEnd;
  if (shape == 0) {
    float a = dot(direction, direction);
    float b = dot(origin, direction);
    float c = dot(origin, origin) - 1.0;
    float discriminant = b * b - a * c;
    if (!finiteFloat(a) || !finiteFloat(discriminant) ||
        a <= 1.0e-12 || discriminant < 0.0)
      return false;
    float root = sqrt(max(discriminant, 0.0));
    rayEnter = max(rayEnter, (-b - root) / a);
    rayExit = min(rayExit, (-b + root) / a);
  } else if (shape == 1) {
    if (!clipFogSlab(origin.x, direction.x, rayEnter, rayExit) ||
        !clipFogSlab(origin.y, direction.y, rayEnter, rayExit) ||
        !clipFogSlab(origin.z, direction.z, rayEnter, rayExit))
      return false;
  } else if (shape == 2) {
    float a = dot(direction.xy, direction.xy);
    float b = dot(origin.xy, direction.xy);
    float c = dot(origin.xy, origin.xy) - 1.0;
    if (a <= 1.0e-12) {
      if (c > 0.0)
        return false;
    } else {
      float discriminant = b * b - a * c;
      if (!finiteFloat(discriminant) || discriminant < 0.0)
        return false;
      float root = sqrt(max(discriminant, 0.0));
      rayEnter = max(rayEnter, (-b - root) / a);
      rayExit = min(rayExit, (-b + root) / a);
    }
    if (!clipFogSlab(origin.z, direction.z, rayEnter, rayExit))
      return false;
  } else {
    return false;
  }

  if (!finiteFloat(rayEnter) || !finiteFloat(rayExit) ||
      rayExit <= rayEnter + 1.0e-5)
    return false;
  rayInterval = vec2(rayEnter, rayExit);
  return true;
}

float fogVolumeWeight(FogVolume volume, vec3 localPos) {
  int shape = int(floor(volume.params.x + 0.5));
  float edge = 2.0;
  if (shape == 0) {
    edge = length(localPos);
  } else if (shape == 1) {
    edge = max(max(abs(localPos.x), abs(localPos.y)), abs(localPos.z));
  } else if (shape == 2) {
    edge = max(length(localPos.xy), abs(localPos.z));
  }
  if (!finiteFloat(edge) || edge >= 1.0)
    return 0.0;
  float feather = clamp(volume.params.z, 0.0, 1.0);
  if (feather <= 1.0e-5)
    return 1.0;
  return 1.0 - smoothstep(max(1.0 - feather, 0.0), 1.0, edge);
}

float densityToSigmaT(float density) {
  float bounded = clamp(density, 0.0, 2.0);
  float response = bounded / (1.0 + 0.5 * bounded);
  return response * 0.0006;
}

float samplePointVolumeShadow(PointLight light, vec3 sampleWorldPos) {
  if (lights.u_pointShadowedLightCount == 0u ||
      !finiteVec4(light.shadow) || !finiteVec3(sampleWorldPos))
    return 1.0;

  int cubeLayer = int(floor(light.shadow.x + 0.5));
  float shadowIntensity = clamp(light.shadow.y, 0.0, 1.0);
  if (cubeLayer < 0 || cubeLayer >= 4 || shadowIntensity <= 1.0e-4)
    return 1.0;

  float shadowRange = max(light.pos.w, 1.0);
  vec3 lightToSample = sampleWorldPos - light.pos.xyz;
  float distance2 = dot(lightToSample, lightToSample);
  if (!finiteFloat(distance2) || distance2 <= 1.0e-12)
    return 1.0;
  float currentDistance = sqrt(distance2);
  if (!finiteFloat(currentDistance) || currentDistance >= shadowRange)
    return 1.0;

  vec3 direction = lightToSample / currentDistance;
  float majorAxis = max(max(abs(direction.x), abs(direction.y)),
                        abs(direction.z));
  float invResolution = max(lights.u_pointShadowFilter.x, 0.0);
  float worldBias = max(lights.u_pointShadowFilter.y, 0.0);
  float texelBiasScale = clamp(lights.u_pointShadowFilter.z, 0.0, 1.0);
  float rangeFadeStart =
    clamp(lights.u_pointShadowFilter.w, 0.50, 0.98);
  if (!finiteFloat(majorAxis) || !finiteFloat(invResolution) ||
      !finiteFloat(worldBias) || invResolution <= 0.0)
    return 1.0;

  // The cube stores radial distance/range. One nearest sample per overlapped
  // march segment is intentional: low-resolution volume integration and the
  // final upsample already filter the result, while surface lighting retains
  // its separate 16-tap PCF path.
  float storedDepth = texture(
    samplerCubeArray(s_pointShadow,
      s_samplers[nonuniformEXT(lights.u_pointShadowSamplerIndex)]),
    vec4(direction, float(cubeLayer))).r;
  if (!finiteFloat(storedDepth))
    return 1.0;

  float currentDepth = clamp(currentDistance / shadowRange, 0.0, 1.0);
  float texelWorld = 2.0 * majorAxis * invResolution * currentDistance;
  // Match the surface receiver's radial-depth contract. Volume samples do not
  // have a receiver normal, so use the additive base + footprint term without
  // the surface-only slope multiplier.
  float biasDepth = clamp(
    (worldBias + texelWorld * texelBiasScale) / shadowRange,
    0.0, 0.01);
  float visibility = currentDepth > storedDepth + biasDepth ? 0.0 : 1.0;
  float rangeFade = 1.0 - smoothstep(
    rangeFadeStart, 1.0, currentDepth);
  return mix(1.0, visibility, shadowIntensity * rangeFade);
}

bool decodeDepth(ivec2 pixel, out float depthN, out bool farClear) {
  depthN = 0.0;
  farClear = false;
  float rawDepth = texelFetch(
    sampler2DArray(s_depth, s_samplers[nonuniformEXT(pc.p_depthSampler)]),
    ivec3(pixel, 0), 0).r;
  float minZ = pc.p_viewportZ.x;
  float maxZ = pc.p_viewportZ.y;
  float denom = maxZ - minZ;
  float quantum = max(pc.p_rtSize.w, 1.0e-7);
  if (!finiteFloat(rawDepth) || !finiteFloat(minZ) || !finiteFloat(maxZ) ||
      !finiteFloat(denom) || abs(denom) <= 1.0e-6 ||
      !finiteFloat(pc.p_rtSize.z) || !finiteFloat(quantum))
    return false;

  float rawLo = min(minZ, maxZ);
  float rawHi = max(minZ, maxZ);
  if (rawDepth < rawLo - quantum || rawDepth > rawHi + quantum)
    return false;

  farClear = abs(rawDepth - pc.p_rtSize.z) <=
      max(0.5 * quantum, 1.0e-7);
  depthN = (rawDepth - minZ) / denom;
  float normalizedQuantum = quantum / max(abs(denom), 1.0e-6);
  if (!finiteFloat(depthN) || depthN < -normalizedQuantum ||
      depthN > 1.0 + normalizedQuantum)
    return false;
  depthN = saturate(depthN);
  return true;
}

bool reconstructWorldPos(vec2 uvLocal, float depthN, out vec3 worldPos) {
  vec4 clip = vec4(uvLocal.x * 2.0 - 1.0, 1.0 - uvLocal.y * 2.0, depthN, 1.0);
  vec4 worldH = clip * csm.u_invViewProj;
  if (!finiteVec4(worldH) || abs(worldH.w) <= 1e-6) {
    worldPos = vec3(0.0);
    return false;
  }
  worldPos = worldH.xyz / worldH.w;
  return finiteVec3(worldPos);
}

// 体积光专用软阴影采样。默认 2x2 双线性；体积太阳 ortho 启用时扩到 3x3
// 并按 softRadius 拉开 texel 间距，软化羽毛/镂空剪影叠成的「密柱」。
float sampleShadowVisibility2x2(uint cascadeIndex, vec2 uv,
                                 ivec3 extent, float referenceDepth) {
  if (any(lessThanEqual(extent, ivec3(0))))
    return 1.0;
  int layer = clamp(int(cascadeIndex), 0, extent.z - 1);
  bool volumeSun = csm.u_volumeSunParams.x > 0.5;
  float softRadius = volumeSun
    ? clamp(csm.u_params.w, 0.75, 3.5)
    : 1.0;
  vec2 texelSize = 1.0 / max(vec2(extent.xy), vec2(1.0));

  if (!volumeSun || softRadius <= 1.05) {
    vec2 texCoord = clamp(uv, vec2(0.0), vec2(1.0)) * vec2(extent.xy) - 0.5;
    ivec2 base = ivec2(floor(texCoord));
    vec2 f = fract(texCoord);
    float sum = 0.0;
    for (int dy = 0; dy <= 1; ++dy) {
      for (int dx = 0; dx <= 1; ++dx) {
        ivec2 texel = clamp(base + ivec2(dx, dy), ivec2(0), extent.xy - ivec2(1));
        float depth = texelFetch(
          sampler2DArray(s_shadow,
            s_samplers[nonuniformEXT(pc.p_shadowSampler)]),
          ivec3(texel, layer), 0).r;
        float vis = referenceDepth <= depth ? 1.0 : 0.0;
        float wx = dx == 0 ? (1.0 - f.x) : f.x;
        float wy = dy == 0 ? (1.0 - f.y) : f.y;
        sum += vis * wx * wy;
      }
    }
    return sum;
  }

  // 3x3 盒滤波 + softRadius 步长：柱边缘更柔，少复现 caster 轮廓缝隙。
  float stepTexel = softRadius;
  float sum = 0.0;
  float weightSum = 0.0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      vec2 sampleUv = uv + vec2(float(dx), float(dy)) * texelSize * stepTexel;
      if (sampleUv.x < 0.0 || sampleUv.x > 1.0 ||
          sampleUv.y < 0.0 || sampleUv.y > 1.0)
        continue;
      ivec2 texel = clamp(
        ivec2(floor(sampleUv * vec2(extent.xy))),
        ivec2(0), extent.xy - ivec2(1));
      float depth = texelFetch(
        sampler2DArray(s_shadow,
          s_samplers[nonuniformEXT(pc.p_shadowSampler)]),
        ivec3(texel, layer), 0).r;
      float vis = referenceDepth <= depth ? 1.0 : 0.0;
      float w = 1.0;
      if (dx == 0 && dy == 0)
        w = 2.0;
      sum += vis * w;
      weightSum += w;
    }
  }
  return weightSum > 1e-6 ? sum / weightSum : 1.0;
}

// Feather the valid light-volume boundary. A binary in/out mask produces a
// visible curtain exactly at the CSM projection edge, especially when the
// directional pass is allowed to fall back to a low unshadowed medium term.
float shadowCoverage(vec3 lightNdc, vec2 uv) {
  float edgeUv = min(min(uv.x, 1.0 - uv.x),
                     min(uv.y, 1.0 - uv.y));
  float edgeZ = min(lightNdc.z, 1.0 - lightNdc.z);
  float uvFeather = max(csm.u_params.y * 8.0, 0.003);
  float zFeather = 0.004;
  return smoothstep(0.0, uvFeather, edgeUv) *
         smoothstep(0.0, zFeather, edgeZ);
}

bool sampleCsmCascade(int cascadeIndex, float rayDistance,
                      vec4 clipOrigin[4], vec4 clipSlope[4],
                      ivec3 shadowExtent,
                      bool flipUvX, bool flipUvY,
                      out float coverage, out float visibility) {
  coverage = 0.0;
  visibility = 1.0;
  vec4 lightClip =
    clipOrigin[cascadeIndex] + clipSlope[cascadeIndex] * rayDistance;
  if (abs(lightClip.w) <= 1e-6)
    return false;
  vec3 lightNdc = lightClip.xyz / lightClip.w;
  if (lightNdc.z < 0.0 || lightNdc.z > 1.0)
    return false;
  vec2 uv = lightNdc.xy * 0.5 + 0.5;
  uv.y = 1.0 - uv.y;
  if (flipUvX)
    uv.x = 1.0 - uv.x;
  if (flipUvY)
    uv.y = 1.0 - uv.y;
  // 体积雾不把“阴影图覆盖区外”当作有效遮挡，避免边角假柱。
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return false;

  coverage = shadowCoverage(lightNdc, uv);
  // 使用 2x2 双线性 PCF 代替单 texel binary 查询。这让 caster 剪影的
  // 尖锐边缘（如火凤凰羽毛空隙）被软化成平滑渐变，ray march 堆叠时
  // 不再呈现为密集的模型轮廓重复。
  float baseBias = max(csm.u_params.x, 0.0) * 1.35;
  float referenceDepth = clamp(lightNdc.z - baseBias, 0.0, 1.0);
  visibility = sampleShadowVisibility2x2(
    uint(cascadeIndex), uv, shadowExtent, referenceDepth);
  return true;
}

void samplePrimaryCsm(float rayDistance, float viewDepth,
                      vec4 clipOrigin[4], vec4 clipSlope[4],
                      ivec3 shadowExtent,
                      bool flipUvX, bool flipUvY, bool flipViewDepth,
                      out float coverage, out float visibility) {
  coverage = 0.0;
  visibility = 1.0;
  int cascadeCount = int(csm.u_params.z + 0.5);
  if (cascadeCount <= 0 ||
      any(lessThanEqual(shadowExtent, ivec3(0))))
    return;
  cascadeCount = clamp(cascadeCount, 1, 4);

  // 体积太阳 ortho：不按相机 view-depth 选层（那会重现柱随 pitch 长缩）。
  // 近层优先；近层 UV 无效时用远层；两层都有效时取更暗的 true occlusion。
  if (csm.u_volumeSunParams.x > 0.5) {
    float coverageNear = 0.0;
    float visibilityNear = 1.0;
    bool okNear = sampleCsmCascade(
      0, rayDistance, clipOrigin, clipSlope, shadowExtent,
      flipUvX, flipUvY, coverageNear, visibilityNear);
    float coverageFar = 0.0;
    float visibilityFar = 1.0;
    bool okFar = false;
    if (cascadeCount > 1) {
      okFar = sampleCsmCascade(
        1, rayDistance, clipOrigin, clipSlope, shadowExtent,
        flipUvX, flipUvY, coverageFar, visibilityFar);
    }
    if (!okNear && !okFar)
      return;
    if (okNear && okFar) {
      float oNear = coverageNear * (1.0 - visibilityNear);
      float oFar = coverageFar * (1.0 - visibilityFar);
      // 近级更锐：覆盖足够时用近级；否则用远级补覆盖，并取更暗遮挡。
      if (coverageNear > 0.55) {
        float o = max(oNear, oFar * 0.85);
        coverage = max(coverageNear, coverageFar * 0.85);
        visibility = coverage > 1e-6
          ? 1.0 - clamp(o / coverage, 0.0, 1.0)
          : 1.0;
      } else {
        float o = max(oNear, oFar);
        coverage = max(coverageNear, coverageFar);
        visibility = coverage > 1e-6
          ? 1.0 - clamp(o / coverage, 0.0, 1.0)
          : 1.0;
      }
    } else if (okNear) {
      coverage = coverageNear;
      visibility = visibilityNear;
    } else {
      coverage = coverageFar;
      visibility = visibilityFar;
    }
    return;
  }

  float splits[4];
  splits[0] = csm.u_splitFar.x;
  splits[1] = csm.u_splitFar.y;
  splits[2] = csm.u_splitFar.z;
  splits[3] = csm.u_splitFar.w;

  float vDepth = flipViewDepth ? viewDepth : abs(viewDepth);
  int c0 = cascadeCount - 1;
  for (int i = 0; i < cascadeCount; i++) {
    if (vDepth <= splits[i]) {
      c0 = i;
      break;
    }
  }

  float coverage0 = 0.0;
  float visibility0 = 1.0;
  bool valid0 = sampleCsmCascade(
    c0, rayDistance, clipOrigin, clipSlope, shadowExtent,
    flipUvX, flipUvY, coverage0, visibility0);

  // 如果主 cascade 采样失败（probe 的光空间 UV 越界），向更粗的 cascade
  // 回退。这修复了俯视视角下阴影柱因 view-depth 选到了过近的 cascade、
  // 而该 cascade 覆盖范围不够导致柱子消失的问题。粗 cascade 的世界空间
  // 覆盖范围更大，光空间 UV 更容易落入有效区域。
  if (!valid0) {
    for (int fallback = c0 + 1; fallback < cascadeCount; ++fallback) {
      valid0 = sampleCsmCascade(
        fallback, rayDistance, clipOrigin, clipSlope, shadowExtent,
        flipUvX, flipUvY, coverage0, visibility0);
      if (valid0) {
        c0 = fallback;
        break;
      }
    }
    if (!valid0)
      return;
  }

  coverage = coverage0;
  visibility = visibility0;

  // The C++ side already uploads the same authored blend range used by the
  // surface receiver. The old volume path ignored it and switched one binary
  // shadow fetch at the split, so camera pitch made columns grow or disappear
  // one cascade at a time. Blend covered true occlusion, not just visibility,
  // so a feathered projection boundary remains energy-consistent.
  float blendRange = max(csm.u_params2.x, 0.0);
  if (blendRange <= 0.0 || c0 + 1 >= cascadeCount)
    return;
  float blendT = clamp(
    (vDepth - (splits[c0] - blendRange)) / blendRange, 0.0, 1.0);
  float blendWeight = blendT * blendT * (3.0 - 2.0 * blendT);
  if (blendWeight <= 0.0)
    return;

  float coverage1 = 0.0;
  float visibility1 = 1.0;
  if (!sampleCsmCascade(
        c0 + 1, rayDistance, clipOrigin, clipSlope, shadowExtent,
        flipUvX, flipUvY, coverage1, visibility1))
    return;

  float trueOcclusion0 = coverage0 * (1.0 - visibility0);
  float trueOcclusion1 = coverage1 * (1.0 - visibility1);
  coverage = mix(coverage0, coverage1, blendWeight);
  float trueOcclusion = mix(
    trueOcclusion0, trueOcclusion1, blendWeight);
  visibility = coverage > 1e-6
    ? 1.0 - clamp(trueOcclusion / coverage, 0.0, 1.0)
    : 1.0;
}

float hgPhase(float mu, float g) {
  float gg = g * g;
  float denom = max(1.0 + gg - 2.0 * g * mu, 1e-3);
  return (1.0 - gg) / (12.5663706 * pow(denom, 1.5));
}

// 软饱和：拉高强度时抬升高光，但不会线性冲到白屏。
float softClip(float v, float maxV) {
  float m = max(maxV, 1e-4);
  float x = max(v, 0.0);
  return (x * m) / max(x + m, 1e-4);
}

vec3 softClipRgb(vec3 v, float maxV) {
  vec3 positive = max(v, vec3(0.0));
  float peak = max(max(positive.r, positive.g), positive.b);
  if (peak <= 1e-6)
    return vec3(0.0);
  // One luminance-like scale preserves colored point-light chroma. Clipping
  // each channel independently desaturates strong warm/cool shafts.
  return positive * (softClip(peak, maxV) / peak);
}

// Interleaved gradient noise (Jimenez 2014). Deterministic per screen pixel,
// used to stratify the discontinuous shadow-visibility probes so their low
// sample count no longer aliases into visible ground banding once the
// readability weight (>1.0) amplifies the quantized occlusion.
float interleavedGradientNoise(vec2 p) {
  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
  ivec2 fullSize = textureSize(
    sampler2DArray(s_depth, s_samplers[nonuniformEXT(pc.p_depthSampler)]),
    0).xy;
  if (any(lessThanEqual(fullSize, ivec2(0)))) {
    emitNoEffect();
    return;
  }
  vec2 texSize = vec2(fullSize);
  vec2 outSize = max(pc.p_rtSize.xy, vec2(1.0));
  // 低分辨率像素中心先映射到 full-res，再减去 DXVK 已注入的 D3D9
  // half-texel，得到与 shadow receiver 一致的 D3D 整数像素坐标。
  vec2 uvOut = gl_FragCoord.xy / outSize;
  vec2 fpixD3D = uvOut * texSize - vec2(0.5);
  vec2 vpMin = pc.p_viewport.xy;
  vec2 vpSize = max(pc.p_viewport.zw, vec2(1.0));

  if (fpixD3D.x < vpMin.x || fpixD3D.y < vpMin.y ||
      fpixD3D.x >= (vpMin.x + vpSize.x) ||
      fpixD3D.y >= (vpMin.y + vpSize.y)) {
    emitNoEffect();
    return;
  }

  float intensity = max(pc.p_params0.x, 0.0);
  float sunIntensity = max(pc.p_sunColorScale.w, 0.0);
  vec3 sunRadiance = max(pc.p_sunColorScale.rgb, vec3(0.0)) * sunIntensity;
  float sunRadiancePeak = max(max(sunRadiance.r, sunRadiance.g), sunRadiance.b);
  bool hasSunVolume = finiteVec3(sunRadiance) &&
                      finiteFloat(sunRadiancePeak) &&
                      sunRadiancePeak > 1e-6;
  // The producer hard-caps volumetric point lights at two. Keep the shader
  // bound identical so the per-pixel ray intervals have a fixed small budget.
  uint pointLightCount = min(lights.u_count, 2u);
  bool hasPointVolume = pointLightCount > 0u;
  if (!finiteFloat(intensity) || !finiteFloat(sunIntensity) ||
      intensity <= 1e-6 || (!hasSunVolume && !hasPointVolume)) {
    emitNoEffect();
    return;
  }

  // Do not linearly filter hardware depth before world reconstruction. Depth
  // is non-linear, so interpolation across a silhouette creates a point that
  // belongs to neither surface and makes the ray march leak or disappear. The
  // chosen integer pixel matches the guide pixel used by the composite pass.
  ivec2 vpMinI = ivec2(vpMin);
  ivec2 vpMaxI = ivec2(vpMin + vpSize) - ivec2(1);
  ivec2 depthPixel = clamp(
    ivec2(floor(fpixD3D + vec2(0.5))), vpMinI, vpMaxI);
  depthPixel = clamp(depthPixel, ivec2(0), fullSize - ivec2(1));
  vec2 uvLocal = (vec2(depthPixel) - vpMin) / vpSize;

  const uint flags = pc.p_flags;
  const bool flipUvX = (flags & 0x1u) != 0u;
  const bool flipUvY = (flags & 0x2u) != 0u;
  const bool flipSun = (flags & 0x4u) != 0u;
  const bool disableNearFade = (flags & 0x8u) != 0u;
  const bool flipViewDepth = (flags & 0x10u) != 0u;
  const bool farIsOne = (flags & 0x20u) != 0u;

  float depthN = 0.0;
  bool skyPixel = false;
  if (!decodeDepth(depthPixel, depthN, skyPixel)) {
    emitNoEffect();
    return;
  }
  if (skyPixel)
    depthN = farIsOne ? 0.999 : 0.001;

  vec3 worldPos = vec3(0.0);
  vec3 cameraPos = csm.u_cameraPos.xyz;
  if (!reconstructWorldPos(uvLocal, depthN, worldPos) ||
      !finiteVec3(cameraPos)) {
    emitNoEffect();
    return;
  }
  vec3 rayVec = worldPos - cameraPos;
  float rayLen = length(rayVec);
  if (!finiteFloat(rayLen) || rayLen <= 1e-4) {
    emitNoEffect();
    return;
  }

  // GPU-side backstop: stale callers cannot restore the old unbounded
  // full-screen ray-march that could exceed the Windows TDR budget.
  int sampleCount = int(clamp(pc.p_params2.x, 4.0, 16.0));
  float maxDistScale = clamp(pc.p_params1.w, 0.05, 2.0);
  float maxWorldDistance = max(pc.p_params2.y * maxDistScale, 25.0);
  // 积分区间（相机到表面沿 ray 的弧长）：
  // - 天空：仍是相机端 [0, min(L,D)]（无地表锚点）。
  // - 非天空且 L>D：地表端 [L-D, L]。旧逻辑固定 [0,D] 时，上帝视角会把
  //   近地表尾段整段丢掉，柱子看起来从模型向地面“长出来/缩回去”。
  // - 非天空且 L<=D：整段 [0,L] 都积分到表面。
  float intervalStart = 0.0;
  float intervalEnd = min(rayLen, maxWorldDistance);
  if (!skyPixel && rayLen > maxWorldDistance) {
    intervalStart = rayLen - maxWorldDistance;
    intervalEnd = rayLen;
  }
  float intervalLength = max(intervalEnd - intervalStart, 0.0);
  if (intervalLength <= 1e-4) {
    emitNoEffect();
    return;
  }
  // 兼容旧命名：积分预算长度仍是 min(L,D)，用于 fade / 能量参考。
  float marchLength = intervalLength;

  vec3 rayDir = rayVec / rayLen;
  // samplePos = cameraPos + rayDir * d, and an affine view transform is
  // linear along that ray. Hoist the two uniform dot products out of the
  // 4..16-step march; only the resulting multiply-add is needed per sample.
  // Keeping the camera intercept instead of assuming zero also preserves the
  // exact uploaded matrix/camera contract under finite rounding or a fallback
  // snapshot.
  float cameraViewDepth = (vec4(cameraPos, 1.0) * csm.u_view).z;
  float rayViewDepthSlope = (vec4(rayDir, 0.0) * csm.u_view).z;
  if (!finiteFloat(cameraViewDepth) || !finiteFloat(rayViewDepthSlope)) {
    emitNoEffect();
    return;
  }
  float shadowStrength = clamp(pc.p_params2.w, 0.0, 1.0);
  float fallbackWeight = clamp(pc.p_viewportZ.w, 0.0, 1.0);

  // CSM clip coordinates are affine along the same camera ray. Pay the eight
  // matrix transforms once per effect pixel, then each longitudinal probe is
  // only two vec4 FMAs. This keeps the new thin-column coverage from turning
  // into up to 128 world-to-light matrix transforms per pixel.
  vec4 csmClipOrigin[4];
  vec4 csmClipSlope[4];
  ivec3 csmShadowExtent = ivec3(0);
  bool hasPublishedCsm = hasSunVolume &&
      csm.u_params.z > 0.5 && shadowStrength > 1e-6;
  if (hasPublishedCsm) {
    csmShadowExtent = textureSize(
      sampler2DArray(s_shadow,
        s_samplers[nonuniformEXT(pc.p_shadowSampler)]), 0);
  }
  for (int cascade = 0; cascade < 4; ++cascade) {
    if (hasPublishedCsm) {
      csmClipOrigin[cascade] =
        vec4(cameraPos, 1.0) * csm.u_lightViewProj[cascade];
      csmClipSlope[cascade] =
        vec4(rayDir, 0.0) * csm.u_lightViewProj[cascade];
    } else {
      csmClipOrigin[cascade] = vec4(0.0);
      csmClipSlope[cascade] = vec4(0.0);
    }
  }

  // Precompute the forward-ray interval covered by every point-light sphere.
  // Clip to the same [intervalStart, intervalEnd] domain as sun march so
  // surface-end intervals still see overlapping point spheres. Invalid or
  // non-intersecting lights use an empty interval. Medium extinction still
  // covers the full interval even in a point-only frame.
  vec2 pointRayIntervals[2];
  bool hasPointRayInterval = false;
  if (hasPointVolume) {
    for (uint li = 0u; li < pointLightCount; li++) {
      pointRayIntervals[li] = vec2(1.0, 0.0);

      vec4 lightPosRange = lights.u_lights[li].pos;
      if (!finiteVec4(lightPosRange))
        continue;

      float range = max(lightPosRange.w, 1.0);
      vec3 cameraToSphere = cameraPos - lightPosRange.xyz;
      float b = dot(cameraToSphere, rayDir);
      float c = dot(cameraToSphere, cameraToSphere) - range * range;
      float discriminant = b * b - c;
      if (!finiteFloat(discriminant) || discriminant < 0.0)
        continue;

      float root = sqrt(max(discriminant, 0.0));
      float rayEnter = max(-b - root, intervalStart);
      float rayExit = min(-b + root, intervalEnd);
      if (!finiteFloat(rayEnter) || !finiteFloat(rayExit) ||
          rayExit <= rayEnter + 1e-5)
        continue;

      pointRayIntervals[li] = vec2(rayEnter, rayExit);
      hasPointRayInterval = true;
    }
  }
  hasPointVolume = hasPointRayInterval;
  if (!hasSunVolume && !hasPointVolume) {
    emitNoEffect();
    return;
  }

  // Compute exact ray support once per pixel. March segments below integrate
  // their overlap with these intervals, so a fog body thinner than ds cannot
  // disappear merely because no fixed segment midpoint landed inside it.
  uint fogVolumeCount = min(fogVolumes.u_count, 8u);
  vec2 fogRayIntervals[8];
  float fogSigmaT[8];
  bool hasFogRayInterval = false;
  float fogDensityHint = 0.0;
  for (uint vi = 0u; vi < 8u; ++vi) {
    fogRayIntervals[vi] = vec2(1.0, 0.0);
    fogSigmaT[vi] = 0.0;
    if (vi >= fogVolumeCount)
      continue;
    FogVolume volume = fogVolumes.u_volumes[vi];
    float volumeDensity = clamp(volume.params.y, 0.0, 2.0);
    if (!finiteFloat(volumeDensity) || volumeDensity <= 1.0e-6)
      continue;
    vec2 volumeInterval;
    if (!intersectFogVolume(volume, cameraPos, rayDir,
                            intervalStart, intervalEnd, volumeInterval))
      continue;
    fogRayIntervals[vi] = volumeInterval;
    fogSigmaT[vi] = densityToSigmaT(volumeDensity);
    fogDensityHint = max(fogDensityHint, volumeDensity);
    hasFogRayInterval = true;
  }

  vec3 sunDir = csm.u_sunDir.xyz;
  float sunLen = length(sunDir);
  if (sunLen <= 1e-6)
    sunDir = vec3(-0.3, -0.2, -1.0);
  else
    sunDir /= sunLen;
  if (flipSun)
    sunDir = -sunDir;

  // Keep the authored forward lobe deliberately narrow. The legacy
  // skyThreshold field is the only available scalar here, but the previous
  // 0.55 HG cap made a low camera several times brighter than the ordinary
  // top-down War3 view. This gentler mapping redistributes normalized phase
  // energy toward side/back scatter without increasing global fog energy.
  float anisotropyG = clamp((pc.p_params1.x - 0.55) * 0.50, 0.0, 0.22);
  // u_sunDir 是光子从太阳射向场景的传播方向；观察方向是 -rayDir。
  // 旧式两边同时取反会把 HG 前向峰翻到背光侧，常见 RTS 视角会因此变淡。
  float mu = dot(sunDir, -rayDir);
  // 相位函数：保证有底噪各向同性项，前向增强形成“光柱”方向感。
  float phaseIso = 0.07957747;
  // Keep enough directional response for a low camera without allowing the HG
  // lobe to erase ordinary top-down/back-scatter shafts. Both inputs are
  // normalized phase functions, so reducing the authored mix redistributes
  // energy instead of globally brightening the fog.
  float phase = mix(phaseIso, hgPhase(mu, anisotropyG), 0.35);
  phase = max(phase, phaseIso * 0.80);
  // UI light intensity is expressed in LDR scene-light units, not radiance per
  // steradian. Normalize the physical phase by its isotropic 1/(4*pi) value so
  // an isotropic medium has unit response instead of an accidental 0.0796x.
  float phaseLdr = phase / phaseIso;

  float density = clamp(pc.p_params0.z, 0.0, 2.0);
  float contrastWeight = clamp(pc.p_params0.w, 0.0, 3.0);
  float decay = clamp(pc.p_params0.y, 0.70, 0.999);
  if (!finiteFloat(density) || !finiteFloat(contrastWeight) ||
      !finiteFloat(decay) ||
      (density <= 1e-6 && !hasFogRayInterval)) {
    emitNoEffect();
    return;
  }

  // density 使用饱和响应，极端 UI 值不会把旧游戏画面压成不透明雾墙。
  // sigmaT 决定场景透过率；decay 映射为单次散射反照率。旧实现用
  // decay 去降低 sigmaT，导致用户把“衰减/保持”拉高时散射反而变弱。
  float decayN = saturate((decay - 0.70) / 0.299);
  // 提高基础散射系数。旧值 0.00028 在 1200 单位射线上只产生 0.336
  // 光学深度（透过率 71%），远不够在体积中形成可感知的明暗对比。
  float sigmaT = densityToSigmaT(density);
  float singleScatteringAlbedo = mix(0.72, 0.96, decayN);

  float fadeNear = clamp(pc.p_params1.y, 0.0, 0.95);
  float fadeFar = clamp(pc.p_params1.z, fadeNear + 0.01, 1.0);
  // 最大距离只应扩大积分上限，不能让同一个近中景像素反向变淡。
  // 淡入参考尺度封顶在 1000 War3 世界单位；用户把距离拉到 6000 时，
  // 500u 射线不再从约 89% 介质存在感跌到约 22%。
  float fadeReferenceDistance = min(maxWorldDistance, 1000.0);
  float distanceNorm = saturate(marchLength / max(fadeReferenceDistance, 1.0));
  float distanceFade = smoothstep(fadeNear, fadeFar, distanceNorm);
  // Clear-air profile: the camera neighbourhood contributes almost no global
  // haze. Authored local volumes do not use this distance fade at all.
  float mediumPresence = disableNearFade ? 1.0 : mix(0.03, 1.0, distanceFade);

  // 俯视短 ray 不能浪费 20% 在 froxelNear 上，否则柱刚贴地就被跳过。
  vec3 worldUpEarly = csm.u_worldUp.xyz;
  float upLenEarly = length(worldUpEarly);
  if (upLenEarly > 1e-6)
    worldUpEarly /= upLenEarly;
  else
    worldUpEarly = vec3(0.0, 0.0, 1.0);
  float topDownEarly = abs(dot(rayDir, worldUpEarly));
  // nearSkip 相对积分区间长度，避免地表端区间再被相机端 froxel 错位。
  float froxelNearFrac = mix(0.18, 0.04, smoothstep(0.40, 0.88, topDownEarly));
  // A local body is an explicit authored support interval and may begin at
  // the camera. Never discard it through the legacy global-medium near skip.
  float nearSkip = hasFogRayInterval ? 0.0 : min(
    max(pc.p_params2.z, 0.1), intervalLength * froxelNearFrac);
  float marchStart = intervalStart + nearSkip;
  float segmentLength = max(intervalEnd - marchStart, 0.0);
  if (!finiteFloat(segmentLength) || segmentLength <= 1e-4) {
    emitNoEffect();
    return;
  }
  float ds = segmentLength / float(sampleCount);

  float heightFogBase = csm.u_params2.y;
  float heightFogFalloff = max(csm.u_params2.z, 1e-5);
  float heightFogStrength = clamp(csm.u_params2.w, 0.0, 2.0);

  float transmittance = 1.0;
  float opticalDepth = 0.0;
  float accumSunReference = 0.0;
  float accumLit = 0.0;
  vec3 accumPoint = vec3(0.0);
  // Whole-ray ratios dilute a short blocker on a long grazing ray. Retain a
  // bounded local blocker ratio plus its actual optical support; this is
  // view-independent evidence, unlike camera-pitch readability heuristics.
  float peakSunLocalOcclusion = 0.0;
  float sunShadowEvidenceOptical = 0.0;

  vec3 worldUp = worldUpEarly;
  float upLen = 1.0;

  // height(camera + ray*d) 对 d 是仿射函数。把两个 dot 移出循环，后续中点
  // 求积每段只需一个标量 FMA，不为稳定性修复额外引入 world-position 成本。
  float cameraHeight = dot(cameraPos, worldUp);
  float rayHeightSlope = dot(rayDir, worldUp);
  if (!finiteFloat(cameraHeight) || !finiteFloat(rayHeightSlope)) {
    emitNoEffect();
    return;
  }

  bool sampleDirectionalCsm = hasPublishedCsm;
  // A thin shadow column must receive the same longitudinal evidence at every
  // camera pitch. The previous 10..24 world-unit spacing explicitly reduced
  // probe density for grazing views, exactly where a distant column already
  // occupies the fewest effect pixels. Ten-unit spacing is still hard-capped at
  // eight probes per march segment (sixteen raw fetches at a cascade seam).
  const float directionalProbeSpacing = 10.0;
  // 可读性护栏：即使用户把密度/距离拉满，也不允许体积 pass 独自
  // 把场景压到接近全黑。默认参数通常远达不到此上限。
  // 提高最大光学深度上限，让高 density 设置下雾能量充足。
  // density=2 时从 0.75 提升到 1.20，不会导致不透明雾墙。
  float densityHint = max(density, fogDensityHint);
  float maxOpticalDepth = mix(
    0.35, 1.20, saturate(densityHint * 0.5));

  // Per-pixel stratified jitter applied ONLY to the discontinuous shadow
  // visibility probes below (never the smooth medium). This is the jitter the
  // longitudinal comment reserves for CSM visibility: it dithers the quantized
  // occlusion boundary across neighboring pixels so a readability weight >1 no
  // longer resolves the low probe count as ground stripes. Screen-space IGN is
  // stable per frame, so medium energy does not swim during camera motion.
  float shadowJitter = interleavedGradientNoise(gl_FragCoord.xy);

  for (int i = 0; i < 16; i++) {
    if (i >= sampleCount)
      break;

    float segmentStart = marchStart + float(i) * ds;
    float segmentEnd = min(segmentStart + ds, intervalEnd);
    float segmentMid = 0.5 * (segmentStart + segmentEnd);

    // 高度介质、Beer-Lambert 消光和点光能量都是平滑被积函数，固定用分段
    // 中点求积；随机抖动只留给下方不连续的 CSM visibility。旧路径让 IGN
    // 同时移动介质采样点，会把屏幕噪声写进总 optical depth，镜头移动时表现为
    // 整体雾能量游动，而不是单纯的阴影边缘去条带。
    float heightCoord = cameraHeight + rayHeightSlope * segmentMid;
    // Exponential height medium. Below the base plane density is allowed to
    // increase smoothly (bounded below), while high terrain fades instead of
    // being multiplied by an extrapolating mix().
    float heightExponent = clamp(
      -(heightCoord - heightFogBase) * heightFogFalloff, -6.0, 1.25);
    float heightTerm = exp(heightExponent);
    // 0..1 controls the height profile; 1..2 increases its physical density.
    float heightProfile = mix(1.0, heightTerm, min(heightFogStrength, 1.0));
    float heightDensityGain = mix(
      1.0, 1.75, saturate(heightFogStrength - 1.0));
    float localDensityMul = heightProfile * heightDensityGain;
    localDensityMul = clamp(localDensityMul, 0.12, 2.8);

    float sigmaTGlobal = sigmaT * localDensityMul * mediumPresence;
    float fullStepOptical = max(sigmaTGlobal * ds, 0.0);
    float mediumSpanStart = sigmaTGlobal > 1.0e-9
      ? segmentStart : intervalEnd;
    float mediumSpanEnd = sigmaTGlobal > 1.0e-9
      ? segmentEnd : intervalStart;
    vec2 fogSegmentIntervals[8];
    float fogSegmentSigmaT[8];
    for (uint vi = 0u; vi < 8u; ++vi) {
      fogSegmentIntervals[vi] = vec2(1.0, 0.0);
      fogSegmentSigmaT[vi] = 0.0;
      if (vi >= fogVolumeCount || fogSigmaT[vi] <= 0.0)
        continue;
      float overlapStart = max(segmentStart, fogRayIntervals[vi].x);
      float overlapEnd = min(segmentEnd, fogRayIntervals[vi].y);
      if (overlapEnd <= overlapStart + 1.0e-6)
        continue;
      float volumeMidpoint = 0.5 * (overlapStart + overlapEnd);
      vec3 volumeLocalPos = fogWorldToLocalPoint(
        fogVolumes.u_volumes[vi],
        cameraPos + rayDir * volumeMidpoint);
      float volumeSigmaT = fogSigmaT[vi] * fogVolumeWeight(
        fogVolumes.u_volumes[vi], volumeLocalPos);
      float localOptical = volumeSigmaT * (overlapEnd - overlapStart);
      if (localOptical <= 0.0)
        continue;
      fullStepOptical += localOptical;
      fogSegmentIntervals[vi] = vec2(overlapStart, overlapEnd);
      fogSegmentSigmaT[vi] = volumeSigmaT;
      mediumSpanStart = min(mediumSpanStart, overlapStart);
      mediumSpanEnd = max(mediumSpanEnd, overlapEnd);
    }
    float remainingOptical = max(maxOpticalDepth - opticalDepth, 0.0);
    float stepOptical = min(fullStepOptical, remainingOptical);
    if (!finiteFloat(stepOptical)) {
      emitNoEffect();
      return;
    }
    // A clear-air ray can encounter its first local volume in a later segment.
    // Zero medium here therefore skips this segment instead of terminating the
    // complete march as the old globally homogeneous model could safely do.
    if (stepOptical <= 1e-7)
      continue;
    float stepTransmittance = exp(-min(stepOptical, 80.0));
    float stepScatter =
      (1.0 - stepTransmittance) * singleScatteringAlbedo;
    // maxOpticalDepth may truncate the last segment. Point-light overlap must
    // not emit from the portion that the shared medium integrator discarded.
    float effectiveStepFraction = clamp(
      stepOptical / max(fullStepOptical, 1e-20), 0.0, 1.0);
    float effectiveSegmentEnd = mix(
      mediumSpanStart, mediumSpanEnd, effectiveStepFraction);

    float segmentSunReferenceOcclusion = 0.0;
    float segmentSunPhysicalOcclusion = 0.0;
    if (sampleDirectionalCsm) {
      // Longitudinal probes (≤8). Cascade-seam may double-fetch (≤16 raw).
      // 段内用平均而非 max：旧 max 会把任一 probe 命中扩成整段二值遮挡，
      // 沿视线叠成多层剪影切片（火凤凰“很多个模型”）。平均更接近区间积分。
      float visibilityStart = max(segmentStart, mediumSpanStart);
      float visibilityEnd = min(effectiveSegmentEnd, mediumSpanEnd);
      float acceptedLength = max(visibilityEnd - visibilityStart, 0.0);
      int probeCount = clamp(
        int(ceil(acceptedLength / directionalProbeSpacing)), 2, 8);
      float refOcclusionSum = 0.0;
      float physicalOcclusionSum = 0.0;
      int acceptedProbes = 0;
      for (int probeIndex = 0; probeIndex < 8; ++probeIndex) {
        if (probeIndex >= probeCount)
          break;
        float probeT = (float(probeIndex) + shadowJitter) / float(probeCount);
        float visibilityDistance = mix(
          visibilityStart, visibilityEnd, probeT);
        float visibilityViewDepth =
          cameraViewDepth + rayViewDepthSlope * visibilityDistance;
        float probeCoverage = 0.0;
        float probeVisibility = 1.0;
        samplePrimaryCsm(
          visibilityDistance, visibilityViewDepth,
          csmClipOrigin, csmClipSlope, csmShadowExtent,
          flipUvX, flipUvY, flipViewDepth,
          probeCoverage, probeVisibility);
        // Keep two complete decisions for the same probe. Reference contains
        // only the authored out-of-coverage fallback; physical additionally
        // contains a real CSM blocker. Their difference is therefore true
        // shadow contrast and cannot be fabricated by a missing CSM region.
        float probeReferenceOcclusion =
          (1.0 - probeCoverage) * (1.0 - fallbackWeight);
        float probePhysicalOcclusion = probeReferenceOcclusion +
          probeCoverage * (1.0 - probeVisibility);
        refOcclusionSum += probeReferenceOcclusion;
        physicalOcclusionSum += probePhysicalOcclusion;
        acceptedProbes += 1;
      }
      float invProbeCount = 1.0 / float(max(acceptedProbes, 1));
      segmentSunReferenceOcclusion = refOcclusionSum * invProbeCount;
      segmentSunPhysicalOcclusion = physicalOcclusionSum * invProbeCount;
    }

    if (hasSunVolume) {
      // CSM validity is local to directional occlusion. It must never gate point
      // scattering. Outside coverage, retain only a bounded unshadowed medium
      // term; when CSM is entirely optional this becomes the explicit fallback.
      // Accumulate an unshadowed reference and the physical CSM result
      // separately. Applying `weight` inside one binary segment saturated as
      // soon as that segment reached zero, so a top-down ray crossing only one
      // of sixteen segments could not gain visible column contrast. The final
      // whole-ray ratio below gives weight its documented 0/1/>1 semantics.
      float sunReferenceVisibility;
      float sunPhysicalVisibility;
      if (csm.u_params.z <= 0.5) {
        sunReferenceVisibility = fallbackWeight;
        sunPhysicalVisibility = fallbackWeight;
      } else if (shadowStrength > 1e-6) {
        sunReferenceVisibility = 1.0 - clamp(
          shadowStrength * segmentSunReferenceOcclusion,
          0.0, 1.0);
        sunPhysicalVisibility = 1.0 - clamp(
          shadowStrength * segmentSunPhysicalOcclusion,
          0.0, 1.0);
      } else {
        sunReferenceVisibility = 1.0;
        sunPhysicalVisibility = 1.0;
      }
      float sunStep = transmittance * phaseLdr * stepScatter;
      float sunReferenceStep = sunStep * sunReferenceVisibility;
      float sunPhysicalStep = sunStep * sunPhysicalVisibility;
      accumSunReference += sunReferenceStep;
      accumLit += sunPhysicalStep;
      // Difference from the same probe's reference is true CSM blocker
      // evidence. Normalize locally so a short shadow interval is not diluted
      // by bright medium elsewhere, then retain optical support as a separate
      // gate so one noisy/vanishingly thin probe cannot create a dark decal.
      float blockerVisibilityLoss = max(
        sunReferenceVisibility - sunPhysicalVisibility, 0.0);
      float segmentLocalOcclusion = clamp(
        blockerVisibilityLoss / max(sunReferenceVisibility, 1.0e-4),
        0.0, 1.0);
      peakSunLocalOcclusion = max(
        peakSunLocalOcclusion, segmentLocalOcclusion);
      sunShadowEvidenceOptical += stepOptical * blockerVisibilityLoss;
    }

    if (hasPointVolume) {
      for (uint li = 0u; li < pointLightCount; li++) {
        vec2 rayInterval = pointRayIntervals[li];
        // A clear-air segment may contain only a thin local-medium chord.
        // Constrain the smooth light sample to the actual medium span as well
        // as the point-light sphere; otherwise the energy length is correct
        // but its inverse-range and phase sample can land in empty air.
        float overlapStart = max(
          max(segmentStart, mediumSpanStart), rayInterval.x);
        float overlapEnd = min(
          min(effectiveSegmentEnd, mediumSpanEnd), rayInterval.y);
        if (overlapEnd <= overlapStart + 1e-6)
          continue;

        vec4 lightPosRange = lights.u_lights[li].pos;
        vec4 lightColorIntensity = lights.u_lights[li].color;
        if (!finiteVec4(lightColorIntensity))
          continue;

        float range = max(lightPosRange.w, 1.0);
        // Evaluate the smoothly varying source at the covered sub-segment's
        // midpoint. Support itself is integrated by overlap length, so a thin
        // or grazing sphere chord can no longer disappear between jittered
        // march midpoints.
        float pointDistance = 0.5 * (overlapStart + overlapEnd);
        vec3 pointSamplePos = cameraPos + rayDir * pointDistance;
        vec3 toLight = lightPosRange.xyz - pointSamplePos;
        float lightDist = length(toLight);
        if (!finiteFloat(lightDist) || lightDist >= range)
          continue;

        vec3 lightDir = toLight / max(lightDist, 1e-3);
        float rangeN = saturate(lightDist / range);
        // Use the same range-normalized authoring contract as the surface
        // receiver. War3 world units are too large for a hidden 0.1R emitter
        // core plus raw inverse-square: at ordinary 0.5R..0.8R distances that
        // made point-volume energy 5x..7x weaker than the visible point light.
        // The squared window and rational falloff remain finite at the source
        // and reach zero with a soft derivative at the authored range.
        float rangeWindow = max(1.0 - rangeN * rangeN, 0.0);
        rangeWindow *= rangeWindow;
        float rangeFalloff = 1.0 / (1.0 + rangeN * rangeN * 6.0);
        float atten = rangeWindow * rangeFalloff;

        // lightDir 指向光源，实际入射传播方向为 -lightDir。
        float pointMu = dot(-lightDir, -rayDir);
        float pointPhase = mix(phaseIso, hgPhase(pointMu, 0.25), 0.45);
        float pointPhaseLdr = max(pointPhase / phaseIso, 0.45);
        vec3 pointColor =
          max(lightColorIntensity.rgb, vec3(0.0)) *
          max(lightColorIntensity.w, 0.0);
        float pointShadowVisibility = 1.0;
        if (contrastWeight > 1e-6) {
          // Keep radiometric energy at the smooth midpoint, but test shadow
          // visibility at two stratified positions. A unit-sized point-shadow
          // cone can be thinner than one march segment and was previously
          // missed whenever the sole midpoint landed outside it. With the
          // product cap of two point lights this is at most four cube fetches
          // per segment.
          float rawPointShadowVisibility = 1.0;
          for (int pointProbe = 0; pointProbe < 2; ++pointProbe) {
            float pointProbeT = (float(pointProbe) + shadowJitter) * 0.5;
            float shadowDistance = mix(
              overlapStart, overlapEnd, pointProbeT);
            vec3 shadowSamplePos =
              cameraPos + rayDir * shadowDistance;
            rawPointShadowVisibility = min(
              rawPointShadowVisibility,
              samplePointVolumeShadow(
                lights.u_lights[li], shadowSamplePos));
          }
          pointShadowVisibility = 1.0 - clamp(
            (1.0 - rawPointShadowVisibility) * contrastWeight, 0.0, 1.0);
        }

        // The medium is piecewise constant over this march segment. Integrate
        // only the sphere-covered sub-segment with the same Beer-Lambert law:
        // first attenuate from segmentStart to overlapStart, then scatter over
        // the overlap length. For a full overlap these reduce exactly to the
        // shared transmittance and stepScatter used by the sun path.
        float preOverlapOptical = max(
          sigmaTGlobal * (overlapStart - segmentStart), 0.0);
        float overlapOptical = max(
          sigmaTGlobal * (overlapEnd - overlapStart), 0.0);
        for (uint vi = 0u; vi < 8u; ++vi) {
          if (vi >= fogVolumeCount || fogSegmentSigmaT[vi] <= 0.0)
            continue;
          vec2 mediumInterval = fogSegmentIntervals[vi];
          float preStart = max(segmentStart, mediumInterval.x);
          float preEnd = min(overlapStart, mediumInterval.y);
          if (preEnd > preStart)
            preOverlapOptical += fogSegmentSigmaT[vi] * (preEnd - preStart);
          float litStart = max(overlapStart, mediumInterval.x);
          float litEnd = min(overlapEnd, mediumInterval.y);
          if (litEnd > litStart)
            overlapOptical += fogSegmentSigmaT[vi] * (litEnd - litStart);
        }
        preOverlapOptical = min(preOverlapOptical, stepOptical);
        overlapOptical = min(
          overlapOptical, max(stepOptical - preOverlapOptical, 0.0));
        float overlapTransmittance = transmittance *
          exp(-min(preOverlapOptical, 80.0));
        float overlapScatter =
          (1.0 - exp(-min(overlapOptical, 80.0))) *
          singleScatteringAlbedo;
        accumPoint += overlapTransmittance * pointColor * atten *
                      pointPhaseLdr * overlapScatter * pointShadowVisibility;
      }
    }

    transmittance *= stepTransmittance;
    opticalDepth += stepOptical;
    if (opticalDepth >= maxOpticalDepth - 1e-5)
      break;
  }

  // Whole-ray physical contrast plus optically supported local evidence. A
  // short blocker on a long low-pitch ray no longer vanishes through the
  // L_lit/L_ref denominator, while evidence gating prevents an unsupported
  // peak probe from becoming a detached black column. weight=0 still disables
  // all authored blocker contrast exactly.
  float resolvedAccumLit = accumLit;
  float resolvedColumnOcclusion = 0.0;
  if (hasSunVolume && accumSunReference > 1e-7) {
    float physicalRatio = clamp(
      accumLit / max(accumSunReference, 1e-7), 0.0, 1.0);
    float pathOcclusion = 1.0 - physicalRatio;
    float peakOcclusion = clamp(peakSunLocalOcclusion, 0.0, 1.0);
    bool volumeSun = csm.u_volumeSunParams.x > 0.5;
    float shadowEvidenceGate = smoothstep(
      0.00035, 0.0060, sunShadowEvidenceOptical);
    float localEvidenceScale = volumeSun ? 0.74 : 0.68;
    float physicalOcclusion = max(
      pathOcclusion,
      peakOcclusion * shadowEvidenceGate * localEvidenceScale);
    float contrastControl = clamp(contrastWeight, 0.0, 2.5);
    float resolvedOcclusion;
    if (contrastControl <= 1.0) {
      resolvedOcclusion = physicalOcclusion * contrastControl;
    } else {
      // A fixed target exponent makes the same blocker evidence resolve to the
      // same contrast at low and top-down camera pitches.
      float targetExponent = volumeSun ? 0.50 : 0.55;
      float readabilityMix = saturate((contrastControl - 1.0) / 1.5);
      float readabilityExponent = mix(
        1.0, targetExponent, readabilityMix);
      resolvedOcclusion = pow(
        max(physicalOcclusion, 0.0), max(readabilityExponent, 0.28));
    }
    resolvedColumnOcclusion = clamp(resolvedOcclusion, 0.0, 1.0);
    float resolvedRatio = 1.0 - resolvedColumnOcclusion;
    resolvedAccumLit = accumSunReference * resolvedRatio;
  }

  float pointContributionPeak = max(max(accumPoint.r, accumPoint.g), accumPoint.b);
  const bool hasSunContribution =
    hasSunVolume && resolvedAccumLit > 1e-7;
  const bool hasPointContribution = pointContributionPeak > 1e-7;
  const bool hasColumnContribution = resolvedColumnOcclusion > 1e-7;
  if (!hasSunContribution && !hasPointContribution &&
      !hasColumnContribution) {
    emitNoEffect();
    return;
  }

  float skyBoost = skyPixel ? 1.10 : 1.0;
  // 上面的 phase 归一化建立了显式 LDR 单位；太阳和点光统一使用
  // color * sourceIntensity。唯一的共享 soft clip 负责饱和，因此 HDR 太阳颜色
  // 保持单调响应，同时不能绕过 LDR 防冲白护栏或丢失光源色相。
  vec3 sunShafts = resolvedAccumLit * sunRadiance * intensity *
                   skyBoost;
  vec3 pointShafts = accumPoint * intensity;
  vec3 scattering = softClipRgb(sunShafts + pointShafts, 0.92);
  float rawTransmittance = clamp(transmittance, exp(-0.75), 1.0);
  float extinctionStrength = clamp(pc.p_viewportZ.z, 0.0, 1.0);
  // Scale optical depth, not transmittance itself: T_strength = exp(-s*tau)
  // = pow(T, s). A linear mix of 1 and T is not Beer-Lambert and becomes
  // increasingly energy-inconsistent as density is raised.
  float sceneTransmittance = pow(rawTransmittance, extinctionStrength);

  // Missing in-scattering alone cannot make a short shaft readable
  // when the authored fog energy is intentionally low. Reuse alpha as the
  // existing base-transmittance channel and apply a small, bounded readability
  // term driven only by the resolved *true* CSM occlusion above. This adds no
  // shadow where coverage is missing, does not require brighter global fog,
  // and caps a fully blocked ray at 18% additional base attenuation.
  // The ratio itself is independent of medium/source energy. Gate it with the
  // actually integrated optical depth and the authored sun-volume energy so a
  // nearly disabled effect cannot leave a detached dark decal behind.
  // Gate base attenuation with blocker-supported optical depth, not the total
  // fog depth. This is view-neutral and cannot darken a ray that only contains
  // unshadowed medium.
  float columnEvidenceGate = smoothstep(
    0.00035, 0.0060, sunShadowEvidenceOptical);
  float columnSourceGate = smoothstep(
    0.004, 0.040, sunRadiancePeak * intensity);
  float columnReadabilityMix =
    saturate((contrastWeight - 1.0) / 1.5);
  // Fixed 24% cap: no camera-pitch branch may make the same column darker.
  float attenScale = 0.62;
  if (csm.u_volumeSunParams.x > 0.5)
    attenScale *= 1.05;
  float maxColumnAtten = 0.24;
  float columnReadabilityAttenuation = min(
    resolvedColumnOcclusion * 0.55 * attenScale * columnEvidenceGate *
      columnSourceGate * columnReadabilityMix,
    maxColumnAtten);
  sceneTransmittance = clamp(
    sceneTransmittance * (1.0 - columnReadabilityAttenuation),
    exp(-2.0), 1.0);

  if (!finiteVec3(scattering) || !finiteFloat(sceneTransmittance)) {
    emitNoEffect();
    return;
  }

  // rgb is premultiplied single scattering. Alpha remains the sole resolved
  // base-transmittance channel: Beer-Lambert extinction plus the bounded,
  // true-CSM-only column readability attenuation above.
  o_color = vec4(scattering, sceneTransmittance);
}
