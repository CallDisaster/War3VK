// WarVK shadow caster push-constant interface constants.
//
// Single source of truth shared by the CPU packer
// (src/d3d9/d3d9_war3_shadow.cpp, ShadowCasterPushConstants::flags) and the
// caster vertex shader (war3_shadow_caster_vert.vert). Pure preprocessor
// macros so both the C++ and the GLSL front-end accept this header.
// AutoTest/test_shadow_caster_interface_constants_static.py pins both sides
// to these values.
#ifndef WAR3_SHADOW_CASTER_INTERFACE_H
#define WAR3_SHADOW_CASTER_INTERFACE_H

// flags low bits: shadow route selection.
#define WAR3_SHADOW_CASTER_FLAG_USE_BLEND 0x1u
#define WAR3_SHADOW_CASTER_FLAG_INDEXED_BLEND 0x2u
#define WAR3_SHADOW_CASTER_FLAG_ALPHA_TEST 0x4u
#define WAR3_SHADOW_CASTER_FLAG_HASH_ALPHA 0x8u
#define WAR3_SHADOW_CASTER_FLAG_STAGE1_TERRAIN 0x10u
#define WAR3_SHADOW_CASTER_FLAG_POINT_SHADOW_LINEAR_DEPTH 0x20u
// flags high bits: GPU-skin direct-input (VS-B1) route.
#define WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_DIRECT_INPUT 0x40u
#define WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_NO_FALLBACK 0x80u

// GPU-skin layout metadata packed into flags bits 8..23.
#define WAR3_SHADOW_CASTER_GPU_SKIN_OUTPUT_FORMAT_SHIFT 8u
#define WAR3_SHADOW_CASTER_GPU_SKIN_LAYOUT_GENERATION_SHIFT 12u
#define WAR3_SHADOW_CASTER_GPU_SKIN_UV_LAYER_COUNT_SHIFT 16u
#define WAR3_SHADOW_CASTER_GPU_SKIN_METADATA_MASK 0x000fff00u

// outputFormat=2, layoutGeneration=1, sourceUvLayerCount=1:
// (2u << 8) | (1u << 12) | (1u << 16)
#define WAR3_SHADOW_CASTER_GPU_SKIN_FORMAT2_LAYOUT1_UV1 0x00011200u

#endif // WAR3_SHADOW_CASTER_INTERFACE_H
