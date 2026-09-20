#pragma once

#include "../../../dxvk/dxvk_buffer.h"
#include "../../../dxvk/dxvk_fence.h"
#include "../../../dxvk/dxvk_image.h"
#include <memory>
#include <optional>

namespace dxvk::war3::tools {
struct HistoryReadback;

bool AsyncScreenshotEnabled();
// false: native path remains responsible. true: handled, NOT saved. A bounded
// queue rejection is reported asynchronously, never converted to a GPU wait.
bool RequestNativeAsyncScreenshot();
bool ArmAsyncScreenshotBurstForTest();

class AsyncScreenshot {
public:
  struct Copy {
    Rc<DxvkBuffer> buffer;
    Rc<DxvkFence> fence;
    uint64_t value;
    uint32_t slot;
    uint64_t evidenceSession;
    uint64_t serial;
    uint64_t presentOrdinal;
  };

  static std::unique_ptr<AsyncScreenshot> Create(const Rc<DxvkDevice>& device);
  ~AsyncScreenshot();
  void beginPresent();
  uint64_t presentOrdinal() const noexcept;
  void prepare(const Rc<DxvkImage>& image,bool advanceBurst=true);
  std::optional<Copy> take(uint32_t slot);
  std::optional<Copy> takeHistory(const std::shared_ptr<HistoryReadback>& request);
  void submitted(uint32_t slot);
  void quarantine(uint32_t slot);
  void reset();

private:
  struct Impl;
  explicit AsyncScreenshot(const Rc<DxvkDevice>& device);
  bool request();
  std::unique_ptr<Impl> m;
  friend bool RequestNativeAsyncScreenshot();
  friend bool ArmAsyncScreenshotBurstForTest();
};

} // namespace dxvk::war3::tools
