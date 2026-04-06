#pragma once

#include <windows.h>

namespace dxvk::war3::hooks {

/**
 * @brief 统一安装并启用单个 MinHook 钩子。
 *
 * 行为约定：
 * 1. 校验 `target/detour/original` 非空；
 * 2. 可选调用 `MH_Initialize`（用于独立路径兜底）；
 * 3. 执行 `MH_CreateHook + MH_EnableHook`；
 * 4. 统一输出失败日志，避免各域日志格式漂移。
 *
 * @param target 目标函数地址。
 * @param detour Hook 函数地址。
 * @param original 原函数/trampoline 存储地址。
 * @param domain 域名（如 Render/UI/Shadow）。
 * @param hookName Hook 点名（用于日志）。
 * @param ensureInitialized 是否确保 MinHook 已初始化。
 * @param logSuccess 是否输出成功日志。
 * @return true 安装成功（含 already-created/enabled）；false 安装失败。
 */
bool InstallMinHook(LPVOID target, LPVOID detour, LPVOID* original,
                    const char* domain, const char* hookName,
                    bool ensureInitialized = false, bool logSuccess = false);

}  // namespace dxvk::war3::hooks

