#include "war3_diagnostics_hub.h"

#include "../../d3d9_war3_debug.h"

#include "../platform/war3_module_api.h"
#include "../core/war3_events.h"
#include "../core/war3_net_event_hook.h"
#include "../core/war3_runtime_profile.h"
#include "war3_perf_monitor.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>

namespace dxvk::war3::tools {

namespace {
std::atomic<bool> s_inGameRenderReady{false};

const char* ModuleStateToString(war3module::War3ModuleRuntimeState state) {
  switch (state) {
  case war3module::War3ModuleRuntimeState::Cold:
    return "Cold";
  case war3module::War3ModuleRuntimeState::Running:
    return "Running";
  case war3module::War3ModuleRuntimeState::ShuttingDown:
    return "ShuttingDown";
  default:
    return "Unknown";
  }
}

std::string GetWarVkTempRuntimePath() {
  char exePath[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) <= 0)
    return {};
  std::string exeDir(exePath);
  size_t pos = exeDir.find_last_of("\\/");
  if (pos == std::string::npos)
    return {};
  exeDir = exeDir.substr(0, pos + 1);

  const std::string warvkDir = exeDir + "WarVK\\";
  const std::string tempDir = warvkDir + "Temp\\";
  CreateDirectoryA(warvkDir.c_str(), nullptr);
  CreateDirectoryA(tempDir.c_str(), nullptr);
  return tempDir + "runtime_status.json";
}

void WriteRuntimeStatusSnapshot(const char* source, uint64_t frameIndex,
                                const war3module::War3ModuleRuntimeStats& stats,
                                bool perfEnabled, bool perfRecording,
                                bool runtimeReady, bool gameStarted) {
  const std::string outPath = GetWarVkTempRuntimePath();
  if (outPath.empty())
    return;

  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto epochMs = duration_cast<milliseconds>(now.time_since_epoch()).count();

  std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
  if (!f.is_open())
    return;

  f << "{\n"
    << "  \"timestampMs\": " << static_cast<long long>(epochMs) << ",\n"
    << "  \"source\": \"" << ((source && source[0]) ? source : "unknown") << "\",\n"
    << "  \"frameIndex\": " << static_cast<unsigned long long>(frameIndex) << ",\n"
    << "  \"module\": {\n"
    << "    \"registered\": " << static_cast<unsigned>(stats.registeredModules) << ",\n"
    << "    \"loaded\": " << static_cast<unsigned>(stats.loadedModules) << ",\n"
    << "    \"state\": \"" << ModuleStateToString(stats.state) << "\",\n"
    << "    \"dispatchCalls\": " << static_cast<unsigned long long>(stats.dispatchCalls) << ",\n"
    << "    \"handlers\": " << static_cast<unsigned long long>(stats.dispatchedHandlers) << ",\n"
    << "    \"callbackErrors\": " << static_cast<unsigned long long>(stats.callbackErrors) << "\n"
    << "  },\n"
    << "  \"perf\": {\n"
    << "    \"enabled\": " << (perfEnabled ? "true" : "false") << ",\n"
    << "    \"recording\": " << (perfRecording ? "true" : "false") << "\n"
    << "  },\n"
    << "  \"profile\": {\n"
    << "    \"name\": \""
    << dxvk::war3::runtime::GetWar3RuntimeProfileName() << "\",\n"
    << "    \"disabledModules\": \""
    << dxvk::war3::runtime::GetWar3RuntimeDisabledModulesCsv() << "\",\n"
    << "    \"enabledModules\": \""
    << dxvk::war3::runtime::GetWar3RuntimeEnabledModulesCsv() << "\"\n"
    << "  },\n"
    << "  \"runtime\": {\n"
    << "    \"runtimeReady\": " << (runtimeReady ? "true" : "false") << ",\n"
    << "    \"gameStarted\": " << (gameStarted ? "true" : "false") << "\n"
    << "  }\n"
    << "}\n";
}
} // namespace

void ExportRuntimeStatusSnapshot(const char* source, uint64_t frameIndex) {
  const auto stats = war3module::GetModuleRuntimeStats();
  auto& perf = War3PerfMonitor::instance();
  const bool runtimeReady =
      dxvk::war3::NetEventHook::get().IsRuntimeReady() ||
      s_inGameRenderReady.load(std::memory_order_relaxed);
  WriteRuntimeStatusSnapshot(source, frameIndex, stats, perf.isEnabled(),
                             perf.isRecording(), runtimeReady,
                             dxvk::war3::War3Events::get().isGameStarted());
}

void MarkInGameRenderReady(const char* source, uint64_t frameIndex) {
  bool expected = false;
  if (!s_inGameRenderReady.compare_exchange_strong(
          expected, true, std::memory_order_relaxed))
    return;

  war3dbg::Print("DXVK War3Diag: InGameRenderReady source=%s frame=%llu\n",
                 (source && source[0]) ? source : "(unknown)",
                 static_cast<unsigned long long>(frameIndex));
  ExportRuntimeStatusSnapshot(source, frameIndex);
}

void ResetRuntimeReadySignals() {
  s_inGameRenderReady.store(false, std::memory_order_relaxed);
}

void LogRuntimeSummaryOnce(const char* source) {
  static std::atomic<bool> s_logged{false};
  bool expected = false;
  if (!s_logged.compare_exchange_strong(expected, true))
    return;

  const auto stats = war3module::GetModuleRuntimeStats();
  auto& perf = War3PerfMonitor::instance();

  war3dbg::Print(
      "DXVK War3Diag: RuntimeSummary source=%s modules=%u loaded=%u "
      "state=%s dispatch=%llu handlers=%llu callbackErr=%llu perfEnabled=%d "
      "perfRecording=%d profile=%s disabled=%s\n",
      (source && source[0]) ? source : "(unknown)",
      static_cast<unsigned>(stats.registeredModules),
      static_cast<unsigned>(stats.loadedModules),
      ModuleStateToString(stats.state),
      static_cast<unsigned long long>(stats.dispatchCalls),
      static_cast<unsigned long long>(stats.dispatchedHandlers),
      static_cast<unsigned long long>(stats.callbackErrors),
      perf.isEnabled() ? 1 : 0, perf.isRecording() ? 1 : 0,
      dxvk::war3::runtime::GetWar3RuntimeProfileName(),
      dxvk::war3::runtime::GetWar3RuntimeDisabledModulesCsv().c_str());

  ExportRuntimeStatusSnapshot(source, 0);
}

void LogRuntimeHealthPeriodic(uint64_t frameIndex, uint32_t interval) {
  // 部分路径下 frameIndex 可能长期为 0，此时按取模会每次都命中，造成刷屏。
  // 这里做去重与零值防抖，保证日志频率稳定。
  static std::atomic<uint64_t> s_lastLoggedFrame{~uint64_t(0)};
  static std::atomic<uint64_t> s_zeroFrameTick{0};

  if (interval == 0)
    return;

  if (frameIndex == 0) {
    const uint64_t tick = s_zeroFrameTick.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((tick % interval) != 0)
      return;
  } else {
    if ((frameIndex % interval) != 0)
      return;

    const uint64_t prev =
        s_lastLoggedFrame.exchange(frameIndex, std::memory_order_relaxed);
    if (prev == frameIndex)
      return;
  }

  const auto stats = war3module::GetModuleRuntimeStats();
  auto& perf = War3PerfMonitor::instance();

  war3dbg::Print(
      "DXVK War3Diag: RuntimeHealth frame=%llu modules=%u loaded=%u state=%s "
      "dispatch=%llu handlers=%llu callbackErr=%llu perfEnabled=%d "
      "perfRecording=%d profile=%s\n",
      static_cast<unsigned long long>(frameIndex),
      static_cast<unsigned>(stats.registeredModules),
      static_cast<unsigned>(stats.loadedModules),
      ModuleStateToString(stats.state),
      static_cast<unsigned long long>(stats.dispatchCalls),
      static_cast<unsigned long long>(stats.dispatchedHandlers),
      static_cast<unsigned long long>(stats.callbackErrors),
      perf.isEnabled() ? 1 : 0, perf.isRecording() ? 1 : 0,
      dxvk::war3::runtime::GetWar3RuntimeProfileName());

  ExportRuntimeStatusSnapshot("periodic", frameIndex);
}

} // namespace dxvk::war3::tools
