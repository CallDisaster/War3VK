#version 460

#extension GL_EXT_nonuniform_qualifier : require
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
layout(set = 1, binding = 10, r8) uniform image2D s_shadowHistoryWrite;

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
  vec4 u_viewportZ; // x=minZ, y=maxZ, z/w unused
  mat4 u_prevViewProj;
  vec4 u_taaParams; // x=taaEnabled, y=blendFactor, z=neighborClamp, w=hasHistory/hasPrev
} ubo;

struct PointLight {
    vec4 pos;   // xyz, w=range
    vec4 color; // rgb, w=intensity
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
    uint u_pad0;
    uint u_pad1;
    uint u_pad2;
    PointShadowLightData u_lights[POINT_SHADOW_MAX_LIGHTS];
} pointShadow;

layout(location = 0) in  vec2 i_pos;
layout(location = 0) out vec4 o_color;

layout(push_constant, scalar)
uniform push_block {
  uint p_colorSampler;
  uint p_shadowSampler;
};

float shadowMapDepth(uint cascadeIndex, vec2 uv) {
  return texture(
    sampler2DArray(s_shadow, s_samplers[nonuniformEXT(p_shadowSampler)]),
    vec3(uv, float(cascadeIndex))).r;
}

float shadowCompare(uint cascadeIndex, vec2 uv, float refDepth) {
  float d = shadowMapDepth(cascadeIndex, uv);
  return (refDepth <= d) ? 1.0 : 0.0;
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

float sampleShadowStableWall(uint cascadeIndex, vec2 uv, float refDepth, float radiusTexel) {
  float invRes = max(ubo.u_params.z, 1e-6);
  vec2 snappedUv = (floor(uv / invRes) + 0.5) * invRes;
  float stableRadius = max(radiusTexel, 1.75);
  return sampleShadowGrid(cascadeIndex, snappedUv, refDepth, stableRadius, 2);
}

float computeShadowVisibility(vec3 worldPos, float viewDepth, float biasExtra, vec2 rot, bool stableWallPath) {
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
  float bias0 = baseBias * computeCascadeBiasScale(c0, cascadeCount, cascadeBiasScale);
  // 关键修复：
  // 之前 refDepth<0 直接返回全亮，会在高视角/远级联下把接触阴影“截掉一块”。
  // 这里改为 clamp，避免底部阴影突然消失。
  float ref0 = clamp(n0.z - bias0, 0.0, 1.0);
  float radius0 = max(ubo.u_params.y, 0.0);
  if (pcssEnabled) {
    float sum = 0.0;
    float cnt = 0.0;
    float invRes = ubo.u_params.z;
    for (int y = -searchRadius; y <= searchRadius; y++) {
      for (int x = -searchRadius; x <= searchRadius; x++) {
        vec2 o = vec2(float(x), float(y)) * pcssSearchRadius * invRes;
        float d = shadowMapDepth(uint(c0), uv0 + o);
        if (d < ref0) {
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

  if (stableWallPath)
    return sampleShadowStableWall(uint(c0), uv0, ref0, radius0);

  float vis0 = sampleShadowPcf(uint(c0), uv0, ref0, radius0, rot);

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
    float bias1 = baseBias * computeCascadeBiasScale(c1, cascadeCount, cascadeBiasScale);
    float ref1 = clamp(n1.z - bias1, 0.0, 1.0);
    float radius1 = max(ubo.u_params.y, 0.0);
    if (pcssEnabled) {
      float sum = 0.0;
      float cnt = 0.0;
      float invRes = ubo.u_params.z;
      for (int y = -searchRadius; y <= searchRadius; y++) {
        for (int x = -searchRadius; x <= searchRadius; x++) {
          vec2 o = vec2(float(x), float(y)) * pcssSearchRadius * invRes;
          float d = shadowMapDepth(uint(c1), uv1 + o);
          if (d < ref1) {
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
    float vis1 = sampleShadowPcf(uint(c1), uv1, ref1, radius1, rot);

    return mix(vis0, vis1, w);
  }

  return vis0;
}

float samplePointShadowPcf(uint lightIndex, vec3 dir, float currentDist,
                           float shadowRange, float biasBase) {
  vec3 dirN = normalize(dir);
  vec3 refUp = (abs(dirN.y) < 0.99) ? vec3(0.0, 1.0, 0.0)
                                     : vec3(1.0, 0.0, 0.0);
  vec3 tangent = normalize(cross(refUp, dirN));
  vec3 bitangent = normalize(cross(dirN, tangent));

  float distRatio = clamp(currentDist / max(shadowRange, 1e-4), 0.0, 1.0);
  float pcfRadius = mix(0.0025, 0.02, distRatio);

  const vec2 taps[8] = vec2[](
    vec2( 0.0,  0.0),
    vec2( 1.0,  0.0),
    vec2(-1.0,  0.0),
    vec2( 0.0,  1.0),
    vec2( 0.0, -1.0),
    vec2( 0.7071,  0.7071),
    vec2(-0.7071,  0.7071),
    vec2( 0.7071, -0.7071)
  );

  float nearZ = 1.0;
  float farZ = max(shadowRange, nearZ + 1.0);
  float invZRange = 1.0 / max(farZ - nearZ, 1e-4);
  // 在深度域比较，偏置保持小量，避免点光阴影长期“全可见”。
  float biasDepth = clamp(max(biasBase, 0.0) * 0.0005 + 0.00015, 0.00015, 0.003);
  // 对 Cube 深度图，receiver 深度应基于原始方向计算（PCF tap 仅偏移采样方向）。
  float maxComp = max(max(abs(dirN.x), abs(dirN.y)), abs(dirN.z));
  float currentViewZ = max(currentDist * maxComp, nearZ + 1e-4);
  float currentDepth = farZ * invZRange - (farZ * nearZ) * invZRange / currentViewZ;
  currentDepth = clamp(currentDepth, 0.0, 1.0);
  float visible = 0.0;

  for (int i = 0; i < 8; ++i) {
    vec3 sampleDir = normalize(dirN + (tangent * taps[i].x + bitangent * taps[i].y) * pcfRadius);
    float storedDepth = texture(
      samplerCubeArray(s_pointShadow, s_samplers[nonuniformEXT(p_shadowSampler)]),
      vec4(sampleDir, float(lightIndex))).r;

    visible += (currentDepth > storedDepth + biasDepth) ? 0.0 : 1.0;
  }

  return visible * (1.0 / 8.0);
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
  if (abs(worldH.w) < 1e-6) {
    o_color = col;
    return;
  }
  vec3 worldPos = worldH.xyz / worldH.w;

  vec4 viewH = vec4(worldPos, 1.0) * ubo.u_view;
  // 兼容 RH/LH：部分投影会让“前方深度”为 -Z，这里取绝对值用于级联选择与过渡。
  float viewDepth = abs(viewH.z);

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

  bool taaEnabled = (ubo.u_taaParams.x > 0.5);
  float receiverMode = ubo.u_params5.w;
  float normalBiasScale = max(ubo.u_params5.x, 0.0);
  float rimIntensity = max(ubo.u_params5.y, 0.0);
  float rimPower = max(ubo.u_params5.z, 0.1);
  float biasExtra = 0.0;
  vec3 viewPos = viewH.xyz;
  bool pointLightsEnabled = ubo.u_params2.w > 0.5 && lights.u_count > 0u;
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
  bool stableWallPath =
      wallFactor > 0.28 &&
      (wallStabilityFactor > 0.08 || lightGrazingFactor > 0.08 ||
       viewGrazingFactor > 0.20);
  if (!taaEnabled && receiverMode > 0.5 && normalBiasScale > 0.0) {
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
  if (!taaEnabled) {
    float rotateMode = ubo.u_params6.y;
    if (rotateMode > 0.5 && !stableWallPath) {
      float seed = 0.0;
      bool useScreenRotate =
          rotateMode < 1.5 ||
          viewDepth > ubo.u_splitFar.y ||
          stableWallPath;
      if (useScreenRotate) {
        seed = fract(dot(vec2(pix), vec2(0.06711056, 0.00583715)));
      } else {
        seed = fract(dot(worldPos.xy, vec2(0.03125, 0.015625)));
      }
      float angle = seed * 6.28318531;
      pcfRot = vec2(cos(angle), sin(angle));
    }
  }

  float vis = 1.0;
  if (debugMode == 2 || strength > 1e-4) {
    float currVis = 1.0;

    if (taaEnabled) {
      // TAA 开启时，当前帧可见性统一由预计算 pass 输出。
      // 这样墙面稳定路径也不会在 receiver 中重复跑一遍 PCF。
      currVis = texelFetch(
        sampler2D(s_shadowCurrent, s_samplers[nonuniformEXT(p_colorSampler)]),
        pix,
        0).r;
    } else {
      currVis = computeShadowVisibility(worldPos, viewDepth, biasExtra, pcfRot, stableWallPath);
    }

    vis = currVis;

    // Shadow TAA：对 vis 做重投影与时域混合（主要用于 Alpha-Test 阴影稳定）
    if (taaEnabled && !stableWallPath) {
      float blend = clamp(ubo.u_taaParams.y, 0.0, 1.0);
      bool clampEnabled = (ubo.u_taaParams.z > 0.5);
      // x：0=关闭，1=启用但无历史，2=启用且有可用历史
      bool hasHistory = (ubo.u_taaParams.x > 1.5);

      if (hasHistory) {
        vec2 uvVp = (vec2(pix) + 0.5 - vpMin) / vpSize;
        vec2 mv = texelFetch(
          sampler2D(s_motionVector, s_samplers[nonuniformEXT(p_colorSampler)]),
          pix,
          0).xy;

        // 运动自适应混合：当运动向量较大时，提高新帧权重以加快收敛
        // 典型相机俯仰变化时 mv 幅度约 0.01~0.05，阈值 0.005 开始增加权重
        float mvLen = length(mv);
        float motionScale = clamp(mvLen * 20.0, 0.0, 1.0); // 0.05 -> 1.0
        float adaptiveBlend = mix(blend, max(blend, 0.3), motionScale);

        // historyUv = uv - mv（mv = curr - prev）
        vec2 historyUv = uvVp - mv;
        vec2 histPixF = vpMin + historyUv * vpSize;
        ivec2 histPix = ivec2(histPixF);

        bool validHistory =
            (histPixF.x >= vpMin.x) && (histPixF.y >= vpMin.y) &&
            (histPixF.x < (vpMin.x + vpSize.x)) &&
            (histPixF.y < (vpMin.y + vpSize.y));

        float histVis = currVis;
        if (validHistory) {
          histVis = texelFetch(
            sampler2D(s_shadowHistory, s_samplers[nonuniformEXT(p_colorSampler)]),
            histPix,
            0).r;

          // 邻域夹紧：使用十字形采样（5 点）代替 3x3（9 点）以节省性能
          if (clampEnabled) {
            float minN = currVis;
            float maxN = currVis;

            // 十字形：中心 + 上下左右
            ivec2 offsets[4] = ivec2[4](
              ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1)
            );
            for (int i = 0; i < 4; i++) {
              ivec2 np = pix + offsets[i];
              if (float(np.x) >= vpMin.x && float(np.y) >= vpMin.y &&
                  float(np.x) < (vpMin.x + vpSize.x) &&
                  float(np.y) < (vpMin.y + vpSize.y)) {
                float n = texelFetch(
                  sampler2D(s_shadowCurrent, s_samplers[nonuniformEXT(p_colorSampler)]),
                  np,
                  0).r;
                minN = min(minN, n);
                maxN = max(maxN, n);
              }
            }
            histVis = clamp(histVis, minN, maxN);
          }

          vis = mix(histVis, currVis, adaptiveBlend);
        } else {
          vis = currVis;
        }
      } else {
        // 首帧：直接用当前值初始化
        vis = currVis;
      }

      // 写回历史（存最终值，以便持续累积稳定）
      imageStore(s_shadowHistoryWrite, pix, vec4(vis, 0.0, 0.0, 0.0));
    } else if (taaEnabled) {
      // 墙面/掠视角 receiver 直接使用当前帧 PCF，避免 history 在高梯度面上
      // 形成“波浪式流动”。
      imageStore(s_shadowHistoryWrite, pix, vec4(currVis, 0.0, 0.0, 0.0));
    }

    if (debugMode == 2) {
      o_color = vec4(vec3(vis), 1.0);
      return;
    }
  }

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
  // 点光源对方向光阴影的提亮系数（避免阴影“完全独立”）
  float shadowLift = 0.0;
  float pointShadowVisAccum = 0.0;
  float pointShadowWeightAccum = 0.0;

  for (uint i = 0; i < lightCount; i++) {
      vec3 lPosW = lights.u_lights[i].pos.xyz;
      float lRange = lights.u_lights[i].pos.w;
      vec3 lColor = lights.u_lights[i].color.rgb;
      float lIntensity = lights.u_lights[i].color.w;

      // 把光源位置也变到 view-space（统一空间）
      vec3 lPosV = (vec4(lPosW, 1.0) * ubo.u_view).xyz;

      vec3 L = lPosV - viewPos;
      float dist = length(L);
      if (dist < lRange) {
          // 使用更抗分层的衰减函数（inverse-square 风格）
          float r = max(lRange, 1e-3);
          float x = dist / r;
          float atten = max(1.0 - x*x, 0.0);
          atten *= lIntensity;
          
          vec3 Ldir = L / max(dist, 1e-6);
          float ndotl = max(dot(normV, Ldir), 0.0);
          float luma = dot(lColor, vec3(0.299, 0.587, 0.114));
          
          float pointShadowFactor = 1.0;
          if (i < pointShadow.u_lightCount) {
              PointShadowLightData ps = pointShadow.u_lights[i];
              if (ps.enabled > 0.5) {
                  // 从光源指向片元（用于 CubeArray 采样）
                  vec3 lightToFrag = worldPos - ps.lightPos.xyz;
                  float currentDist = length(lightToFrag);
                  float shadowRange = max(ps.lightPos.w, 1.0);
                  float shadowStrength = clamp(ps.shadowIntensity, 0.0, 1.0);

                  if (currentDist < shadowRange * 0.999) {
                      vec3 dir = normalize(lightToFrag);
                      float visPoint = samplePointShadowPcf(i, dir, currentDist, shadowRange, ps.bias);
                      pointShadowFactor = mix(1.0, visPoint, shadowStrength);
                  }
              }
          }
          
          float lightTerm = ndotl * atten * pointShadowFactor;
          pointShadowVisAccum += pointShadowFactor * atten;
          pointShadowWeightAccum += atten;
          float liftSrc = lightTerm * luma;
          float lift = liftSrc / (1.0 + liftSrc);
          shadowLift = max(shadowLift, lift);
          accumLight += lColor * lightTerm;
      }
  }

  // 方向光与点光源阴影联动：点光源阴影平均可见度会轻度影响方向光最终乘子。
  float avgPointShadowVis = 1.0;
  if (pointShadowWeightAccum > 1e-4) {
    avgPointShadowVis = pointShadowVisAccum / pointShadowWeightAccum;
  }
  float coupledSunVis = mix(vis, vis * avgPointShadowVis, 0.35);
  float coupledMul = 1.0 - strength + strength * coupledSunVis;

  // Add lighting (additive)
  float liftAmount = clamp(shadowLift * 0.5, 0.0, 1.0);
  float sunMul = mix(coupledMul, 1.0, liftAmount);
  vec3 finalColor = col.rgb * sunMul + accumLight + rimAdd;

  o_color = vec4(finalColor, col.a);
}
