#include "../src/d3d9/war3/tools/war3_native_render_input_export.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace n = dxvk::war3::render::native_input;
namespace e = dxvk::war3::tools::evidence;
using json = nlohmann::json;
static unsigned checks = 0;
static void check(bool value) { ++checks; assert(value); }

struct Inputs {
  uint32_t render[n::RenderStateCount] = {};
  uint32_t stage[n::StageStateCount] = {};
  uint32_t sampler[n::SamplerStateCount] = {};
  uint32_t material[n::MaterialWordCount] = {};
  uint32_t transform[n::TextureTransformWordCount] = {};
  Inputs() {
    for (size_t i = 0; i < n::RenderStateCount; ++i) render[i] = 100u + uint32_t(i);
    for (size_t i = 0; i < n::StageStateCount; ++i) stage[i] = 200u + uint32_t(i);
    for (size_t i = 0; i < n::SamplerStateCount; ++i) sampler[i] = 300u + uint32_t(i);
    sampler[size_t(n::SamplerStateIndex::MipmapLodBias)] = n::FloatBits(-0.0f);
    for (size_t i = 0; i < n::MaterialWordCount; ++i) material[i] = n::FloatBits(float(i));
    for (size_t i = 0; i < n::TextureTransformWordCount; ++i) transform[i] = n::FloatBits(float(i));
    material[0] = n::FloatBits(-0.0f);
    material[size_t(n::MaterialWordIndex::DiffuseA)] = 0;
    transform[2] = n::FloatBits(-0.0f);
  }
  n::MaterialFactsViews views() const { return {render, stage, sampler, material, transform}; }
};

static e::FinalShadowMaterialFacts finalFacts() {
  e::FinalShadowMaterialFacts final;
  final.alphaRefBits = n::FloatBits(-0.0f);
  final.alphaTest = true; final.alphaBlend = true; final.depthTest = true;
  final.depthWrite = false; final.additive = true;
  final.uvBinding = 2; final.uvStride = 20; final.uvOffset = 4; final.uvFormat = 103;
  final.textureView = UINT64_MAX; final.sampler = 9007199254740993ull;
  final.samplerIndex = 7;
  return final;
}

static json caseJson(const char* name, const n::NativeDrawMaterialFacts& facts) {
  // Batch/event frames intentionally differ from the producer frame domain.
  return {{"name", name}, {"frame", "17"}, {"eventFrame", "19"},
    {"part", "1192960"}, {"layer", 0}, {"mapEpoch", "7"}, {"deviceEpoch", "9"},
    {"renderInputEvidence", e::EncodeNativeRenderInputEvidence(facts, finalFacts())}};
}

static json fixtureCases() {
  Inputs input;
  const n::MaterialFactsKey key{31, 7, 9, 0x123400, 0, 3};
  const auto normal = n::CaptureMaterialFacts(true, key, input.views());
  json cases = json::array();
  cases.push_back(caseJson("normal", normal));
  const auto invalid = reinterpret_cast<const uint32_t*>(uintptr_t(1));
  cases.push_back(caseJson("unknown", n::CaptureMaterialFacts(false, key,
    {invalid, invalid, invalid, invalid, invalid})));
  input.material[0] = 0x7fc12345u;
  cases.push_back(caseJson("nan", n::CaptureMaterialFacts(true, key, input.views())));
  input.material[0] = n::FloatBits(-0.0f);
  auto retainedKey = key; retainedKey.renderFrame = 987654321;
  cases.push_back(caseJson("retained", n::CaptureMaterialFacts(true, retainedKey, input.views())));
  for (auto& word : input.material) word = 0;
  cases.push_back(caseJson("zero_alpha", n::CaptureMaterialFacts(true, key, input.views())));
  auto dirtyUnknown = normal; dirtyUnknown.available = 0;
  cases.push_back(caseJson("dirty_unknown", dirtyUnknown));
  auto malformed = normal; malformed.renderFrame = 0;
  cases.push_back(caseJson("zero_source_frame", malformed));
  malformed = normal; malformed.mapEpoch = 0;
  cases.push_back(caseJson("zero_map_epoch", malformed));
  malformed = normal; malformed.deviceEpoch = 0;
  cases.push_back(caseJson("zero_device_epoch", malformed));
  return cases;
}

int main(int argc, char** argv) {
  const auto cases = fixtureCases();
  if (argc == 2 && std::strcmp(argv[1], "--fixture") == 0) {
    std::cout << cases.dump() << '\n';
    return 0;
  }
  if (argc != 1) return 2;
  check(cases.size() == 9);
  const auto& normal = cases[0]["renderInputEvidence"];
  check(normal.size() == 5 && normal["schema"] == 1);
  check(normal["captureStage"] == "PreparedShadowReplay");
  check(normal["requiredSharedFieldsMissing"] == json::array({
    "nativeInstanceLifecycle", "originalMdxGeosetMapping", "evaluatedGeoaAlphaRgb",
    "animationClock", "texturePixels", "allScenePassCoverage"}));
  const auto& native = normal["nativeDrawMaterialObservation"];
  check(native.size() == 14 && native["schema"] == 1);
  check(native["available"].is_boolean() && native["available"] == true);
  check(native["frameDomain"] == "ShadowProducerSerial" && native["sourceFrame"] == "31");
  check(native["mapEpoch"] == "7" && native["deviceEpoch"] == "9" && native["part"] == "1192960");
  check(native["layer"] == 0 && native["shaderMask"] == 3);
  Inputs input;
  const char* arrays[] = {"renderStates", "textureStage0", "sampler0", "materialBits", "textureTransform0Bits"};
  const size_t counts[] = {17, 10, 13, 17, 16};
  const uint32_t* words[] = {input.render, input.stage, input.sampler, input.material, input.transform};
  for (size_t array = 0; array < 5; ++array) {
    check(native[arrays[array]].is_array() && native[arrays[array]].size() == counts[array]);
    for (size_t i = 0; i < counts[array]; ++i)
      check(native[arrays[array]][i].is_number_unsigned() && native[arrays[array]][i] == words[array][i]);
  }
  const auto& final = normal["finalShadowMaterial"];
  check(final.size() == 13 && final["alphaRefBits"] == 0x80000000u);
  check(final["textureView"] == "18446744073709551615" && final["sampler"] == "9007199254740993");
  for (const char* flag : {"alphaTest", "alphaBlend", "depthTest", "depthWrite", "additive"})
    check(final[flag].is_boolean());
  check(final["uvBinding"] == 2 && final["uvStride"] == 20 && final["uvOffset"] == 4);
  check(final["uvFormat"] == 103 && final["samplerIndex"] == 7);
  for (size_t caseIndex : {size_t(1), size_t(5)}) {
    const auto& unknown = cases[caseIndex]["renderInputEvidence"]["nativeDrawMaterialObservation"];
    check(unknown["available"] == false);
    for (const char* identity : {"sourceFrame", "mapEpoch", "deviceEpoch", "part"}) check(unknown[identity] == "0");
    check(unknown["layer"] == 0 && unknown["shaderMask"] == 0);
    for (const char* array : arrays) for (const auto& word : unknown[array]) check(word == 0);
    check(cases[caseIndex]["renderInputEvidence"]["finalShadowMaterial"] == final);
  }
  check(cases[2]["renderInputEvidence"]["nativeDrawMaterialObservation"]["materialBits"][0] == 0x7fc12345u);
  check(cases[3]["renderInputEvidence"]["nativeDrawMaterialObservation"]["sourceFrame"] == "987654321");
  check(cases[4]["renderInputEvidence"]["nativeDrawMaterialObservation"]["materialBits"] == json::array({0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}));
  const char* malformed[] = {"sourceFrame", "mapEpoch", "deviceEpoch"};
  for (size_t i = 0; i < 3; ++i) {
    const auto& available = cases[6 + i]["renderInputEvidence"]["nativeDrawMaterialObservation"];
    check(available["available"] == true && available[malformed[i]] == "0");
  }
  std::printf("PASS %u native render input export checks\n", checks);
}
