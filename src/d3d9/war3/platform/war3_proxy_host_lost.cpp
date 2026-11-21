// war3_proxy_host_lost.cpp - see war3_proxy_host_lost.h

#include "war3_proxy_host_lost.h"

#include "war3_player_notice.h"
#include "../../../util/log/log.h"
#include "../../../util/util_string.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace dxvk::war3::proxy {

namespace {

// The first sentence of the dialog, by the cause (Connection::TerminalName).
const char* Headline(const char* kind) {
  if (std::strcmp(kind, "host-exited") == 0)
    return "WarVK 的渲染进程意外退出了，游戏画面无法继续显示。";
  if (std::strcmp(kind, "host-hung") == 0)
    return "WarVK 的渲染进程长时间没有响应，游戏画面无法继续显示。";
  if (std::strcmp(kind, "host-device-lost") == 0)
    return "显卡设备丢失（显卡驱动崩溃或被重置），游戏画面无法继续显示。";
  return "WarVK 与渲染进程之间的连接出现内部错误，游戏画面无法继续显示。";
}

std::string Hex8(uint32_t value) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
  return buffer;
}

std::string LocalTime() {
  SYSTEMTIME now = {};
  GetLocalTime(&now);
  char buffer[40] = {};
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay,
                now.wHour, now.wMinute, now.wSecond);
  return buffer;
}

} // namespace

void ReportRenderHostLostAndExit(const WarVKD3D9TerminalFacts& facts, const HostLostContext& context) {
  // Whatever fails below, the process still ends with this code.
  try {
    const char* kind = facts.kind ? facts.kind : "unknown";
    const std::wstring reportPath = notice::EnsureLogDirectory() + L"\\render-host-lost.txt";
    const std::string reportPathUtf8 = notice::WideToUtf8(reportPath.c_str());

    const std::string title = "WarVK：渲染进程已停止";
    const std::string body = std::string(Headline(kind)) +
        "点击“确定”后游戏将关闭，当前这局游戏不会保存。\n\n"
        "可能的原因：显卡驱动崩溃或被重置、显存或内存不足、渲染进程被安全软件或其他程序结束。\n"
        "重新启动游戏即可继续。如果反复出现，请把下面这份报告发给作者：\n\n" + reportPathUtf8;

    const std::string technical =
        "---- 技术信息 ----\n"
        "time=" + LocalTime() + "\n" +
        "kind=" + kind + " detail=" + (facts.detail ? facts.detail : "") + " result=" + Hex8(facts.result) + "\n" +
        "hostExit=" + (facts.hostExitKnown ? std::to_string(facts.hostExit) + " (" + Hex8(facts.hostExit) + ")"
                                           : std::string("unknown (the host had not exited)")) + "\n" +
        "published=" + std::to_string(facts.published) + " applied=" + std::to_string(facts.applied) + "\n" +
        "hostLog=" + notice::WideToUtf8(facts.hostLog) + "\n" +
        "presents=" + std::to_string(context.presents) + " draws=" + std::to_string(context.draws) +
        " backBuffer=" + std::to_string(context.width) + "x" + std::to_string(context.height) + "\n" +
        "exitCode=" + Hex8(kRenderHostLostExitCode) + "\n";

    const notice::DialogDecision dialog = notice::DecideDialog("DXVK_WAR3_HOST_LOST_DIALOG");
    const std::string file = title + "\n\n" + body + "\n\n" + technical +
        "dialog=" + (dialog.show ? "shown" : "suppressed") + " desktop=" + dialog.desktop + "\n";
    const bool written = notice::WriteReportFile(reportPath, file);

    Logger::err(str::format("WarVK proxy mode: render host lost: kind=", kind,
        " report=", written ? reportPathUtf8 : std::string("<write failed>"),
        " dialog=", dialog.show ? "shown" : "suppressed"));
    Logger::err(technical);

    if (dialog.show) {
      Logger::info("WarVK render host lost dialog: open");
      const int result = notice::ShowErrorDialog(title, body);
      Logger::info(str::format("WarVK render host lost dialog: closed result=", result));
    }
    Logger::info(str::format("WarVK proxy mode: ending the game process, exit code ", Hex8(kRenderHostLostExitCode)));
  } catch (...) {
    // The report could not be composed; the session's end is already logged.
  }
  TerminateProcess(GetCurrentProcess(), kRenderHostLostExitCode);
  // TerminateProcess on the current process does not return.
  for (;;)
    Sleep(INFINITE);
}

} // namespace dxvk::war3::proxy
