#pragma once

#include "../render/war3_native_draw_material_facts.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace dxvk::war3::tools::evidence {

// These are final prepared shadow-replay inputs, not native material semantics.
// Resource addresses are observation tokens, not ownership/lifetime proofs.
struct FinalShadowMaterialFacts {
  uint32_t alphaRefBits = 0;
  bool alphaTest = false;
  bool alphaBlend = false;
  bool depthTest = false;
  bool depthWrite = false;
  bool additive = false;
  uint32_t uvBinding = 0;
  uint32_t uvStride = 0;
  uint32_t uvOffset = 0;
  uint32_t uvFormat = 0;
  uint64_t textureView = 0;
  uint64_t sampler = 0;
  uint32_t samplerIndex = 0;
};

static_assert(std::is_trivially_copyable<FinalShadowMaterialFacts>::value
  && std::is_standard_layout<FinalShadowMaterialFacts>::value,
  "Final shadow facts must remain an owned fixed-size value");

inline nlohmann::json EncodeNativeRenderInputEvidence(
    const war3::render::native_input::NativeDrawMaterialFacts& native,
    const FinalShadowMaterialFacts& final) {
  using json = nlohmann::json;
  // Unknown never inherits enclosing draw identity or stale payload words.
  const war3::render::native_input::NativeDrawMaterialFacts unknown;
  const bool available = native.available == 1;
  const auto& observed = available ? native : unknown;
  json observation = {
    {"schema", observed.schemaVersion}, {"available", available},
    {"frameDomain", "ShadowProducerSerial"},
    {"sourceFrame", std::to_string(observed.renderFrame)},
    {"mapEpoch", std::to_string(observed.mapEpoch)},
    {"deviceEpoch", std::to_string(observed.deviceEpoch)},
    {"part", std::to_string(observed.renderablePart)},
    {"layer", observed.layerIndex}, {"shaderMask", observed.shaderMask},
    {"renderStates", observed.renderStates},
    {"textureStage0", observed.textureStage0},
    {"sampler0", observed.sampler0},
    {"materialBits", observed.material},
    {"textureTransform0Bits", observed.textureTransform0},
  };
  json finalMaterial = {
    {"alphaRefBits", final.alphaRefBits}, {"alphaTest", final.alphaTest},
    {"alphaBlend", final.alphaBlend}, {"depthTest", final.depthTest},
    {"depthWrite", final.depthWrite}, {"additive", final.additive},
    {"uvBinding", final.uvBinding}, {"uvStride", final.uvStride},
    {"uvOffset", final.uvOffset}, {"uvFormat", final.uvFormat},
    {"textureView", std::to_string(final.textureView)},
    {"sampler", std::to_string(final.sampler)}, {"samplerIndex", final.samplerIndex},
  };
  // Preserve raw bits and the observed producer frame, including malformed
  // available identities. Strict readers reject them instead of laundering
  // them with a batch/event frame or an enclosing draw's epochs.
  return {
    {"schema", 1}, {"captureStage", "PreparedShadowReplay"},
    {"finalShadowMaterial", std::move(finalMaterial)},
    {"nativeDrawMaterialObservation", std::move(observation)},
    {"requiredSharedFieldsMissing", json::array({
      "nativeInstanceLifecycle", "originalMdxGeosetMapping", "evaluatedGeoaAlphaRgb",
      "animationClock", "texturePixels", "allScenePassCoverage",
    })},
  };
}

} // namespace dxvk::war3::tools::evidence
