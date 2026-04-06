#version 460

#extension GL_EXT_nonuniform_qualifier : require

/**
 * SMAA Edge Detection Pass
 * 
 * Detects edges based on luma contrast.
 * Output: RG8 texture with horizontal and vertical edge weights
 */

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D s_color;

layout(push_constant) uniform push_block {
    uint p_colorSampler;
    float p_invWidth;
    float p_invHeight;
    float p_threshold;
};

layout(location = 0) in vec2 i_pos;
layout(location = 0) out vec2 o_edges;

#define SMAA_THRESHOLD p_threshold

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec4 sampleColor(vec2 uv) {
    return texture(sampler2D(s_color, s_samplers[nonuniformEXT(p_colorSampler)]), uv);
}

void main() {
    // AA pass 自己会把 viewport/scissor 设为整张 RT，这里直接吃 VS 传下来的
    // UV，和官方 SMAA 的 VS/PS 配套方式保持一致，避免再手搓一套 gl_FragCoord
    // 坐标导致搜索/邻域阶段出现半像素偏差。
    vec2 texcoord = i_pos;
    vec2 invResolution = vec2(p_invWidth, p_invHeight);

    vec4 offset0 = texcoord.xyxy + vec4(-1.0, 0.0, 0.0, -1.0) * invResolution.xyxy;
    vec4 offset1 = texcoord.xyxy + vec4( 1.0, 0.0, 0.0,  1.0) * invResolution.xyxy;
    vec4 offset2 = texcoord.xyxy + vec4(-2.0, 0.0, 0.0, -2.0) * invResolution.xyxy;

    float L = luminance(sampleColor(texcoord).rgb);
    float Lleft = luminance(sampleColor(offset0.xy).rgb);
    float Ltop = luminance(sampleColor(offset0.zw).rgb);

    vec2 delta = abs(vec2(L - Lleft, L - Ltop));
    vec2 edges = step(vec2(SMAA_THRESHOLD), delta);

    if (dot(edges, vec2(1.0)) == 0.0) {
        o_edges = vec2(0.0);
        return;
    }

    float Lright = luminance(sampleColor(offset1.xy).rgb);
    float Lbottom = luminance(sampleColor(offset1.zw).rgb);
    vec2 delta2 = abs(vec2(L - Lright, L - Lbottom));

    vec2 maxDelta = max(delta, delta2);

    float Lleftleft = luminance(sampleColor(offset2.xy).rgb);
    float Ltoptop = luminance(sampleColor(offset2.zw).rgb);
    vec2 delta3 = abs(vec2(Lleft - Lleftleft, Ltop - Ltoptop));

    maxDelta = max(maxDelta, delta3);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    edges *= step(finalDelta, vec2(2.0) * delta);
    o_edges = edges;
}
