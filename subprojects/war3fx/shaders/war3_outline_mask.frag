#version 460
#extension GL_EXT_nonuniform_qualifier : require

// ===== 片段输入 =====
layout(location = 0) in vec2 v_uv;

// ===== 输出：遮罩值（R8）=====
// ===== 输出：遮罩值（R8）=====
layout(location = 0) out float o_visible;
layout(location = 1) out float o_all;

// ===== 采样器堆 =====
layout(set = 0, binding = 0) uniform sampler s_samplers[];

// ===== Alpha 测试纹理 =====
layout(set = 1, binding = 1) uniform texture2D u_alphaTex;  
// ===== 场景深度纹理 (Manual Depth Compare) =====
layout(set = 1, binding = 2) uniform texture2D u_sceneDepth;

// ===== 推送常量（与 shadow caster 保持一致）=====
layout(push_constant) uniform push_block {
  mat4 p_mvp;
  uint p_paletteOffset;
  uint p_blendCount;
  uint p_flags;        // bit2=alphaTest
  float p_alphaRef;
  uint p_samplerIndex;
};

void main() {
  // Alpha 测试：与阴影投射一致
  // 注意：这里不直接 discard，避免在 All Mask 中留下透明孔洞导致“内描边”。
  bool alphaPass = true;
  if ((p_flags & 0x4u) != 0u) {
    float alpha = texture(
        sampler2D(u_alphaTex, s_samplers[nonuniformEXT(p_samplerIndex)]), v_uv)
                      .a;
    alphaPass = alpha >= p_alphaRef;
  }

  // Manual Depth Compare with Epsilon
  // sampler index 0 is typically a linear sampler or nearest sampler available in the heap
  // We use the same sampler as shadow sampling or just use s_samplers[0] assuming it's valid?
  // Actually, we can assume sampler 0 is valid or use a specific one.
  // d3d9_war3_shadow.cpp binds m_samplerLinear at index ?
  // Usually sampler heap[0] is valid.
  
  // Use texelFetch for exact pixel fetch (no filtering needed for depth)
  // 复用 push constant 的 samplerIndex 以保证 sampler 有效
  float sceneZ = texelFetch(
    sampler2D(u_sceneDepth, s_samplers[nonuniformEXT(p_samplerIndex)]),
    ivec2(gl_FragCoord.xy),
    0).r;
  
  // 深度容差：减少“可见边缘被误判为遮挡”的虚描边
  // 基础偏移 + 斜率偏移（同时考虑当前深度与场景深度的变化量）
  float depthGrad = max(fwidth(gl_FragCoord.z), fwidth(sceneZ));
  float bias = 0.002 + 3.0 * depthGrad;
  
  bool visible = (gl_FragCoord.z <= sceneZ + bias);

  // Visible Mask：遵循 AlphaTest + 深度可见性
  o_visible = (alphaPass && visible) ? 1.0 : 0.0;

  // All Mask：忽略 AlphaTest，避免树叶/镂空纹理产生内部边缘
  o_all = 1.0;
}
