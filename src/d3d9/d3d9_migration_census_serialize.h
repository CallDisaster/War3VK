#pragma once

#include "d3d9_migration_census.h"
#include <ostream>

namespace dxvk::d3d9::migration {

// Cold serialization only. All census data is an owned coherent copy; refusal
// values are independently sampled atomics. No device or game pointers enter.
// reason is a caller-owned literal, never an address recovered from a draw.
struct CensusExportInfo {
  uint64_t deviceOrdinal = 0;
  uint32_t processBits = 0;
  uint32_t behaviorFlags = 0;
  const char* reason = "present-milestone";
  bool invariantPosition = false;
  bool forceSampleRateShading = false;
  bool ffUbershaderVS = false;
  bool ffUbershaderFS = false;
};

template<typename Values>
void WriteCensusArray(std::ostream& out, const Values& values) {
  out << '[';
  bool first = true;
  for (const auto& value : values) {
    if (!first) out << ',';
    first = false;
    out << value;
  }
  out << ']';
}

template<typename Samples>
void WriteCensusKeys(std::ostream& out, const Samples& samples) {
  out << "{\"storedUnique\":" << samples.storedUnique
      << ",\"observed\":" << samples.observed
      << ",\"duplicates\":" << samples.duplicates
      << ",\"droppedObservations\":" << samples.dropped
      << ",\"missing\":" << samples.missing << ",\"keys\":[";
  for (uint32_t i = 0; i < samples.storedUnique; ++i) {
    if (i) out << ',';
    WriteCensusArray(out, samples.keys[i]);
  }
  out << "]}";
}

inline void WriteCensusSnapshot(std::ostream& out, const CensusCopy& copy,
    const ObservationRefusals& refusals, const CensusExportInfo& info) {
  const auto& s = copy.values;
  const uint64_t presents = s.apiCalls[std::size_t(Api::Present)];
  out << "WARVK_D3D9_MIGRATION_CENSUS {\"schema\":\"warvk-d3d9-migration-census/v2\""
      << ",\"deviceOrdinal\":" << info.deviceOrdinal
      << ",\"snapshotSequence\":" << copy.sequence
      << ",\"reason\":\"" << info.reason << "\",\"presentAttempts\":" << presents
      << ",\"processBits\":" << info.processBits
      << ",\"behaviorFlags\":" << info.behaviorFlags
      << ",\"diagnosticForcedDeviceLock\":false,\"sourceAttribution\":\"Unknown\""
      << ",\"observationPolicy\":\"argument-and-local-key-only\""
      << ",\"synchronizationPolicy\":\"bounded-try-cell\""
      << ",\"observationRefusals\":{\"recordBusy\":" << refusals.recordBusy
      << ",\"snapshotBusy\":" << refusals.snapshotBusy
      << ",\"counterSaturated\":" << refusals.counterSaturated << '}'
      << ",\"includesInternalCalls\":true,\"gpuCompletionProven\":false"
      << ",\"shaderPairMeaning\":\"bound-handles-not-effective-shader\""
      << ",\"fullSceneCapture\":false,\"portableKeyAbi\":false"
      << ",\"textureStageMaskScope\":\"pixel-fixed-function-0-through-7\""
      << ",\"invariantPosition\":" << (info.invariantPosition ? "true" : "false")
      << ",\"forceSampleRateShading\":" << (info.forceSampleRateShading ? "true" : "false")
      << ",\"ffUbershaderVS\":" << (info.ffUbershaderVS ? "true" : "false")
      << ",\"ffUbershaderFS\":" << (info.ffUbershaderFS ? "true" : "false")
      << ",\"observedCalls\":" << s.observedCalls << ",\"apiCalls\":{";
  for (std::size_t i = 0; i < s.apiCalls.size(); ++i) {
    if (i) out << ',';
    out << '"' << d3d9::migration::ApiNames[i] << "\":" << s.apiCalls[i];
  }
  out << "},\"unclassifiedCalls\":" << s.unclassifiedCalls
      << ",\"drawAttempts\":" << s.drawAttempts << ",\"drawKinds\":";
  WriteCensusArray(out, s.drawKinds);
  out << ",\"primitiveTypes\":"; WriteCensusArray(out, s.primitiveTypes);
  out << ",\"shaderPairs\":"; WriteCensusArray(out, s.shaderPairs);
  out << ",\"drawMissingFields\":"; WriteCensusArray(out, s.drawMissingFields);
  out << ",\"unknownSourceDrawAttempts\":" << s.unknownSourceDrawAttempts
      << ",\"primitiveTypeMissing\":" << s.primitiveTypeMissing
      << ",\"primitiveTypeInvalid\":" << s.primitiveTypeInvalid
      << ",\"shaderPairMissing\":" << s.shaderPairMissing
      << ",\"shaderPairInvalid\":" << s.shaderPairInvalid
      << ",\"invalidDrawKinds\":" << s.invalidDrawKinds
      << ",\"invalidSourceAttributions\":" << s.invalidSourceAttributions
      << ",\"drawUnknownMaskBits\":" << s.drawUnknownMaskBits
      << ",\"maxPrimitiveCount\":" << s.maxPrimitiveCount
      << ",\"maxStreamStride\":" << s.maxStreamStride
      << ",\"maxVertexCount\":" << s.maxVertexCount
      << ",\"fvfBitUnion\":" << s.fvfBitUnion
      << ",\"textureStageMaskUnion\":" << s.textureStageMaskUnion
      << ",\"drawSamplesDropped\":" << s.drawSamplesDropped << ",\"drawSamples\":[";
  for (uint32_t i = 0; i < s.drawSampleCount; ++i) {
    if (i) out << ',';
    const auto& f = s.drawSamples[i];
    out << "{\"kind\":" << unsigned(f.kind) << ",\"knownMask\":" << f.knownMask
        << ",\"primitiveType\":" << f.primitiveType << ",\"primitiveCount\":" << f.primitiveCount
        << ",\"shaderMask\":" << f.shaderMask << ",\"fvf\":" << f.fvf
        << ",\"streamStride\":" << f.streamStride << ",\"textureStageMask\":" << f.textureStageMask
        << ",\"vertexCount\":" << f.vertexCount << ",\"indexFormat\":" << f.indexFormat << '}';
  }
  out << "],\"observedTextureRequests\":" << s.observedTextureRequests
      << ",\"textureKinds\":"; WriteCensusArray(out, s.textureKinds);
  out << ",\"textureMissingFields\":"; WriteCensusArray(out, s.textureMissingFields);
  out << ",\"invalidTextureKinds\":" << s.invalidTextureKinds
      << ",\"textureUnknownMaskBits\":" << s.textureUnknownMaskBits
      << ",\"maxTextureWidth\":" << s.maxTextureWidth
      << ",\"maxTextureHeight\":" << s.maxTextureHeight
      << ",\"maxTextureDepth\":" << s.maxTextureDepth
      << ",\"maxRequestedMipLevels\":" << s.maxRequestedMipLevels
      << ",\"fullMipChainRequests\":" << s.fullMipChainRequests
      << ",\"textureSamplesDropped\":" << s.textureSamplesDropped << ",\"textureSamples\":[";
  for (uint32_t i = 0; i < s.textureSampleCount; ++i) {
    if (i) out << ',';
    const auto& f = s.textureSamples[i];
    out << "{\"kind\":" << unsigned(f.kind) << ",\"knownMask\":" << f.knownMask
        << ",\"width\":" << f.width << ",\"height\":" << f.height << ",\"depth\":" << f.depth
        << ",\"levels\":" << f.levels << ",\"format\":" << f.format
        << ",\"usage\":" << f.usage << ",\"pool\":" << f.pool << '}';
  }
  out << "],\"vsKeys\":"; WriteCensusKeys(out, s.vsKeys);
  out << ",\"fsKeys\":"; WriteCensusKeys(out, s.fsKeys);
  out << ",\"saturatedUpdates\":" << s.saturatedUpdates << '}';
}

}
