#version 460

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

// 运动向量输出（相机运动）：uv - prevUv
//
// 说明：
// - 当前阶段仅生成“相机运动向量”（通过深度重建 worldPos，再用 prevViewProj 重投影）。
// - 不包含逐物体的动画/位移/骨骼等运动向量（未来做全屏 TAA 可再扩展）。

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 1) uniform texture2DArray s_depth;

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

layout(push_constant, scalar)
uniform push_block {
  uint p_colorSampler;
  uint p_shadowSampler;
};

layout(location = 0) out vec2 o_motion;

void main() {
  ivec2 pix = ivec2(gl_FragCoord.xy);

  vec2 vpMin = ubo.u_viewport.xy;
  vec2 vpSize = ubo.u_viewport.zw;
  vec2 uvVp = (vec2(pix) + 0.5 - vpMin) / max(vpSize, vec2(1.0));

  // 非 viewport 区域：不输出运动向量（避免重投影采样越界导致“脏边”）
  if (uvVp.x < 0.0 || uvVp.x > 1.0 || uvVp.y < 0.0 || uvVp.y > 1.0) {
    o_motion = vec2(0.0);
    return;
  }

  // 没有上一帧时，C++ 会把 u_prevViewProj 填为“当前帧 viewProj”，此时 mv 自然为 0

  float depth = texelFetch(
    sampler2DArray(s_depth, s_samplers[nonuniformEXT(p_colorSampler)]),
    ivec3(pix, 0),
    0).r;

  float minZ = ubo.u_viewportZ.x;
  float maxZ = ubo.u_viewportZ.y;
  float depthN = depth;
  float denom = maxZ - minZ;
  if (abs(denom) > 1e-6)
    depthN = (depth - minZ) / denom;
  depthN = clamp(depthN, 0.0, 1.0);

  // D3D-style NDC（与 receiver 保持一致）
  vec4 clip = vec4(uvVp.x * 2.0 - 1.0, 1.0 - uvVp.y * 2.0, depthN, 1.0);

  vec4 worldH = clip * ubo.u_invViewProj;
  if (abs(worldH.w) < 1e-6) {
    o_motion = vec2(0.0);
    return;
  }

  vec3 worldPos = worldH.xyz / worldH.w;
  vec4 prevClip = vec4(worldPos, 1.0) * ubo.u_prevViewProj;
  if (abs(prevClip.w) < 1e-6) {
    o_motion = vec2(0.0);
    return;
  }

  vec2 prevNdc = prevClip.xy / prevClip.w;
  vec2 prevUvVp = vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);

  // 运动向量：当前 uvVp - 上一帧 uvVp（用于 historyUv = uv - mv）
  o_motion = uvVp - prevUvVp;
}
