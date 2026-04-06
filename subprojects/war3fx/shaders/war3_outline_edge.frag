#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

layout(location = 0) out vec4 o_color;

// ===== 采样器堆 =====
layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D u_maskVisible;
layout(set = 1, binding = 1) uniform texture2D u_maskAll;

// ===== 推送常量 =====
layout(push_constant, scalar) uniform push_block {
  uint p_visibleSampler;
  uint p_allSampler;
  float p_invWidth;
  float p_invHeight;
  float p_widthPx;
  uint p_showVisible;  // 0/1
  uint p_showOccluded; // 0/1
  vec4 p_color;        // RGBA
};

ivec2 clampCoord(ivec2 pix, ivec2 size) {
  return clamp(pix, ivec2(0), size - ivec2(1));
}

float sampleMask(texture2D tex, uint samplerIndex, ivec2 pix) {
  return texelFetch(
      sampler2D(tex, s_samplers[nonuniformEXT(samplerIndex)]), pix, 0)
      .r;
}

float sampleVisibleDilated(ivec2 pix, ivec2 size) {
  float maxN = step(0.5, sampleMask(u_maskVisible, p_visibleSampler,
                                    clampCoord(pix, size)));
  float radiusF = clamp(floor(p_widthPx + 1.5), 2.0, 8.0);
  int radius = int(radiusF);

  for (int i = 1; i <= radius; i++) {
    ivec2 stepPix = ivec2(i, 0);
    maxN = max(maxN, step(0.5, sampleMask(u_maskVisible, p_visibleSampler,
                                          clampCoord(pix + stepPix, size))));
    maxN = max(maxN, step(0.5, sampleMask(u_maskVisible, p_visibleSampler,
                                          clampCoord(pix - stepPix, size))));
    stepPix = ivec2(0, i);
    maxN = max(maxN, step(0.5, sampleMask(u_maskVisible, p_visibleSampler,
                                          clampCoord(pix + stepPix, size))));
    maxN = max(maxN, step(0.5, sampleMask(u_maskVisible, p_visibleSampler,
                                          clampCoord(pix - stepPix, size))));
    stepPix = ivec2(i, i);
    maxN = max(maxN, step(0.5, sampleMask(u_maskVisible, p_visibleSampler,
                                          clampCoord(pix + stepPix, size))));
    maxN = max(maxN, step(0.5, sampleMask(u_maskVisible, p_visibleSampler,
                                          clampCoord(pix - stepPix, size))));
    stepPix = ivec2(i, -i);
    maxN = max(maxN, step(0.5, sampleMask(u_maskVisible, p_visibleSampler,
                                          clampCoord(pix + stepPix, size))));
    maxN = max(maxN, step(0.5, sampleMask(u_maskVisible, p_visibleSampler,
                                          clampCoord(pix - stepPix, size))));
  }

  return maxN;
}

float edgeFromMask(texture2D tex, uint samplerIndex, ivec2 pix, ivec2 size,
                   int offset) {
  float center = sampleMask(tex, samplerIndex, clampCoord(pix, size));

  float maxN = 0.0;
  ivec2 stepPix = ivec2(offset, 0);
  maxN = max(maxN, sampleMask(tex, samplerIndex,
                              clampCoord(pix + stepPix, size)));
  maxN = max(maxN, sampleMask(tex, samplerIndex,
                              clampCoord(pix - stepPix, size)));
  stepPix = ivec2(0, offset);
  maxN = max(maxN, sampleMask(tex, samplerIndex,
                              clampCoord(pix + stepPix, size)));
  maxN = max(maxN, sampleMask(tex, samplerIndex,
                              clampCoord(pix - stepPix, size)));
  stepPix = ivec2(offset, offset);
  maxN = max(maxN, sampleMask(tex, samplerIndex,
                              clampCoord(pix + stepPix, size)));
  maxN = max(maxN, sampleMask(tex, samplerIndex,
                              clampCoord(pix - stepPix, size)));
  stepPix = ivec2(offset, -offset);
  maxN = max(maxN, sampleMask(tex, samplerIndex,
                              clampCoord(pix + stepPix, size)));
  maxN = max(maxN, sampleMask(tex, samplerIndex,
                              clampCoord(pix - stepPix, size)));

  float edge = step(0.5, maxN) - step(0.5, center);
  return edge;
}

void main() {
  ivec2 pix = ivec2(gl_FragCoord.xy);
  ivec2 visSize = textureSize(
      sampler2D(u_maskVisible, s_samplers[nonuniformEXT(p_visibleSampler)]), 0);
  ivec2 allSize =
      textureSize(sampler2D(u_maskAll, s_samplers[nonuniformEXT(p_allSampler)]),
                  0);
  int edgeOffset = max(1, int(floor(p_widthPx + 0.5)));

  float edgeVisible = 0.0;
  float edgeOccluded = 0.0;
  float visDilated = 0.0;
  float edgeAll = 0.0;
  if (p_showVisible != 0u || p_showOccluded != 0u) {
    visDilated = sampleVisibleDilated(pix, visSize);
    edgeAll = edgeFromMask(u_maskAll, p_allSampler, pix, allSize, edgeOffset);
  }
  if (p_showVisible != 0u) {
    // 使用填充后的轮廓边缘，避免 Alpha 裁剪造成内部边缘
    edgeVisible = edgeAll * visDilated;
  }
  if (p_showOccluded != 0u) {
    edgeOccluded = edgeAll * (1.0 - visDilated);
  }

  float edge = 0.0;
  if (p_showVisible != 0u) {
    edge = max(edge, edgeVisible);
  }
  if (p_showOccluded != 0u) {
    edge = max(edge, edgeOccluded);
  }
  if (edge <= 0.0) {
    discard;
  }

  o_color = vec4(p_color.rgb, p_color.a * edge);
}
