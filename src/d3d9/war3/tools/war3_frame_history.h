#pragma once
#include "war3_frame_evidence_core.h"
#include "../../../dxvk/dxvk_device.h"
#include "../../../dxvk/dxvk_image.h"
#include <atomic>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace dxvk::war3::tools {
namespace evidence { class RecorderControlLease; }
// Metadata is immutable after queued. The established screenshot worker only
// publishes done/success; it never calls back into history or game runtime.
struct HistoryReadback {
  evidence::Key key{};
  uint64_t session=0,present=0,ordinal=0,qpc=0;
  uint64_t targetValue=0,observedValue=0;
  uint32_t width=0,height=0;
  std::wstring path;
  std::atomic<bool> queued{false},done{false},success{false};
};
class FrameHistory {
public:
  static std::shared_ptr<FrameHistory> Create(const Rc<DxvkDevice>& device);
  static void Release(const std::shared_ptr<FrameHistory>& owner);
  ~FrameHistory();
  struct Capture {Rc<DxvkImage> image;evidence::Key key{};uint64_t session=0,index=0,present=0;};
  struct Export {Rc<DxvkImage> image;std::shared_ptr<HistoryReadback> request;};
  // Device-owner under D3D9 lock only. Control and worker do not operate GPU refs.
  Capture capture(const Rc<DxvkImage>& source,evidence::Key key,uint64_t present);
  Export nextExport();
  void cancel(const char* reason=nullptr) noexcept;
  bool selfContained() const noexcept;
private:
  explicit FrameHistory(const Rc<DxvkDevice>& device);
  struct Impl;
  std::unique_ptr<Impl> m;
  void runRecorder() noexcept;
  void stopRecorder() noexcept;
  friend bool TriggerFrameHistoryFromGame() noexcept;
  friend nlohmann::json FrameHistoryControl(const nlohmann::json&,const evidence::RecorderControlLease*);
};
bool TriggerFrameHistoryFromGame() noexcept;
bool HandleFrameHistoryShortcut(uint32_t message,uint64_t key,bool ctrl,bool shift,bool repeat) noexcept;
struct FrameHistoryHud {
  bool enabled=false;uint32_t state=0,bufferedMs=0,requiredMs=0,saved=0,total=0,notice=0;
  bool watcherAlive=false,packageReady=false,selfContained=false;
  const char* error="";
};
FrameHistoryHud QueryFrameHistoryHud() noexcept;
} // namespace dxvk::war3::tools
