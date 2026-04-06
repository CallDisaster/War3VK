#pragma once

#include <cstdint>
#include <atomic>
#include <string>

namespace dxvk {

    /**
     * @brief War3VK 防盗卖警告系统
     * 
     * 在游戏第一帧渲染后向玩家显示免费下载提示。
     * 使用字符串混淆和运行时解密增加篡改难度。
     */
    class War3VKBranding {
    public:
        // 在适当时机调用（Present 或第一帧后）
        // 传入 gameTime 以便控制显示时机（例如 > 2.0s）
        static void TryShowBrandingMessage(float gameTime);

        // 强制显示（调试用）
        static void ForceShow();

        // 检查是否已显示
        static bool HasShown() { return s_shown.load(std::memory_order_relaxed); }

    private:
        // 解密并获取消息文本
        static std::string DecryptMessage(uint32_t index);

        // 实际调用 DisplayTextToPlayer
        static bool DisplayToLocalPlayer(const char* message, float displayTime);

        // 校验消息完整性（简单的哈希校验）
        static bool VerifyIntegrity();

        static std::atomic<bool> s_shown;
        static std::atomic<bool> s_initialized;
        static uint32_t s_frameCounter;
        
        // 延迟帧数（等待游戏完全初始化）
        static constexpr uint32_t kDelayFrames = 60; // 约 1 秒
    };

} // namespace dxvk
