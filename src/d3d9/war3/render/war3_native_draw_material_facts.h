#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace dxvk::war3::render::native_input {

inline constexpr uint32_t MaterialFactsSchemaVersion = 1;
inline constexpr size_t RenderStateCount = 17;
inline constexpr size_t StageStateCount = 10;
inline constexpr size_t SamplerStateCount = 13;
inline constexpr size_t MaterialWordCount = 17;
inline constexpr size_t TextureTransformWordCount = 16;

// D3D9 state IDs, in wire order. No D3D9 declarations are needed by readers.
inline constexpr uint32_t RenderStates[RenderStateCount] = {
  15, 25, 24, 27, 19, 20, 171, 206, 207, 208, 209, 7, 14, 23, 22, 168, 60,
};
inline constexpr uint32_t StageStates[StageStateCount] = {
  1, 2, 3, 4, 5, 6, 11, 24, 26, 27,
};
inline constexpr uint32_t SamplerStates[SamplerStateCount] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
};

enum class RenderStateIndex : size_t {
  AlphaTestEnable, AlphaFunc, AlphaRef, AlphaBlendEnable, SrcBlend, DestBlend,
  BlendOp, SeparateAlphaBlendEnable, SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha,
  ZEnable, ZWriteEnable, ZFunc, CullMode, ColorWriteEnable, TextureFactor,
};
enum class StageStateIndex : size_t {
  ColorOp, ColorArg1, ColorArg2, AlphaOp, AlphaArg1, AlphaArg2, TexCoordIndex,
  TextureTransformFlags, ColorArg0, AlphaArg0,
};
enum class SamplerStateIndex : size_t {
  AddressU, AddressV, AddressW, BorderColor, MagFilter, MinFilter, MipFilter,
  MipmapLodBias, MaxMipLevel, MaxAnisotropy, SrgbTexture, ElementIndex, DmapOffset,
};
enum class MaterialWordIndex : size_t {
  DiffuseR, DiffuseG, DiffuseB, DiffuseA, AmbientR, AmbientG, AmbientB, AmbientA,
  SpecularR, SpecularG, SpecularB, SpecularA, EmissiveR, EmissiveG, EmissiveB,
  EmissiveA, Power,
};

// An observed draw key is not proof of native allocation ownership, object
// lifetime, texture identity, slot ownership, or GPU consumption permission.
struct MaterialFactsKey {
  uint64_t renderFrame = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint64_t renderablePart = 0;
  uint32_t layerIndex = 0;
  uint32_t shaderMask = 0; // bit 0: vertex shader present; bit 1: pixel shader present.
};

struct NativeDrawMaterialFacts {
  uint32_t schemaVersion = MaterialFactsSchemaVersion;
  uint32_t available = 0;
  uint64_t renderFrame = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint64_t renderablePart = 0;
  uint32_t layerIndex = 0;
  uint32_t shaderMask = 0;
  uint32_t renderStates[RenderStateCount] = {};
  uint32_t textureStage0[StageStateCount] = {};
  uint32_t sampler0[SamplerStateCount] = {};
  uint32_t material[MaterialWordCount] = {}; // Diffuse/Ambient/Specular/Emissive RGBA, Power.
  uint32_t textureTransform0[TextureTransformWordCount] = {}; // Raw data[0..3], each x/y/z/w f32 bits.
};

static_assert(std::is_trivially_copyable<NativeDrawMaterialFacts>::value,
  "Material observations must remain copyable without ownership work");
static_assert(std::is_standard_layout<NativeDrawMaterialFacts>::value,
  "Material observations require a fixed field layout");
static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559,
  "Material observations require IEEE binary32");

inline uint32_t FloatBits(float value) noexcept {
  uint32_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

inline float SetFloatBits(uint32_t bits) noexcept {
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

inline bool FloatBitsFinite(uint32_t bits) noexcept {
  return (bits & 0x7f800000u) != 0x7f800000u;
}

inline bool MaterialFactsKeyValid(const MaterialFactsKey& key) noexcept {
  // Zero is a valid material-layer index. No native layer range is inferred.
  return key.renderFrame && key.mapEpoch && key.deviceEpoch && key.renderablePart
    && (key.shaderMask & ~3u) == 0;
}

inline bool MaterialFactsSchemaValid(const NativeDrawMaterialFacts& facts) noexcept {
  return facts.schemaVersion == MaterialFactsSchemaVersion && facts.available == 1
    && facts.renderFrame && facts.mapEpoch && facts.deviceEpoch && facts.renderablePart
    && (facts.shaderMask & ~3u) == 0;
}

inline bool MaterialFactsFinite(const NativeDrawMaterialFacts& facts) noexcept {
  for (uint32_t bits : facts.material)
    if (!FloatBitsFinite(bits)) return false;
  for (uint32_t bits : facts.textureTransform0)
    if (!FloatBitsFinite(bits)) return false;
  return FloatBitsFinite(facts.sampler0[size_t(SamplerStateIndex::MipmapLodBias)]);
}

inline bool MaterialFactsSameDrawIdentity(const NativeDrawMaterialFacts& facts,
    uint64_t mapEpoch, uint64_t deviceEpoch, uint64_t part, uint32_t layer) noexcept {
  // renderFrame belongs to ShadowProducerSerial, not the enclosing CPU batch.
  // Identity filtering must retain nonfinite raw observations for strict readers.
  return MaterialFactsSchemaValid(facts) && facts.mapEpoch == mapEpoch
    && facts.deviceEpoch == deviceEpoch && facts.renderablePart == part
    && facts.layerIndex == layer;
}

inline bool MaterialFactsMatches(const NativeDrawMaterialFacts& facts,
    const MaterialFactsKey& key) noexcept {
  return MaterialFactsSchemaValid(facts) && MaterialFactsFinite(facts)
    && MaterialFactsKeyValid(key) && facts.renderFrame == key.renderFrame
    && facts.mapEpoch == key.mapEpoch && facts.deviceEpoch == key.deviceEpoch
    && facts.renderablePart == key.renderablePart && facts.layerIndex == key.layerIndex
    && facts.shaderMask == key.shaderMask;
}

// Borrowed views of producer-owned CPU arrays in the orders above, all with
// their fixed declared counts. The caller establishes thread/read validity;
// these helpers neither access native memory nor retain any pointer.
struct MaterialFactsViews {
  const uint32_t* renderStates = nullptr;
  const uint32_t* textureStage0 = nullptr;
  const uint32_t* sampler0 = nullptr;
  const uint32_t* material = nullptr;
  const uint32_t* textureTransform0 = nullptr;
};

inline NativeDrawMaterialFacts CaptureMaterialFacts(bool enabled,
    const MaterialFactsKey& key, const MaterialFactsViews& views) noexcept {
  NativeDrawMaterialFacts facts;
  // Keep the disabled branch before inspecting any view or copying any bytes.
  if (!enabled) return facts;
  if (!MaterialFactsKeyValid(key) || !views.renderStates || !views.textureStage0
      || !views.sampler0 || !views.material || !views.textureTransform0)
    return facts;

  facts.renderFrame = key.renderFrame;
  facts.mapEpoch = key.mapEpoch;
  facts.deviceEpoch = key.deviceEpoch;
  facts.renderablePart = key.renderablePart;
  facts.layerIndex = key.layerIndex;
  facts.shaderMask = key.shaderMask;
  std::memcpy(facts.renderStates, views.renderStates, sizeof(facts.renderStates));
  std::memcpy(facts.textureStage0, views.textureStage0, sizeof(facts.textureStage0));
  std::memcpy(facts.sampler0, views.sampler0, sizeof(facts.sampler0));
  std::memcpy(facts.material, views.material, sizeof(facts.material));
  std::memcpy(facts.textureTransform0, views.textureTransform0, sizeof(facts.textureTransform0));
  // Available means fully observed, not semantically valid or finite. Keeping
  // invalid raw float bits allows diagnostics to explain a rejected observation.
  facts.available = 1;
  return facts;
}

} // namespace dxvk::war3::render::native_input
