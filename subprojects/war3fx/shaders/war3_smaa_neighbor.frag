#version 460

#extension GL_EXT_nonuniform_qualifier : require

/**
 * SMAA Neighborhood Blending Pass
 * 
 * Final pass that blends neighboring pixels based on calculated weights.
 * Output: Final anti-aliased color
 */

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D s_color;
layout(set = 1, binding = 1) uniform texture2D s_blendWeights;

layout(push_constant) uniform push_block {
    uint p_colorSampler;
    uint p_blendSampler;
    float p_invWidth;
    float p_invHeight;
};

layout(location = 0) in vec2 i_pos;
layout(location = 0) out vec4 o_color;

vec4 sampleColor(vec2 uv) {
    return texture(sampler2D(s_color, s_samplers[nonuniformEXT(p_colorSampler)]), uv);
}

vec4 sampleBlend(vec2 uv) {
    return texture(sampler2D(s_blendWeights, s_samplers[nonuniformEXT(p_blendSampler)]), uv);
}

void main() {
    // Neighbor pass 必须和 Edge/Blend 使用同一套 UV 基准，否则权重会落到错误
    // 邻居上，视觉上就会变成“只画一条细线，不真正填充锯齿”。
    vec2 texcoord = i_pos;
    vec2 invResolution = vec2(p_invWidth, p_invHeight);
    
    // 原版 SMAA 在顶点着色器中预计算偏移量，这里手动计算:
    // offset.xy = texcoord + vec2(1.0, 0.0) * invResolution  (右边像素)
    // offset.zw = texcoord + vec2(0.0, -1.0) * invResolution (上方像素)
    // 当前这套 fullscreen UV 是左上为原点、Y 向下增长，所以 Top 必须是 -Y，
    // 否则边缘权重会被应用到错误方向，表现成“描了一条边但没把台阶填平”。
    vec4 offset = texcoord.xyxy + vec4(1.0, 0.0, 0.0, -1.0) * invResolution.xyxy;
    
    // 采样混合权重 (遵循原版 SMAA.hlsl 的通道布局):
    // a.x = 右边像素的 alpha (用于当前像素水平左边缘混合)
    // a.y = 上方像素的 green (用于当前像素垂直上边缘混合)
    // a.wz = 当前像素的 red 和 blue (水平右边缘 / 垂直下边缘)
    vec4 a;
    a.x = sampleBlend(offset.xy).a;      // Right pixel's alpha
    a.y = sampleBlend(offset.zw).g;      // Top pixel's green
    a.wz = sampleBlend(texcoord).rb;     // Current pixel's red (.r) and blue (.b)
    
    // Is there any blending weight with a value greater than 0.0?
    if (dot(a, vec4(1.0)) < 1e-5) {
        o_color = sampleColor(texcoord);
        return;
    }
    
    // Blend horizontally or vertically?
    // max(horizontal) > max(vertical)
    bool h = max(a.x, a.z) > max(a.y, a.w);
    
    // Calculate the blending offsets:
    // 垂直混合：向上和向下偏移
    vec4 blendingOffset = vec4(0.0, a.y, 0.0, a.w);
    vec2 blendingWeight = a.yw;
    if (h) {
        // 水平混合：向右和向左偏移
        blendingOffset = vec4(a.x, 0.0, a.z, 0.0);
        blendingWeight = a.xz;
    }
    blendingWeight /= dot(blendingWeight, vec2(1.0));
    
    // Calculate the texture coordinates:
    // blendingOffset 是权重值 (0-1)，直接用作亚像素偏移
    // 第一个偏移 +invResolution，第二个偏移 -invResolution
    vec4 blendingCoord = fma(blendingOffset, 
                              vec4(invResolution, -invResolution),
                              texcoord.xyxy);
    
    // Blend using bilinear filtering!
    vec4 color = blendingWeight.x * sampleColor(blendingCoord.xy);
    color += blendingWeight.y * sampleColor(blendingCoord.zw);
    
    o_color = color;
}
