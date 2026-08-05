#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace dxvk::war3::platform {

/**
 * @brief 初始化 War3 运行时核心模块。
 *
 * 包含：
 * - `dxvk::war3::Initialize` 初始化；
 * - ShaderManager 热重载；
 * - NetEventHook 初始化。
 *
 * @param gameBase `Game.dll` 基址（为 0 时使用自动探测路径）。
 */
void InitializeRuntimeCore(uintptr_t gameBase);

/**
 * @brief 重置 War3 运行时核心模块。
 *
 * 包含：
 * - 渲染追踪器与缓存清理；
 * - 渲染器帧状态复位；
 * - 运行时状态机复位；
 * - NetEventHook 清理。
 */
void ResetRuntimeCore();

/**
 * @brief 绑定晚注入 / native 路线获取到的 `IDirect3DDevice9`。
 *
 * 该入口不依赖 `IDirect3DDevice9Ex`，仅把原生设备句柄交给
 * `NativeD3D9BackendRuntime`。
 */
void BindNativeShadowDevice(IDirect3DDevice9* device);

/**
 * @brief 驱动 native shadow backend 消费当前最新的 semantic submission frame。
 *
 * @param captureLiveState 先从当前 render-thread registries 发布一次 live
 *        semantic contract。Stage11/native 验证时需要它，避免只消费上一轮
 *        control-plane 或 EndFrame 才补建的旧 frame。
 * @param maxExtraBuildPasses 对小型 pending/in-progress semantic build 追加的
 *        有界推进次数。用于追上 root/unit supplemented frame，不能作为无界
 *        同步 build。
 *
 * @returns 本次是否成功生成了至少一个 native backend submission record。
 */
bool DriveNativeShadowBackend(bool captureLiveState = false,
                              uint32_t maxExtraBuildPasses = 0);

/**
 * @brief 执行 native shadow backend 当前已准备好的 draw queue。
 *
 * 该入口与 `DriveNativeShadowBackend()` 分离，方便晚注入宿主在真正的
 * native shadow pass 时机执行 draw，而不是在语义 frame 构建时误画。
 */
bool ExecuteNativeShadowBackendPreparedFrame();

} // namespace dxvk::war3::platform

