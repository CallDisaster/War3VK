#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace dxvk {
namespace war3dbg {

inline void Print(const char *fmt, ...) {
  static bool consoleInit = false;
  if (!consoleInit) {
    if (AllocConsole()) {
      FILE *dummy;
      freopen_s(&dummy, "CONOUT$", "w", stdout);
      freopen_s(&dummy, "CONOUT$", "w", stderr);
      // 强制 UTF-8 输出，避免中文在非 UTF-8 系统（如 CP936）下显示乱码
      SetConsoleOutputCP(CP_UTF8);
      SetConsoleCP(CP_UTF8);
      // 切换到支持 Unicode 的字体（Consolas 或 Lucida Console）
      HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
      if (hOut && hOut != INVALID_HANDLE_VALUE) {
        CONSOLE_FONT_INFOEX fontInfo = {};
        fontInfo.cbSize = sizeof(fontInfo);
        fontInfo.dwFontSize.Y = 14;
        fontInfo.FontWeight = FW_NORMAL;
        fontInfo.FontFamily = FF_MODERN | TMPF_VECTOR;
        wcscpy_s(fontInfo.FaceName, L"Consolas");
        SetCurrentConsoleFontEx(hOut, FALSE, &fontInfo);
      }
    }
    consoleInit = true;
  }

  char msg[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  OutputDebugStringA(msg);
  std::printf("%s", msg);
  std::fflush(stdout);
}

void InstallCrashHandlerOnce();

inline bool RenderLogEnabled() {
  static int s_cached = -1;
  if (s_cached >= 0)
    return s_cached != 0;
  const char *env = std::getenv("DXVK_WAR3_RENDER_LOG");
  s_cached = (env && *env && std::strtoul(env, nullptr, 10) != 0) ? 1 : 0;
  return s_cached != 0;
}

} // namespace war3dbg
} // namespace dxvk

#define WAR3_RENDER_LOG(fmt, ...)                                              \
  do {                                                                         \
    if (::dxvk::war3dbg::RenderLogEnabled()) {                                 \
      ::dxvk::war3dbg::Print(fmt, ##__VA_ARGS__);                              \
    }                                                                          \
  } while (0)
