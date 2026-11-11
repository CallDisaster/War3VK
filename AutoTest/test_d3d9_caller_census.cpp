// Pure caller-census component: module classification, per-caller accounts,
// bounded sets/classes and the cold writer. No device, game or GPU.
#include "../src/d3d9/d3d9_caller_census.h"
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
}

int main() {
  CallerRanges ranges;
  ranges.game = {0x1000, 0x2000};
  ranges.self = {0x4000, 0x5000};
  Check(ranges.classify(At(0x1000)) == Caller::Game, "game begin inclusive");
  Check(ranges.classify(At(0x1fff)) == Caller::Game, "game last byte");
  Check(ranges.classify(At(0x2000)) == Caller::Other, "game end exclusive");
  Check(ranges.classify(At(0x4800)) == Caller::Self, "self range");
  Check(ranges.classify(At(0x0fff)) == Caller::Other, "below game");
  Check(ranges.classify(nullptr) == Caller::Other, "null caller");
  Check(CallerRanges{}.classify(At(0x1000)) == Caller::Other, "unknown modules classify nothing as game");

  CallerCensus c;
  c.noteCall(Api::StretchRect, Caller::Game);
  c.noteCall(Api::StretchRect, Caller::Self);
  c.noteCall(Api::StretchRect, Caller::Self);
  c.noteCall(Api::Count, Caller::Game);
  c.noteCall(Api::Clear, Caller::Count);
  auto& s = c.snapshot();
  Check(s.callers[0].apiCalls[std::size_t(Api::StretchRect)] == 1, "game api count");
  Check(s.callers[1].apiCalls[std::size_t(Api::StretchRect)] == 2, "self api count");
  Check(s.callers[2].apiCalls[std::size_t(Api::StretchRect)] == 0, "other api count");
  Check(s.invalidCallerApis == 2, "invalid api and caller counted, not classified");

  for (uint32_t index : {0u, 31u, 32u, 255u}) c.noteState(Api::SetRenderState, Caller::Game, StateKind::Render, index);
  c.noteState(Api::SetRenderState, Caller::Game, StateKind::Render, 256);
  c.noteState(Api::SetTextureStageState, Caller::Self, StateKind::TextureStage, 63);
  c.noteState(Api::SetTextureStageState, Caller::Self, StateKind::TextureStage, 64);
  c.noteState(Api::SetSamplerState, Caller::Other, StateKind::Sampler, 31);
  c.noteState(Api::SetSamplerState, Caller::Other, StateKind::Sampler, 32);
  Check(s.callers[0].renderStates[0] == 0x80000001u && s.callers[0].renderStates[1] == 1u
        && s.callers[0].renderStates[7] == 0x80000000u, "render state bits");
  Check(s.callers[0].apiCalls[std::size_t(Api::SetRenderState)] == 5, "out-of-range state still an api call");
  Check(s.callers[0].stateOutOfRange == 1 && s.callers[1].stateOutOfRange == 1 && s.callers[2].stateOutOfRange == 1,
        "state bounds per kind");
  Check(s.callers[1].textureStageStates == (uint64_t(1) << 63), "texture stage bit 63");
  Check(s.callers[2].samplerStates == 0x80000000u, "sampler bit 31");

  for (uint32_t i = 0; i < 40; ++i) c.noteFvf(Caller::Game, 0x100 + i % 34);
  Check(s.callers[0].fvfCount == CallerFvfCapacity, "fvf set capacity");
  // 34 distinct values: 32 retained, 32 and 33 dropped; repeats of 0..5 are found.
  Check(s.callers[0].fvfsDropped == 2, "fvf drops beyond capacity, duplicates not dropped");
  Check(s.callers[0].apiCalls[std::size_t(Api::SetFVF)] == 40, "every SetFVF counted");

  c.noteDraw(Caller::Game, DrawKind::IndexedPrimitive, 4, 5896, 5285, true);
  c.noteDraw(Caller::Game, DrawKind::IndexedPrimitive, 5, 10, 70000, false);
  c.noteDraw(Caller::Game, DrawKind::Primitive, 3, 4096, 0, false);
  c.noteDraw(Caller::Self, DrawKind::Primitive, 0, 1, 0, false);
  c.noteDraw(Caller::Self, DrawKind::Primitive, 7, 1, 0, false);
  c.noteDraw(Caller::Self, DrawKind::Unknown, 4, 1, 0, false);
  const auto& g = s.callers[0];
  Check(g.apiCalls[std::size_t(Api::DrawIndexedPrimitive)] == 2 && g.apiCalls[std::size_t(Api::DrawPrimitive)] == 1, "draw apis");
  Check(g.drawKinds[std::size_t(DrawKind::IndexedPrimitive)] == 2, "draw kinds");
  Check(g.primitiveTypes[4] == 1 && g.primitiveTypes[5] == 1 && g.primitiveTypes[3] == 1, "primitive types");
  Check(g.primitiveCountOverProxyBound == 1, "only counts above the proxy bound");
  Check(g.maxPrimitiveCount == 5896 && g.maxVertexCount == 5285, "unknown vertex count ignored");
  Check(s.callers[1].primitiveTypeInvalid == 2, "type 0 and 7 invalid");
  Check(s.invalidCallerApis == 3, "unknown draw kind is invalid, not a draw");

  TextureFacts managed{TextureKind::Texture2D, AllTextureFieldsKnown, 256, 128, 1, 9, 21, 0, 1};
  TextureFacts target{TextureKind::Texture2D, AllTextureFieldsKnown, 1902, 963, 1, 1, 22, 1, 0};
  c.noteTexture(Caller::Game, managed);
  managed.width = 512; managed.levels = 0;
  c.noteTexture(Caller::Game, managed);
  c.noteTexture(Caller::Self, target);
  c.noteTexture(Caller::Game, target);
  Check(s.textureClassCount == 3, "class key includes caller");
  Check(s.textureClasses[0].count == 2 && s.textureClasses[0].maxWidth == 512 && s.textureClasses[0].maxLevels == 9
        && s.textureClasses[0].fullMipChainRequests == 1, "class aggregates");
  Check(s.callers[0].apiCalls[std::size_t(Api::CreateTexture)] == 3, "texture api per caller");
  for (uint32_t i = 0; i < 40; ++i) {
    TextureFacts f{TextureKind::Texture2D, AllTextureFieldsKnown, 4, 4, 1, 1, 100 + i, 0, 1};
    c.noteTexture(Caller::Other, f);
  }
  Check(s.textureClassCount == CallerTextureClassCapacity && s.textureClassesDropped == 11, "class capacity");
  Check(s.saturatedUpdates == 0, "no saturation");

  ConcurrentCallerCensus concurrent;
  concurrent.setRanges(ranges);
  Check(concurrent.gameModuleKnown(), "game range known");
  concurrent.noteCall(Api::Reset, At(0x1800));
  concurrent.noteState(Api::SetSamplerState, StateKind::Sampler, 5, At(0x4100));
  concurrent.noteDraw(DrawKind::Primitive, 4, 2, 0, false, At(0x9000));
  CallerCensusCopy first, second;
  Check(concurrent.trySnapshot(first) && concurrent.trySnapshot(second), "snapshots");
  Check(first.sequence == 1 && second.sequence == 2 && first.recordBusy == 0, "sequence and refusals");
  Check(first.values.callers[0].apiCalls[std::size_t(Api::Reset)] == 1
        && first.values.callers[1].samplerStates == (1u << 5)
        && first.values.callers[2].primitiveTypes[4] == 1, "concurrent wrapper classifies");
  ConcurrentCallerCensus unknown;
  Check(!unknown.gameModuleKnown(), "no game range");

  std::ostringstream out;
  WriteCallerCensus(out, first, 7, "present-milestone", true);
  const auto text = out.str();
  Check(text.rfind("WARVK_D3D9_CALLER_CENSUS {\"schema\":\"warvk-d3d9-caller-census/v1\"", 0) == 0, "marker and schema");
  Check(text.find("\"deviceOrdinal\":7,\"snapshotSequence\":1,\"reason\":\"present-milestone\"") != std::string::npos, "identity");
  Check(text.find("\"game\":{\"apiCalls\":{\"Reset\":1,") != std::string::npos, "game account");
  Check(text.find("\"samplerStates\":[5]") != std::string::npos, "sampler indices");
  Check(text.find("\"primitiveTypes\":[0,0,0,0,1,0,0]") != std::string::npos, "other primitive types");
  Check(text.back() == '}', "complete object");
  std::printf("%s\n", text.c_str());
  std::printf("caller census checks=%u failures=%u bits=%u bytes=%u\n", checks, failures,
    unsigned(sizeof(void*) * 8), unsigned(sizeof(CallerCensusCopy)));
  return failures ? 1 : 0;
}
