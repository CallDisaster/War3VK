#pragma once
// WarVK JASS constants.
//
// This package intentionally does not declare new natives. Maps can enter the
// game first, then WarVK talks to the DLL through existing low-frequency
// Blizzard natives that are wrapped by the clean-room WarVK command bridge.

// Bridge command prefix consumed by the DLL after WarVK is loaded.
#define WARVK_BRIDGE_PREFIX "warvk:"

// Mid-game loading policy. Pure JASS cannot call Win32 LoadLibrary by itself,
// so this path asks an already-present YDWE Lua runtime to run our clean
// WarVK-owned loader module. If that runtime is unavailable, polling will keep
// reporting "bridge not ready" instead of pretending the DLL can self-load.
#define WARVK_LOADER_MODE_EXTERNAL 0
#define WARVK_LOADER_MODE_YDWE_EXEC_LUA 1
#define WARVK_LOADER_MODE_MEMORY_BRIDGE 2
#define WARVK_LOADER_MODE_AI_BRIDGE 3
#define WARVK_LOADER_MODE_DEFAULT WARVK_LOADER_MODE_AI_BRIDGE

#define WARVK_LOAD_USE_YDWE_EXEC_LUA 1
#define WARVK_LOAD_USE_MEMORY_BRIDGE 0
#define WARVK_LOAD_USE_AI_BRIDGE 1
#define WARVK_LOADER_AI_SCRIPT "warvk.ai"
#define WARVK_LOADER_AI_SCRIPT_IMPORTED "war3mapImported\\warvk.ai"
#define WARVK_LOADER_EXEC_LUA_COMMAND "exec-lua:warvk_loader"
#define WARVK_LOADER_MAX_REQUESTS 1

// Readiness polling policy after the load request has been emitted.
#define WARVK_AUTO_INIT_DELAY_SEC 1.00
#define WARVK_LOAD_RETRY_PERIOD 0.25
#define WARVK_LOAD_MAX_ATTEMPTS 80

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

    constant integer WARVK_OBJECT_NONE = 0
    constant integer WARVK_OBJECT_POINT_LIGHT = 1
    constant integer WARVK_OBJECT_LIGHTNING = 2

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
