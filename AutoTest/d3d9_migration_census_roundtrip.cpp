// CPU-only fixture for the exact production try-cell -> copy -> v2 writer.
// Synthetic API arguments are not game observations or GPU execution.
#include "../src/d3d9/d3d9_migration_census_serialize.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace m = dxvk::d3d9::migration;

static void require(bool value) {
  if (!value) {
    std::fputs("roundtrip fixture unexpectedly refused a serial transaction\n", stderr);
    std::exit(1);
  }
}

static void emit(m::ConcurrentCensus& census, uint64_t deviceOrdinal,
                 const char* reason) {
  m::CensusCopy copy;
  require(census.trySnapshot(copy));
  const auto refusals = census.observationRefusals();
  const m::CensusExportInfo info = {deviceOrdinal, sizeof(void*)*8, 66, reason,
    false, true, true, true};
  m::WriteCensusSnapshot(std::cout, copy, refusals, info);
  std::cout << '\n';
}

int main() {
  // An unused device can emit an empty destruction record. Do not invent a
  // Present milestone with zero Presents, or reuse a destroyed device identity.
  m::ConcurrentCensus unused;
  emit(unused, 1, "normal-device-destruction");
  m::ConcurrentCensus census;
  for (uint32_t i = 0; i < 48; ++i) {
    m::DrawFacts draw;
    draw.kind = m::DrawKind(1 + i % 4);
    draw.knownMask = m::PrimitiveTypeKnown | m::PrimitiveCountKnown;
    draw.primitiveType = 4;
    draw.primitiveCount = 1 + i;
    if (draw.kind == m::DrawKind::IndexedPrimitive ||
        draw.kind == m::DrawKind::IndexedPrimitiveUP) {
      draw.knownMask |= m::VertexCountKnown;
      draw.vertexCount = 3 * (i + 1);
    }
    if (draw.kind == m::DrawKind::PrimitiveUP ||
        draw.kind == m::DrawKind::IndexedPrimitiveUP) {
      draw.knownMask |= m::StreamStrideKnown;
      draw.streamStride = 40;
    }
    if (draw.kind == m::DrawKind::IndexedPrimitiveUP) {
      draw.knownMask |= m::IndexFormatKnown;
      draw.indexFormat = 101;
    }
    require(census.noteDraw(draw));
    m::TextureFacts texture;
    texture.kind = m::TextureKind(1 + i % 3);
    texture.knownMask = m::AllTextureFieldsKnown;
    texture.width = 128;
    texture.height = texture.kind == m::TextureKind::Cube ? 128 : 64;
    texture.depth = texture.kind == m::TextureKind::Volume ? 8 : 1;
    texture.levels = i % 2 ? 8 : 0;
    texture.format = 21;
    texture.pool = 1;
    require(census.noteTexture(texture));
    m::VsKeySamples::Key vs = {};
    m::FsKeySamples::Key fs = {};
    // First 32 retained; later unseen observations refused by fixed capacity.
    // These synthetic key words are serialized only, NOT compiled as shaders.
    vs[0] = i;
    fs[0] = i;
    require(census.noteVsKey(vs));
    require(census.noteFsKey(fs));
  }
  bool milestone = false;
  require(census.notePresent(milestone));
  require(milestone);
  emit(census, 2, "present-milestone");
  require(census.noteCall(m::Api::Reset));
  emit(census, 2, "normal-device-destruction");
  return 0;
}
