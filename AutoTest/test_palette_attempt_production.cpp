// Actual production recorder, not a model of its recovery predicate.
#include "../src/d3d9/war3/tools/war3_palette_object_evidence.h"
#include <cstdio>
#include <initializer_list>

using namespace dxvk::war3::tools::evidence;
namespace {
struct Step {
  PaletteObjectStage stage;
  uint64_t attempt;
  PaletteObjectSource source = PaletteObjectSource::ArenaSlot;
  uint64_t hit = 71;
  bool cleared = false;
};
using S = PaletteObjectStage;
using P = PaletteObjectSource;
constexpr uint64_t unknown = ~uint64_t(0);
PaletteObjectEvidence recorder;
PaletteObjectEventRecord terminal;
unsigned checks = 0, failures = 0;
void Emit(void*, const PaletteObjectEventRecord& e) {
  if (e.terminal != PaletteObjectTerminal::None) terminal = e;
}
void Check(bool value, const char* name) {
  ++checks;
  if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
}
void Run(const char* name, std::initializer_list<Step> steps, bool recovered, bool advanceFrame = true) {
  recorder.Configure(Emit, nullptr);
  recorder.Reset(1, 1);
  terminal = {};
  PaletteObjectKey key{};
  key.renderablePart = 42; key.sessionGeneration = 1; key.mapEpoch = 1;
  key.lifecycleIdentity = 3; key.deviceEpoch = 1;
  key.identityWeak = false; key.epochUnknown = false;
  PaletteObjectFrames f{};
  f.renderFrame = 1;
  for (const auto& step : steps) {
    f.attemptSerial = step.attempt;
    switch (step.stage) {
      case S::Rejected: recorder.NoteReject(key, PaletteObjectRejectReason::R1, f); break;
      case S::ServedCandidate: recorder.NoteServed(key, step.source, step.hit, f); break;
      case S::Enqueued: recorder.NoteEnqueued(key, step.source, step.hit, f, step.cleared); break;
      case S::Drawn: recorder.NoteDrawn(key, step.source, step.hit, f, step.cleared); break;
      default: break;
    }
    if (advanceFrame) ++f.renderFrame;
  }
  recorder.CloseWindow(f.renderFrame);
  Check((terminal.terminal == PaletteObjectTerminal::Recovered) == recovered, name);
  Check(recorder.counters().closedRecovered == unsigned(recovered), "terminal counter agrees");
}
void CheckTerminalAssociation() {
  PaletteObjectKey key{};
  key.renderablePart = 42; key.sessionGeneration = 1; key.mapEpoch = 1;
  key.lifecycleIdentity = 3; key.deviceEpoch = 1;
  key.identityWeak = false; key.epochUnknown = false;
  PaletteObjectFrames f{}; f.renderFrame = 1; f.attemptSerial = 0x100000007ull;
  recorder.Reset(1, 1); terminal = {};
  recorder.NoteFirstSight(key, f);
  recorder.CloseWindow(2);
  Check(terminal.terminal == PaletteObjectTerminal::ObservationClosed, "first sight stays observation");
  Check(terminal.frames.attemptSerial == f.attemptSerial, "observation terminal preserves attempt");
  recorder.Reset(1, 1); terminal = {};
  recorder.NoteReject(key, PaletteObjectRejectReason::R1, f);
  ++f.renderFrame; ++f.attemptSerial;
  recorder.NoteServed(key, P::ArenaSlot, 71, f);
  recorder.NoteObjectGone(key);
  Check(terminal.terminal == PaletteObjectTerminal::ObjectGone, "object gone stays terminal");
  Check(terminal.frames.attemptSerial == f.attemptSerial, "object gone uses current not first attempt");
}
}
int main() {
  Run("one complete attempt", {{S::Rejected,1},{S::ServedCandidate,1},{S::Enqueued,1},{S::Drawn,1}}, true);
  Run("three incomplete attempts cannot combine", {{S::Rejected,1},{S::ServedCandidate,2},{S::Enqueued,3},{S::Drawn,4}}, false);
  Run("old served cannot authorize later enqueue/draw", {{S::Rejected,1},{S::ServedCandidate,2},{S::Enqueued,3},{S::Drawn,3}}, false);
  Run("unknown draw cannot certify", {{S::Rejected,1},{S::ServedCandidate,2},{S::Enqueued,2},{S::Drawn,unknown}}, false);
  Run("all unknown cannot certify", {{S::Rejected,unknown},{S::ServedCandidate,unknown},{S::Enqueued,unknown},{S::Drawn,unknown}}, false);
  Run("final source mismatch", {{S::Rejected,1},{S::ServedCandidate,2},{S::Enqueued,2},{S::Drawn,2,P::PoseKernel}}, false);
  Run("final hit mismatch", {{S::Rejected,1},{S::ServedCandidate,2},{S::Enqueued,2},{S::Drawn,2,P::ArenaSlot,72}}, false);
  Run("enqueue cleared", {{S::Rejected,1},{S::ServedCandidate,2},{S::Enqueued,2,P::ArenaSlot,71,true},{S::Drawn,2}}, false);
  Run("draw cleared", {{S::Rejected,1},{S::ServedCandidate,2},{S::Enqueued,2},{S::Drawn,2,P::ArenaSlot,71,true}}, false);
  Run("interleaved A B A cannot resurrect A", {{S::Rejected,1},{S::ServedCandidate,2},{S::Enqueued,2},{S::ServedCandidate,3},{S::Drawn,2}}, false);
  Run("new incomplete attempt invalidates old completion", {{S::Rejected,1},{S::ServedCandidate,1},{S::Enqueued,1},{S::Drawn,1},{S::ServedCandidate,2}}, false);
  Run("later complete retry allowed", {{S::Rejected,1},{S::ServedCandidate,1},{S::Rejected,2},{S::ServedCandidate,2},{S::Enqueued,2},{S::Drawn,2}}, true);
  Run("missing served in old attempt does not poison complete retry", {{S::Rejected,1},{S::Enqueued,1},{S::Rejected,2},{S::ServedCandidate,2},{S::Enqueued,2},{S::Drawn,2}}, true);
  Check(terminal.frames.attemptSerial == 2 && terminal.source == P::ArenaSlot && terminal.hitKey == 71,
        "missing-served retry terminal belongs to complete attempt 2");
  Run("missing enqueue in old attempt does not poison complete retry", {{S::Rejected,1},{S::ServedCandidate,1},{S::Drawn,1},{S::Rejected,2},{S::ServedCandidate,2},{S::Enqueued,2},{S::Drawn,2}}, true);
  Check(terminal.frames.attemptSerial == 2 && terminal.sawSubmit && terminal.sawDraw,
        "missing-enqueue retry terminal has attempt 2 submit and draw witnesses");
  Run("true old attempt backtrack remains sticky across complete retry", {{S::Rejected,1},{S::ServedCandidate,1},{S::Enqueued,1},{S::ServedCandidate,1},{S::Rejected,2},{S::ServedCandidate,2},{S::Enqueued,2},{S::Drawn,2}}, false);
  Check(terminal.terminal == PaletteObjectTerminal::Unclosed && terminal.frames.attemptSerial == 2 &&
        terminal.source == P::ArenaSlot && terminal.hitKey == 71 && terminal.sawSubmit && terminal.sawDraw,
        "historical order violation rejects even a complete current terminal");
  Check(recorder.counters().droppedPayloadConflict == 0 && recorder.counters().droppedDuplicatePerFrame == 0,
        "historical backtrack refusal is not caused by deduplication or lost payload");
  Run("known zero attempt allowed", {{S::Rejected,0},{S::ServedCandidate,0},{S::Enqueued,0},{S::Drawn,0}}, true);
  Run("64 bit serial exact", {{S::Rejected,1},{S::ServedCandidate,0x100000001ull},{S::Enqueued,1},{S::Drawn,1}}, false);
  Run("unknown source", {{S::Rejected,1},{S::ServedCandidate,1,P::Unknown},{S::Enqueued,1,P::Unknown},{S::Drawn,1,P::Unknown}}, false);
  Run("zero hit key", {{S::Rejected,1},{S::ServedCandidate,1,P::ArenaSlot,0},{S::Enqueued,1,P::ArenaSlot,0},{S::Drawn,1,P::ArenaSlot,0}}, false);
  Run("same attempt cannot erase bad enqueue", {{S::Rejected,1},{S::ServedCandidate,1},{S::Enqueued,1,P::ArenaSlot,72},{S::Enqueued,1},{S::Drawn,1}}, false);
  Run("new attempt may recover after bad enqueue", {{S::Rejected,1},{S::ServedCandidate,1},{S::Enqueued,1,P::ArenaSlot,72},{S::ServedCandidate,2},{S::Enqueued,2},{S::Drawn,2}}, true);
  Run("adjacent same-frame duplicate remains harmless", {{S::Rejected,1},{S::Rejected,1},{S::ServedCandidate,1},{S::ServedCandidate,1},{S::Enqueued,1},{S::Enqueued,1},{S::Drawn,1},{S::Drawn,1}}, true, false);
  for (const S repeated : {S::Rejected, S::ServedCandidate, S::Enqueued}) {
    Run("same-frame backtrack is not equivalent duplicate", {{S::Rejected,1},{S::ServedCandidate,1},{S::Enqueued,1},{S::Drawn,1},{repeated,1}}, false, false);
    Check(recorder.counters().droppedPayloadConflict == 1, "coalesced backtrack remains visible as loss");
  }
  Run("same-frame different reject attempt is conflict", {{S::Rejected,1},{S::Rejected,2},{S::ServedCandidate,1},{S::Enqueued,1},{S::Drawn,1}}, false, false);
  Check(recorder.counters().droppedPayloadConflict == 1, "different-attempt reject not silently deduplicated");
  CheckTerminalAssociation();
  std::printf("ATTEMPT_PRODUCTION checks=%u failures=%u recorderBytes=%zu\n", checks, failures, sizeof(recorder));
  return failures ? 1 : 0;
}
