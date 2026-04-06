#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 1) uniform texture2D u_alphaTex;

layout(push_constant) uniform OutlineConstants {
    layout(offset = 0) mat4 mvp;
    layout(offset = 64) uint paletteOffset;
    layout(offset = 68) uint blendCount;
    layout(offset = 72) uint flags;
    layout(offset = 76) float alphaRef;
    layout(offset = 80) uint samplerIndex;
    // Padding 12 bytes automatically handled by offset
    layout(offset = 96) vec4 outlineColor;
} constants;

layout(location = 0) out vec4 fragColor;

void main() {
    if ((constants.flags & 0x4u) != 0u) {
        float alpha = texture(sampler2D(u_alphaTex, s_samplers[nonuniformEXT(constants.samplerIndex)]), v_uv).a;
        if (alpha < constants.alphaRef) {
            discard;
        }
    }
    fragColor = constants.outlineColor;
}
