#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <windows.h>
#include <array>
#include <algorithm>
#include <functional>

// Basic Types
using byte_t = uint8_t;
using string = std::string;

// GLM Mocks
namespace glm {
    struct vec3 { 
        float x, y, z; 
        
        vec3() : x(0), y(0), z(0) {}
        vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
    };
    struct vec4 { 
        float x, y, z, w; 
        
        vec4() : x(0), y(0), z(0), w(0) {}
        vec4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
    };
    struct mat3x3 { 
        float m[9]; // Simple column-major or row-major as needed. War3 uses 3x3 usually.
    };
}

// Function types
template<typename R, typename... Args>
using fast_call_t = R(__fastcall*)(Args...);

template<typename R, typename... Args>
using this_call_t = R(__thiscall*)(Args...);

template<typename R, typename... Args>
using std_call_t = R(__stdcall*)(Args...);

template<typename R, typename... Args>
using c_decl_t = R(__cdecl*)(Args...);

// Helper for calling function pointers
template<typename R, typename... Args>
inline R call_fast(uintptr_t func, Args... args) {
    return ((fast_call_t<R, Args...>)func)(args...);
}

template<typename R, typename... Args>
inline R call_this(uintptr_t func, Args... args) {
    return ((this_call_t<R, Args...>)func)(args...);
}

template<typename R, typename... Args>
inline R call_std(uintptr_t func, Args... args) {
    return ((std_call_t<R, Args...>)func)(args...);
}

// Macros for calling conventions
#ifndef fast_call
#define fast_call __fastcall
#endif

#ifndef this_call
#define this_call __thiscall
#endif

#ifndef std_call
#define std_call __stdcall
#endif

#ifndef c_decl
#define c_decl __cdecl
#endif

// XSTRFUNC mock
struct XSTRFUNCTYPE {
    const char* str;
    constexpr XSTRFUNCTYPE(const char* s) : str(s) {}
    constexpr const char* operator()() const { return str; }
};
#define XSTRFUNC(s) XSTRFUNCTYPE(s)

// Prehash map mock (using standard unordered_map for now)
template<typename T>
using prehash_map_t = std::unordered_map<uint32_t, T>;
