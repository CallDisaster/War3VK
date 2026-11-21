#pragma once

// Frame evidence and the performance log of proxy mode, in the host: the
// picture and the frame timing are here, the keyboard and the panel are the
// game's (src/d3d9/war3/platform/war3_proxy_diag_block.h).
//
// Frame evidence: every frame with a world scene leaves a reduced copy of the
// finished picture in a ring on the GPU, with what the host knows about that
// frame: timing, draws, the camera and sun the data layer sent, what the
// shadow pass decided, the audit of the casters' geometry. Ctrl+Shift+C in the
// game keeps a few frames more and then writes the ring as JPEG pictures and
// one evidence.json, beside the session's logs. The ring goes on recording,
// so a session may save any number of packages.
//
// Performance log: while the player records, one CSV row per presented frame.
//
// Nothing here may fail a frame: a ring that cannot be created, a buffer that
// cannot be allocated or a file that cannot be written ends the evidence, not
// the device.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "shadow_outcome.h"
#include "../../../src/dxvk/dxvk_context.h"
#include "../../../src/dxvk/dxvk_device.h"

namespace warvk::diag { struct Block; }

namespace warvk::host::dxvkcore {

// One presented frame, as the host knows it.
struct EvidenceFrame {
  uint64_t hostFrame = 0, qpc = 0;
  uint32_t intervalUs = 0;
  uint32_t draws = 0, drawBatches = 0;
  uint32_t width = 0, height = 0;
  // The data layer's records of the frame.
  bool beforeUi = false, hasCamera = false, hasLighting = false, hasShadowFrame = false;
  std::array<float, 16> view{}, projection{};
  std::array<float, 4> sunDirection{};
  float timeOfDay = -1.0f;
  uint32_t recordCasters = 0, recordOmitted = 0, recordOmissionReasons = 0;
  ShadowOutcome shadow;
  // Owner-thread wall time of the frame by what blocked it (WaitKind order).
  std::array<uint32_t, 16> waitUs{};
  uint64_t deviceUsedBytes = 0, deviceAllocatedBytes = 0;
  // The BeforeUi effects: bit0 SSAO, bit1 snow, bit2 AA, bit3 bloom recorded,
  // and the storage they own.
  uint32_t effectsRecorded = 0;
  uint64_t effectBytes = 0;
  // The newest game frame in the shared block when the host presented.
  uint32_t gameFrame = 0, gameIntervalUs = 0, gameConsidered = 0, gameSent = 0, gameOmitted = 0, gameFlags = 0;
};

class EvidenceRecorder {
public:
  EvidenceRecorder(const dxvk::Rc<dxvk::DxvkDevice>& device, uint32_t gamePid);
  ~EvidenceRecorder();
  EvidenceRecorder(const EvidenceRecorder&) = delete;
  EvidenceRecorder& operator=(const EvidenceRecorder&) = delete;

  // The caster audit should run: the recorder is attached and recording, and
  // no performance log is being written.
  bool wantsAudit() const noexcept;
  // Ends the evidence for this session (the device is fine).
  void stop() noexcept { m_off = true; }
  // Completes a performance log still open: the session is ending.
  void flushLogs() noexcept;

  // At Present, on the owner thread, with the frame's command list open on
  // ctx and the color target complete: the picture goes into the ring (when
  // the frame had a world scene), the facts with it, and a row into the
  // performance log. `names` are the columns of facts.waitUs.
  void frame(dxvk::DxvkContext& ctx, const dxvk::Rc<dxvk::DxvkImage>& color, EvidenceFrame facts,
      const char* const* waitNames, uint32_t waitCount);
  // After that Present: a package is due. The owner then opens a command list,
  // calls recordExport, submits it, waits for the GPU and calls exportReady.
  bool exportDue() noexcept;
  void recordExport(dxvk::DxvkContext& ctx);
  void exportReady();
  // The targets changed size or the device is going: the ring is dropped.
  void dropRing() noexcept;

private:
  struct Slot { dxvk::Rc<dxvk::DxvkImage> image; dxvk::Rc<dxvk::DxvkImageView> view; EvidenceFrame facts; bool used = false; };
  struct Package;

  void attach();
  void fail(const char* reason) noexcept;
  bool ensureRing(uint32_t width, uint32_t height);
  void publishStats(const EvidenceFrame& facts);
  void perfRow(const EvidenceFrame& facts, const char* const* waitNames, uint32_t waitCount);
  void perfStop(bool failed);
  void reapWorker();

  dxvk::Rc<dxvk::DxvkDevice> m_device;
  uint32_t m_gamePid = 0;
  void* m_mapping = nullptr;
  diag::Block* m_block = nullptr;
  bool m_off = false;           // the game's block is not one this host understands
  bool m_noRing = false;        // no frame evidence: switched off, or no memory for it
  const char* m_noRingReason = "";
  uint32_t m_attachCountdown = 0;

  // The ring.
  std::vector<Slot> m_slots;
  uint32_t m_head = 0, m_filled = 0;       // next slot to write, slots in use
  uint32_t m_sourceWidth = 0, m_sourceHeight = 0, m_width = 0, m_height = 0, m_divisor = 1;
  dxvk::Rc<dxvk::DxvkImageView> m_sourceView;
  dxvk::Rc<dxvk::DxvkImage> m_sourceImage;
  int32_t m_requestSeen = 0;
  uint32_t m_postLeft = 0;      // frames still to keep after a request
  bool m_triggered = false, m_exportDue = false;
  uint64_t m_triggerFrame = 0;
  uint32_t m_packages = 0;
  uint64_t m_ringBytes = 0;

  // The render statistics panel.
  uint32_t m_statSequence = 0, m_statCountdown = 0;
  uint64_t m_statFrames = 0, m_statWorldFrames = 0, m_statAverageUs = 0;

  // The package being written by the worker, and the buffer its pictures are in.
  std::unique_ptr<Package> m_package;
  dxvk::Rc<dxvk::DxvkBuffer> m_exportBuffer;
  std::thread m_worker;
  std::atomic<bool> m_workerDone{false}, m_stopWorker{false};
  std::vector<std::string> m_waitNames;

  // The performance log.
  void* m_perfFile = nullptr;   // HANDLE
  std::string m_perfPending;
  std::wstring m_perfPath;
  int32_t m_perfSession = 0;
  uint64_t m_perfBeginQpc = 0;
  uint32_t m_perfRows = 0;
  std::vector<uint32_t> m_perfIntervals; // microseconds, for the summary
  int64_t m_frequency = 1;
  uint64_t m_lastQpc = 0;
};

} // namespace warvk::host::dxvkcore
