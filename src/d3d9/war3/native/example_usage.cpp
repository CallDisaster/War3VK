// example_usage.cpp
// War3 Native Renderer - 使用示例
// 
// 本文件展示了如何使用native渲染器替换原版游戏函数
// 包括Hook、测试和性能监控等示例代码。

#include "war3_native_renderer.h"
#include <cstdio>
#include <chrono>

// ============================================================================
// 示例1：基本Hook替换
// ============================================================================

namespace example1 {

// 原版函数指针（假设通过Detours保存）
using CWorld_RenderScene_Func = int(__thiscall*)(CWorldFrameWar3*);
using RenderWorld_DispatchStage_Func = int(__thiscall*)(
    CWorldFrameWar3*, RenderStage, RenderCategory, RenderCategory, int);

CWorld_RenderScene_Func original_CWorld_RenderScene = nullptr;
RenderWorld_DispatchStage_Func original_RenderWorld_DispatchStage = nullptr;

/**
 * @brief Hooked CWorld_RenderScene - 基本Hook示例
 */
int __thiscall Hooked_CWorld_RenderScene(CWorldFrameWar3* world) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // 调用native实现
    int result = war3::native::Native_CWorld_RenderScene(world);
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime).count();
    
    printf("CWorld_RenderScene completed in %lld us\n", duration);
    
    return result;
}

/**
 * @brief Hooked RenderWorld_DispatchStage - 阶段Hook示例
 */
int __thiscall Hooked_RenderWorld_DispatchStage(
    CWorldFrameWar3* world,
    RenderStage stageId,
    RenderCategory category,
    RenderCategory renderCategory,
    int unknown
) {
    // 在特定阶段添加调试输出
    if (stageId == RenderStage::Stage9_TerrainShadow6) {
        printf("Rendering Units stage\n");
    }
    
    return war3::native::Native_RenderWorld_DispatchStage(
        world, stageId, category, renderCategory, unknown);
}

/**
 * @brief 安装Hook
 */
void InstallHooks() {
    printf("Installing native renderer hooks...\n");
    
    // 使用Detours/MHook等库安装Hook
    // 示例伪代码：
    // DetourAttach(&original_CWorld_RenderScene, Hooked_CWorld_RenderScene);
    // DetourAttach(&original_RenderWorld_DispatchStage, Hooked_RenderWorld_DispatchStage);
    
    printf("Hooks installed successfully\n");
}

/**
 * @brief 卸载Hook
 */
void UninstallHooks() {
    printf("Uninstalling native renderer hooks...\n");
    
    // DetourDetach(&original_CWorld_RenderScene, Hooked_CWorld_RenderScene);
    // DetourDetach(&original_RenderWorld_DispatchStage, Hooked_RenderWorld_DispatchStage);
    
    printf("Hooks uninstalled successfully\n");
}

} // namespace example1

// ============================================================================
// 示例2：性能监控和统计
// ============================================================================

namespace example2 {

struct RenderStats {
    int64_t totalTime;
    int64_t frameCount;
    int64_t unitsRendered;
    int64_t buildingsRendered;
    int64_t effectsRendered;
    
    RenderStats() : totalTime(0), frameCount(0),
        unitsRendered(0), buildingsRendered(0), effectsRendered(0) {}
};

static RenderStats g_stats;

/**
 * @brief Hooked CWorld_RenderScene - 性能监控版本
 */
int __thiscall Monitored_CWorld_RenderScene(CWorldFrameWar3* world) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    int result = war3::native::Native_CWorld_RenderScene(world);
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime).count();
    
    g_stats.totalTime += duration;
    g_stats.frameCount++;
    
    // 每60帧输出一次统计
    if (g_stats.frameCount % 60 == 0) {
        printf("=== Render Stats ===\n");
        printf("Frames: %lld\n", g_stats.frameCount);
        printf("Avg time: %.2f ms\n", g_stats.totalTime / 1000.0 / g_stats.frameCount);
        printf("Units: %lld\n", g_stats.unitsRendered);
        printf("Buildings: %lld\n", g_stats.buildingsRendered);
        printf("Effects: %lld\n", g_stats.effectsRendered);
        printf("==================\n");
    }
    
    return result;
}

/**
 * @brief Hooked WorldObjects_RenderGroup - 对象计数版本
 */
int __userpurge Monitored_WorldObjects_RenderGroup(
    CWorldFrameWar3* world,
    RenderCategory category,
    WorldGroupIndex groupIdx
) {
    int result = war3::native::Native_WorldObjects_RenderGroup(
        world, category, groupIdx);
    
    // 统计不同类型的对象数量
    switch (groupIdx) {
        case WorldGroupIndex::Group0:
            g_stats.unitsRendered++;
            break;
        case WorldGroupIndex::Group1:
            g_stats.buildingsRendered++;
            break;
        case WorldGroupIndex::Group2:
            g_stats.effectsRendered++;
            break;
    }
    
    return result;
}

/**
 * @brief 重置统计
 */
void ResetStats() {
    g_stats = RenderStats();
    printf("Stats reset\n");
}

/**
 * @brief 获取统计信息
 */
const RenderStats& GetStats() {
    return g_stats;
}

} // namespace example2

// ============================================================================
// 示例3：条件渲染（调试模式）
// ============================================================================

namespace example3 {

static bool g_debugMode = false;
static bool g_vulkanMode = false;

/**
 * @brief 切换调试模式
 */
void SetDebugMode(bool enabled) {
    g_debugMode = enabled;
    printf("Debug mode: %s\n", enabled ? "ON" : "OFF");
}

/**
 * @brief 切换Vulkan模式
 */
void SetVulkanMode(bool enabled) {
    g_vulkanMode = enabled;
    printf("Vulkan mode: %s\n", enabled ? "ON" : "OFF");
}

/**
 * @brief 混合渲染函数
 */
int __thiscall Hybrid_CWorld_RenderScene(CWorldFrameWar3* world) {
    // 条件1：调试模式使用native实现（便于调试）
    if (g_debugMode) {
        printf("[DEBUG] Using native renderer\n");
        return war3::native::Native_CWorld_RenderScene(world);
    }
    
    // 条件2：Vulkan模式使用native实现（性能优化）
    if (g_vulkanMode) {
        printf("[VULKAN] Using native renderer\n");
        return war3::native::Native_CWorld_RenderScene(world);
    }
    
    // 条件3：其他情况使用原版实现
    printf("[DEFAULT] Using original renderer\n");
    // return original_CWorld_RenderScene(world);
    return war3::native::Native_CWorld_RenderScene(world);
}

} // namespace example3

// ============================================================================
// 示例4：阶段过滤和自定义
// ============================================================================

namespace example4 {

struct StageFilter {
    bool skyBox;
    bool terrain;
    bool units;
    bool buildings;
    bool effects;
    bool shadows;
    bool transparent;
    
    StageFilter() : skyBox(true), terrain(true), units(true),
        buildings(true), effects(true), shadows(true), transparent(true) {}
};

static StageFilter g_filter;

/**
 * @brief 设置阶段过滤器
 */
void SetStageFilter(const StageFilter& filter) {
    g_filter = filter;
    printf("Stage filter updated\n");
}

/**
 * @brief Hooked RenderWorld_DispatchStage - 阶段过滤版本
 */
int __thiscall Filtered_RenderWorld_DispatchStage(
    CWorldFrameWar3* world,
    RenderStage stageId,
    RenderCategory category,
    RenderCategory renderCategory,
    int unknown
) {
    // 根据过滤器决定是否跳过该阶段
    switch (stageId) {
        case RenderStage::Stage0_PreRenderContext:
            if (!g_filter.skyBox) return 0;
            break;
        case RenderStage::Stage1_TerrainShadow0:
            if (!g_filter.terrain) return 0;
            break;
        case RenderStage::Stage9_TerrainShadow6:
            if (!g_filter.units) return 0;
            break;
        case RenderStage::Stage2_TerrainShadow1:
            if (!g_filter.buildings) return 0;
            break;
        case RenderStage::Stage8_TerrainShadow10:
            if (!g_filter.effects) return 0;
            break;
        case RenderStage::Stage12_Group1:
            if (!g_filter.shadows) return 0;
            break;
        default:
            // 透明对象
            if ((int)stageId >= 10 && !g_filter.transparent) return 0;
            break;
    }
    
    return war3::native::Native_RenderWorld_DispatchStage(
        world, stageId, category, renderCategory, unknown);
}

} // namespace example4

// ============================================================================
// 示例5：错误处理和回退
// ============================================================================

namespace example5 {

static int g_errorCount = 0;
static const int MAX_ERRORS = 10;

/**
 * @brief 带错误处理的渲染函数
 */
int __thiscall Safe_CWorld_RenderScene(CWorldFrameWar3* world) {
    __try {
        int result = war3::native::Native_CWorld_RenderScene(world);
        g_errorCount = 0; // 重置错误计数
        return result;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_errorCount++;
        printf("!!! RENDER ERROR !!! Count: %d\n", g_errorCount);
        
        if (g_errorCount < MAX_ERRORS) {
            // 尝试继续使用native实现
            printf("Retrying native renderer...\n");
            return war3::native::Native_CWorld_RenderScene(world);
        } else {
            // 回退到原版实现
            printf("!!! FALLING BACK TO ORIGINAL RENDERER !!!\n");
            // return original_CWorld_RenderScene(world);
            return 0;
        }
    }
}

} // namespace example5

// ============================================================================
// 主函数 - 演示所有示例
// ============================================================================

int main() {
    printf("=== War3 Native Renderer - Usage Examples ===\n\n");
    
    // 示例1：基本Hook
    printf("Example 1: Basic Hook\n");
    example1::InstallHooks();
    printf("\n");
    
    // 示例2：性能监控
    printf("Example 2: Performance Monitoring\n");
    printf("Rendering with stats...\n");
    // 模拟60帧渲染
    for (int i = 0; i < 60; i++) {
        // CWorldFrameWar3* world = ...; // 从游戏获取
        // example2::Monitored_CWorld_RenderScene(world);
    }
    printf("\n");
    
    // 示例3：条件渲染
    printf("Example 3: Conditional Rendering\n");
    example3::SetDebugMode(true);
    example3::SetVulkanMode(false);
    printf("\n");
    
    // 示例4：阶段过滤
    printf("Example 4: Stage Filtering\n");
    example4::StageFilter filter;
    filter.effects = false; // 禁用特效
    filter.shadows = false; // 禁用阴影
    example4::SetStageFilter(filter);
    printf("\n");
    
    // 示例5：错误处理
    printf("Example 5: Error Handling\n");
    printf("Renderer with error handling\n");
    printf("\n");
    
    printf("=== All Examples Completed ===\n");
    
    // 清理
    example1::UninstallHooks();
    
    return 0;
}

// ============================================================================
// 实际游戏集成示例（伪代码）
// ============================================================================

/*
// 在DXVK初始化时
void DXVK_InitNativeRenderer() {
    printf("Initializing War3 Native Renderer\n");
    
    // 获取原版函数地址
    original_CWorld_RenderScene = (CWorld_RenderScene_Func)0x6F3681C0;
    original_RenderWorld_DispatchStage = (RenderWorld_DispatchStage_Func)0x6F363020;
    
    // 安装Hook
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_CWorld_RenderScene, 
                example1::Hooked_CWorld_RenderScene);
    DetourAttach(&(PVOID&)original_RenderWorld_DispatchStage,
                example1::Hooked_RenderWorld_DispatchStage);
    DetourTransactionCommit();
    
    printf("Native renderer initialized\n");
}

// 在DXVK关闭时
void DXVK_ShutdownNativeRenderer() {
    printf("Shutting down War3 Native Renderer\n");
    
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)original_CWorld_RenderScene,
                example1::Hooked_CWorld_RenderScene);
    DetourDetach(&(PVOID&)original_RenderWorld_DispatchStage,
                example1::Hooked_RenderWorld_DispatchStage);
    DetourTransactionCommit();
    
    printf("Native renderer shutdown\n");
}
*/
