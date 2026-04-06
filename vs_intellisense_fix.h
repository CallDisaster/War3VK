#pragma once

#ifdef __INTELLISENSE__

// =================================================================
// 1. 核心编译器伪装
// =================================================================
// 告诉 MinGW 头文件：我们是现代 GCC
#ifndef __GNUC__
#define __GNUC__ 15
#endif
#ifndef __GNUC_MINOR__
#define __GNUC_MINOR__ 2
#endif
#ifndef __GNUC_PATCHLEVEL__
#define __GNUC_PATCHLEVEL__ 0
#endif

// 强制 C++17 标准 (解决 enable_if, string_view 等模板问题)
#undef __cplusplus
#define __cplusplus 201703L

// =================================================================
// 2. 修复 VS 无法识别的 GCC 扩展类型
// =================================================================
// 修复 AVX/SSE 头文件报错 (VS 不认识 __bf16)
#define __bf16 short
#define __float128 long double

// 修复 std::string 崩溃的核心原因
// MinGW 的 c++locale.h 依赖这个内部类型
#define __c_locale int

// =================================================================
// 3. 修复 MinGW 与 VS 的方言冲突
// =================================================================
// 使用 typedef 解决带空格的类型 (VS 宏解析器有空格bug)
typedef unsigned char  intellisense_boolean;
#define boolean intellisense_boolean

// 修复 RPC 句柄
typedef void* intellisense_rpc_handle;
#define RPC_IF_HANDLE intellisense_rpc_handle

// 修复 restrict 关键字 (VS 不认 __restrict__)
#define __restrict__ 

// =================================================================
// 4. 强制开启 C99/C++ 标准库特性
// =================================================================
// 解决 <cmath> 里的 acos, tan, trunc 报错
#define _GLIBCXX_USE_C99_MATH 1
#define _GLIBCXX_USE_C99 1
#define _GLIBCXX_USE_C99_LONG_LONG 1
#define _POSIX_C_SOURCE 200809L

// =================================================================
// 5. 兼容性属性补丁
// =================================================================
#define __cdecl __attribute__((__cdecl__))
#define __stdcall __attribute__((__stdcall__))
#define __fastcall __attribute__((__fastcall__))
#define __thiscall __attribute__((__thiscall__))
#define __declspec(x) __attribute__((x))
#define __int64 long long
#define __inline inline
#define __forceinline inline

// =================================================================
// 6. 项目特定宏
// =================================================================
#define ENABLE_PREMIUM_FUNCTION 1

// 身份验证
#ifndef _WIN32
#define _WIN32
#endif
#ifndef WIN32
#define WIN32
#endif
#ifndef _X86_
#define _X86_
#endif
#ifndef __i386__
#define __i386__
#endif
#ifndef __MINGW32__
#define __MINGW32__
#endif

#endif // __INTELLISENSE__