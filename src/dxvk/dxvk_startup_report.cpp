#include "dxvk_startup_report.h"

#include "../util/thread.h"

namespace dxvk::startup_report {

  namespace {

    constexpr size_t MaxAdapters = 16;

    dxvk::mutex& reportMutex() {
      static dxvk::mutex mutex;
      return mutex;
    }

    DxvkStartupReportSnapshot& reportState() {
      static DxvkStartupReportSnapshot state;
      return state;
    }

  }


  void beginInstance() {
    std::lock_guard lock(reportMutex());
    reportState() = DxvkStartupReportSnapshot();
  }


  void setStage(DxvkStartupStage stage) {
    std::lock_guard lock(reportMutex());
    reportState().stage = stage;
  }


  void addAdapter(DxvkStartupAdapterRecord record) {
    std::lock_guard lock(reportMutex());
    auto& adapters = reportState().adapters;

    if (adapters.size() < MaxAdapters)
      adapters.push_back(std::move(record));
  }


  DxvkStartupReportSnapshot snapshot() {
    std::lock_guard lock(reportMutex());
    return reportState();
  }

}
