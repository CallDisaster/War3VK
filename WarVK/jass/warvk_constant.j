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
