#version 460
#extension GL_EXT_nonuniform_qualifier : require

// ===== 片段输入 =====
layout(location = 0) in vec2 v_uv;  // 从顶点着色器传递的UV坐标

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

float hash12(vec2 p) {
  return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
  // 检查是否启用Alpha测试 (bit2)
  if ((p_flags & 0x4u) != 0u) {
    bool hashed = (p_flags & 0x8u) != 0u;
    // 采样纹理获取Alpha值
    // 使用 Bindless Sampler 进行采样
    float alpha = texture(sampler2D(u_alphaTex, s_samplers[nonuniformEXT(p_samplerIndex)]), v_uv).a;
    
    if (hashed) {
      // Hash Alpha：为 Alpha-Test 阴影提供“分数覆盖率”的近似
      // 关键点：
      // - 噪声锚定到 alpha 贴图 texel，而不是 ShadowMap 像素。这样 CSM/相机
      //   轻微漂移时，dither 模板仍跟随树叶纹理本身，不会在叶片上滑动。
      // - 不要混入 palette/matrix offset：draw-time buffer 排布可能随帧变化，
      //   把它作为盐会让整棵树的 dither 模板每帧换一张。
      float hashWidth = 0.06 + 0.05 * (1.0 - p_alphaRef);
      float delta = alpha - p_alphaRef;
      if (delta <= -hashWidth) {
        discard;
      } else if (delta < hashWidth) {
        float t = (delta + hashWidth) / (2.0 * hashWidth);
        ivec2 texSize = textureSize(
          sampler2D(u_alphaTex, s_samplers[nonuniformEXT(p_samplerIndex)]),
          0);
        vec2 texel = floor(fract(v_uv) * max(vec2(texSize), vec2(1.0)));
        vec2 hashP = texel + vec2(float(p_samplerIndex) * 17.0, 0.0);
        float noise = hash12(hashP);
        if (t < noise) {
          discard;
        }
      }
    } else {
      // 如果Alpha低于阈值，丢弃该片段（不写入阴影图）
      if (alpha < p_alphaRef) {
        discard;
      }
    }
  }

  // Directional CSM uses fixed-function depth. Do not write gl_FragDepth:
  // doing so disables early depth and, on the 32-bit driver path, the shared
  // point/cascade shader eventually caused VK_ERROR_DEVICE_LOST.
}
