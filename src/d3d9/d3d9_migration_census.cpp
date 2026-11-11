#if defined(WARVK_D3D9_MIGRATION_CENSUS_DEV)
#include "d3d9_device.h"
#include "d3d9_migration_census_serialize.h"
#include "d3d9_caller_census.h"
#include "d3d9_buffer_census.h"
#include "../util/log/log.h"
#include <atomic>
#include <sstream>

namespace dxvk {
namespace {
std::atomic<uint64_t> g_migrationDeviceOrdinal{0};
}

void D3D9DeviceEx::MigrationNoteApi(d3d9::migration::Api api) {
  m_migrationCensus.noteCall(api);
}

void D3D9DeviceEx::MigrationNoteTexture(
    const d3d9::migration::TextureFacts& facts) {
  m_migrationCensus.noteTexture(facts);
}

void D3D9DeviceEx::MigrationNoteDraw(d3d9::migration::DrawKind kind,
    D3DPRIMITIVETYPE primitiveType, UINT primitiveCount,
    UINT explicitStride, bool hasExplicitStride,
    UINT vertexCount, bool vertexCountKnown,
    D3DFORMAT indexFormat, bool indexFormatKnown) {
  using namespace d3d9::migration;
  DrawFacts facts;
  facts.kind = kind;
  facts.primitiveType = primitiveType;
  facts.primitiveCount = primitiveCount;
  facts.knownMask = PrimitiveTypeKnown | PrimitiveCountKnown;
  // Absence of an explicit argument is unknown, not a request to inspect
  // a bound declaration/buffer/texture/shader or a mapped upload page.
  if (hasExplicitStride) {
    facts.streamStride = explicitStride;
    facts.knownMask |= StreamStrideKnown;
  }
  if (vertexCountKnown) {
    facts.vertexCount = vertexCount;
    facts.knownMask |= VertexCountKnown;
  }
  if (indexFormatKnown) {
    facts.indexFormat = indexFormat;
    facts.knownMask |= IndexFormatKnown;
  }
  m_migrationCensus.noteDraw(facts);
}

void D3D9DeviceEx::MigrationNoteVsKey(const D3D9FFShaderKeyVSData& key) {
  d3d9::migration::VsKeySamples::Key words;
  for (std::size_t i = 0; i < words.size(); ++i) words[i] = key.Primitive[i];
  m_migrationCensus.noteVsKey(words);
}

void D3D9DeviceEx::MigrationNoteFsKey(const D3D9FFShaderKeyFS& key) {
  d3d9::migration::FsKeySamples::Key words;
  for (std::size_t i = 0; i < caps::TextureStageCount; ++i) {
    words[2*i] = key.Stages[i].Primitive[0];
    words[2*i+1] = key.Stages[i].Primitive[1];
  }
  m_migrationCensus.noteFsKey(words);
}

void D3D9DeviceEx::MigrationPresent() {
  bool emit = false;
  m_migrationCensus.notePresent(emit);
  m_bufferCensus.notePresent();
  // At most seven cold Present snapshots plus normal device destruction.
  // Never log/file-write per draw; the logger is outside the recorder cell.
  if (emit) MigrationLog("present-milestone");
}

uint64_t D3D9DeviceEx::MigrationAllocateDeviceOrdinal() {
  // Cold construction, outside capture and before publication. Unlike hot
  // refusal counters this process-wide identity can use a 64-bit atomic.
  return g_migrationDeviceOrdinal.fetch_add(1, std::memory_order_relaxed) + 1;
}

void D3D9DeviceEx::MigrationLog(const char* reason) {
  CallerCensusLog(reason);
  d3d9::migration::CensusCopy copy;
  if (!m_migrationCensus.trySnapshot(copy)) return;
  const auto refusals = m_migrationCensus.observationRefusals();
  // Refusal observations are independent atomic reads AFTER the coherent
  // copy. They do not share its instant/sequence and are not a call total.
  // Formatting/allocation/Logger must remain outside the bounded try-cell.
  const d3d9::migration::CensusExportInfo info = {
    m_migrationDeviceOrdinal, sizeof(void*)*8, m_behaviorFlags, reason,
    m_d3d9Options.invariantPosition, m_d3d9Options.forceSampleRateShading,
    m_d3d9Options.ffUbershaderVS, m_d3d9Options.ffUbershaderFS
  };
  std::ostringstream out;
  d3d9::migration::WriteCensusSnapshot(out, copy, refusals, info);
  Logger::info(out.str());
}

d3d9::migration::CallerRanges D3D9DeviceEx::CallerCensusRanges() {
  // Cold, before the device is published: image ranges from the loaded PE
  // headers. Game.dll may be absent (a test host); then nothing is "game".
  auto range = [](HMODULE module) {
    d3d9::migration::ModuleRange result;
    if (!module) return result;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return result;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return result;
    result.begin = reinterpret_cast<uintptr_t>(base);
    result.end = result.begin + nt->OptionalHeader.SizeOfImage;
    return result;
  };
  d3d9::migration::CallerRanges ranges;
  ranges.game = range(GetModuleHandleW(L"Game.dll"));
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&g_migrationDeviceOrdinal), &self);
  ranges.self = range(self);
  return ranges;
}

void D3D9DeviceEx::CallerCensusLog(const char* reason) {
  // Copy under the caller cell only; formatting and Logger stay outside it.
  d3d9::migration::CallerCensusCopy copy;
  if (!m_callerCensus.trySnapshot(copy)) return;
  std::ostringstream out;
  d3d9::migration::WriteCallerCensus(out, copy, m_migrationDeviceOrdinal, reason, m_callerCensus.gameModuleKnown());
  Logger::info(out.str());
  d3d9::migration::BufferCensusCopy buffers;
  if (!m_bufferCensus.trySnapshot(buffers)) return;
  std::ostringstream line;
  d3d9::migration::WriteBufferCensus(line, buffers, m_migrationDeviceOrdinal, reason, m_bufferCensus.gameModuleKnown());
  Logger::info(line.str());
}

void D3D9DeviceEx::BufferCensusLock(D3D9CommonBuffer* pResource, UINT OffsetToLock,
    UINT SizeToLock, DWORD Flags, const void* Caller) {
  m_bufferCensus.noteLock(pResource->m_bufferCensusCursor, OffsetToLock, SizeToLock, Flags, Caller);
}

void D3D9DeviceEx::BufferCensusUnlock(D3D9CommonBuffer* pResource) {
  m_bufferCensus.noteUnlock(pResource->m_bufferCensusCursor);
}
}
#endif
