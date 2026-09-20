#pragma once
#include "war3_frame_history_core.h"
#include <atomic>
#include <cstdint>

namespace dxvk::war3::tools::history {
// Image-export progress facts, frozen by the main thread for both DSH threads.
// Pure data: no allocation, no Rc, no OS/GPU/log calls, no ownership.
struct ExportProgress {
  uint32_t retained = 0, queued = 0, done = 0, failed = 0;
  bool requestsValid = false;
};
enum class ExportOutcome { Pending, Complete, ReadbackFailed, InvalidProgress };

// Structural population only, in the frozen order: invalid requests, retained
// outside 1..MaxSlots, or any failed<=done<=queued<=retained inversion are
// InvalidProgress. A valid population with done<retained stays Pending even
// when failed>0 (all jobs must settle); once every job is done, failed>0 is
// ReadbackFailed, otherwise Complete. No field sums, so nothing can overflow.
inline ExportOutcome ClassifyExportProgress(const ExportProgress& p) noexcept {
  if (!p.requestsValid) return ExportOutcome::InvalidProgress;
  if (p.retained == 0 || p.retained > MaxSlots) return ExportOutcome::InvalidProgress;
  if (p.failed > p.done || p.done > p.queued || p.queued > p.retained) return ExportOutcome::InvalidProgress;
  if (p.done < p.retained) return ExportOutcome::Pending;
  return p.failed ? ExportOutcome::ReadbackFailed : ExportOutcome::Complete;
}

// One job's publication atomics. A null pointer is a structural defect
// (missing request / order index outside MaxSlots) and is never dereferenced.
struct ExportJobView {
  const std::atomic<bool>* queued = nullptr;
  const std::atomic<bool>* done = nullptr;
  const std::atomic<bool>* success = nullptr;
};

// Single bounded pass over at most MaxSlots jobs, shared by production
// (Impl::settleExportProgressLocked) and the real CPU handoff test. The
// mandated read order is acquire-load of done first; only a done job has its
// queued/success read, and because the producer publishes queued, then the
// result, then done with release, both are guaranteed visible here. A not-done
// job contributes its queued state and may become nextPending (the first
// not-done, not-queued sequence index, MaxSlots when none). Stops at the first
// invalid view; concurrent completions can only keep this call conservatively
// Pending, never guess GPU completion.
template <typename JobAt>
ExportProgress SampleExportProgress(uint32_t count, JobAt&& jobAt, uint32_t& nextPending) noexcept {
  ExportProgress p;
  nextPending = MaxSlots;
  p.requestsValid = true;
  if (count > MaxSlots) { p.requestsValid = false; return p; }
  for (uint32_t i = 0; i < count; ++i) {
    const ExportJobView job = jobAt(i);
    if (!job.queued || !job.done || !job.success) { p.requestsValid = false; break; }
    ++p.retained;
    if (job.done->load(std::memory_order_acquire)) {
      ++p.done;
      if (job.queued->load(std::memory_order_relaxed)) ++p.queued;
      if (!job.success->load(std::memory_order_relaxed)) ++p.failed;
    } else if (job.queued->load(std::memory_order_relaxed)) {
      ++p.queued;
    } else if (nextPending == MaxSlots) {
      nextPending = i;
    }
  }
  return p;
}

// Terminal decision of the one settlement entry. Pure data; it describes what
// SettleExportCommit must publish. There is deliberately no packageReady field
// and no resource release: image completion never signs off the
// manifest/CPU/incident package, and invalid progress is only an explicit CPU
// fault, never in-flight resource reuse.
struct ExportCommitDecision {
  ExportOutcome outcome = ExportOutcome::Pending;
  CaptureLifecycle::State publish = CaptureLifecycle::Exporting;
  const char* fault = "";
  uint32_t saved = 0;
  bool terminal = false;
};

inline ExportCommitDecision DecideExportCommit(const ExportProgress& p) noexcept {
  ExportCommitDecision d;
  d.outcome = ClassifyExportProgress(p);
  if (d.outcome != ExportOutcome::InvalidProgress) d.saved = p.done - p.failed;
  if (d.outcome == ExportOutcome::Complete) {
    d.publish = CaptureLifecycle::Complete;
    d.terminal = true;
  } else if (d.outcome == ExportOutcome::ReadbackFailed) {
    d.publish = CaptureLifecycle::Fault;
    d.fault = "image-export-failed";
    d.terminal = true;
  } else if (d.outcome == ExportOutcome::InvalidProgress) {
    d.publish = CaptureLifecycle::Fault;
    d.fault = "image-export-progress-invalid";
    d.terminal = true;
  }
  return d;
}

// The only gate for committing an image-export terminal state: never commit
// outside Exporting and never revive a cancelled, stopping, Fault or Discard
// session. Repeating the same observation stays idempotent because the gate
// closes with the first real transition.
inline bool ShouldCommitExport(bool exporting, bool cancelled, bool stopping) noexcept {
  return exporting && !cancelled && !stopping;
}

// Narrow commit-target view binding exactly the mutable cells the unique
// settlement entry may touch: the image lifecycle state, the existing fault
// string and recording flag, the HUD state/saved/error cells and the shortcut
// mailbox behind an opaque disarm. It owns nothing and changes no lock, GPU,
// fence, registry or resource-lifetime rule. Production binds the real Impl
// members and HUD singleton; the CPU handoff test binds plain storage.
struct ExportCommitTargets {
  std::atomic<CaptureLifecycle::State>* state = nullptr;
  const std::atomic<bool>* cancelled = nullptr;
  const std::atomic<bool>* stopping = nullptr;
  const char** fault = nullptr;
  std::atomic<bool>* recording = nullptr;
  std::atomic<uint32_t>* hudState = nullptr;
  std::atomic<uint32_t>* hudSaved = nullptr;
  std::atomic<const char*>* hudError = nullptr;
  void* shortcutMailbox = nullptr;
  void (*disarmShortcut)(void*) = nullptr;
};

struct ExportCommitApply {
  bool eligible = false;   // the gate actually opened for this call
  bool committed = false;  // a terminal transition was published by this call
};

// The one real commit step, run verbatim by production
// (Impl::settleExportProgressLocked) and by the CPU handoff test. The gate is
// read from the live targets; only an Exporting, non-cancelled, non-stopping
// state applies the shared decision. Every eligible settle refreshes HUD saved;
// a terminal decision performs the complete existing side-effect set exactly
// once: Complete publishes state=Complete and hudState=Complete; otherwise it
// performs the same six actions as Impl::fail, in the same order: state=Fault,
// fault=reason, recording=false, shortcut disarm, hudState=Fault,
// hudError=reason. No allocation, no lock, no GPU; it never signs packageReady
// and never releases or reuses in-flight readback resources.
inline ExportCommitApply SettleExportCommit(const ExportCommitDecision& d,
                                            const ExportCommitTargets& t) noexcept {
  ExportCommitApply a;
  const bool exporting = t.state && t.state->load(std::memory_order_acquire) == CaptureLifecycle::Exporting;
  const bool cancelled = t.cancelled && t.cancelled->load();
  const bool stopping = t.stopping && t.stopping->load();
  a.eligible = ShouldCommitExport(exporting, cancelled, stopping);
  if (!a.eligible) return a;
  if (t.hudSaved) t.hudSaved->store(d.saved);
  if (!d.terminal) return a;
  if (d.publish == CaptureLifecycle::Complete) {
    if (t.state) t.state->store(CaptureLifecycle::Complete);
    if (t.hudState) t.hudState->store(CaptureLifecycle::Complete);
  } else {
    if (t.state) t.state->store(CaptureLifecycle::Fault);
    if (t.fault) *t.fault = d.fault;
    if (t.recording) t.recording->store(false);
    if (t.disarmShortcut && t.shortcutMailbox) t.disarmShortcut(t.shortcutMailbox);
    if (t.hudState) t.hudState->store(CaptureLifecycle::Fault);
    if (t.hudError) t.hudError->store(d.fault);
  }
  a.committed = true;
  return a;
}

// One full settle: bounded sample, frozen classification, decision, gate and
// the single commit step. nextPending is the job sequence index (MaxSlots when
// none); each caller maps it to its own slot array.
struct ExportSettleResult {
  ExportProgress progress{};
  ExportCommitDecision decision{};
  ExportCommitApply apply{};
  uint32_t nextPending = MaxSlots;
};

template <typename JobAt>
ExportSettleResult SettleExportProgress(uint32_t count, JobAt&& jobAt,
                                        const ExportCommitTargets& targets) noexcept {
  ExportSettleResult r;
  r.progress = SampleExportProgress(count, jobAt, r.nextPending);
  r.decision = DecideExportCommit(r.progress);
  r.apply = SettleExportCommit(r.decision, targets);
  return r;
}

// nextExport forwards a job only when the settle did not commit a terminal
// state, the gate was open, and a not-done, not-queued job exists.
inline bool ShouldForwardExportJob(const ExportSettleResult& r) noexcept {
  return r.apply.eligible && !r.apply.committed && r.nextPending != MaxSlots;
}
} // namespace dxvk::war3::tools::history