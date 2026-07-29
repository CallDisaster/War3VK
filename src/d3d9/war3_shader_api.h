/**
 * @file war3_shader_api.h
 * @brief War3MapReforge 外部 Shader API 公开头文件
 * 
 * @details
 * 本文件定义了 War3MapReforge 渲染器对外暴露的 Shader API，允许二开开发者：
 * - 访问原生渲染数据（帧缓冲、深度、Draw Call）
 * - 注册渲染事件回调
 * - 禁用内置渲染效果
 * 
 * @version 1.1.0
 * @date 2024-12-21
 * 
 * @copyright Copyright (c) 2024 War3MapReforge
 */

#pragma once

#include <cstdint>
#include <functional>

// API 导出宏
// 在 d3d9.dll 内部编译时使用 dllexport
// 外部使用时（二开开发者）使用 dllimport
#if defined(WAR3_SHADER_API_INTERNAL) || defined(DXVK_NATIVE)
    #define WAR3_SHADER_API
#elif defined(WAR3_SHADER_API_EXPORTS)
    #define WAR3_SHADER_API __declspec(dllexport)
#else
    #define WAR3_SHADER_API __declspec(dllimport)
#endif

namespace war3shader {

//=============================================================================
// 前向声明
//=============================================================================

struct RenderContext;
struct FrameBuffer;
struct DrawCall;
struct CameraData;
struct LightData;

//=============================================================================
// 版本信息
//=============================================================================

/**
 * @brief API 版本号
 */
constexpr uint32_t API_VERSION_MAJOR = 1;
constexpr uint32_t API_VERSION_MINOR = 2;
constexpr uint32_t API_VERSION_PATCH = 2;

/**
 * @brief 获取 API 版本号
 * @return 格式: (major << 16) | (minor << 8) | patch
 */
WAR3_SHADER_API uint32_t GetAPIVersion();

//=============================================================================
// 渲染事件 ID
//=============================================================================

/**
 * @brief 渲染事件类型
 * 
 * @details
 * 事件按渲染顺序触发：
 * 1. FRAME_BEGIN - 帧开始
 * 2. WORLD_RENDER_BEGIN - 世界渲染开始（地形、单位等）
 * 3. WORLD_RENDER_END - 世界渲染结束
 * 4. SHADOW_PASS_BEGIN - 阴影 Pass 开始（如果启用）
 * 5. SHADOW_PASS_END - 阴影 Pass 结束
 * 6. UI_RENDER_BEGIN - UI 渲染开始
 * 7. UI_RENDER_END - UI 渲染结束
 * 8. FRAME_END - 帧结束
 */
enum class RenderEventID : uint32_t {
    FRAME_BEGIN = 0,           ///< 帧开始，可在此初始化每帧数据
    WORLD_RENDER_BEGIN,        ///< 世界渲染开始前
    WORLD_RENDER_END,          ///< 世界渲染结束后，原生颜色/深度已准备好
    SHADOW_PASS_BEGIN,         ///< 阴影 Pass 开始前（内置阴影）
    SHADOW_PASS_END,           ///< 阴影 Pass 结束后
    POST_PROCESS_BEGIN,        ///< 后处理开始前
    POST_PROCESS_END,          ///< 后处理结束后
    UI_RENDER_BEGIN,           ///< UI 渲染开始前
    UI_RENDER_END,             ///< UI 渲染结束后
    FRAME_END,                 ///< 帧结束，最终输出
    
    COUNT                      ///< 事件总数（内部使用）
};

//=============================================================================
// 渲染阶段语义
//=============================================================================

/**
 * @brief 渲染阶段（对外稳定语义）
 *
 * @note 仅用于“阶段语义”，不等价于内部渲染状态机的细分阶段。
 */
enum class RenderStageId : uint32_t {
    Unknown = 0,
    Frame,         ///< 帧级阶段（Begin/End）
    World,         ///< 世界渲染阶段
    Shadow,        ///< 阴影相关阶段
    Outline,       ///< 描边阶段
    PostProcess,   ///< 后处理阶段
    Overlay,       ///< 叠加/屏幕空间覆盖（调试/测试用）
    Ui,            ///< UI 阶段
};

/**
 * @brief 阶段能力位掩码（RenderContext::stageCaps）
 */
enum StageCaps : uint32_t {
    STAGE_CAP_NONE       = 0,
    STAGE_CAP_COLOR      = 1u << 0,  ///< 可读颜色缓冲
    STAGE_CAP_DEPTH      = 1u << 1,  ///< 可读深度缓冲
    STAGE_CAP_DRAW_CALLS = 1u << 2,  ///< 可读 DrawCall 列表
    STAGE_CAP_LIGHTS     = 1u << 3,  ///< 可读光源列表
    STAGE_CAP_SHADOWMAP  = 1u << 4,  ///< 可读阴影贴图（预留）
    STAGE_CAP_UI         = 1u << 5,  ///< UI 相关阶段
};

//=============================================================================
// 数据结构定义
//=============================================================================

/**
 * @brief 4x4 矩阵（行优先）
 */
struct alignas(16) Matrix4x4 {
    float m[4][4];
    
    float* operator[](int row) { return m[row]; }
    const float* operator[](int row) const { return m[row]; }
};

/**
 * @brief 3D 向量
 */
struct Vector3 {
    float x, y, z;
};

/**
 * @brief 4D 向量
 */
struct Vector4 {
    float x, y, z, w;
};

/**
 * @brief 2D 向量（UI）
 */
struct UiVec2 {
    float x, y;
};

/**
 * @brief UI 颜色（线性空间 0-1）
 */
struct UiColor {
    float r, g, b, a;
};

/**
 * @brief UI 矩形
 */
struct UiRect {
    UiVec2 min;
    UiVec2 max;
};

/**
 * @brief 视口信息
 */
struct Viewport {
    uint32_t x;         ///< 左上角 X
    uint32_t y;         ///< 左上角 Y
    uint32_t width;     ///< 宽度
    uint32_t height;    ///< 高度
    float minDepth;     ///< 最小深度 (0.0)
    float maxDepth;     ///< 最大深度 (1.0)
};

/**
 * @brief 相机数据
 * 
 * @details
 * 包含当前帧的相机变换矩阵，所有矩阵均为行优先存储。
 */
struct CameraData {
    Matrix4x4 view;             ///< View 矩阵
    Matrix4x4 projection;       ///< Projection 矩阵
    Matrix4x4 viewProjection;   ///< View * Projection
    Matrix4x4 invView;          ///< View 逆矩阵
    Matrix4x4 invProjection;    ///< Projection 逆矩阵
    Matrix4x4 invViewProjection;///< ViewProjection 逆矩阵
    
    Vector3 position;           ///< 相机世界坐标
    Vector3 forward;            ///< 相机前方向
    Vector3 up;                 ///< 相机上方向
    Vector3 right;              ///< 相机右方向
    
    float nearPlane;            ///< 近裁剪面
    float farPlane;             ///< 远裁剪面
    float fov;                  ///< 视场角（弧度）
    float aspectRatio;          ///< 宽高比
    
    Viewport viewport;          ///< 视口
};

/**
 * @brief 光源类型
 */
enum class LightType : uint32_t {
    DIRECTIONAL = 0,    ///< 方向光（太阳）
    POINT = 1,          ///< 点光源
    SPOT = 2,           ///< 聚光灯（暂不支持）
};

/**
 * @brief 光源标志位
 */
enum LightFlags : uint32_t {
    LIGHT_FLAG_NONE         = 0,
    LIGHT_FLAG_CASTS_SHADOW = 1 << 0,  ///< 投射阴影
    LIGHT_FLAG_ACTIVE       = 1 << 1,  ///< 激活状态
};

/**
 * @brief 光源数据
 * 
 * @note ABI 稳定：使用 flags 位掩码代替 bool
 */
struct LightData {
    LightType type;         ///< 光源类型
    Vector3 position;       ///< 位置（点光源/聚光灯）
    Vector3 direction;      ///< 方向（方向光/聚光灯）
    Vector4 color;          ///< 颜色 (RGB + 强度)
    float range;            ///< 范围（点光源）
    float intensity;        ///< 强度
    uint32_t flags;         ///< LightFlags 组合
};

/**
 * @brief 顶点格式
 */
enum class VertexFormat : uint32_t {
    UNKNOWN = 0,
    FLOAT2 = 1,     ///< 2x float
    FLOAT3 = 2,     ///< 3x float
    FLOAT4 = 3,     ///< 4x float
    SHORT2 = 4,     ///< 2x short
    SHORT4 = 5,     ///< 4x short
    UBYTE4 = 6,     ///< 4x ubyte
    UBYTE4N = 7,    ///< 4x ubyte normalized
};

/**
 * @brief 图元类型
 */
enum class PrimitiveType : uint32_t {
    TRIANGLES = 0,
    TRIANGLE_STRIP = 1,
    TRIANGLE_FAN = 2,
    LINES = 3,
    LINE_STRIP = 4,
    POINTS = 5,
};

/**
 * @brief Draw Call 标志位
 */
enum DrawCallFlags : uint32_t {
    DRAWCALL_FLAG_NONE           = 0,
    DRAWCALL_FLAG_32BIT_INDEX    = 1 << 0,  ///< 使用 32 位索引
    DRAWCALL_FLAG_ALPHA_TEST     = 1 << 1,  ///< 启用 Alpha 测试
    DRAWCALL_FLAG_DEPTH_WRITE    = 1 << 2,  ///< 写入深度
    DRAWCALL_FLAG_DEPTH_TEST     = 1 << 3,  ///< 测试深度
    DRAWCALL_FLAG_ALPHA_BLEND    = 1 << 4,  ///< 启用 Alpha 混合
    DRAWCALL_FLAG_ADDITIVE_BLEND = 1 << 5,  ///< 加法混合（常见特效）
    DRAWCALL_FLAG_BLOOM_HINT     = 1 << 6,  ///< Bloom 提示（由外部高阶 API 标记）
};

/**
 * @brief DrawCall 分类掩码（RenderContext::drawCalls[].layerMask）
 */
enum DrawCallLayerMask : uint32_t {
    DRAWCALL_LAYER_NONE        = 0,
    DRAWCALL_LAYER_TERRAIN     = 1 << 0,
    DRAWCALL_LAYER_UNIT        = 1 << 1,
    DRAWCALL_LAYER_BUILDING    = 1 << 2,
    DRAWCALL_LAYER_DESTRUCTIBLE= 1 << 3,
    DRAWCALL_LAYER_ITEM        = 1 << 4,
    DRAWCALL_LAYER_EFFECT      = 1 << 5,
    DRAWCALL_LAYER_DOODAD      = 1 << 6,
    DRAWCALL_LAYER_WORLD       = 1 << 7,
    DRAWCALL_LAYER_TRANSPARENT = 1 << 8,
    DRAWCALL_LAYER_UI          = 1 << 9,
    DRAWCALL_LAYER_POSTPROCESS = 1 << 10,
};

/**
 * @brief Draw Call 信息
 * 
 * @details
 * 描述一次渲染调用的所有参数，包括几何数据、变换矩阵和材质信息。
 * 
 * @note ABI 稳定：所有布尔字段使用 flags 位掩码表示
 */
struct DrawCall {
    // 几何数据
    const void* vertexData;         ///< 顶点数据指针
    uint32_t vertexCount;           ///< 顶点数量
    uint32_t vertexStride;          ///< 顶点步长（字节）
    VertexFormat positionFormat;    ///< 位置格式
    uint32_t positionOffset;        ///< 位置偏移
    
    const void* indexData;          ///< 索引数据指针（可为 null）
    uint32_t indexCount;            ///< 索引数量
    
    PrimitiveType primitiveType;    ///< 图元类型
    
    // 变换
    Matrix4x4 worldMatrix;          ///< 世界变换矩阵
    
    // 材质
    uint32_t textureHandle;         ///< 主纹理句柄
    uint32_t textureWidth;          ///< 纹理宽度
    uint32_t textureHeight;         ///< 纹理高度
    
    // 渲染状态（使用 flags 位掩码）
    uint32_t flags;                 ///< DrawCallFlags 组合
    float alphaRef;                 ///< Alpha 参考值
    
    // 分类
    uint32_t layerMask;             ///< 图层掩码（地形/单位/特效等）
    uint32_t objectId;              ///< 对象唯一标识 (jHandle/UnitID)
};

/**
 * @brief 纹理格式
 */
enum class TextureFormat : uint32_t {
    UNKNOWN = 0,
    R8G8B8A8_UNORM = 1,
    R8G8B8A8_SRGB = 2,
    B8G8R8A8_UNORM = 3,
    R16G16B16A16_FLOAT = 4,
    R32G32B32A32_FLOAT = 5,
    D16_UNORM = 6,
    D24_UNORM_S8_UINT = 7,
    D32_FLOAT = 8,
};

/**
 * @brief 帧缓冲信息
 * 
 * @details
 * 描述一个帧缓冲（颜色或深度），包含数据指针和格式信息。
 */
struct FrameBuffer {
    const void* data;           ///< 数据指针（GPU 资源句柄）
    uint32_t width;             ///< 宽度
    uint32_t height;            ///< 高度
    TextureFormat format;       ///< 格式
    uint32_t mipLevels;         ///< Mip 级别数
    uint32_t arrayLayers;       ///< 数组层数
    
    // Vulkan 特定（高级用户）
    void* vkImage;              ///< VkImage 句柄
    void* vkImageView;          ///< VkImageView 句柄
    uint32_t vkLayout;          ///< VkImageLayout
};

/**
 * @brief 渲染上下文
 * 
 * @details
 * 事件回调中传递的上下文，包含当前帧的所有渲染数据。
 */
struct RenderContext {
    // 当前事件与阶段语义
    RenderEventID eventId;      ///< 当前回调事件
    RenderStageId stageId;      ///< 阶段语义
    uint32_t stageCaps;         ///< 阶段能力（StageCaps 位掩码）

    // 时间
    float gameTime;             ///< 游戏时间 (0-24)
    float frameTime;            ///< 帧时间（秒）
    float frameTimeCounter;     ///< 累计时间
    uint64_t frameIndex;        ///< 帧序号
    
    // 相机
    CameraData camera;          ///< 相机数据
    
    // 帧缓冲
    FrameBuffer colorBuffer;    ///< 原生颜色缓冲
    FrameBuffer depthBuffer;    ///< 原生深度缓冲
    
    // 光源
    LightData sunLight;         ///< 太阳光（方向光）
    uint32_t pointLightCount;   ///< 点光源数量
    const LightData* pointLights; ///< 点光源数组
    
    // Draw Calls（仅在特定事件可用）
    uint32_t drawCallCount;     ///< Draw Call 数量
    const DrawCall* drawCalls;  ///< Draw Call 数组
    
    // 设置
    bool nativeShadowsEnabled;  ///< 内置阴影是否启用
    bool nativeLightingEnabled; ///< 内置光照是否启用
    bool nativePostProcessEnabled; ///< 内置后处理是否启用
};

//=============================================================================
// 回调类型定义
//=============================================================================

/**
 * @brief 渲染事件回调函数类型（C 风格）
 * 
 * @warning ABI 稳定性说明：
 * - 使用 C 风格函数指针，避免 std::function 跨编译器 ABI 不兼容
 * - userData 用于传递上下文，由调用方负责生命周期管理
 * 
 * @param eventId 事件 ID
 * @param context 渲染上下文指针（仅在回调期间有效，勿缓存跨帧使用）
 * @param userData 用户数据指针
 */
typedef void (*War3RenderEventFn)(RenderEventID eventId, const RenderContext* context, void* userData);

//=============================================================================
// API 函数
//=============================================================================

/**
 * @brief 注册渲染事件回调（C 风格）
 * 
 * @warning 生命周期说明：
 * - context 指针仅在回调执行期间有效
 * - 请勿缓存 context 或其内部数组指针（pointLights/drawCalls）跨帧使用
 * - 回调中可安全调用 Register/Unregister（不会死锁）
 * 
 * @param eventId 要监听的事件 ID
 * @param callback C 风格回调函数指针
 * @param userData 用户数据指针（会原样传递给回调）
 * @return 成功返回回调 ID（用于注销），失败返回 0
 * 
 * @code{.cpp}
 * // 示例：注册世界渲染结束事件
 * void MyCallback(war3shader::RenderEventID id, const war3shader::RenderContext* ctx, void* user) {
 *     MyData* data = (MyData*)user;
 *     // 获取相机矩阵
 *     auto& view = ctx->camera.view;
 *     // ...
 * }
 * 
 * MyData myData;
 * auto callbackId = war3shader::RegisterRenderEvent(
 *     war3shader::RenderEventID::WORLD_RENDER_END,
 *     MyCallback,
 *     &myData
 * );
 * @endcode
 */
WAR3_SHADER_API uint32_t RegisterRenderEvent(RenderEventID eventId, War3RenderEventFn callback, void* userData);

/**
 * @brief 注销渲染事件回调
 * 
 * @param callbackId 注册时返回的回调 ID
 * @return 成功返回 true
 */
WAR3_SHADER_API bool UnregisterRenderEvent(uint32_t callbackId);

/**
 * @brief 禁用内置阴影渲染
 * 
 * @param disable true 禁用，false 启用
 * @return 之前的状态
 */
WAR3_SHADER_API bool DisableNativeShadows(bool disable);

/**
 * @brief 禁用内置光照计算
 * 
 * @param disable true 禁用，false 启用
 * @return 之前的状态
 */
WAR3_SHADER_API bool DisableNativeLighting(bool disable);

/**
 * @brief 禁用内置后处理
 * 
 * @param disable true 禁用，false 启用
 * @return 之前的状态
 */
WAR3_SHADER_API bool DisableNativePostProcess(bool disable);

/**
 * @brief 描边句柄管理（高阶 API）
 */
WAR3_SHADER_API void AddOutlineHandle(uint32_t handle);
// 将最近一次渲染捕获的对象加入描边（用于验证句柄链路）
WAR3_SHADER_API bool AddOutlineLastRenderHandle();
// 通过渲染对象索引加入描边（用于快速验证互通）
WAR3_SHADER_API bool AddOutlineHandleByIndex(uint32_t index);
WAR3_SHADER_API void RemoveOutlineHandle(uint32_t handle);
WAR3_SHADER_API void ClearOutlineHandles();
WAR3_SHADER_API uint32_t GetOutlineHandleCount();
WAR3_SHADER_API bool IsOutlineHandle(uint32_t handle);
// 调试：强制描边全部对象 / 强制启用描边设置
WAR3_SHADER_API bool SetOutlineAllObjectsEnabled(bool enabled);
WAR3_SHADER_API bool SetOutlineForceEnabled(bool enabled);

/**
 * @brief Bloom 句柄管理（高阶 API）
 * 
 * @param boost 亮度增强系数（>0 触发 DRAWCALL_FLAG_BLOOM_HINT）
 */
WAR3_SHADER_API void AddBloomHandle(uint32_t handle, float boost);
WAR3_SHADER_API void RemoveBloomHandle(uint32_t handle);
WAR3_SHADER_API void ClearBloomHandles();
WAR3_SHADER_API float GetBloomBoost(uint32_t handle);
WAR3_SHADER_API uint32_t GetBloomHandleCount();

/**
 * @brief 查询渲染对象追踪状态（用于验证逻辑层与渲染层数据互通）
 */
WAR3_SHADER_API bool IsRenderHandleTracked(uint32_t handle);
WAR3_SHADER_API uint32_t GetRenderObjectCount();
WAR3_SHADER_API uint32_t GetLastRenderHandle();
WAR3_SHADER_API uint32_t GetRenderObjectHandleByIndex(uint32_t index);
WAR3_SHADER_API uint32_t GetRenderObjectKindByIndex(uint32_t index);
WAR3_SHADER_API uint32_t GetRenderObjectRawcodeByIndex(uint32_t index);

/**
 * @brief 点光源管理（高阶 API）
 */
WAR3_SHADER_API int32_t AddPointLight(
    float x, float y, float z,
    float range,
    float r, float g, float b,
    float intensity,
    float shadowIntensity);
WAR3_SHADER_API bool UpdatePointLight(
    int32_t id,
    float x, float y, float z,
    float range,
    float r, float g, float b,
    float intensity);
WAR3_SHADER_API bool UpdatePointLightEx(
    int32_t id,
    float x, float y, float z,
    float range,
    float r, float g, float b,
    float intensity,
    float shadowIntensity);
WAR3_SHADER_API bool SetPointLightShadowIntensity(int32_t id, float shadowIntensity);
WAR3_SHADER_API bool RemovePointLight(int32_t id);
WAR3_SHADER_API void ClearPointLights();
WAR3_SHADER_API uint32_t GetPointLightCount();

/**
 * @brief 渲染设置 API（高阶控制）
 */
WAR3_SHADER_API bool SetLightingEnabled(bool enabled);
WAR3_SHADER_API bool SetSunDirection(float x, float y, float z);
WAR3_SHADER_API bool SetSunColor(float r, float g, float b);
WAR3_SHADER_API bool SetSunIntensity(float intensity);
WAR3_SHADER_API bool SetShadowEnabled(bool enabled);
WAR3_SHADER_API bool SetShadowStrength(float strength);
WAR3_SHADER_API bool SetShadowBias(float bias);
WAR3_SHADER_API bool SetShadowCasterBias(float constantBias, float slopeBias, float clamp);
WAR3_SHADER_API bool SetShadowDepthRangeMargin(float margin);
WAR3_SHADER_API bool SetShadowPcfRadius(float radius);
WAR3_SHADER_API bool SetShadowPcfKernel(uint32_t kernel);
WAR3_SHADER_API bool SetShadowPcfRotate(bool enabled);
WAR3_SHADER_API bool SetShadowPcfRotateMode(uint32_t mode);
WAR3_SHADER_API bool SetShadowPcssSearchKernel(uint32_t kernel);
WAR3_SHADER_API bool SetShadowCascadeBiasScale(float scale);
WAR3_SHADER_API bool SetShadowPcfCascadeRadiusScale(float scale);
WAR3_SHADER_API bool SetShadowAlphaHashed(bool enabled);
WAR3_SHADER_API bool SetShadowAlphaUseMip(bool enabled);
WAR3_SHADER_API bool SetShadowAlphaMipLodBias(float bias);
WAR3_SHADER_API bool SetShadowAlphaFarAlphaRefBias(float bias);
WAR3_SHADER_API bool SetNativeShadowMode(uint32_t mode);
WAR3_SHADER_API bool SetShadowLockSun(bool enabled);
WAR3_SHADER_API bool SetShadowLockSunTime(float time01);
WAR3_SHADER_API bool SetShadowStableSnapWhenSunMoving(bool enabled);
WAR3_SHADER_API bool SetShadowDebugMode(uint32_t mode);
WAR3_SHADER_API bool SetShadowReceiverMode(uint32_t mode);
WAR3_SHADER_API bool SetShadowNormalBiasScale(float scale);
WAR3_SHADER_API bool SetShadowFilterMode(uint32_t mode);
WAR3_SHADER_API bool SetShadowAltitudeMode(uint32_t mode);
WAR3_SHADER_API bool SetShadowLengthScale(float scale);
WAR3_SHADER_API bool SetShadowMaxLengthScale(float scale);
WAR3_SHADER_API bool SetShadowRimIntensity(float intensity);
WAR3_SHADER_API bool SetShadowRimPower(float power);
WAR3_SHADER_API bool SetPointLightsEnabled(bool enabled);
WAR3_SHADER_API bool SetPointShadowEnabled(bool enabled);
WAR3_SHADER_API bool SetPointShadowBias(float bias);
WAR3_SHADER_API bool SetVolumetricLightEnabled(bool enabled);
WAR3_SHADER_API bool SetVolumetricLightParams(
    float intensity, float density, float weight, float decay,
    uint32_t sampleCount);
WAR3_SHADER_API bool SetVolumetricLightFade(
    float fadeNear, float fadeFar, float maxRayDistance);
WAR3_SHADER_API bool SetVolumetricHeightFog(
    float baseHeight, float falloff, float strength);
WAR3_SHADER_API bool SetVolumetricResolutionDivisor(uint32_t divisor);
WAR3_SHADER_API bool SetOutlineEnabled(bool enabled);
WAR3_SHADER_API bool SetOutlineWidth(float widthPx);
WAR3_SHADER_API bool SetOutlineColor(float r, float g, float b, float a);
WAR3_SHADER_API bool SetOutlineMode(uint32_t mode);
WAR3_SHADER_API bool SetOutlineVisibility(bool showVisible, bool showOccluded);
WAR3_SHADER_API bool SetOutlineScreenSpace(bool enabled);
WAR3_SHADER_API bool SetPostFxEnabled(bool enabled);
WAR3_SHADER_API bool SetExposure(float exposure);
WAR3_SHADER_API bool SetBloomEnabled(bool enabled);
WAR3_SHADER_API bool SetBloomParams(float threshold, float softKnee, float intensity);
WAR3_SHADER_API bool SetAcesEnabled(bool enabled);
WAR3_SHADER_API bool SetSsaoEnabled(bool enabled);
WAR3_SHADER_API bool SetSsaoParams(float radiusPx, float strength, float bias, float power);
WAR3_SHADER_API bool SetAaMode(uint32_t mode);
WAR3_SHADER_API bool SetFxaaParams(float subpix, float edgeThreshold, float edgeThresholdMin);
WAR3_SHADER_API bool SetSmaaParams(float threshold, int32_t search, int32_t diagSearch);
WAR3_SHADER_API bool SetDayNightEnabled(bool enabled);
WAR3_SHADER_API bool SetDayNightMinFactor(float minFactor);
WAR3_SHADER_API bool SetDayNightAmbient(
    float dayR, float dayG, float dayB,
    float nightR, float nightG, float nightB);

/**
 * @brief 获取当前渲染上下文
 * 
 * @details
 * 可在事件回调外部调用，获取最近一帧的渲染数据。
 * 
 * @return 渲染上下文指针，如果尚未开始渲染则返回 nullptr
 */
WAR3_SHADER_API const RenderContext* GetCurrentRenderContext();

/**
 * @brief 获取原生颜色缓冲
 * 
 * @details
 * 返回世界渲染完成后、后处理之前的原生颜色缓冲。
 * 
 * @return 帧缓冲指针，如果不可用返回 nullptr
 */
WAR3_SHADER_API const FrameBuffer* GetRawColorBuffer();

/**
 * @brief 获取原生深度缓冲
 * 
 * @return 帧缓冲指针，如果不可用返回 nullptr
 */
WAR3_SHADER_API const FrameBuffer* GetRawDepthBuffer();

/**
 * @brief 获取当前帧的 Draw Call 列表
 * 
 * @param outCount 输出 Draw Call 数量
 * @return Draw Call 数组指针，如果不可用返回 nullptr
 */
WAR3_SHADER_API const DrawCall* GetDrawCalls(uint32_t* outCount);

/**
 * @brief 获取相机数据
 * 
 * @return 相机数据指针，如果不可用返回 nullptr
 */
WAR3_SHADER_API const CameraData* GetCameraData();

/**
 * @brief 获取太阳光数据
 * 
 * @return 光源数据指针
 */
WAR3_SHADER_API const LightData* GetSunLight();

/**
 * @brief 获取点光源列表
 * 
 * @param outCount 输出点光源数量
 * @return 点光源数组指针
 */
WAR3_SHADER_API const LightData* GetPointLights(uint32_t* outCount);

/**
 * @brief 获取游戏时间
 * 
 * @return 游戏时间 (0-24)
 */
WAR3_SHADER_API float GetGameTime();

/**
 * @brief 获取帧时间
 * 
 * @return 帧时间（秒）
 */
WAR3_SHADER_API float GetFrameTime();

//=============================================================================
// UI API（ImGui Overlay）
//=============================================================================

/**
 * @brief UI 绘制层级
 */
enum class UiLayer : uint32_t {
    Background = 0,
    Foreground = 1,
};

/**
 * @brief UI 绘制回调
 * 
 * @note 回调在 ImGui 帧内触发，可安全调用 UiDraw* 或 ImGui 原生 API。
 */
typedef void (*War3UiDrawFn)(const RenderContext* context, void* userData);

/**
 * @brief 注册 UI 绘制回调
 * @return 成功返回回调 ID，用于注销
 */
WAR3_SHADER_API uint32_t RegisterUiDrawCallback(War3UiDrawFn callback, void* userData);

/**
 * @brief 注销 UI 绘制回调
 */
WAR3_SHADER_API bool UnregisterUiDrawCallback(uint32_t callbackId);

/**
 * @brief 获取当前 UI 画布尺寸（像素）
 */
WAR3_SHADER_API UiVec2 UiGetDisplaySize();

/**
 * @brief 切换绘制层级（背景/前景）
 */
WAR3_SHADER_API void UiSetLayer(UiLayer layer);

/**
 * @brief 基础图元绘制
 */
WAR3_SHADER_API void UiDrawLine(UiVec2 p1, UiVec2 p2, UiColor color, float thickness);
WAR3_SHADER_API void UiDrawRect(UiRect rect, UiColor color, float rounding, float thickness);
WAR3_SHADER_API void UiFillRect(UiRect rect, UiColor color, float rounding);
WAR3_SHADER_API void UiDrawCircle(UiVec2 center, float radius, UiColor color, int segments, float thickness);
WAR3_SHADER_API void UiFillCircle(UiVec2 center, float radius, UiColor color, int segments);
WAR3_SHADER_API void UiDrawTriangle(UiVec2 p1, UiVec2 p2, UiVec2 p3, UiColor color, float thickness);
WAR3_SHADER_API void UiFillTriangle(UiVec2 p1, UiVec2 p2, UiVec2 p3, UiColor color);
WAR3_SHADER_API void UiDrawPolyline(const UiVec2* points, uint32_t count, UiColor color, bool closed, float thickness);
WAR3_SHADER_API void UiDrawBezierCubic(UiVec2 p1, UiVec2 p2, UiVec2 p3, UiVec2 p4, UiColor color, float thickness, int segments);

/**
 * @brief 文本绘制
 */
WAR3_SHADER_API void UiDrawText(UiVec2 pos, UiColor color, const char* text);

/**
 * @brief 图像绘制（ImTextureID 透传）
 */
WAR3_SHADER_API void UiDrawImage(void* textureId, UiVec2 min, UiVec2 max, UiVec2 uvMin, UiVec2 uvMax, UiColor tint);

/**
 * @brief 裁剪区域
 */
WAR3_SHADER_API void UiPushClipRect(UiVec2 min, UiVec2 max, bool intersect);
WAR3_SHADER_API void UiPopClipRect();

/**
 * @brief 获取 ImGuiContext（高级用法）
 * 
 * @note 仅在 UI 回调或 UI 帧内有效。
 */
WAR3_SHADER_API void* GetImGuiContext();

//=============================================================================
// 高级 API（Vulkan 直接访问）
//=============================================================================

/**
 * @brief 获取 Vulkan 设备句柄
 * 
 * @warning 高级 API，需要熟悉 Vulkan
 * @return VkDevice 句柄
 */
WAR3_SHADER_API void* GetVulkanDevice();

/**
 * @brief 获取 Vulkan 物理设备句柄
 * 
 * @return VkPhysicalDevice 句柄
 */
WAR3_SHADER_API void* GetVulkanPhysicalDevice();

/**
 * @brief 获取 Vulkan 实例句柄
 * 
 * @return VkInstance 句柄
 */
WAR3_SHADER_API void* GetVulkanInstance();

/**
 * @brief 获取 Vulkan 命令缓冲（当前帧）
 * 
 * @warning 仅在事件回调中有效
 * @return VkCommandBuffer 句柄
 */
WAR3_SHADER_API void* GetVulkanCommandBuffer();

} // namespace war3shader

//=============================================================================
// 内部 API（供渲染器调用，二开开发者无需关注）
//=============================================================================

// 前向声明 dxvk 命名空间（在 war3shader 外部）
namespace dxvk {
    struct War3PipelineInput;
}

namespace war3shader {
namespace internal {

/**
 * @brief 检查是否禁用内置阴影（内部使用）
 */
bool IsNativeShadowsDisabled();

/**
 * @brief 检查是否禁用内置光照（内部使用）
 */
bool IsNativeLightingDisabled();

/**
 * @brief 检查是否禁用内置后处理（内部使用）
 */
bool IsNativePostProcessDisabled();

/**
 * @brief 是否存在任何渲染事件监听者（内部使用）
 */
bool HasAnyRenderListeners();

/**
 * @brief 分发渲染事件（内部使用）
 */
void DispatchRenderEvent(RenderEventID eventId);

/**
 * @brief 更新渲染上下文（内部使用）
 */
void UpdateRenderContext(const ::dxvk::War3PipelineInput& input);

/**
 * @brief 更新帧缓冲信息（内部使用）
 */
void UpdateFrameBuffers(
    const void* colorData, uint32_t colorWidth, uint32_t colorHeight, TextureFormat colorFormat,
    const void* depthData, uint32_t depthWidth, uint32_t depthHeight, TextureFormat depthFormat,
    void* colorImage, void* colorView, uint32_t colorLayout,
    void* depthImage, void* depthView, uint32_t depthLayout);

/**
 * @brief 设置 Vulkan 句柄（内部使用）
 */
void SetVulkanHandles(void* instance, void* physicalDevice, void* device);

/**
 * @brief 设置 Vulkan 命令缓冲（内部使用）
 */
void SetVulkanCommandBuffer(void* commandBuffer);

/**
 * @brief 帧开始初始化（内部使用）
 */
void BeginFrame();

/**
 * @brief UI 系统内部调用（启动/派发/收尾）
 */
void SetImGuiContext(void* context);
void BeginUiFrame();
void DispatchUiCallbacks();
void FlushUiCommands();
void EndUiFrame();

} // namespace internal
} // namespace war3shader
