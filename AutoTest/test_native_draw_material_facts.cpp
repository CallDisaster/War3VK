#include "../src/d3d9/war3/render/war3_native_draw_material_facts.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace n = dxvk::war3::render::native_input;
static unsigned checks = 0;
static void check(bool value) { ++checks; assert(value); }

struct Inputs {
  uint32_t renderStates[n::RenderStateCount] = {};
  uint32_t stage[n::StageStateCount] = {};
  uint32_t sampler[n::SamplerStateCount] = {};
  uint32_t material[n::MaterialWordCount] = {};
  uint32_t transform[n::TextureTransformWordCount] = {};
  n::MaterialFactsViews views() const {
    return {renderStates, stage, sampler, material, transform};
  }
};

static void unknown(const n::NativeDrawMaterialFacts& facts) {
  check(facts.schemaVersion == 1 && facts.available == 0);
  check(facts.renderFrame == 0 && facts.mapEpoch == 0 && facts.deviceEpoch == 0
    && facts.renderablePart == 0 && facts.layerIndex == 0 && facts.shaderMask == 0);
  for (auto word : facts.renderStates) check(word == 0);
  for (auto word : facts.textureStage0) check(word == 0);
  for (auto word : facts.sampler0) check(word == 0);
  for (auto word : facts.material) check(word == 0);
  for (auto word : facts.textureTransform0) check(word == 0);
  check(!n::MaterialFactsSchemaValid(facts));
}

int main() {
  const n::MaterialFactsKey key{31, 7, 9, 0x123400, 0, 3};
  const auto invalid = reinterpret_cast<const uint32_t*>(uintptr_t(1));
  const n::MaterialFactsViews invalidViews{invalid, invalid, invalid, invalid, invalid};
  // This exercises the actual helper: any attempted view read would fault.
  const auto disabled = n::CaptureMaterialFacts(false, key, invalidViews);
  unknown(disabled);
  check(!n::MaterialFactsMatches(disabled, key));
  check(!n::MaterialFactsSameDrawIdentity(disabled, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  unknown(n::CaptureMaterialFacts(true, key, {}));

  const uint32_t expectedRender[] = {15,25,24,27,19,20,171,206,207,208,209,7,14,23,22,168,60};
  const uint32_t expectedStage[] = {1,2,3,4,5,6,11,24,26,27};
  check(n::RenderStateCount == 17 && n::StageStateCount == 10 && n::SamplerStateCount == 13);
  check(n::MaterialWordCount == 17 && n::TextureTransformWordCount == 16);
  for (size_t i = 0; i < n::RenderStateCount; ++i) check(n::RenderStates[i] == expectedRender[i]);
  for (size_t i = 0; i < n::StageStateCount; ++i) check(n::StageStates[i] == expectedStage[i]);
  for (size_t i = 0; i < n::SamplerStateCount; ++i) check(n::SamplerStates[i] == i + 1);
  check(size_t(n::RenderStateIndex::AlphaRef) == 2);
  check(size_t(n::RenderStateIndex::TextureFactor) == 16);
  check(size_t(n::StageStateIndex::TexCoordIndex) == 6);
  check(size_t(n::StageStateIndex::AlphaArg0) == 9);
  check(size_t(n::SamplerStateIndex::MipmapLodBias) == 7);
  check(size_t(n::SamplerStateIndex::DmapOffset) == 12);
  check(size_t(n::MaterialWordIndex::DiffuseA) == 3);
  check(size_t(n::MaterialWordIndex::AmbientR) == 4);
  check(size_t(n::MaterialWordIndex::SpecularR) == 8);
  check(size_t(n::MaterialWordIndex::EmissiveR) == 12);
  check(size_t(n::MaterialWordIndex::Power) == 16);

  Inputs input;
  // Known all-zero material (including zero diffuse alpha) is not unknown.
  auto facts = n::CaptureMaterialFacts(true, key, input.views());
  check(facts.available == 1 && n::MaterialFactsSchemaValid(facts));
  check(n::MaterialFactsFinite(facts) && n::MaterialFactsMatches(facts, key));
  check(n::MaterialFactsSameDrawIdentity(facts, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  check(facts.material[size_t(n::MaterialWordIndex::DiffuseA)] == 0);
  check(facts.material[0] != n::FloatBits(1.0f));

  for (size_t i = 0; i < n::RenderStateCount; ++i) input.renderStates[i] = 100u + uint32_t(i);
  for (size_t i = 0; i < n::StageStateCount; ++i) input.stage[i] = 200u + uint32_t(i);
  for (size_t i = 0; i < n::SamplerStateCount; ++i) input.sampler[i] = 300u + uint32_t(i);
  input.sampler[size_t(n::SamplerStateIndex::MipmapLodBias)] = n::FloatBits(-0.0f);
  for (size_t i = 0; i < n::MaterialWordCount; ++i) input.material[i] = n::FloatBits(float(i));
  for (size_t i = 0; i < n::TextureTransformWordCount; ++i) input.transform[i] = n::FloatBits(float(i));
  input.material[3] = n::FloatBits(-0.0f);
  input.transform[2] = n::FloatBits(-0.0f);
  facts = n::CaptureMaterialFacts(true, key, input.views());
  check(std::memcmp(facts.renderStates, input.renderStates, sizeof(input.renderStates)) == 0);
  check(std::memcmp(facts.textureStage0, input.stage, sizeof(input.stage)) == 0);
  check(std::memcmp(facts.sampler0, input.sampler, sizeof(input.sampler)) == 0);
  check(std::memcmp(facts.material, input.material, sizeof(input.material)) == 0);
  check(std::memcmp(facts.textureTransform0, input.transform, sizeof(input.transform)) == 0);
  check(facts.material[3] == 0x80000000u && facts.textureTransform0[2] == 0x80000000u);
  check(n::FloatBits(n::SetFloatBits(0x80000000u)) == 0x80000000u);
  check(n::FloatBits(n::SetFloatBits(0x00000001u)) == 1u);
  check(n::MaterialFactsMatches(facts, key));
  auto retainedKey = key; retainedKey.renderFrame = 17;
  const auto retained = n::CaptureMaterialFacts(true, retainedKey, input.views());
  check(!n::MaterialFactsMatches(retained, key));
  check(n::MaterialFactsSameDrawIdentity(retained, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  check(!n::MaterialFactsSameDrawIdentity(facts, key.mapEpoch + 1, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  check(!n::MaterialFactsSameDrawIdentity(facts, key.mapEpoch, key.deviceEpoch + 1,
    key.renderablePart, key.layerIndex));
  check(!n::MaterialFactsSameDrawIdentity(facts, key.mapEpoch, key.deviceEpoch,
    key.renderablePart + 1, key.layerIndex));
  check(!n::MaterialFactsSameDrawIdentity(facts, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex + 1));
  input.material[0] = n::FloatBits(99.0f);
  check(facts.material[0] == n::FloatBits(0.0f)); // Copy has no borrowed lifetime.

  for (unsigned field = 0; field < 6; ++field) {
    auto staleKey = key;
    switch (field) {
      case 0: ++staleKey.renderFrame; break;
      case 1: ++staleKey.mapEpoch; break;
      case 2: ++staleKey.deviceEpoch; break;
      case 3: ++staleKey.renderablePart; break;
      case 4: ++staleKey.layerIndex; break;
      case 5: staleKey.shaderMask = 0; break;
    }
    check(!n::MaterialFactsMatches(facts, staleKey));
  }
  for (unsigned field = 0; field < 5; ++field) {
    auto invalidKey = key;
    switch (field) {
      case 0: invalidKey.renderFrame = 0; break;
      case 1: invalidKey.mapEpoch = 0; break;
      case 2: invalidKey.deviceEpoch = 0; break;
      case 3: invalidKey.renderablePart = 0; break;
      case 4: invalidKey.shaderMask = 4; break;
    }
    // Invalid key must also be rejected before any view read.
    unknown(n::CaptureMaterialFacts(true, invalidKey, invalidViews));
    check(!n::MaterialFactsMatches(facts, invalidKey));
  }
  for (unsigned field = 0; field < 5; ++field) {
    auto missing = input.views();
    switch (field) {
      case 0: missing.renderStates = nullptr; break;
      case 1: missing.textureStage0 = nullptr; break;
      case 2: missing.sampler0 = nullptr; break;
      case 3: missing.material = nullptr; break;
      case 4: missing.textureTransform0 = nullptr; break;
    }
    unknown(n::CaptureMaterialFacts(true, key, missing));
  }

  // Verify each float word, including quiet/signalling NaNs and both infinities.
  const uint32_t nonfinite[] = {0x7f800000u, 0xff800000u, 0x7fc12345u, 0x7f800001u};
  for (auto badBits : nonfinite) {
    check(!n::FloatBitsFinite(badBits));
    for (size_t i = 0; i < n::MaterialWordCount; ++i) {
      auto bad = facts; bad.material[i] = badBits;
      check(!n::MaterialFactsFinite(bad) && !n::MaterialFactsMatches(bad, key));
      check(n::MaterialFactsSameDrawIdentity(bad, key.mapEpoch, key.deviceEpoch,
        key.renderablePart, key.layerIndex));
    }
    for (size_t i = 0; i < n::TextureTransformWordCount; ++i) {
      auto bad = facts; bad.textureTransform0[i] = badBits;
      check(!n::MaterialFactsFinite(bad) && !n::MaterialFactsMatches(bad, key));
      check(n::MaterialFactsSameDrawIdentity(bad, key.mapEpoch, key.deviceEpoch,
        key.renderablePart, key.layerIndex));
    }
    auto bad = facts;
    bad.sampler0[size_t(n::SamplerStateIndex::MipmapLodBias)] = badBits;
    check(!n::MaterialFactsFinite(bad) && !n::MaterialFactsMatches(bad, key));
    check(n::MaterialFactsSameDrawIdentity(bad, key.mapEpoch, key.deviceEpoch,
      key.renderablePart, key.layerIndex));
    input.material[0] = badBits;
    bad = n::CaptureMaterialFacts(true, key, input.views());
    check(bad.available == 1 && bad.material[0] == badBits);
    check(n::MaterialFactsSchemaValid(bad) && !n::MaterialFactsMatches(bad, key));
  }
  check(n::FloatBitsFinite(0x80000000u) && n::FloatBitsFinite(0x00000001u));
  check(n::FloatBitsFinite(0x7f7fffffu) && n::FloatBitsFinite(0xff7fffffu));
  auto badSchema = facts; badSchema.schemaVersion = 2;
  check(!n::MaterialFactsSchemaValid(badSchema) && !n::MaterialFactsMatches(badSchema, key));
  check(!n::MaterialFactsSameDrawIdentity(badSchema, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  badSchema = facts; badSchema.available = 2;
  check(!n::MaterialFactsSchemaValid(badSchema));
  badSchema = facts; badSchema.available = 0;
  check(!n::MaterialFactsMatches(badSchema, key));
  badSchema = facts; badSchema.shaderMask = 4;
  check(!n::MaterialFactsSchemaValid(badSchema));
  check(!n::MaterialFactsSameDrawIdentity(badSchema, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  badSchema = facts; badSchema.renderFrame = 0;
  check(!n::MaterialFactsSameDrawIdentity(badSchema, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  badSchema = facts; badSchema.mapEpoch = 0;
  check(!n::MaterialFactsSameDrawIdentity(badSchema, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  badSchema = facts; badSchema.deviceEpoch = 0;
  check(!n::MaterialFactsSameDrawIdentity(badSchema, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  badSchema = facts; badSchema.renderablePart = 0;
  check(!n::MaterialFactsSameDrawIdentity(badSchema, key.mapEpoch, key.deviceEpoch,
    key.renderablePart, key.layerIndex));
  // A rejected observation cannot poison the next valid same-layer capture.
  input.material[0] = n::FloatBits(0.0f);
  check(n::MaterialFactsMatches(n::CaptureMaterialFacts(true, key, input.views()), key));
  std::printf("PASS %u native material facts checks\n", checks);
}
