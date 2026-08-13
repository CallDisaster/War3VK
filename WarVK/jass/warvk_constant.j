#pragma once
// WarVK JASS constants.
//
// This package intentionally does not declare new natives. WarVK is installed
// as the proxy d3d9.dll before Warcraft III starts, and maps talk to it through
// existing Blizzard natives wrapped by the clean-room WarVK command bridge.

// Bridge command prefix consumed by the proxy runtime.
#define WARVK_BRIDGE_PREFIX "warvk:"

// Antialiasing modes.
#define WARVK_AAMODE_NONE 0
#define WARVK_AAMODE_FXAA 1
#define WARVK_AAMODE_SMAA_LOW 2
#define WARVK_AAMODE_SMAA_MEDIUM 3
#define WARVK_AAMODE_SMAA_HIGH 4
#define WARVK_AAMODE_SMAA_ULTRA 5

// Shadow debug modes.
#define WARVK_SHADOW_DEBUG_NONE 0
#define WARVK_SHADOW_DEBUG_CASCADES 1
#define WARVK_SHADOW_DEBUG_FACTOR 2
#define WARVK_SHADOW_DEBUG_DEPTH 3

// Outline modes.
#define WARVK_OUTLINE_MODE_FILL 0
#define WARVK_OUTLINE_MODE_SILHOUETTE 1

// Compatibility alias used by older trigger metadata.
#define SHADOW_DEBUG_CASCADES 1

// WarVK JAPI v1 constants.
globals
    constant integer WARVK_PROTOCOL_VERSION = 1

    constant integer WARVK_FEATURE_SUN = 1
    constant integer WARVK_FEATURE_CSM = 2
    constant integer WARVK_FEATURE_POINT_LIGHT = 4
    constant integer WARVK_FEATURE_VOLUMETRIC = 8
    constant integer WARVK_FEATURE_OUTLINE = 16
    constant integer WARVK_FEATURE_BLOOM = 32
    constant integer WARVK_FEATURE_POSTFX = 64
    constant integer WARVK_FEATURE_AA = 128
    constant integer WARVK_FEATURE_DAY_NIGHT = 256
    constant integer WARVK_FEATURE_LIGHTNING = 512
    constant integer WARVK_FEATURE_MANAGED_OBJECT = 1024
    constant integer WARVK_FEATURE_TIME = 2048
    constant integer WARVK_FEATURE_STATS = 4096
    constant integer WARVK_FEATURE_MATH_CURVE = 8192
    constant integer WARVK_FEATURE_POLYLINE_CURVE = 16384
    constant integer WARVK_FEATURE_LOCAL_FOG = 32768

    // 体积光后端。LEGACY 是当前发布兼容路径；FROXEL_* 为 2.0 候选。
    constant integer WARVK_VOLUMETRIC_BACKEND_LEGACY = 0
    constant integer WARVK_VOLUMETRIC_BACKEND_FROXEL_MEDIUM = 1
    constant integer WARVK_VOLUMETRIC_BACKEND_FROXEL_HIGH = 2

    // 渲染光照时钟来源。只影响 WarVK 光照，不修改 Warcraft 玩法时间。
    constant integer WARVK_LIGHTING_CLOCK_GAME_TIME = 0
    constant integer WARVK_LIGHTING_CLOCK_HELD = 1
    constant integer WARVK_LIGHTING_CLOCK_INDEPENDENT = 2

    // 标量公式转 integer 的舍入方式。
    constant integer WARVK_MATH_ROUND_NEAREST = 0
    constant integer WARVK_MATH_ROUND_FLOOR = 1
    constant integer WARVK_MATH_ROUND_CEIL = 2
    constant integer WARVK_MATH_ROUND_TRUNCATE = 3

    constant integer WARVK_OBJECT_NONE = 0
    constant integer WARVK_OBJECT_POINT_LIGHT = 1
    constant integer WARVK_OBJECT_LIGHTNING = 2
    constant integer WARVK_OBJECT_LOCAL_FOG = 3

    // 闪电模板渲染模式。渲染器始终关闭深度写入；
    // 模式 1/3 仅开启相对场景的深度测试。
    // 0=Alpha 无深度  1=Alpha 深度测试  2=加法无深度  3=加法深度测试（推荐）
    constant integer WARVK_LIGHTNING_RENDER_ALPHA_NO_DEPTH = 0
    constant integer WARVK_LIGHTNING_RENDER_ALPHA_DEPTH = 1
    constant integer WARVK_LIGHTNING_RENDER_ADDITIVE_NO_DEPTH = 2
    constant integer WARVK_LIGHTNING_RENDER_ADDITIVE_DEPTH = 3

    // 公式曲线坐标模式。
    // OFFSET：公式返回 vec2(右偏移, 上偏移)，底层保留起终点直线。
    // LOCAL：公式返回 vec3(前进, 右偏移, 上偏移)，自动对齐起终点。
    // WORLD：公式直接返回世界坐标 vec3。
    constant integer WARVK_CURVE_COORDINATE_OFFSET = 0
    constant integer WARVK_CURVE_COORDINATE_LOCAL = 1
    constant integer WARVK_CURVE_COORDINATE_WORLD = 2

    constant integer WARVK_CURVE_COMPONENT_X = 0
    constant integer WARVK_CURVE_COMPONENT_Y = 1
    constant integer WARVK_CURVE_COMPONENT_Z = 2

    constant integer WARVK_ERROR_NONE = 0
    constant integer WARVK_ERROR_PAYLOAD_TOO_LONG = 1
    constant integer WARVK_ERROR_NON_ASCII = 2
    constant integer WARVK_ERROR_CONTROL_CHARACTER = 3
    constant integer WARVK_ERROR_EMPTY_TOKEN = 4
    constant integer WARVK_ERROR_TOO_MANY_ARGUMENTS = 5
    constant integer WARVK_ERROR_UNSUPPORTED_VERSION = 6
    constant integer WARVK_ERROR_MISSING_COMMAND = 7
    constant integer WARVK_ERROR_UNKNOWN_COMMAND = 8
    constant integer WARVK_ERROR_CARRIER_MISMATCH = 9
    constant integer WARVK_ERROR_ARGUMENT_COUNT = 10
    constant integer WARVK_ERROR_INVALID_INTEGER = 11
    constant integer WARVK_ERROR_INTEGER_OVERFLOW = 12
    constant integer WARVK_ERROR_INVALID_BOOLEAN = 13
    constant integer WARVK_ERROR_INVALID_ID = 14
    constant integer WARVK_ERROR_INVALID_REAL = 15
    constant integer WARVK_ERROR_REAL_OUT_OF_RANGE = 16
    constant integer WARVK_ERROR_BACKEND_UNAVAILABLE = 17
    constant integer WARVK_ERROR_UNSUPPORTED_FEATURE = 18
    constant integer WARVK_ERROR_BACKEND_REJECTED = 19
    constant integer WARVK_ERROR_INTERNAL = 20
    constant integer WARVK_ERROR_BACKEND_CONTRACT = 21
    constant integer WARVK_ERROR_INVALID_ARGUMENT_TYPE = 22
endglobals
