// FixedDrawEncoder and FixedDrawDecoder against the functions they stand in for. Over long random
// walks of draws and fixed-function state:
//   - the encoder's packet must equal EncodeFixedDraw's byte for byte and its verdict
//     ValidFixedFunction's, whatever changed between two draws and whether the state is valid or not;
//   - the decoder's result and outputs must equal DecodeFixedDraw's for every packet, including
//     packets damaged after encoding, and it must leave its outputs alone when it refuses one.
#include "../tools/render_host/d3d9_ff_fast_encode.h"
#include "../tools/render_host/d3d9_ff_fast_decode.h"
#include <cstdio>
#include <limits>
#include <random>
using namespace warvk::host;

static uint64_t g_checks = 0, g_failures = 0;
static void Check(bool ok, const char* what, uint64_t step) {
  ++g_checks;
  if (!ok && g_failures++ < 20) std::printf("FAIL step %llu: %s\n", static_cast<unsigned long long>(step), what);
}

namespace {
struct Walk {
  std::mt19937 rng;
  d9::DrawCommand draw;
  d9::FixedFunctionState state;
  bool allowInvalid;
  Walk(uint32_t seed, bool invalid) : rng(seed), allowInvalid(invalid) {
    draw.resources[0] = {1, 1}; draw.resources[5] = {2, 1};
  }
  uint32_t pick(uint32_t n) { return rng() % n; }
  float scalar() {
    if (allowInvalid && pick(40) == 0) {
      switch (pick(4)) {
        case 0: return std::numeric_limits<float>::infinity();
        case 1: return -std::numeric_limits<float>::infinity();
        case 2: return std::numeric_limits<float>::quiet_NaN();
        default: return -0.0f;
      }
    }
    if (pick(16) == 0) return std::numeric_limits<float>::denorm_min() * float(pick(1000));
    return float(int(rng() % 65536) - 32768) / 128.0f;
  }
  D3DCOLORVALUE colour() { return {scalar(), scalar(), scalar(), scalar()}; }
  D3DLIGHT9 light() {
    D3DLIGHT9 l{};
    l.Type = D3DLIGHTTYPE(allowInvalid && pick(12) == 0 ? pick(6) : 1 + pick(3));
    l.Diffuse = colour(); l.Specular = colour(); l.Ambient = colour();
    l.Position = {scalar(), scalar(), scalar()}; l.Direction = {scalar(), scalar(), scalar()};
    const auto positive = [&] { const float v = scalar(); return allowInvalid && pick(10) == 0 ? v : std::fabs(v); };
    l.Range = positive(); l.Falloff = scalar(); l.Attenuation0 = positive(); l.Attenuation1 = positive();
    l.Attenuation2 = positive(); l.Theta = scalar(); l.Phi = scalar();
    return l;
  }
  // A resource reference DecodeDraw takes in this slot; in the invalid walks, now and then one it refuses.
  d9::ResourceRef ref(uint32_t slot) {
    if (allowInvalid && pick(25) == 0) return {rng(), rng() % 3};
    const bool optional = slot >= 1 && slot <= 4;
    if (optional && pick(3) == 0) return {};
    return {1 + pick(d9::MaxResources), 1 + pick(1000)};
  }
  // One draw's worth of change: always the draw's own words, sometimes one or more state sections.
  void step() {
    auto& s = draw.state;
    s.startIndex = rng(); s.primitiveCount = rng(); s.numVertices = rng(); s.minVertexIndex = rng();
    s.baseVertexIndex = int32_t(rng());
    if (pick(4) == 0) {
      const uint32_t slot = pick(6);
      draw.resources[slot] = ref(slot);
      // Fixed function has at most one of the two shaders.
      if (!allowInvalid && draw.resources[3].id && draw.resources[4].id) draw.resources[slot == 3 ? 4 : 3] = {};
    }
    if (pick(8) == 0) { s.streamOffset = rng(); s.stride = rng(); }
    if (pick(16) == 0) { s.viewport.X = rng(); s.viewport.Y = rng(); s.viewport.Width = rng(); s.viewport.Height = rng();
      s.viewport.MinZ = scalar(); s.viewport.MaxZ = scalar(); }
    if (pick(6) == 0) { s.render.zEnable = pick(3); s.render.zWriteEnable = pick(2); s.render.zFunc = pick(9);
      s.render.cullMode = pick(4); s.render.alphaBlendEnable = pick(2); s.render.srcBlend = pick(16);
      s.render.destBlend = pick(16); s.render.blendOp = pick(6); s.render.colorWriteMask = pick(16); }
    if (pick(32) == 0) s.primitiveType = D3DPRIMITIVETYPE(pick(7));
    if (pick(20) == 0) s.vertexFloatConstants[pick(uint32_t(s.vertexFloatConstants.size()))] = scalar();
    if (pick(20) == 0) s.pixelFloatConstants[pick(uint32_t(s.pixelFloatConstants.size()))] = scalar();

    if (pick(2) == 0) state.transforms[0].m[pick(4)][pick(4)] = scalar();           // the world matrix, often
    if (pick(8) == 0) state.transforms[pick(11)].m[pick(4)][pick(4)] = scalar();
    if (pick(10) == 0) {
      const uint32_t i = pick(uint32_t(state.render.size()));
      uint32_t value = pick(4);
      if (allowInvalid && pick(6) == 0) value = rng();
      else while (!d9::ValidFFRender(d9::FFRenderStates[i], value)) value = pick(4);
      state.render[i] = value;
    }
    if (pick(5) == 0) {
      const uint32_t stage = pick(8), i = pick(uint32_t(state.stages[stage].size()));
      uint32_t value = pick(8);
      if (allowInvalid && pick(6) == 0) value = rng();
      else while (!d9::ValidFFStage(stage, d9::FFStageStates[i], value)) value = pick(8);
      state.stages[stage][i] = value;
    }
    if (pick(12) == 0) { state.material.Diffuse = colour();
      state.material.Power = allowInvalid && pick(3) == 0 ? scalar() : std::fabs(scalar()); }
    if (pick(24) == 0) { state.material.Ambient = colour(); state.material.Specular = colour(); state.material.Emissive = colour(); }
    if (pick(9) == 0) {
      state.lightCount = allowInvalid && pick(15) == 0 ? 9 + pick(100) : pick(9);
      // A slot that comes into use holds a light in the valid walks.
      if (!allowInvalid) for (uint32_t i = 0; i < state.lightCount; ++i)
        if (!d9::ValidLight(state.lights[i])) state.lights[i] = light();
    }
    if (pick(7) == 0) state.lights[pick(8)] = light();
    // An all-zero slot: anywhere in the invalid walks, past the lights in use in the valid ones.
    if (pick(30) == 0) {
      const uint32_t slot = pick(8);
      if (allowInvalid || slot >= state.lightCount) state.lights[slot] = D3DLIGHT9{};
    }
    if (pick(50) == 0) for (auto& l : state.lights) l = light();
    if (pick(200) == 0) state = d9::FixedFunctionState{};
    if (pick(300) == 0) { draw = d9::DrawCommand{}; draw.resources[0] = {1, 1}; draw.resources[5] = {2, 1}; }
  }
};

uint64_t g_decoded = 0, g_refused = 0;
// The decoder against DecodeFixedDraw on one packet. Outputs start from a recognisable value, which a
// refusal must leave in place.
void CompareDecode(d9::FixedDrawDecoder& decoder, const d9::FixedDrawPacket& packet, uint64_t step) {
  d9::DrawCommand expectedDraw, actualDraw;
  expectedDraw.state.stride = actualDraw.state.stride = 0x5eed5eedu;
  d9::FixedFunctionState expectedState, actualState;
  expectedState.lightCount = actualState.lightCount = 0x5eedu;
  const bool expected = d9::DecodeFixedDraw(packet.data(), d9::FixedDrawBytes, expectedDraw, expectedState);
  const bool actual = decoder.decode(packet.data(), actualDraw, actualState);
  Check(actual == expected, "decoder's result differs from DecodeFixedDraw", step);
  Check(d9::EncodeDraw(actualDraw) == d9::EncodeDraw(expectedDraw), "decoded draw differs", step);
  Check(!std::memcmp(&actualState, &expectedState, sizeof(expectedState)), "decoded state differs", step);
  (expected ? g_decoded : g_refused)++;
}

void Compare(d9::FixedDrawEncoder& encoder, d9::FixedDrawDecoder& decoder, Walk& walk, uint64_t step) {
  const auto expected = d9::EncodeFixedDraw(walk.draw, walk.state);
  const auto& actual = encoder.encode(walk.draw, walk.state);
  Check(actual == expected, "packet differs from EncodeFixedDraw", step);
  Check(encoder.valid() == d9::ValidFixedFunction(walk.state), "verdict differs from ValidFixedFunction", step);
  CompareDecode(decoder, expected, step);
  // Now and then the packet arrives damaged: a flipped byte anywhere, a count that does not match the
  // slots, something in a slot past the count. The next packet is whole again.
  if (walk.pick(6) == 0) {
    auto damaged = expected;
    switch (walk.pick(4)) {
      case 0: damaged[walk.pick(d9::FixedDrawBytes)] ^= uint8_t(1u << walk.pick(8)); break;
      case 1: damaged[d9::DrawBytes + walk.pick(d9::FixedDrawBytes - d9::DrawBytes)] = uint8_t(walk.rng()); break;
      case 2: { const uint32_t count = walk.pick(12); std::memcpy(damaged.data() + d9::FixedDrawBytes - 8 * 104 - 4, &count, 4); break; }
      default: damaged[d9::FixedDrawBytes - 1 - walk.pick(8 * 104)] = uint8_t(1 + walk.pick(255)); break;
    }
    CompareDecode(decoder, damaged, step);
  }
}
}

int main() {
  // Before the first draw nothing is valid.
  { d9::FixedDrawEncoder encoder; Check(!encoder.valid(), "an encoder without a draw claims a valid state", 0); }
  // The default state and draw, from a new encoder and decoder, twice.
  {
    d9::FixedDrawEncoder encoder; d9::FixedDrawDecoder decoder; Walk walk(1, false);
    Compare(encoder, decoder, walk, 0); Compare(encoder, decoder, walk, 1);
    Check(encoder.valid(), "the default state is valid", 1);
  }
  Check(!d9::FixedDrawDecoder().decode(nullptr, *new d9::DrawCommand, *new d9::FixedFunctionState), "a null packet decodes", 0);
  // Valid-state walks, then walks that also visit every kind of invalid state.
  uint64_t valid = 0, invalid = 0, steps = 0;
  for (uint32_t seed = 0; seed < 8; ++seed) {
    for (const bool allowInvalid : {false, true}) {
      d9::FixedDrawEncoder encoder; d9::FixedDrawDecoder decoder; Walk walk(0x46440000u + seed, allowInvalid);
      for (uint32_t i = 0; i < 40000; ++i, ++steps) {
        walk.step();
        Compare(encoder, decoder, walk, steps);
        (d9::ValidFixedFunction(walk.state) ? valid : invalid)++;
        // The same draw again: nothing changed, and nothing may change in the answer.
        if ((i & 7) == 0) Compare(encoder, decoder, walk, steps);
      }
      // An encoder and a decoder that start in the middle of the walk agree with those that followed it.
      d9::FixedDrawEncoder late;
      const auto packet = encoder.encode(walk.draw, walk.state);
      Check(late.encode(walk.draw, walk.state) == packet, "late encoder disagrees", steps);
      Check(late.valid() == encoder.valid(), "late encoder's verdict disagrees", steps);
      d9::FixedDrawDecoder lateDecoder;
      CompareDecode(lateDecoder, packet, steps);
    }
  }
  Check(valid > 50000 && invalid > 50000, "the walks covered both valid and invalid states", steps);
  Check(g_decoded > 50000 && g_refused > 50000, "the walks covered both accepted and refused packets", steps);
  // Lights shrinking and growing over slots that held other lights, each count against each.
  {
    d9::FixedDrawEncoder encoder; d9::FixedDrawDecoder decoder; Walk walk(7, false);
    for (auto& l : walk.state.lights) l = walk.light();
    for (uint32_t from = 0; from <= 8; ++from) for (uint32_t to = 0; to <= 8; ++to) {
      walk.state.lightCount = from; Compare(encoder, decoder, walk, from * 16 + to);
      walk.state.lights[walk.pick(8)] = walk.light();
      walk.state.lightCount = to; Compare(encoder, decoder, walk, from * 16 + to);
    }
  }
  std::printf("ff-fast-encode: %u-bit checks=%llu failures=%llu validStates=%llu invalidStates=%llu decoded=%llu refused=%llu\n",
    unsigned(sizeof(void*) * 8), static_cast<unsigned long long>(g_checks), static_cast<unsigned long long>(g_failures),
    static_cast<unsigned long long>(valid), static_cast<unsigned long long>(invalid),
    static_cast<unsigned long long>(g_decoded), static_cast<unsigned long long>(g_refused));
  return g_failures ? 1 : 0;
}
