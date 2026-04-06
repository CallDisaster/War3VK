#version 460

#extension GL_EXT_scalar_block_layout : require

// 单位被遮挡描边 Vertex Shader
// 与 shadow caster 类似，但使用相机的 ViewProj 而非光源的

// ===== 顶点输入 =====
layout(location = 0) in vec4 in_pos;           // 位置
layout(location = 1) in vec4 in_blendWeight;   // 混合权重
layout(location = 2) in vec4 in_blendIndices;  // 混合索引

// ===== 统一缓冲区 =====
layout(set = 1, binding = 0, std140, row_major) readonly buffer WorldMatrices {
  mat4 u_worldMatrices[];
};

// ===== 推送常量 =====
layout(push_constant, scalar, row_major)
uniform push_block {
  mat4 p_mvp;              // 非混合: world*viewProj; 混合: viewProj
  uint p_paletteOffset;    // 基础矩阵索引 (paletteIndex * 256)
  uint p_blendCount;       // 0..3
  uint p_flags;            // bit0=useBlend, bit1=indexed
  vec4 p_outlineColor;     // 描边颜色 RGBA
};

void main() {
  // 非混合模式：直接变换
  if ((p_flags & 0x1u) == 0u) {
    gl_Position = in_pos * p_mvp;
    return;
  }

  // 混合模式：加权骨骼变换
  float remaining = 1.0;
  vec4 sum = vec4(0.0);

  uint blendCount = min(p_blendCount, 3u);
  for (uint i = 0u; i <= blendCount; i++) {
    uint idx = i;
    if ((p_flags & 0x2u) != 0u) {
      idx = uint(round(in_blendIndices[i]));
    }

    // 防止越界读取
    idx = min(idx, 255u);

    float w = remaining;
    if (i != blendCount) {
      w = in_blendWeight[i];
      remaining -= w;
    }

    mat4 wm = u_worldMatrices[p_paletteOffset + idx];
    sum += (in_pos * wm) * w;
  }

  gl_Position = sum * p_mvp;
}
