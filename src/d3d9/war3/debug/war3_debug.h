// war3_debug.h - 调试日志和工具
// 统一的日志输出接口

#pragma once

#include <cstdio>
#include <cstdarg>

namespace dxvk::war3 {

// 日志级别
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

// 日志输出
void Log(LogLevel level, const char* fmt, ...);

// 便捷宏
#define WAR3_LOG_DEBUG(fmt, ...) ::dxvk::war3::Log(::dxvk::war3::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define WAR3_LOG_INFO(fmt, ...)  ::dxvk::war3::Log(::dxvk::war3::LogLevel::Info, fmt, ##__VA_ARGS__)
#define WAR3_LOG_WARN(fmt, ...)  ::dxvk::war3::Log(::dxvk::war3::LogLevel::Warning, fmt, ##__VA_ARGS__)
#define WAR3_LOG_ERROR(fmt, ...) ::dxvk::war3::Log(::dxvk::war3::LogLevel::Error, fmt, ##__VA_ARGS__)

// 一次性日志（只输出一次）
#define WAR3_LOG_ONCE(fmt, ...) do { \
    static bool logged = false; \
    if (!logged) { logged = true; WAR3_LOG_INFO(fmt, ##__VA_ARGS__); } \
} while(0)

// OutputDebugString 直接输出
void DebugPrint(const char* fmt, ...);

} // namespace dxvk::war3
