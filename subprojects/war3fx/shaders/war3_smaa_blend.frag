#version 460

#extension GL_EXT_nonuniform_qualifier : require

/**
 * SMAA Blend Weight Calculation Pass
 *
 * Calculates blend weights for each edge using area and search lookup textures.
 * Output: RGBA8 texture with blend weights
 */

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D s_edges;
layout(set = 1, binding = 1) uniform texture2D s_areaTex;
layout(set = 1, binding = 2) uniform texture2D s_searchTex;

layout(push_constant) uniform push_block {
    uint p_edgeSampler;
    uint p_areaSampler;
    uint p_searchSampler;
    float p_invWidth;
    float p_invHeight;
    int p_maxSearchSteps;
    int p_maxSearchStepsDiag;
};

layout(location = 0) in vec2 i_pos;
layout(location = 0) out vec4 o_weights;

#define SMAA_AREATEX_MAX_DISTANCE 16.0
#define SMAA_AREATEX_PIXEL_SIZE (1.0 / vec2(160.0, 560.0))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)
#define SMAA_SEARCHTEX_SIZE vec2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE vec2(64.0, 16.0)
#define SMAA_CORNER_ROUNDING 25.0
#define SMAA_CORNER_ROUNDING_NORM (SMAA_CORNER_ROUNDING / 100.0)

vec2 sampleEdges(vec2 uv) {
    return texture(sampler2D(s_edges, s_samplers[nonuniformEXT(p_edgeSampler)]), uv).rg;
}

vec2 sampleEdgesOffset(vec2 uv, ivec2 offset, vec2 invResolution) {
    return sampleEdges(uv + vec2(offset) * invResolution);
}

float sampleSearch(vec2 uv) {
    return texture(sampler2D(s_searchTex, s_samplers[nonuniformEXT(p_searchSampler)]), uv).r;
}

vec2 sampleArea(vec2 uv) {
    return texture(sampler2D(s_areaTex, s_samplers[nonuniformEXT(p_areaSampler)]), uv).rg;
}

float searchLength(vec2 e, float offset) {
    vec2 scale = SMAA_SEARCHTEX_SIZE * vec2(0.5, -1.0);
    vec2 bias = SMAA_SEARCHTEX_SIZE * vec2(offset, 1.0);
    scale += vec2(-1.0, 1.0);
    bias += vec2(0.5, -0.5);
    scale *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    return sampleSearch(scale * e + bias);
}

float searchXLeft(vec2 texcoord, float end, vec2 invResolution) {
    vec2 e = vec2(0.0, 1.0);
    for (int i = 0; i < p_maxSearchSteps; i++) {
        if (texcoord.x <= end || e.g <= 0.8281 || e.r > 0.0) break;
        e = sampleEdges(texcoord);
        texcoord = fma(vec2(-2.0, 0.0), invResolution, texcoord);
    }
    float offset = fma(-(255.0 / 127.0), searchLength(e, 0.0), 3.25);
    return fma(invResolution.x, offset, texcoord.x);
}

float searchXRight(vec2 texcoord, float end, vec2 invResolution) {
    vec2 e = vec2(0.0, 1.0);
    for (int i = 0; i < p_maxSearchSteps; i++) {
        if (texcoord.x >= end || e.g <= 0.8281 || e.r > 0.0) break;
        e = sampleEdges(texcoord);
        texcoord = fma(vec2(2.0, 0.0), invResolution, texcoord);
    }
    float offset = fma(-(255.0 / 127.0), searchLength(e, 0.5), 3.25);
    return fma(-invResolution.x, offset, texcoord.x);
}

float searchYUp(vec2 texcoord, float end, vec2 invResolution) {
    vec2 e = vec2(1.0, 0.0);
    for (int i = 0; i < p_maxSearchSteps; i++) {
        if (texcoord.y <= end || e.r <= 0.8281 || e.g > 0.0) break;
        e = sampleEdges(texcoord);
        texcoord = fma(vec2(0.0, -2.0), invResolution, texcoord);
    }
    float offset = fma(-(255.0 / 127.0), searchLength(e.gr, 0.0), 3.25);
    return fma(invResolution.y, offset, texcoord.y);
}

float searchYDown(vec2 texcoord, float end, vec2 invResolution) {
    vec2 e = vec2(1.0, 0.0);
    for (int i = 0; i < p_maxSearchSteps; i++) {
        if (texcoord.y >= end || e.r <= 0.8281 || e.g > 0.0) break;
        e = sampleEdges(texcoord);
        texcoord = fma(vec2(0.0, 2.0), invResolution, texcoord);
    }
    float offset = fma(-(255.0 / 127.0), searchLength(e.gr, 0.5), 3.25);
    return fma(-invResolution.y, offset, texcoord.y);
}

vec2 area(vec2 dist, float e1, float e2, float offset) {
    vec2 texcoord = vec2(SMAA_AREATEX_MAX_DISTANCE) * round(4.0 * vec2(e1, e2)) + dist;
    texcoord = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
    texcoord.y = SMAA_AREATEX_SUBTEX_SIZE * offset + texcoord.y;
    return sampleArea(texcoord);
}

void detectHorizontalCorner(inout vec2 weights, vec4 texcoord, vec2 d, vec2 invResolution) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    float denom = leftRight.x + leftRight.y;
    rounding = (denom > 0.0) ? (rounding / denom) : vec2(0.0);

    vec2 factor = vec2(1.0);
    factor.x -= rounding.x * sampleEdgesOffset(texcoord.xy, ivec2(0, 1), invResolution).r;
    factor.x -= rounding.y * sampleEdgesOffset(texcoord.zw, ivec2(1, 1), invResolution).r;
    factor.y -= rounding.x * sampleEdgesOffset(texcoord.xy, ivec2(0, -2), invResolution).r;
    factor.y -= rounding.y * sampleEdgesOffset(texcoord.zw, ivec2(1, -2), invResolution).r;
    weights *= clamp(factor, 0.0, 1.0);
}

void detectVerticalCorner(inout vec2 weights, vec4 texcoord, vec2 d, vec2 invResolution) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    float denom = leftRight.x + leftRight.y;
    rounding = (denom > 0.0) ? (rounding / denom) : vec2(0.0);

    vec2 factor = vec2(1.0);
    factor.x -= rounding.x * sampleEdgesOffset(texcoord.xy, ivec2(1, 0), invResolution).g;
    factor.x -= rounding.y * sampleEdgesOffset(texcoord.zw, ivec2(1, 1), invResolution).g;
    factor.y -= rounding.x * sampleEdgesOffset(texcoord.xy, ivec2(-2, 0), invResolution).g;
    factor.y -= rounding.y * sampleEdgesOffset(texcoord.zw, ivec2(-2, 1), invResolution).g;
    weights *= clamp(factor, 0.0, 1.0);
}

void main() {
    vec2 invResolution = vec2(p_invWidth, p_invHeight);
    // 和官方 SMAA 一样，Blend pass 直接消费 VS 传下来的 texcoord。
    vec2 texcoord = i_pos;
    vec2 pixcoord = texcoord / invResolution;

    vec4 offset0 = texcoord.xyxy + invResolution.xyxy * vec4(-0.25, -0.125, 1.25, -0.125);
    vec4 offset1 = texcoord.xyxy + invResolution.xyxy * vec4(-0.125, -0.25, -0.125, 1.25);
    vec4 offset2 = vec4(offset0.xz, offset1.yw) +
        invResolution.xxyy * vec4(-2.0, 2.0, -2.0, 2.0) * float(p_maxSearchSteps);

    vec4 weights = vec4(0.0);
    vec2 e = sampleEdges(texcoord);

    if (e.g > 0.0) {
        vec2 d;
        vec3 coords;
        coords.x = searchXLeft(offset0.xy, offset2.x, invResolution);
        coords.y = offset1.y;
        d.x = coords.x;

        float e1 = sampleEdges(coords.xy).r;

        coords.z = searchXRight(offset0.zw, offset2.y, invResolution);
        d.y = coords.z;

        d = abs(round(vec2(d.x, d.y) / invResolution.x - pixcoord.xx));

        float e2 = sampleEdges(coords.zy + vec2(1.0, 0.0) * invResolution).r;
        weights.rg = area(sqrt(d), e1, e2, 0.0);

        vec4 cornerCoord = vec4(coords.x, texcoord.y, coords.z, texcoord.y);
        detectHorizontalCorner(weights.rg, cornerCoord, d, invResolution);
    }

    if (e.r > 0.0) {
        vec2 d;
        vec3 coords;
        coords.y = searchYUp(offset1.xy, offset2.z, invResolution);
        coords.x = offset0.x;
        d.x = coords.y;

        float e1 = sampleEdges(coords.xy).g;

        coords.z = searchYDown(offset1.zw, offset2.w, invResolution);
        d.y = coords.z;

        d = abs(round(vec2(d.x, d.y) / invResolution.y - pixcoord.yy));

        float e2 = sampleEdges(coords.xz + vec2(0.0, 1.0) * invResolution).g;
        weights.ba = area(sqrt(d), e1, e2, 0.0);

        vec4 cornerCoord = vec4(texcoord.x, coords.y, texcoord.x, coords.z);
        detectVerticalCorner(weights.ba, cornerCoord, d, invResolution);
    }

    o_weights = weights;
}
