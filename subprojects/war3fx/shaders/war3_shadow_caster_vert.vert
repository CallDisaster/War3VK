#version 460

#extension GL_EXT_scalar_block_layout : require

// ===== 顶点输入 =====
layout(location = 0) in vec4 in_pos;           // 位置
layout(location = 1) in vec4 in_blendWeight;   // 混合权重
layout(location = 2) in vec4 in_blendIndices;  // 混合索引
layout(location = 3) in vec2 in_uv;            // 纹理坐标 (用于Alpha测试)

// ===== 顶点输出 =====
layout(location = 0) out vec2 v_uv;            // 传递给片段着色器的UV

// ===== 统一缓冲区 =====
layout(set = 1, binding = 0, std140, row_major) readonly buffer WorldMatrices {
  mat4 u_worldMatrices[];
};

// ===== 推送常量 =====
layout(push_constant, scalar, row_major)
uniform push_block {
  mat4 p_mvp;              // 统一: lightVP（非混合物体的 worldMatrix 从 SSBO 读取）
  uint p_paletteOffset;    // 混合: paletteIndex * 256；非混合: objectBase + drawIndex
  uint p_blendCount;       // 0..3
  uint p_flags;            // 低位0..5沿用归档阴影语义；高位保留但不消费
  float p_alphaRef;        // Alpha阈值 (0.0-1.0)
  uint p_samplerIndex;     // 与片段着色器布局保持一致
  float p_terrainDepthBias;// S1 terrain caster-only NDC depth bias
  uint p_pad0;
  uint p_pad1;
  vec4 p_pointLightPosRange; // xyz=点光源位置, w=范围
};

void applyTerrainDepthBias(inout vec4 clipPos) {
  if ((p_flags & 0x10u) != 0u && p_terrainDepthBias > 0.0 && clipPos.w > 0.0) {
    clipPos.z = min(clipPos.z + p_terrainDepthBias * clipPos.w, clipPos.w);
  }
}

void main() {
  vec4 position = in_pos;
  vec2 uv = in_uv;
  v_uv = uv;

  // Diagnostic fail-safe: the complete world/light transform is already in
  // the push constants, so this path performs no matrix-SSBO access. It is
  // used to distinguish captured geometry faults from matrix ring lifetime
  // faults without changing the vertex/index buffers under test.
  if ((p_flags & 0x40u) != 0u) {
    vec4 clipPos = position * p_mvp;
    applyTerrainDepthBias(clipPos);
    gl_Position = clipPos;
    return;
  }

  // 非混合模式：GPU 端 MVP 计算（worldMatrix 从 SSBO 读取）
  if ((p_flags & 0x1u) == 0u) {
    mat4 wm = u_worldMatrices[p_paletteOffset + uint(gl_InstanceIndex)];
    vec4 worldPos = position * wm;
    vec4 clipPos = worldPos * p_mvp;
    applyTerrainDepthBias(clipPos);
    gl_Position = clipPos;
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
    sum += (position * wm) * w;
  }

  vec4 clipPos = sum * p_mvp;
  applyTerrainDepthBias(clipPos);
  gl_Position = clipPos;
}
