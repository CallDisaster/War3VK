// war3_debug.cpp - 调试日志实现

#include "war3_debug.h"
#include <windows.h>
#include <cstdio>

namespace dxvk::war3 {

void Log(LogLevel level, const char* fmt, ...) {
    char buffer[2048];
    
    // 前缀
    const char* prefix = "";
    switch (level) {
        case LogLevel::Debug:   prefix = "[DEBUG] "; break;
        case LogLevel::Info:    prefix = "[INFO] "; break;
        case LogLevel::Warning: prefix = "[WARN] "; break;
        case LogLevel::Error:   prefix = "[ERROR] "; break;
    }
    
    // 格式化消息
    va_list args;
    va_start(args, fmt);
    int offset = snprintf(buffer, sizeof(buffer), "DXVK War3: %s", prefix);
    vsnprintf(buffer + offset, sizeof(buffer) - offset, fmt, args);
    va_end(args);
    
    // 输出
    OutputDebugStringA(buffer);
}

void DebugPrint(const char* fmt, ...) {
    char buffer[2048];
    
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    OutputDebugStringA(buffer);
}

} // namespace dxvk::war3
