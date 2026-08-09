/**
 * @file war3_shaderpack.h
 * @brief War3MapReforge ShaderPack 执行框架公开头文件
 * 
 * @details
 * 本文件定义了 ShaderPack 的加载、执行和参数控制 API。在显式启用
 * WARVK_ENABLE_RAW_SHADERPACK_DEV 的开发构建中，二开开发者可以：
 * - 从文件夹加载自定义 shader pack
 * - 设置 uniform 参数（float/vec/mat）
 * - 绑定自定义纹理（LUT、噪声、mask）
 * - 热重载 shader
 * 
 * ## 设计原则
 * - **最小侵入**：固定 descriptor 布局、固定 UBO、固定 pass 格式
 * - **ABI 稳定**：使用 C 风格函数指针和 uint32_t flags
 * - **易于调试**：提供错误日志和编译错误信息
 * 
 * ## ShaderPack 目录结构
 * ```
 * my_shaderpack/
 * ├── pack.json           # 配置文件（可选）
 * ├── composite.frag      # 主后处理 shader
 * ├── composite1.frag     # 额外 pass（可选）
 * ├── composite2.frag     # 额外 pass（可选）
 * └── textures/
 *     ├── lut.png         # LUT 纹理
 *     └── noise.png       # 噪声纹理
 * ```
 * 
 * ## Shader 输入（固定布局）
 * ```glsl
 * // 采样器堆 (set=0, binding=0)
 * layout(set = 0, binding = 0) uniform sampler s_samplers[];
 *
 * // UBO (set=1, binding=0)
 * layout(set = 1, binding = 0, std140) uniform War3PackUBO {
 *     mat4 u_view;
 *     mat4 u_proj;
 *     mat4 u_invView;
 *     mat4 u_invProj;
 *     vec3 u_cameraPos;
 *     float u_gameTime;      // 0-24
 *     vec2 u_resolution;
 *     float u_frameTime;
 *     float u_totalTime;
 * };
 * 
 * // 纹理 (set=1, binding=1-7)
 * layout(set = 1, binding = 1) uniform texture2D u_colorTex;   // 原生颜色
 * layout(set = 1, binding = 2) uniform texture2D u_depthTex;   // 原生深度
 * layout(set = 1, binding = 3) uniform texture2D u_tex0;       // 用户纹理槽 0
 * layout(set = 1, binding = 4) uniform texture2D u_tex1;       // 用户纹理槽 1
 * layout(set = 1, binding = 5) uniform texture2D u_tex2;       // 用户纹理槽 2
 * layout(set = 1, binding = 6) uniform texture2D u_tex3;       // 用户纹理槽 3
 * layout(set = 1, binding = 7) uniform texture2D u_prevPass;   // 上一个 pass 输出
 * 
 * // push constants (采样器索引 + 用户参数)
 * layout(push_constant) uniform War3PackPush {
 *     uint u_samplerColor;
 *     uint u_samplerDepth;
 *     uint u_samplerTex0;
 *     uint u_samplerTex1;
 *     uint u_samplerTex2;
 *     uint u_samplerTex3;
 *     uint u_samplerPrev;
 *     uint u_samplerPad;
 *     vec4 u_params0;   // 用户参数槽 0
 *     vec4 u_params1;   // 用户参数槽 1
 *     vec4 u_params2;   // 用户参数槽 2
 *     vec4 u_params3;   // 用户参数槽 3
 * };
 *
 * // 采样示例（GL_EXT_samplerless_texture_functions）
 * // vec4 color = texture(sampler2D(u_colorTex, s_samplers[u_samplerColor]), uv);
 * ```
 * 
 * @version 1.2003
 * @date 2024-12-21
 * 
 * @copyright Copyright (c) 2024 War3MapReforge
 */

#pragma once

#include <cstdint>

// API 导出宏（与 war3_shader_api.h 共用）
#if defined(WAR3_SHADER_API_INTERNAL) || defined(DXVK_NATIVE)
    #define WAR3_PACK_API
#elif defined(WAR3_SHADER_API_EXPORTS)
    #define WAR3_PACK_API __declspec(dllexport)
#else
    #define WAR3_PACK_API __declspec(dllimport)
#endif

namespace war3shader {

//=============================================================================
// 常量定义
//=============================================================================

constexpr uint32_t SHADERPACK_MAX_PASSES = 4;       ///< 最大 pass 数量
constexpr uint32_t SHADERPACK_MAX_TEXTURES = 4;     ///< 最大用户纹理数量
constexpr uint32_t SHADERPACK_MAX_PARAMS = 4;       ///< 最大用户参数槽数量

//=============================================================================
// 错误码
//=============================================================================

/**
 * @brief ShaderPack 错误码
 */
enum class ShaderPackError : uint32_t {
    OK = 0,                     ///< 成功
    NOT_FOUND,                  ///< pack 目录不存在
    INVALID_FORMAT,             ///< pack.json 格式错误
    SHADER_NOT_FOUND,           ///< shader 文件不存在
    SHADER_COMPILE_ERROR,       ///< shader 编译失败
    TEXTURE_NOT_FOUND,          ///< 纹理文件不存在
    TEXTURE_LOAD_ERROR,         ///< 纹理加载失败
    TOO_MANY_PASSES,            ///< pass 数量超过限制
    NO_PACK_LOADED,             ///< 未加载任何 pack
    INVALID_SLOT,               ///< 无效的槽位索引
    INTERNAL_ERROR,             ///< 内部错误
    POLICY_DISABLED,            ///< 发布构建禁用原始 SPIR-V 加载
};

/**
 * @brief ShaderPack 状态标志
 */
enum ShaderPackFlags : uint32_t {
    PACK_FLAG_NONE      = 0,
    PACK_FLAG_LOADED    = 1 << 0,   ///< pack 已加载
    PACK_FLAG_ENABLED   = 1 << 1,   ///< pack 已启用
    PACK_FLAG_HAS_ERROR = 1 << 2,   ///< pack 有错误
    PACK_FLAG_POLICY_DISABLED = 1 << 3, ///< 发布策略禁止原始 SPIR-V
};

//=============================================================================
// 纹理槽定义
//=============================================================================

/**
 * @brief 用户纹理槽
 */
enum class TextureSlot : uint32_t {
    TEX0 = 0,       ///< 用户纹理槽 0
    TEX1 = 1,       ///< 用户纹理槽 1
    TEX2 = 2,       ///< 用户纹理槽 2
    TEX3 = 3,       ///< 用户纹理槽 3
};

/**
 * @brief 参数槽
 */
enum class ParamSlot : uint32_t {
    PARAMS0 = 0,    ///< vec4 参数槽 0
    PARAMS1 = 1,    ///< vec4 参数槽 1
    PARAMS2 = 2,    ///< vec4 参数槽 2
    PARAMS3 = 3,    ///< vec4 参数槽 3
};

//=============================================================================
// ShaderPack 信息
//=============================================================================

/**
 * @brief ShaderPack 信息结构
 */
struct ShaderPackInfo {
    char name[64];              ///< pack 名称
    char path[256];             ///< pack 路径
    uint32_t passCount;         ///< pass 数量
    uint32_t flags;             ///< ShaderPackFlags
    ShaderPackError lastError;  ///< 最后一个错误
};

//=============================================================================
// API 函数
//=============================================================================

/**
 * @brief 加载 ShaderPack
 * 
 * @param path pack 目录路径（相对于游戏目录或绝对路径）
 * @return 错误码
 * 
 * @code{.cpp}
 * auto err = war3shader::LoadShaderPack("shaders/my_pack");
 * if (err != war3shader::ShaderPackError::OK) {
 *     const char* msg = war3shader::GetLastShaderError();
 *     printf("加载失败: %s\n", msg);
 * }
 * @endcode
 */
WAR3_PACK_API ShaderPackError LoadShaderPack(const char* path);

/**
 * @brief 重新加载当前 ShaderPack
 * 
 * @return 错误码
 */
WAR3_PACK_API ShaderPackError ReloadShaderPack();

/**
 * @brief 保存当前 ShaderPack 配置（用于调试/热更新）
 * 
 * @return 错误码
 */
WAR3_PACK_API ShaderPackError SaveShaderPack();

/**
 * @brief 启用/禁用 ShaderPack
 * 
 * @param enable true 启用，false 禁用
 * @return 之前的状态
 */
WAR3_PACK_API bool EnableShaderPack(bool enable);

/**
 * @brief 检查 ShaderPack 是否已加载
 * 
 * @return true 如果已加载
 */
WAR3_PACK_API bool IsShaderPackLoaded();

/**
 * @brief 当前二进制是否允许从磁盘加载原始 SPIR-V ShaderPack
 *
 * 发布构建始终返回 false；该能力只能由显式开发编译宏启用，不能通过
 * 环境变量或运行时配置绕过。
 */
WAR3_PACK_API bool IsRawShaderPackLoadingEnabled();

/**
 * @brief 获取 ShaderPack 信息
 * 
 * @param outInfo 输出信息结构
 * @return 错误码
 */
WAR3_PACK_API ShaderPackError GetShaderPackInfo(ShaderPackInfo* outInfo);

//=============================================================================
// 纹理绑定
//=============================================================================

/**
 * @brief 从文件加载纹理并绑定到槽位
 * 
 * @param slot 纹理槽
 * @param path 纹理文件路径
 * @return 错误码
 *
 * @note 支持 PNG/JPG/TGA/BMP/GIF/HDR/PNM，相对路径默认以 pack 目录解析。
 * 
 * @code{.cpp}
 * war3shader::SetTextureFromFile(war3shader::TextureSlot::TEX0, "lut/color_grading.png");
 * @endcode
 */
WAR3_PACK_API ShaderPackError SetTextureFromFile(TextureSlot slot, const char* path);

/**
 * @brief 清除纹理槽
 * 
 * @param slot 纹理槽
 * @return 错误码
 */
WAR3_PACK_API ShaderPackError ClearTexture(TextureSlot slot);

//=============================================================================
// 参数设置
//=============================================================================

/**
 * @brief 设置 vec4 参数
 * 
 * @param slot 参数槽
 * @param x, y, z, w 参数值
 * @return 错误码
 * 
 * @code{.cpp}
 * // 设置 bloom 强度和阈值
 * war3shader::SetParamVec4(war3shader::ParamSlot::PARAMS0, 1.5f, 0.8f, 0.0f, 0.0f);
 * @endcode
 */
WAR3_PACK_API ShaderPackError SetParamVec4(ParamSlot slot, float x, float y, float z, float w);

/**
 * @brief 设置 float 参数（写入 vec4.x）
 * 
 * @param slot 参数槽
 * @param value 参数值
 * @return 错误码
 */
WAR3_PACK_API ShaderPackError SetParamFloat(ParamSlot slot, float value);

/**
 * @brief 获取 vec4 参数
 * 
 * @param slot 参数槽
 * @param outX, outY, outZ, outW 输出参数值
 * @return 错误码
 */
WAR3_PACK_API ShaderPackError GetParamVec4(ParamSlot slot, float* outX, float* outY, float* outZ, float* outW);

//=============================================================================
// 错误处理
//=============================================================================

/**
 * @brief 获取最后一个 shader 编译错误信息
 * 
 * @return 错误信息字符串，如果没有错误则返回空字符串
 * 
 * @note 返回的字符串在下次调用 Load/Reload 前有效
 */
WAR3_PACK_API const char* GetLastShaderError();

/**
 * @brief 获取错误码对应的描述
 * 
 * @param error 错误码
 * @return 错误描述字符串
 */
WAR3_PACK_API const char* GetErrorString(ShaderPackError error);

//=============================================================================
// 高级控制
//=============================================================================

/**
 * @brief 设置中间 RT 分辨率缩放
 * 
 * @param scale 缩放比例（0.25-2.0），1.0 = 原生分辨率
 * @return 错误码
 */
WAR3_PACK_API ShaderPackError SetRenderScale(float scale);

/**
 * @brief 获取当前中间 RT 分辨率缩放
 * 
 * @return 缩放比例
 */
WAR3_PACK_API float GetRenderScale();

/**
 * @brief 设置特定 pass 的启用状态
 * 
 * @param passIndex pass 索引（0-3）
 * @param enable true 启用，false 禁用
 * @return 错误码
 */
WAR3_PACK_API ShaderPackError SetPassEnabled(uint32_t passIndex, bool enable);

//=============================================================================
// 调试
//=============================================================================

/**
 * @brief 设置 shader 编译日志回调
 * 
 * @param callback 日志回调函数
 * @param userData 用户数据
 */
typedef void (*ShaderLogCallback)(const char* message, void* userData);
WAR3_PACK_API void SetShaderLogCallback(ShaderLogCallback callback, void* userData);

/**
 * @brief 获取 shader 编译统计
 * 
 * @param outCompileCount 编译次数
 * @param outErrorCount 错误次数
 */
WAR3_PACK_API void GetShaderStats(uint32_t* outCompileCount, uint32_t* outErrorCount);

//=============================================================================
// Shadow Receiver 控制
//=============================================================================

/**
 * @brief 启用/禁用 Vulkan Shadow Receiver 覆盖
 * 
 * @param enable true 使用 pack 的 shadow_receiver，false 使用内置阴影接收器
 * @return 之前的状态
 */
WAR3_PACK_API bool EnableShadowReceiverOverride(bool enable);

/**
 * @brief 是否启用 Vulkan Shadow Receiver 覆盖
 * 
 * @return true 已启用
 */
WAR3_PACK_API bool IsShadowReceiverOverrideEnabled();

} // namespace war3shader
