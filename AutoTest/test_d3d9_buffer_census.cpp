// Pure buffer-census component: creation classes, Lock patterns, frames and
// the cold writer. No device, game or GPU.
#include "../src/d3d9/d3d9_buffer_census.h"
#include <cstdio>
#include <sstream>
#include <string>

using namespace dxvk::d3d9::migration;

namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}
const void* At(uintptr_t value) { return reinterpret_cast<const void*>(value); }
constexpr uint32_t Dynamic = 0x200, WriteOnly = 0x8;
}

int main() {
  BufferCensus c;
  const auto& s = c.snapshot();
  const BufferFacts ring{BufferKind::Vertex, 4096, Dynamic | WriteOnly, 0, 0, 0x112};
  BufferCursor a, b, other, index, unknown;
  c.noteCreate(a, Caller::Game, ring);
  c.noteCreate(b, Caller::Game, ring);
  c.noteCreate(other, Caller::Self, ring);
  c.noteCreate(index, Caller::Game, {BufferKind::Index, 1000, WriteOnly, 0, 101, 0});
  c.noteCreate(unknown, Caller::Count, ring);
  c.noteCreate(unknown, Caller::Game, {BufferKind::Count, 1, 0, 0, 0, 0});
  Check(s.classCount == 3 && a.classIndex == 0 && b.classIndex == 0 && other.classIndex == 1 && index.classIndex == 2,
    "classes by creator and facts");
  Check(s.classes[0].created == 2 && s.classes[1].created == 1 && s.classes[2].facts.format == 101, "creation counts");
  Check(unknown.classIndex == NotClassified, "invalid caller or kind is not classified");

  // A NOOVERWRITE ring with a DISCARD wrap, in two frames.
  c.noteLock(a, Caller::Game, 0, 1024, LockDiscard);
  c.noteUnlock(a);
  c.noteLock(a, Caller::Game, 1024, 2048, LockNoOverwrite);
  c.noteUnlock(a);
  c.notePresent();
  c.noteLock(a, Caller::Game, 0, 256, LockDiscard);
  c.noteUnlock(a);
  c.noteLock(a, Caller::Self, 256, 0, LockNoOverwrite | LockNoSysLock);
  const auto& r = s.classes[0];
  Check(r.locks == 4 && r.lockCallers[0] == 3 && r.lockCallers[1] == 1 && r.unlocks == 3, "lock and unlock counts");
  Check(r.discard == 2 && r.noOverwrite == 2 && r.noSysLock == 1 && !r.otherFlags, "flag counts");
  Check(r.sequential == 2 && r.wrapped == 1 && r.wrappedDiscard == 1 && !r.beyondWritten, "ring pattern");
  Check(r.sizeZero == 1 && !r.whole && r.bytes == 1024 + 2048 + 256 + 3840 && r.maxBytes == 3840, "lock bytes");
  Check(r.sizeBins[0] == 1 && r.sizeBins[1] == 1 && r.sizeBins[2] == 2, "size bins");
  Check(r.framesLocked == 2 && r.maxLocksPerFrame == 2 && r.maxBytesPerFrame == 256 + 3840, "frames");
  c.noteLock(a, Caller::Game, 0, 0, 0);
  Check(r.nested == 1 && r.whole == 1 && r.wrapped == 2 && r.wrappedDiscard == 1, "nested whole lock");

  // A second buffer of the class has its own cursor.
  c.noteLock(b, Caller::Game, 512, 16, 0);
  Check(r.beyondWritten == 1 && r.sequential == 2 && r.wrapped == 2, "per-buffer cursor");
  c.noteLock(b, Caller::Game, 4000, 200, LockReadOnly | 0x1);
  Check(r.outOfRange == 1 && r.readOnly == 1 && r.otherFlags == 1 && r.bytes == 1024 + 2048 + 256 + 3840 + 4096 + 16,
    "out of range and unknown flags");
  Check(b.lastEnd == 528 && b.written == 528 && b.depth == 2, "a refused range leaves the cursor end");
  c.noteLock(index, Caller::Game, 0, 1000, 0);
  Check(s.classes[2].whole == 1 && s.classes[2].sizeBins[1] == 1, "index whole lock");
  c.noteLock(unknown, Caller::Game, 0, 4, 0);
  c.noteUnlock(unknown);
  Check(s.unclassifiedLocks == 1 && s.unclassifiedUnlocks == 1, "unclassified buffer");

  BufferCensus bins;
  BufferCursor big;
  bins.noteCreate(big, Caller::Game, {BufferKind::Vertex, 4u << 20, 0, 0, 0, 0});
  for (uint32_t size : {256u, 257u, 1048576u, 1048577u}) bins.noteLock(big, Caller::Game, 0, size, 0);
  const auto& bb = bins.snapshot().classes[0];
  Check(bb.sizeBins[0] == 1 && bb.sizeBins[1] == 1 && bb.sizeBins[6] == 1 && bb.sizeBins[7] == 1, "bin bounds");

  BufferCensus full;
  BufferCursor cursor;
  for (uint32_t i = 0; i < BufferClassCapacity + 3; ++i)
    full.noteCreate(cursor, Caller::Game, {BufferKind::Vertex, 64 + i, 0, 0, 0, 0});
  Check(full.snapshot().classCount == BufferClassCapacity && full.snapshot().classesDropped == 3, "class capacity");
  Check(cursor.classIndex == BufferClassCapacity - 1, "a dropped class keeps the cursor");
  Check(s.saturatedUpdates == 0, "no saturation");

  CallerRanges ranges;
  ranges.game = {0x1000, 0x2000};
  ranges.self = {0x4000, 0x5000};
  ConcurrentBufferCensus concurrent;
  concurrent.setRanges(ranges);
  Check(concurrent.gameModuleKnown(), "game range known");
  BufferCursor live;
  concurrent.noteCreate(live, ring, At(0x1800));
  concurrent.noteLock(live, 0, 64, LockDiscard, At(0x4100));
  concurrent.noteUnlock(live);
  concurrent.notePresent();
  BufferCensusCopy first, second;
  Check(concurrent.trySnapshot(first) && concurrent.trySnapshot(second), "snapshots");
  Check(first.sequence == 1 && second.sequence == 2 && first.recordBusy == 0, "sequence and refusals");
  Check(first.values.classes[0].caller == 0 && first.values.classes[0].lockCallers[1] == 1
        && first.values.presents == 1, "concurrent wrapper classifies");

  std::ostringstream out;
  WriteBufferCensus(out, first, 7, "present-milestone", true);
  const auto text = out.str();
  Check(text.rfind("WARVK_D3D9_BUFFER_CENSUS {\"schema\":\"warvk-d3d9-buffer-census/v1\"", 0) == 0, "marker and schema");
  Check(text.find("\"deviceOrdinal\":7,\"snapshotSequence\":1,\"reason\":\"present-milestone\"") != std::string::npos, "identity");
  Check(text.find("\"lockSizeBounds\":[256,1024,4096,16384,65536,262144,1048576]") != std::string::npos, "bounds");
  Check(text.find("{\"caller\":\"game\",\"kind\":\"vertex\",\"length\":4096,\"usage\":520,\"pool\":0,\"format\":0,\"fvf\":274,"
                  "\"created\":1,\"lockCallers\":[0,1,0],\"locks\":1,\"unlocks\":1,\"bytes\":64") != std::string::npos, "class");
  Check(text.find("\"flags\":{\"discard\":1,\"noOverwrite\":0,") != std::string::npos, "flags object");
  Check(text.back() == '}', "complete object");
  std::printf("%s\n", text.c_str());
  std::printf("buffer census checks=%u failures=%u bits=%u bytes=%u\n", checks, failures,
    unsigned(sizeof(void*) * 8), unsigned(sizeof(BufferCensusCopy)));
  return failures ? 1 : 0;
}
