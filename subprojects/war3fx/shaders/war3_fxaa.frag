#version 460

#extension GL_EXT_nonuniform_qualifier : require

/**
 * FXAA 3.11 Fragment Shader
 * 
 * Based on NVIDIA FXAA 3.11 by Timothy Lottes
 * https://github.com/NVIDIAGameWorks/GraphicsSamples
 */

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D s_color;

layout(push_constant) uniform push_block {
    uint p_colorSampler;
    float p_invWidth;
    float p_invHeight;
    float p_qualitySubpix;
    float p_qualityEdgeThreshold;
    float p_qualityEdgeThresholdMin;
};

layout(location = 0) in vec2 i_pos;
layout(location = 0) out vec4 o_color;

#define FXAA_SPAN_MAX 8.0
#define FXAA_REDUCE_MIN (1.0/128.0)
#define FXAA_REDUCE_MUL (1.0/8.0)

float luminance(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec4 sampleColor(vec2 uv) {
    return texture(sampler2D(s_color, s_samplers[nonuniformEXT(p_colorSampler)]), uv);
}

void main() {
    // Robust UV calculation using screen coords
    vec2 invResolution = vec2(p_invWidth, p_invHeight);
    vec2 uv = gl_FragCoord.xy * invResolution;
    
    // Sample center and neighbors
    vec3 rgbM  = sampleColor(uv).rgb;
    vec3 rgbNW = sampleColor(uv + vec2(-1.0, -1.0) * invResolution).rgb;
    vec3 rgbNE = sampleColor(uv + vec2( 1.0, -1.0) * invResolution).rgb;
    vec3 rgbSW = sampleColor(uv + vec2(-1.0,  1.0) * invResolution).rgb;
    vec3 rgbSE = sampleColor(uv + vec2( 1.0,  1.0) * invResolution).rgb;
    
    // Compute luminance
    float lumM  = luminance(rgbM);
    float lumNW = luminance(rgbNW);
    float lumNE = luminance(rgbNE);
    float lumSW = luminance(rgbSW);
    float lumSE = luminance(rgbSE);
    
    // Compute edge direction
    float lumMin = min(lumM, min(min(lumNW, lumNE), min(lumSW, lumSE)));
    float lumMax = max(lumM, max(max(lumNW, lumNE), max(lumSW, lumSE)));
    float lumRange = lumMax - lumMin;
    
    // Early exit if not on edge
    if (lumRange < max(p_qualityEdgeThresholdMin, lumMax * p_qualityEdgeThreshold)) {
        o_color = vec4(rgbM, 1.0);
        return;
    }
    
    // Compute edge direction
    vec2 dir;
    dir.x = -((lumNW + lumNE) - (lumSW + lumSE));
    dir.y =  ((lumNW + lumSW) - (lumNE + lumSE));
    
    float dirReduce = max((lumNW + lumNE + lumSW + lumSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX), dir * rcpDirMin)) * invResolution;
    
    // Sample along edge
    vec3 rgbA = 0.5 * (
        sampleColor(uv + dir * (1.0/3.0 - 0.5)).rgb +
        sampleColor(uv + dir * (2.0/3.0 - 0.5)).rgb);
    
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        sampleColor(uv + dir * -0.5).rgb +
        sampleColor(uv + dir *  0.5).rgb);
    
    float lumB = luminance(rgbB);
    
    // Choose final color
    if ((lumB < lumMin) || (lumB > lumMax)) {
        o_color = vec4(rgbA, 1.0);
    } else {
        // Subpixel anti-aliasing
        float lumaAvg = (lumNW + lumNE + lumSW + lumSE) * 0.25;
        float subpixA = clamp(abs(lumaAvg - lumM) / lumRange, 0.0, 1.0);
        float subpixB = (-2.0 * subpixA + 3.0) * subpixA * subpixA;
        float subpixC = subpixB * subpixB * p_qualitySubpix;
        
        vec3 final = mix(rgbB, rgbM, subpixC);
        o_color = vec4(final, 1.0);
    }
}
