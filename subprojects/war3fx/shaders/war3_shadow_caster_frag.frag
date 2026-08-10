#version 460
#extension GL_EXT_nonuniform_qualifier : require

// ===== 片段输入 =====
layout(location = 0) in vec2 v_uv;  // 从顶点着色器传递的UV坐标
layout(location = 1) in vec3 v_surfaceCoord; // 模型空间，跨相机/刚体运动稳定

// ===== 资源声明 (Route B: Sampler Heap / Bindless) =====
// Set 0: Bindless Samplers (Global)
layout(set = 0, binding = 0) uniform sampler s_samplers[];

// Set 1: Resources (Storage Buffer, Textures)
// binding 0 = SSBO (Vertex Shader)
// binding 1 = Texture (Fragment Shader)
layout(set = 1, binding = 1) uniform texture2D u_alphaTex;

// ===== 推送常量 (与顶点着色器共享) =====
layout(push_constant) uniform push_block {
  mat4 p_mvp;
  uint p_paletteOffset;
  uint p_blendCount;
  uint p_flags;            // bit2=alphaTest启用
  float p_alphaRef;        // Alpha阈值
  uint p_samplerIndex;     // [NEW] Bindless Sampler Index
  float p_terrainDepthBias;// 与顶点着色器布局保持一致
  uint p_pad0;              // 点阴影路径: cube face 分辨率
  uint p_pad1;
  vec4 p_pointLightPosRange;
};

bool finiteFloat(float value) {
  return !isnan(value) && !isinf(value);
}

bool finiteVec2(vec2 value) {
  return all(not(isnan(value))) && all(not(isinf(value)));
}

bool finiteVec3(vec3 value) {
  return all(not(isnan(value))) && all(not(isinf(value)));
}

// Wyman and McGuire, "Hashed Alpha Testing", I3D 2017. The noise is
// surface-anchored and discretized at the directional shadow pixel scale.
float hashAlpha2(vec2 value) {
  return fract(1.0e4 * sin(17.0 * value.x + 0.1 * value.y) *
               (0.1 + abs(sin(13.0 * value.y + value.x))));
}

float hashAlpha3(vec3 value) {
  return hashAlpha2(vec2(hashAlpha2(value.xy), value.z));
}

float uniformizeInterpolatedHashes(float mixedHash, float interpolation) {
  float x = clamp(mixedHash, 0.0, 1.0);
  float t = clamp(interpolation, 0.0, 1.0);
  float a = min(t, 1.0 - t);
  if (a <= 1.0e-5)
    return clamp(x, 1.0e-6, 1.0);

  float denominator = 2.0 * a * (1.0 - a);
  float threshold = 0.0;
  if (x < a)
    threshold = x * x / denominator;
  else if (x < 1.0 - a)
    threshold = (x - 0.5 * a) / (1.0 - a);
  else
    threshold = 1.0 - ((1.0 - x) * (1.0 - x) / denominator);
  return clamp(threshold, 1.0e-6, 1.0);
}

bool stableHashedAlphaThreshold(vec3 surfaceCoord,
                                vec3 surfaceDx,
                                vec3 surfaceDy,
                                out float threshold) {
  threshold = 0.5;
  if (!finiteVec3(surfaceCoord) || !finiteVec3(surfaceDx) ||
      !finiteVec3(surfaceDy))
    return false;

  const float hashScale = 1.0;
  float maxDerivative = max(length(surfaceDx), length(surfaceDy));
  if (!finiteFloat(maxDerivative) || maxDerivative <= 1.0e-8)
    return false;

  float pixelScale = 1.0 / (hashScale * maxDerivative);
  if (!finiteFloat(pixelScale) || pixelScale <= 0.0)
    return false;

  float logScale = clamp(log2(pixelScale), -24.0, 24.0);
  vec2 scales = exp2(vec2(floor(logScale), ceil(logScale)));
  vec3 scaled0 = scales.x * surfaceCoord;
  vec3 scaled1 = scales.y * surfaceCoord;
  if (!finiteVec3(scaled0) || !finiteVec3(scaled1))
    return false;
  vec2 hashes = vec2(
    hashAlpha3(floor(scaled0)),
    hashAlpha3(floor(scaled1)));
  float interpolation = fract(logScale);
  threshold = uniformizeInterpolatedHashes(
    mix(hashes.x, hashes.y, interpolation), interpolation);
  return finiteFloat(threshold);
}

float stableCoverageBlend(vec2 uvDx, vec2 uvDy, vec2 textureExtent) {
  if (!finiteVec2(uvDx) || !finiteVec2(uvDy) ||
      !finiteVec2(textureExtent) || any(lessThanEqual(textureExtent, vec2(0.0))))
    return 0.0;

  vec2 footprint = vec2(
    length(uvDx * textureExtent),
    length(uvDy * textureExtent));
  float maxFootprint = max(footprint.x, footprint.y);
  if (!finiteFloat(maxFootprint) || maxFootprint <= 1.0)
    return 0.0;

  float minFootprint = min(footprint.x, footprint.y);
  float anisotropy = minFootprint > 1.0e-8
    ? max(footprint.x / minFootprint, footprint.y / minFootprint)
    : 1.0;
  if (!finiteFloat(anisotropy))
    anisotropy = 1.0;
  float lod = max(log2(maxFootprint), 0.0);
  // The paper found a six-level quadratic fade kept magnified edges clean.
  float normalized = clamp(anisotropy * lod / 6.0, 0.0, 1.0);
  return normalized * normalized;
}

void main() {
  // 检查是否启用Alpha测试 (bit2)
  if ((p_flags & 0x4u) != 0u) {
    bool hashed = (p_flags & 0x8u) != 0u;
    float authoredAlphaRef = finiteFloat(p_alphaRef)
      ? clamp(p_alphaRef, 0.0, 1.0)
      : 0.5;
    // Compute all implicit-derivative inputs before any data-dependent discard.
    // Explicit gradients keep sampling well-defined if the alpha decision later
    // diverges inside a fragment quad.
    vec2 uvDx = dFdx(v_uv);
    vec2 uvDy = dFdy(v_uv);
    vec3 surfaceDx = dFdx(v_surfaceCoord);
    vec3 surfaceDy = dFdy(v_surfaceCoord);
    float alpha = textureGrad(
      sampler2D(u_alphaTex, s_samplers[nonuniformEXT(p_samplerIndex)]),
      v_uv, uvDx, uvDy).a;
    
    if (hashed) {
      float hashedThreshold = authoredAlphaRef;
      bool thresholdValid = stableHashedAlphaThreshold(
        v_surfaceCoord, surfaceDx, surfaceDy, hashedThreshold);
      vec2 textureExtent = vec2(max(textureSize(
        sampler2D(u_alphaTex, s_samplers[nonuniformEXT(p_samplerIndex)]), 0),
        ivec2(1)));
      float coverageBlend = thresholdValid
        ? stableCoverageBlend(uvDx, uvDy, textureExtent)
        : 0.0;
      float threshold = mix(
        authoredAlphaRef, hashedThreshold, coverageBlend);
      if (alpha < threshold)
        discard;
    } else {
      // 如果Alpha低于阈值，丢弃该片段（不写入阴影图）
      if (alpha < authoredAlphaRef) {
        discard;
      }
    }
  }

  // Directional CSM uses fixed-function depth. Do not write gl_FragDepth:
  // doing so disables early depth and, on the 32-bit driver path, the shared
  // point/cascade shader eventually caused VK_ERROR_DEVICE_LOST.
}
