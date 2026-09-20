// CPU handoff test for the real production-shared image-export commit boundary.
// Everything under test is in the production header
// src/d3d9/war3/tools/war3_frame_history_export_core.h:
// ClassifyExportProgress, SampleExportProgress, DecideExportCommit,
// ShouldCommitExport, ShouldForwardExportJob and the actual shared commit step
// SettleExportCommit / SettleExportProgress that Impl::settleExportProgressLocked
// runs under the image mutex. The Harness below only binds storage through
// ExportCommitTargets and adapts job views; it contains no state branch, no
// eligibility test, no HUD/fault/recording/shortcut publication of its own.
// Nothing in the boundary allocates or touches GPU/OS/log.
#include "../src/d3d9/war3/tools/war3_frame_history_export_core.h"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace dxvk::war3::tools::history;

namespace {
unsigned g_checks = 0;
void check(bool value, const char* what) {
  ++g_checks;
  if (!value) throw std::runtime_error(what);
}
constexpr uint32_t kComplete = uint32_t(CaptureLifecycle::Complete);
constexpr uint32_t kFault = uint32_t(CaptureLifecycle::Fault);
constexpr uint32_t kExporting = uint32_t(CaptureLifecycle::Exporting);

struct Job { std::atomic<bool> queued{false}, done{false}, success{false}; };

// Storage-only fixture: the commit cells the shared step writes plus the job
// atomics the shared sampler reads. The only "adapter" code is targets() and
// jobView(); every decision, gate and publication is the production header's.
struct Harness {
  std::vector<std::unique_ptr<Job>> jobs;

  std::atomic<CaptureLifecycle::State> state{CaptureLifecycle::Exporting};
  std::atomic<bool> cancelled{false}, stopping{false}, recording{true};
  const char* fault = "";
  std::atomic<uint32_t> hudState{kExporting}, hudSaved{0};
  std::atomic<const char*> hudError{""};
  uint32_t disarmCount = 0;
  bool packageReady = false; // outside the boundary: must never be signed off

  ExportCommitTargets targets() {
    ExportCommitTargets t;
    t.state = &state; t.cancelled = &cancelled; t.stopping = &stopping;
    t.fault = &fault; t.recording = &recording;
    t.hudState = &hudState; t.hudSaved = &hudSaved; t.hudError = &hudError;
    t.shortcutMailbox = &disarmCount;
    t.disarmShortcut = [](void* m) noexcept { ++*static_cast<uint32_t*>(m); };
    return t;
  }
  ExportJobView jobView(uint32_t i) const noexcept {
    ExportJobView view;
    if (i >= jobs.size() || !jobs[i]) return view;
    view.queued = &jobs[i]->queued; view.done = &jobs[i]->done; view.success = &jobs[i]->success;
    return view;
  }
  ExportProgress sample(uint32_t& nextPending) {
    return SampleExportProgress(uint32_t(jobs.size()), [this](uint32_t i) noexcept { return jobView(i); }, nextPending);
  }
  // The complete real settle (sample, classify, decide, gate, commit) runs in
  // the production header; this only supplies storage and the job adapter.
  ExportSettleResult settle() {
    return SettleExportProgress(uint32_t(jobs.size()), [this](uint32_t i) noexcept { return jobView(i); }, targets());
  }
  ExportSettleResult statusEntry() { return settle(); } // status() calls the same settle entry
  int32_t nextExportEntry() {                            // nextExport() settles then forwards
    const ExportSettleResult r = settle();
    return ShouldForwardExportJob(r) ? int32_t(r.nextPending) : -1;
  }
  void addJob(bool queued, bool done, bool success) {
    auto job = std::make_unique<Job>();
    job->queued.store(queued); job->done.store(done); job->success.store(success);
    jobs.push_back(std::move(job));
  }
  void addMissingRequest() { jobs.push_back(nullptr); }
  void finish(uint32_t i, bool success) { // producer order: queued, result, done(release)
    jobs[i]->queued.store(true, std::memory_order_release);
    jobs[i]->success.store(success, std::memory_order_relaxed);
    jobs[i]->done.store(true, std::memory_order_release);
  }
};

// The shared commit step must publish the whole state/HUD/fault/recording/
// shortcut set and must be inert on repeat. A forgotten HUD/error update or a
// duplicate commit fails here.
void testSharedCommitPublishesEverySideEffect() {
  { Harness h; h.addJob(true, true, true);
    const auto a = h.statusEntry();
    check(a.apply.committed && h.state.load() == CaptureLifecycle::Complete, "complete: state");
    check(h.hudState.load() == kComplete, "complete: HUD state");
    check(h.hudSaved.load() == 1, "complete: HUD saved");
    check(std::string(h.fault).empty() && std::string(h.hudError.load()).empty(), "complete: fault and error untouched");
    check(h.recording.load() && h.disarmCount == 0, "complete: no fault actions");
    const auto again = h.settle();
    check(!again.apply.committed && h.disarmCount == 0 && !h.packageReady, "complete: repeat settle inert"); }
  { Harness h; h.addJob(true, true, false);
    const auto a = h.statusEntry();
    check(a.apply.committed && h.state.load() == CaptureLifecycle::Fault, "fault: state");
    check(std::string(h.fault) == "image-export-failed", "fault: Impl fault string");
    check(h.hudState.load() == kFault, "fault: HUD state");
    check(std::string(h.hudError.load()) == "image-export-failed", "fault: HUD error");
    check(!h.recording.load(), "fault: recording stopped");
    check(h.disarmCount == 1, "fault: shortcut disarmed exactly once");
    const auto again = h.settle();
    check(!again.apply.committed && h.disarmCount == 1 && std::string(h.hudError.load()) == "image-export-failed", "fault: repeat settle inert, no duplicate commit"); }
}

void testBothOrdersConvergeAllSuccess() {
  { Harness h; for (int i = 0; i < 4; ++i) h.addJob(true, true, true);
    const auto a = h.statusEntry();
    check(a.apply.committed && a.decision.outcome == ExportOutcome::Complete, "status-first commits Complete");
    check(h.state.load() == CaptureLifecycle::Complete && h.hudState.load() == kComplete, "status-first publishes state and HUD state");
    check(h.hudSaved.load() == 4 && std::string(h.fault).empty(), "status-first saved and clean fault");
    check(h.recording.load() && h.disarmCount == 0, "status-first keeps recording, no disarm");
    check(h.nextExportEntry() == -1, "status-first nextExport forwards nothing");
    check(!h.packageReady, "image Complete never signs packageReady"); }
  { Harness h; for (int i = 0; i < 4; ++i) h.addJob(true, true, true);
    check(h.nextExportEntry() == -1, "nextExport-first forwards nothing");
    check(h.state.load() == CaptureLifecycle::Complete && h.hudState.load() == kComplete, "nextExport-first terminal state/HUD");
    check(h.hudSaved.load() == 4 && std::string(h.fault).empty(), "nextExport-first saved/fault");
    const auto a = h.statusEntry();
    check(!a.apply.committed, "nextExport-first repeat query stays settled");
    check(h.disarmCount == 0 && !h.packageReady, "nextExport-first: no duplicate action or package signing"); }
}

void testPartialFailureWaitsThenCloses() {
  const auto make = [](Harness& h) {
    h.addJob(true, true, true);    // settled, success
    h.addJob(true, true, false);   // settled, failed
    h.addJob(false, false, false); // pending, unqueued
  };
  Harness h; make(h);
  const auto a = h.statusEntry();
  check(!a.apply.committed && a.decision.outcome == ExportOutcome::Pending, "failed job still waits for all settlement");
  check(h.state.load() == CaptureLifecycle::Exporting && h.hudSaved.load() == 1, "pending keeps Exporting and refreshes saved");
  check(h.nextExportEntry() == 2, "next unqueued job forwarded");
  h.finish(2, false);
  const auto b = h.statusEntry();
  check(b.apply.committed && b.decision.outcome == ExportOutcome::ReadbackFailed, "all settled with failures closes");
  check(h.state.load() == CaptureLifecycle::Fault && h.hudState.load() == kFault, "fault publishes state and HUD state");
  check(std::string(h.fault) == "image-export-failed" && std::string(h.hudError.load()) == "image-export-failed", "fault reason reaches fault and HUD error");
  check(!h.recording.load() && h.disarmCount == 1, "fault stops recording and disarms exactly once");
  check(h.hudSaved.load() == 1, "fault saved from the same facts");
  check(!h.packageReady, "readback failure never signs packageReady");
  Harness r; make(r);
  check(r.nextExportEntry() == 2, "reverse order forwards the pending job first");
  r.finish(2, false);
  const auto c = r.statusEntry();
  check(c.apply.committed && c.decision.outcome == ExportOutcome::ReadbackFailed && r.state.load() == CaptureLifecycle::Fault, "reverse order closes identically");
  check(std::string(r.fault) == "image-export-failed" && std::string(r.hudError.load()) == "image-export-failed" && r.hudSaved.load() == 1 && r.disarmCount == 1, "reverse order publishes identical side effects");
}

void testAllFailedCloses() {
  Harness h; for (int i = 0; i < 3; ++i) h.addJob(true, true, false);
  check(h.nextExportEntry() == -1, "all failed forwards nothing");
  check(h.state.load() == CaptureLifecycle::Fault && h.hudState.load() == kFault, "all failed reaches Fault");
  check(h.hudSaved.load() == 0 && std::string(h.fault) == "image-export-failed" && std::string(h.hudError.load()) == "image-export-failed", "all failed saved/reason/HUD error");
  check(!h.recording.load() && h.disarmCount == 1, "all failed stops recording and disarms once");
}

void testRepeatedQueriesAreStable() {
  Harness h; h.addJob(true, false, false); h.addJob(false, false, false);
  for (int i = 0; i < 3; ++i) {
    const auto a = h.statusEntry();
    check(!a.apply.committed && a.decision.outcome == ExportOutcome::Pending, "pending polls stay pending");
  }
  check(h.state.load() == CaptureLifecycle::Exporting && h.disarmCount == 0, "no commit while pending");
  h.finish(0, true); h.finish(1, true);
  const auto t = h.statusEntry();
  check(t.apply.committed && h.state.load() == CaptureLifecycle::Complete, "terminal commits once");
  const auto u = h.statusEntry();
  const auto v = h.nextExportEntry();
  check(!u.apply.committed && v == -1, "repeated queries never repeat the completion");
  check(h.hudSaved.load() == 2 && h.disarmCount == 0 && h.state.load() == CaptureLifecycle::Complete, "terminal state stable, actions not repeated");
}

void testCancelStopAndFaultAreNeverRevived() {
  { Harness h; for (int i = 0; i < 2; ++i) h.addJob(true, true, true); h.cancelled.store(true);
    const auto a = h.statusEntry();
    check(!a.apply.eligible && !a.apply.committed && h.state.load() == CaptureLifecycle::Exporting, "cancelled export not committed by a poll");
    check(h.nextExportEntry() == -1, "cancelled export forwards nothing"); }
  { Harness h; for (int i = 0; i < 2; ++i) h.addJob(true, true, true); h.stopping.store(true);
    check(h.nextExportEntry() == -1 && h.state.load() == CaptureLifecycle::Exporting, "stopping export not committed"); }
  { Harness h; h.addJob(true, true, false); h.addJob(true, true, false);
    check(h.statusEntry().apply.committed && h.state.load() == CaptureLifecycle::Fault, "fault committed");
    h.jobs.clear(); h.addJob(true, true, true); // population "improves" afterwards
    const auto b = h.statusEntry();
    check(!b.apply.committed && h.state.load() == CaptureLifecycle::Fault && h.hudState.load() == kFault, "Fault never revived to Complete");
    check(h.disarmCount == 1 && std::string(h.hudError.load()) == "image-export-failed", "no second fault action"); }
  { Harness h; h.state.store(CaptureLifecycle::Discard); h.addJob(true, true, true);
    const auto a = h.statusEntry();
    check(!a.apply.committed && h.state.load() == CaptureLifecycle::Discard, "Discard never revived by a poll"); }
}

void testClassificationAndDecisionMapping() {
  const auto cls = [](uint32_t retained, uint32_t queued, uint32_t done, uint32_t failed, bool valid = true) {
    ExportProgress p; p.retained = retained; p.queued = queued; p.done = done; p.failed = failed; p.requestsValid = valid;
    return ClassifyExportProgress(p);
  };
  check(cls(0, 0, 0, 0) == ExportOutcome::InvalidProgress, "empty export is invalid");
  check(cls(MaxSlots, MaxSlots, MaxSlots, 0) == ExportOutcome::Complete, "MaxSlots complete");
  check(cls(MaxSlots + 1, 0, 0, 0) == ExportOutcome::InvalidProgress, "above MaxSlots invalid");
  check(cls(UINT32_MAX, UINT32_MAX, UINT32_MAX, 0) == ExportOutcome::InvalidProgress, "UINT32_MAX rejected without sums");
  check(cls(3, 3, 3, 1) == ExportOutcome::ReadbackFailed, "failures after full settlement");
  check(cls(3, 2, 2, 1) == ExportOutcome::Pending, "failed but unsettled waits");
  check(cls(3, 1, 2, 0) == ExportOutcome::InvalidProgress, "done>queued inversion");
  check(cls(3, 3, 2, 3) == ExportOutcome::InvalidProgress, "failed>done inversion");
  check(cls(3, 4, 0, 0) == ExportOutcome::InvalidProgress, "queued>retained inversion");
  check(cls(1, 1, 1, 0, false) == ExportOutcome::InvalidProgress, "invalid requests");
  check(cls(1, 0, 0, 0) == ExportOutcome::Pending, "single pending job");
  check(cls(4, 4, 4, 4) == ExportOutcome::ReadbackFailed, "all failed");
  { ExportProgress p; p.retained = 2; p.queued = 2; p.done = 2; p.failed = 0; p.requestsValid = true;
    const auto d = DecideExportCommit(p);
    check(d.terminal && d.publish == CaptureLifecycle::Complete && d.saved == 2 && std::string(d.fault).empty(), "Complete decision"); }
  { ExportProgress p; p.retained = 2; p.queued = 2; p.done = 2; p.failed = 1; p.requestsValid = true;
    const auto d = DecideExportCommit(p);
    check(d.terminal && d.publish == CaptureLifecycle::Fault && d.saved == 1 && std::string(d.fault) == "image-export-failed", "ReadbackFailed decision"); }
  { ExportProgress p;
    const auto d = DecideExportCommit(p);
    check(d.terminal && d.publish == CaptureLifecycle::Fault && d.saved == 0 && std::string(d.fault) == "image-export-progress-invalid", "InvalidProgress is an explicit CPU fault"); }
  { ExportProgress p; p.retained = 3; p.queued = 1; p.done = 1; p.failed = 0; p.requestsValid = true;
    const auto d = DecideExportCommit(p);
    check(!d.terminal && d.publish == CaptureLifecycle::Exporting && d.saved == 1, "Pending decision"); }
  check(!ShouldCommitExport(false, false, false) && !ShouldCommitExport(true, true, false) &&
        !ShouldCommitExport(true, false, true) && ShouldCommitExport(true, false, false), "commit gate truth table");
}

void testInvalidProgressAndStructure() {
  { Harness h; h.addJob(true, true, true); h.addMissingRequest();
    const auto a = h.statusEntry();
    check(!a.progress.requestsValid && a.decision.outcome == ExportOutcome::InvalidProgress, "missing request is invalid");
    check(a.apply.committed && h.state.load() == CaptureLifecycle::Fault && std::string(h.fault) == "image-export-progress-invalid" && std::string(h.hudError.load()) == "image-export-progress-invalid", "invalid progress commits explicit CPU fault on state and HUD error");
    check(!h.recording.load() && h.disarmCount == 1 && !h.packageReady, "invalid fault stops recording, disarms once, never signs packageReady"); }
  { Harness h; const auto a = h.statusEntry();
    check(a.decision.outcome == ExportOutcome::InvalidProgress && a.apply.committed, "empty population invalid"); }
  { Harness h; for (uint32_t i = 0; i < MaxSlots + 1; ++i) h.addJob(true, true, true);
    uint32_t next = MaxSlots; const auto p = h.sample(next);
    check(!p.requestsValid, "oversize population rejected before reads");
    check(DecideExportCommit(p).outcome == ExportOutcome::InvalidProgress, "oversize becomes invalid"); }
  { Harness h; for (uint32_t i = 0; i < MaxSlots; ++i) h.addJob(true, true, true);
    const auto a = h.statusEntry();
    check(a.apply.committed && a.decision.outcome == ExportOutcome::Complete && a.progress.retained == MaxSlots, "bounded full table completes"); }
}

void testNextPendingSelection() {
  { Harness h; h.addJob(true, true, true); h.addJob(true, false, false); h.addJob(false, false, false);
    uint32_t next = MaxSlots; const auto p = h.sample(next);
    check(p.done == 1 && p.queued == 2 && next == 2, "first unqueued pending selected"); }
  { Harness h; h.addJob(true, false, false); h.addJob(false, false, false);
    uint32_t next = MaxSlots; h.sample(next);
    check(next == 1, "queued in-flight job skipped, unqueued selected"); }
  { Harness h; h.addJob(true, true, true); h.addJob(true, false, false);
    uint32_t next = MaxSlots; h.sample(next);
    check(next == MaxSlots, "no unqueued pending job"); }
  { Harness h; for (int i = 0; i < 4; ++i) h.addJob(true, true, i != 3);
    const auto a = h.statusEntry();
    check(a.decision.outcome == ExportOutcome::ReadbackFailed, "settled failure reaches ReadbackFailed"); }
}

void testConcurrentHandoffNeverTearsSuccess() {
  const uint32_t kJobs = 8;
  for (uint32_t trial = 0; trial < 40; ++trial) {
    Harness h; for (uint32_t i = 0; i < kJobs; ++i) h.addJob(false, false, false);
    std::atomic<bool> go{false};
    std::thread writer([&] {
      while (!go.load(std::memory_order_acquire)) {}
      for (uint32_t i = 0; i < kJobs; ++i) {
        h.jobs[i]->queued.store(true, std::memory_order_release);
        h.jobs[i]->success.store(true, std::memory_order_relaxed);
        h.jobs[i]->done.store(true, std::memory_order_release);
      }
    });
    go.store(true, std::memory_order_release);
    bool ok = true, complete = false;
    for (uint32_t iter = 0; iter < 200000 && !complete; ++iter) {
      uint32_t next = MaxSlots; const auto p = h.sample(next); const auto d = DecideExportCommit(p);
      if (d.outcome != ExportOutcome::Pending && d.outcome != ExportOutcome::Complete) ok = false;
      if (d.outcome == ExportOutcome::Complete) { complete = true; if (p.done != kJobs || p.retained != kJobs) ok = false; }
    }
    writer.join();
    const auto f = h.settle(); // full shared commit observes the same completion
    check(ok, "acquire-done-first sample never tears queued/done");
    check(f.apply.committed && f.decision.outcome == ExportOutcome::Complete && h.state.load() == CaptureLifecycle::Complete, "shared settle commits completion");
  }
}

void testConcurrentHandoffNeverMisreportsFailures() {
  const uint32_t kJobs = 8;
  for (uint32_t trial = 0; trial < 40; ++trial) {
    Harness h; for (uint32_t i = 0; i < kJobs; ++i) h.addJob(false, false, false);
    std::atomic<bool> go{false};
    std::thread writer([&] {
      while (!go.load(std::memory_order_acquire)) {}
      for (uint32_t i = 0; i < kJobs; ++i) {
        h.jobs[i]->queued.store(true, std::memory_order_release);
        h.jobs[i]->success.store(i % 3 != 0, std::memory_order_relaxed);
        h.jobs[i]->done.store(true, std::memory_order_release);
      }
    });
    go.store(true, std::memory_order_release);
    bool ok = true;
    for (uint32_t iter = 0; iter < 50000; ++iter) {
      uint32_t next = MaxSlots; const auto p = h.sample(next); const auto d = DecideExportCommit(p);
      if (d.outcome == ExportOutcome::InvalidProgress || d.outcome == ExportOutcome::Complete) ok = false;
    }
    writer.join();
    const auto f = h.settle();
    check(ok, "failures never misreport Complete or tear into InvalidProgress");
    check(f.apply.committed && f.decision.outcome == ExportOutcome::ReadbackFailed && std::string(h.hudError.load()) == "image-export-failed", "shared settle closes to fault with HUD error");
  }
}
} // namespace

int main() {
  try {
    testSharedCommitPublishesEverySideEffect();
    testBothOrdersConvergeAllSuccess();
    testPartialFailureWaitsThenCloses();
    testAllFailedCloses();
    testRepeatedQueriesAreStable();
    testCancelStopAndFaultAreNeverRevived();
    testClassificationAndDecisionMapping();
    testInvalidProgressAndStructure();
    testNextPendingSelection();
    testConcurrentHandoffNeverTearsSuccess();
    testConcurrentHandoffNeverMisreportsFailures();
    std::cout << "frame history export commit checks=" << g_checks << " PASS\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "frame history export commit FAIL: " << e.what() << "\n";
    return 1;
  }
}