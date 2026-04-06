#version 460

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2DArray s_color;
layout(set = 1, binding = 1) uniform texture2DArray s_depth;
layout(set = 1, binding = 2) uniform texture2DArray s_shadow;
layout(set = 1, binding = 5) uniform textureCube s_pointShadow;  // [NEW] Point Light Cube Shadow

layout(set = 1, binding = 3, scalar, row_major)
uniform ShadowData {
  mat4 u_view;
  mat4 u_invViewProj;
  mat4 u_lightViewProj[4];
  vec4 u_splitFar;  // view-space z
  vec4 u_params;    // x=shadowStrength, y=pcfRadius, z=invShadowRes, w=cascadeCount
  vec4 u_params2;   // x=receiverBias, y=cascadeBlendRange, z=debugMode, w=unused
  vec4 u_params3;   // x=invColorW, y=invColorH, z=pcssEnable, w=pcssSearchRadius(texel)
  vec4 u_params4;   // x=pcssMinRadius(texel), y=pcssMaxRadius(texel), z=pcssDepthScale, w=unused
  vec4 u_viewport;  // x=vpX, y=vpY, z=vpW, w=vpH
  vec4 u_viewportZ; // x=minZ, y=maxZ, z/w unused
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

// [NEW] Point Shadow Data
layout(set = 1, binding = 6, scalar)
uniform PointShadowBlock {
    vec4 u_pointShadowLightPos;  // xyz=position, w=range (for first shadow-casting light)
    float u_pointShadowBias;
    float u_pointShadowEnabled;
    float u_pad2[2];
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

float sampleShadowPcf3x3(uint cascadeIndex, vec2 uv, float refDepth, float radiusTexel) {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    return 1.0;

  float invRes = ubo.u_params.z;
  float radius = max(radiusTexel, 0.0);

  float sum = 0.0;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      vec2 o = vec2(float(x), float(y)) * radius * invRes;
      vec2 tapUv = uv + o;
      // Avoid address mode artifacts (clamp-to-edge can turn out-of-bounds taps into false shadows)
      if (tapUv.x < 0.0 || tapUv.x > 1.0 || tapUv.y < 0.0 || tapUv.y > 1.0)
        sum += 1.0;
      else
        sum += shadowCompare(cascadeIndex, tapUv, refDepth);
    }
  }
  return sum * (1.0 / 9.0);
}

float computeShadowVisibility(vec3 worldPos, float viewDepth) {
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

  float bias = max(ubo.u_params2.x, 0.0);
  float blendRange = max(ubo.u_params2.y, 0.0);
  bool pcssEnabled = (ubo.u_params3.z > 0.5);
  float pcssSearchRadius = max(ubo.u_params3.w, 0.0);
  float pcssMinRadius = max(ubo.u_params4.x, 0.0);
  float pcssMaxRadius = max(ubo.u_params4.y, pcssMinRadius);
  float pcssDepthScale = max(ubo.u_params4.z, 0.0);

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
  float ref0 = n0.z - bias;
  // If the reference depth is outside [0..1], we're outside the cascade depth range.
  if (ref0 < 0.0 || ref0 > 1.0)
    return 1.0;
  float radius0 = max(ubo.u_params.y, 0.0);
  if (pcssEnabled) {
    float sum = 0.0;
    float cnt = 0.0;
    float invRes = ubo.u_params.z;
    for (int y = -1; y <= 1; y++) {
      for (int x = -1; x <= 1; x++) {
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
  float vis0 = sampleShadowPcf3x3(uint(c0), uv0, ref0, radius0);

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
    float ref1 = n1.z - bias;
    if (ref1 < 0.0 || ref1 > 1.0)
      return vis0;
    float radius1 = max(ubo.u_params.y, 0.0);
    if (pcssEnabled) {
      float sum = 0.0;
      float cnt = 0.0;
      float invRes = ubo.u_params.z;
      for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
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
    float vis1 = sampleShadowPcf3x3(uint(c1), uv1, ref1, radius1);

    return mix(vis0, vis1, w);
  }

  return vis0;
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
  int layer = int(gl_Layer);

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

  float vis = 1.0;
  if (debugMode == 2 || strength > 1e-4) {
    vis = computeShadowVisibility(worldPos, viewDepth);
    if (debugMode == 2) {
      o_color = vec4(vec3(vis), 1.0);
      return;
    }
  }

  float mul = 1.0 - strength + strength * vis;
  
  // Apply Shadows
  vec3 finalColor = col.rgb * mul;

  if (lights.u_count == 0u) {
    o_color = vec4(finalColor, col.a);
    return;
  }

  // [NEW] Point Lights (Neighbor-Sampling Normal Reconstruction)
  // 修复条纹伪影：用邻域深度采样 + 选同面邻居构造法线，避免 dFdx/dFdy 跨深度台阶退化
  
  // Center pixel view position
  float dC = depthN;
  vec3 viewPos = (vec4(worldPos, 1.0) * ubo.u_view).xyz;
  float depthDenom = max(maxZ - minZ, 1e-6);
  
  // Fetch 4 neighbors' depth (inline)
  float nzR = texelFetch(sampler2DArray(s_depth, s_samplers[nonuniformEXT(p_colorSampler)]), ivec3(pix.x + 1, pix.y, layer), 0).r;
  float dR = clamp((nzR - minZ) / depthDenom, 0.0, 1.0);
  
  float nzL = texelFetch(sampler2DArray(s_depth, s_samplers[nonuniformEXT(p_colorSampler)]), ivec3(pix.x - 1, pix.y, layer), 0).r;
  float dL = clamp((nzL - minZ) / depthDenom, 0.0, 1.0);
  
  float nzD = texelFetch(sampler2DArray(s_depth, s_samplers[nonuniformEXT(p_colorSampler)]), ivec3(pix.x, pix.y + 1, layer), 0).r;
  float dD = clamp((nzD - minZ) / depthDenom, 0.0, 1.0);
  
  float nzU = texelFetch(sampler2DArray(s_depth, s_samplers[nonuniformEXT(p_colorSampler)]), ivec3(pix.x, pix.y - 1, layer), 0).r;
  float dU = clamp((nzU - minZ) / depthDenom, 0.0, 1.0);
  
  // Reconstruct neighbor view positions (inline)
  // Reconstruct neighbor view positions (用与中心点一致的路径，避免 mat3 row_major 布局问题)
  vec2 uvR = (vec2(float(pix.x + 1), float(pix.y)) - vpMin) / vpSize;
  vec4 clipR = vec4(uvR.x * 2.0 - 1.0, 1.0 - uvR.y * 2.0, dR, 1.0);
  vec4 wHR = clipR * ubo.u_invViewProj;
  vec3 worldR = wHR.xyz / max(wHR.w, 1e-6);
  vec3 Pr = (vec4(worldR, 1.0) * ubo.u_view).xyz;
  
  vec2 uvL = (vec2(float(pix.x - 1), float(pix.y)) - vpMin) / vpSize;
  vec4 clipL = vec4(uvL.x * 2.0 - 1.0, 1.0 - uvL.y * 2.0, dL, 1.0);
  vec4 wHL = clipL * ubo.u_invViewProj;
  vec3 worldL = wHL.xyz / max(wHL.w, 1e-6);
  vec3 Pl = (vec4(worldL, 1.0) * ubo.u_view).xyz;
  
  vec2 uvD = (vec2(float(pix.x), float(pix.y + 1)) - vpMin) / vpSize;
  vec4 clipD = vec4(uvD.x * 2.0 - 1.0, 1.0 - uvD.y * 2.0, dD, 1.0);
  vec4 wHD = clipD * ubo.u_invViewProj;
  vec3 worldD = wHD.xyz / max(wHD.w, 1e-6);
  vec3 Pd = (vec4(worldD, 1.0) * ubo.u_view).xyz;
  
  vec2 uvU = (vec2(float(pix.x), float(pix.y - 1)) - vpMin) / vpSize;
  vec4 clipU = vec4(uvU.x * 2.0 - 1.0, 1.0 - uvU.y * 2.0, dU, 1.0);
  vec4 wHU = clipU * ubo.u_invViewProj;
  vec3 worldU = wHU.xyz / max(wHU.w, 1e-6);
  vec3 Pu = (vec4(worldU, 1.0) * ubo.u_view).xyz;
  
  // Pick "more coplanar" side with tie-break (防止深度量化导致选择翻来翻去)
  float dzR = abs(dR - dC);
  float dzL = abs(dC - dL);
  float dzD = abs(dD - dC);
  float dzU = abs(dC - dU);
  bool useR = (dzR + 1e-5) < dzL;
  bool useD = (dzD + 1e-5) < dzU;
  vec3 dX = useR ? (Pr - viewPos) : (viewPos - Pl);
  vec3 dY = useD ? (Pd - viewPos) : (viewPos - Pu);
  
  vec3 normV_raw = cross(dY, dX);
  float n2 = dot(normV_raw, normV_raw);
  
  // Fallback：导数退化时（n2 接近 0），用默认向上法线防止 ndotl 变成整行 0
  vec3 normV = (n2 > 1e-12) ? normalize(normV_raw) : vec3(0.0, 0.0, 1.0);
  
  // Face camera in view-space
  vec3 viewDirV = normalize(-viewPos);
  if (dot(normV, viewDirV) < 0.0)
      normV = -normV;

  vec3 accumLight = vec3(0.0);

  for (uint i = 0; i < lights.u_count; i++) {
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
          
          // [NEW] Point Light Shadow - 采样 Cube Shadow Map
          float pointShadowFactor = 1.0;
          if (i == 0 && pointShadow.u_pointShadowEnabled > 0.5) {
              // 从光源指向 fragment（用于 Cube 采样）
              vec3 lightToFrag = worldPos - pointShadow.u_pointShadowLightPos.xyz;
              float currentDist = length(lightToFrag);
              float shadowRange = pointShadow.u_pointShadowLightPos.w;
              
              // 只在范围内采样
              if (currentDist < shadowRange * 0.98) {
                  // 采样 Cube Shadow Map
                  float storedDepth = texture(
                      samplerCube(s_pointShadow, s_samplers[nonuniformEXT(p_shadowSampler)]),
                      lightToFrag).r;
                  
                  // NDC → 线性深度转换
                  float nearZ = 1.0;
                  float farZ = shadowRange;
                  float closestDist = nearZ * farZ / (farZ - storedDepth * (farZ - nearZ));
                  
                  // 比较距离判断遮挡（固定 bias = 5 单位）
                  float bias = 5.0;
                  pointShadowFactor = (currentDist - bias > closestDist) ? 0.0 : 1.0;
              }
          }
          
          accumLight += lColor * ndotl * atten * pointShadowFactor;
      }
  }

  // Add lighting (additive)
  finalColor += accumLight;

  o_color = vec4(finalColor, col.a);
}
