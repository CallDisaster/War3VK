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
      // - 噪声必须锚定在“ShadowMap 像素网格”上，否则 UV/投影的微小漂移会导致噪声本身变化，
      //   进而表现为树叶阴影边缘抖动/闪烁（尤其是高机位/远级联）。
      // - 因此这里使用 gl_FragCoord（当前 ShadowMap 像素坐标）生成稳定随机阈值。
      float hashWidth = 0.08 + 0.08 * (1.0 - p_alphaRef);
      float delta = alpha - p_alphaRef;
      if (delta <= -hashWidth) {
        discard;
      } else if (delta < hashWidth) {
        float t = (delta + hashWidth) / (2.0 * hashWidth);
        ivec2 pix = ivec2(gl_FragCoord.xy);
        vec2 hashP = vec2(pix) + vec2(float(p_samplerIndex) * 0.37, float(p_paletteOffset) * 0.13);
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
  
  // 深度写入由硬件自动完成，无需额外输出
}
