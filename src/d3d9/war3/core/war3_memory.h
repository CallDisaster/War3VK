// war3_memory.h - 内存安全读取工具
// 从 d3d9_war3_hook.cpp 提取的内存操作工具函数

#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace dxvk::war3 {

// 检查内存保护属性是否可读
bool IsReadableProtect(DWORD protect);

// 检查内存保护属性是否可执行
bool IsExecutableProtect(DWORD protect);

// 安全检查指定内存范围是否可读
bool IsReadableRange(const void* p, size_t size);

// 安全检查指定内存范围是否可执行
bool IsExecutableRange(const void* p, size_t size);

// ============================================================================
// 高性能版本（用于热路径）
// ============================================================================
// 说明：
// - IsReadableRange/SafeRead* 内部会频繁调用 VirtualQuery，在对象很多时开销很大。
// - Fast 版本使用“线程本地缓存 + 单次 VirtualQuery”来加速 4/8 字节的小范围读取。
// - 仅保证对“小范围读取”友好：若 size 跨越 region 边界会直接返回 false。
bool IsReadableRangeFast(const void* p, size_t size);

// 安全读取指定偏移的值
template<typename T>
inline bool SafeRead(const void* base, size_t offset, T& out) {
    const void* addr = reinterpret_cast<const char*>(base) + offset;
    if (!IsReadableRange(addr, sizeof(T)))
        return false;
    out = *reinterpret_cast<const T*>(addr);
    return true;
}

// Fast 安全读取（热路径）
template<typename T>
inline bool SafeReadFast(const void* base, size_t offset, T& out) {
    const void* addr = reinterpret_cast<const char*>(base) + offset;
    if (!IsReadableRangeFast(addr, sizeof(T)))
        return false;
    out = *reinterpret_cast<const T*>(addr);
    return true;
}

// 安全读取指针
inline bool SafeReadPtr(const void* base, size_t offset, void*& out) {
    return SafeRead<void*>(base, offset, out);
}

// Fast 安全读取指针
inline bool SafeReadPtrFast(const void* base, size_t offset, void*& out) {
    return SafeReadFast<void*>(base, offset, out);
}

// 安全读取 uint32_t
inline bool SafeReadU32(const void* base, size_t offset, uint32_t& out) {
    return SafeRead<uint32_t>(base, offset, out);
}

// Fast 安全读取 uint32_t
inline bool SafeReadU32Fast(const void* base, size_t offset, uint32_t& out) {
    return SafeReadFast<uint32_t>(base, offset, out);
}

} // namespace dxvk::war3
