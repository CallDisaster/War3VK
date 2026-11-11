#pragma once

#include <cstdint>
#include <string>

namespace dxvk::war3::startup {

/**
 * @brief 启动失败发生在哪一步。
 */
enum class FailureSite : uint32_t {
  Interface, ///< Direct3DCreate9：Vulkan 运行库、实例或显卡筛选失败
  Device,    ///< CreateDevice：已选中显卡，但创建 Vulkan 设备失败
};

/**
 * @brief 把启动失败告诉玩家，而不是只写日志。
 *
 * 收集 DXGI、注册表和 DXVK 记下的显卡信息，生成中文说明：原因、
 * 检测到的显卡和驱动版本、需要的最低驱动和下载地址。说明写到
 * 游戏目录 WarVK\Log\startup-error.txt，并在玩家的桌面上弹出对话框。
 *
 * 每个进程只报告一次。不抛异常。
 *
 * 对话框只在默认桌面（玩家正在看的桌面）弹出；AutoTest 的隔离桌面
 * 不弹，避免测试卡住。环境变量 DXVK_WAR3_STARTUP_DIALOG=0 关闭，
 * =force 在任何桌面都弹。
 */
void ReportStartupFailure(FailureSite site, const std::string& detail);

} // namespace dxvk::war3::startup
