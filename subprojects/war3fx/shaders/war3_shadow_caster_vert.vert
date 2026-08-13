#version 460

#extension GL_EXT_scalar_block_layout : require

// ===== 顶点输入 =====
layout(location = 0) in vec4 in_pos;           // 位置
layout(location = 1) in vec4 in_blendWeight;   // 混合权重
layout(location = 2) in vec4 in_blendIndices;  // 混合索引
layout(location = 3) in vec2 in_uv;            // 纹理坐标 (用于Alpha测试)

// ===== 顶点输出 =====
layout(location = 0) out vec2 v_uv;            // 传递给片段着色器的UV
layout(location = 1) out vec3 v_surfaceCoord;  // 稳定的模型空间覆盖率坐标

// ===== 统一缓冲区 =====
layout(set = 1, binding = 0, std140, row_major) readonly buffer WorldMatrices {
  mat4 u_worldMatrices[];
};

// VS-B1 keeps only immutable model input plus the current palette. Bindings
// 3/4 are always valid storage descriptors; non-direct draws bind the matrix
// buffer as a harmless fallback and never enter the private route below.
layout(set = 1, binding = 3, std430) readonly restrict buffer GpuSkinStaticSource {
  uint u_gpuSkinSourceWords[];
};

layout(set = 1, binding = 4, std430) readonly restrict buffer GpuSkinPalette {
  uint u_gpuSkinPaletteWords[];
};

// ===== 推送常量 =====
layout(push_constant, scalar, row_major)
uniform push_block {
  mat4 p_mvp;              // 统一: lightVP（非混合物体的 worldMatrix 从 SSBO 读取）
  uint p_paletteOffset;    // 混合: paletteIndex * 256；非混合: objectBase + drawIndex
  uint p_blendCount;       // legacy=0..3; VS-B1=palette matrix count
  uint p_flags;            // low bits=shadow route, high bits=VS-B1 layout metadata
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

uint loadGpuSkinSourceWord(uint byteOffset) {
  return u_gpuSkinSourceWords[byteOffset >> 2u];
}

uint loadGpuSkinSourceByte(uint byteOffset) {
  uint packed = u_gpuSkinSourceWords[byteOffset >> 2u];
  return (packed >> ((byteOffset & 3u) * 8u)) & 0xffu;
}

float loadGpuSkinPaletteFloat(uint byteOffset) {
  return uintBitsToFloat(u_gpuSkinPaletteWords[byteOffset >> 2u]);
}

bool tryLoadGpuSkinDirectVertex(out vec4 position, out vec2 uv) {
  const uint gpuSkinDirectFlag = 0x40u;
  const uint gpuSkinNoFallbackFlag = 0x80u;
  const uint gpuSkinMetadataMask = 0x000fff00u;
  const uint gpuSkinFormat2Layout1Uv1 = 0x00011200u;

  if ((p_flags & (gpuSkinDirectFlag | gpuSkinNoFallbackFlag)) !=
          (gpuSkinDirectFlag | gpuSkinNoFallbackFlag) ||
      (p_flags & gpuSkinMetadataMask) != gpuSkinFormat2Layout1Uv1 ||
      p_pad1 == 0u || p_blendCount == 0u || p_blendCount > 256u ||
      gl_VertexIndex < 0 || uint(gl_VertexIndex) >= p_pad1) {
    return false;
  }

  uint vertex = uint(gl_VertexIndex);
  uint vertexCount = p_pad1;
  uint positionBase = 0u;
  uint normalBase = positionBase + vertexCount * 12u;
  uint groupSlotBase = normalBase + vertexCount * 12u;
  uint texcoord0Base = (groupSlotBase + vertexCount + 3u) & ~3u;
  uint positionAddress = positionBase + vertex * 12u;
  uint texcoord0Address = texcoord0Base + vertex * 8u;

  precise float px = uintBitsToFloat(
      loadGpuSkinSourceWord(positionAddress + 0u));
  precise float py = uintBitsToFloat(
      loadGpuSkinSourceWord(positionAddress + 4u));
  precise float pz = uintBitsToFloat(
      loadGpuSkinSourceWord(positionAddress + 8u));

  uint groupSlot = loadGpuSkinSourceByte(groupSlotBase + vertex);
  if (groupSlot >= p_blendCount)
    return false;

  uint matrixAddress = groupSlot * 48u;
  precise float m0 = loadGpuSkinPaletteFloat(matrixAddress + 0u);
  precise float m1 = loadGpuSkinPaletteFloat(matrixAddress + 4u);
  precise float m2 = loadGpuSkinPaletteFloat(matrixAddress + 8u);
  precise float m3 = loadGpuSkinPaletteFloat(matrixAddress + 12u);
  precise float m4 = loadGpuSkinPaletteFloat(matrixAddress + 16u);
  precise float m5 = loadGpuSkinPaletteFloat(matrixAddress + 20u);
  precise float m6 = loadGpuSkinPaletteFloat(matrixAddress + 24u);
  precise float m7 = loadGpuSkinPaletteFloat(matrixAddress + 28u);
  precise float m8 = loadGpuSkinPaletteFloat(matrixAddress + 32u);
  precise float m9 = loadGpuSkinPaletteFloat(matrixAddress + 36u);
  precise float m10 = loadGpuSkinPaletteFloat(matrixAddress + 40u);
  precise float m11 = loadGpuSkinPaletteFloat(matrixAddress + 44u);

  // Match the compute/native 3x4 scalar sequence exactly. A matrix multiply
  // may reassociate or contract these operations and reintroduce pose drift.
  precise float positionX01 = m0 * px + m3 * py;
  precise float positionX2 = positionX01 + m6 * pz;
  precise float positionX = positionX2 + m9;
  precise float positionY01 = m1 * px + m4 * py;
  precise float positionY2 = positionY01 + m7 * pz;
  precise float positionY = positionY2 + m10;
  precise float positionZ01 = m2 * px + m5 * py;
  precise float positionZ2 = positionZ01 + m8 * pz;
  precise float positionZ = positionZ2 + m11;

  position = vec4(positionX, positionY, positionZ, 1.0);
  uv = vec2(0.0);
  if ((p_flags & 0x4u) != 0u) {
    uv = vec2(
        uintBitsToFloat(loadGpuSkinSourceWord(texcoord0Address + 0u)),
        uintBitsToFloat(loadGpuSkinSourceWord(texcoord0Address + 4u)));
  }
  return true;
}

void main() {
  vec4 position = in_pos;
  vec2 uv = in_uv;
  v_uv = uv;
  // Hash coverage is anchored to the immutable pre-transform surface. Never
  // salt this coordinate with frame, palette, buffer or sampler identities:
  // those can change while the logical caster remains the same.
  v_surfaceCoord = in_pos.xyz;

  // VS-B1 aliases binding 0 to bind-pose positions and intentionally skips
  // the native/compute output VB. Reconstruct the current pose from the exact
  // immutable source + palette before applying light VP. VS-A/B0 still bind a
  // fully skinned 32-byte vertex and therefore keep the direct projection path.
  if ((p_flags & 0x40u) != 0u) {
    if ((p_flags & 0x80u) != 0u &&
        !tryLoadGpuSkinDirectVertex(position, uv)) {
      // B1 has no safe dynamic-VB fallback. Host preflight makes this an
      // all-draw invariant failure; place the primitive outside clip space.
      gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
      v_uv = vec2(0.0);
      return;
    }
    v_uv = uv;
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
