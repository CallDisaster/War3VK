#pragma once
// WarVK JASS 常量

// 默认不启用原生绑定（避免地图加载期 native 校验失败）
// 如需 native 方式，请在地图脚本侧自行 #define WARVK_USE_NATIVE 1

#ifdef WarVK_JapiPlaceHolder
#error "[WarVK] 自定义 WarVK_JapiPlaceHolder 被视作禁止行为"
#else
#define WarVK_JapiPlaceHolder call ConvertRacePref(0) YDNL return
#endif

// 抗锯齿模式
#define WARVK_AAMODE_NONE 0
#define WARVK_AAMODE_FXAA 1
#define WARVK_AAMODE_SMAA_LOW 2
#define WARVK_AAMODE_SMAA_MEDIUM 3
#define WARVK_AAMODE_SMAA_HIGH 4
#define WARVK_AAMODE_SMAA_ULTRA 5

// 阴影调试模式
#define WARVK_SHADOW_DEBUG_NONE 0
#define WARVK_SHADOW_DEBUG_CASCADES 1
#define WARVK_SHADOW_DEBUG_FACTOR 2
#define WARVK_SHADOW_DEBUG_DEPTH 3

// 描边模式
#define WARVK_OUTLINE_MODE_FILL 0
#define WARVK_OUTLINE_MODE_SILHOUETTE 1

//阴影调试
#define SHADOW_DEBUG_CASCADES 1
