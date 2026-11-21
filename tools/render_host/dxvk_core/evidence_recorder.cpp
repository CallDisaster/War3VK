#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "evidence_recorder.h"
#include "shadow_host_services.h"

#include "../../../src/d3d9/war3/platform/war3_proxy_diag_block.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <windows.h>
#include <psapi.h>
#include <objbase.h>
#include <wincodec.h>

namespace warvk::host::dxvkcore {

using dxvk::Rc;

namespace {

// What the ring may hold: this much picture memory on the GPU, this many
// frames, and so many frames after the request.
// A player needs a few seconds to react to a one-frame glitch, at any frame rate.
constexpr uint64_t RingBytes = 1ull << 30;
constexpr uint32_t RingFramesMost = 720u, RingFramesLeast = 16u, PostFrames = 6u;

uint64_t Qpc() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return uint64_t(t.QuadPart); }

std::string Utf8(const std::wstring& text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(size > 0 ? size : 0), '\0');
  if (size > 0)
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

// A path as a JSON string.
std::string JsonText(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (static_cast<unsigned char>(c) < 0x20) { char code[8]; std::snprintf(code, sizeof(code), "\\u%04x", unsigned(c)); out += code; }
    else out += c;
  }
  return out + "\"";
}

std::wstring Stamp() {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t text[32];
  std::swprintf(text, 32, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  return text;
}

bool WriteAll(const std::wstring& path, const std::string& bytes) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
  CloseHandle(file);
  return ok;
}

// ---- pictures ----------------------------------------------------------------
// The Windows imaging component writes the JPEG files: it is part of every
// Windows this runs on. The identifiers are spelled out so that nothing but
// ole32 is linked for them.
const GUID kWicFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
const GUID kWicFactoryInterface = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
const GUID kJpeg = { 0x19e4a5aa, 0x5662, 0x4fc5, { 0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57 } };
const GUID kBgr24 = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c } };

template <typename T> void Drop(T*& object) { if (object) { object->Release(); object = nullptr; } }

bool SaveJpeg(IWICImagingFactory* factory, const std::wstring& path, const uint8_t* bgr, uint32_t width, uint32_t height) {
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* options = nullptr;
  bool ok = SUCCEEDED(factory->CreateStream(&stream))
      && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
      && SUCCEEDED(factory->CreateEncoder(kJpeg, nullptr, &encoder))
      && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))
      && SUCCEEDED(encoder->CreateNewFrame(&frame, &options));
  if (ok && options) {
    PROPBAG2 name = {};
    wchar_t quality[] = L"ImageQuality";
    name.pstrName = quality;
    VARIANT value;
    std::memset(&value, 0, sizeof(value));
    value.vt = VT_R4;
    value.fltVal = 0.92f;
    options->Write(1, &name, &value);
  }
  WICPixelFormatGUID format = kBgr24;
  ok = ok && SUCCEEDED(frame->Initialize(options)) && SUCCEEDED(frame->SetSize(width, height))
      && SUCCEEDED(frame->SetPixelFormat(&format)) && std::memcmp(&format, &kBgr24, sizeof(GUID)) == 0
      && SUCCEEDED(frame->WritePixels(height, width * 3u, width * 3u * height, const_cast<BYTE*>(bgr)))
      && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  Drop(options); Drop(frame); Drop(encoder); Drop(stream);
  return ok;
}

// Without the imaging component: the same pixels as an uncompressed BMP.
bool SaveBmp(const std::wstring& path, const uint8_t* bgr, uint32_t width, uint32_t height) {
  const uint32_t row = (width * 3u + 3u) & ~3u;
  std::string file(54u + size_t(row) * height, '\0');
  BITMAPFILEHEADER head = {};
  head.bfType = 0x4d42; head.bfSize = DWORD(file.size()); head.bfOffBits = 54;
  BITMAPINFOHEADER info = {};
  info.biSize = sizeof(info); info.biWidth = LONG(width); info.biHeight = LONG(height);
  info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = row * height;
  std::memcpy(file.data(), &head, 14);
  std::memcpy(file.data() + 14, &info, 40);
  for (uint32_t y = 0; y < height; ++y)
    std::memcpy(file.data() + 54 + size_t(row) * (height - 1u - y), bgr + size_t(width) * 3u * y, size_t(width) * 3u);
  return WriteAll(path, file);
}

void Append(std::string& out, const char* format, ...) {
  char text[1024];
  va_list arguments;
  va_start(arguments, format);
  const int length = std::vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  if (length > 0)
    out.append(text, size_t(std::min<int>(length, int(sizeof(text)) - 1)));
}

void AppendFloats(std::string& out, const float* values, uint32_t count) {
  out += '[';
  for (uint32_t i = 0; i < count; ++i)
    Append(out, i ? ",%.9g" : "%.9g", std::isfinite(values[i]) ? double(values[i]) : 0.0);
  out += ']';
}

} // namespace

// What the worker writes: the frames of one request, oldest first.
struct RecordedFrame : EvidenceFrame {
  shadowhost::CasterBoundsCounts bounds;
};

struct EvidenceExport {
  std::vector<RecordedFrame> frames;
  const uint8_t* pixels = nullptr;      // BGRA, frames.size() pictures of width x height
  uint32_t width = 0, height = 0, divisor = 1, sourceWidth = 0, sourceHeight = 0;
  uint64_t triggerFrame = 0, frequency = 1;
  uint32_t gamePid = 0, ordinal = 0;
  int32_t request = 0;
  std::wstring root;
  std::vector<diag::GameFrame> gameFrames;
  std::vector<std::string> waitNames;
  diag::Block* block = nullptr;
};

struct EvidenceRecorder::Package {
  // Indexed by the same slot as its picture. Only the owner writes the
  // ring. An export takes both facts by value before its worker starts.
  std::vector<shadowhost::CasterBoundsCounts> ringBounds;
  std::unique_ptr<EvidenceExport> writing;
  struct Row {
    std::string prefix;
    uint64_t id = 0, vertex = 0, index = 0, reused = 0;
  };
  std::array<Row, 5> rows;
  unsigned head = 0, count = 0;

  void finishRows(std::string& output, uint64_t through, const shadowhost::CasterBoundsCounts& result) {
    while (count && rows[head].id <= through) {
      const auto& row = rows[head];
      output += row.prefix;
      const bool ready = row.id && row.id == result.completedTimingId;
      Append(output, ",%.3f,%.3f,%llu,%llu,%llu\r\n", ready ? result.shadowMapGpuUs : 0.,
          ready ? result.shadowReceiverGpuUs : 0., (unsigned long long)row.vertex,
          (unsigned long long)row.index, (unsigned long long)row.reused);
      head = (head + 1) % rows.size();
      --count;
    }
  }
};

EvidenceRecorder::EvidenceRecorder(const Rc<dxvk::DxvkDevice>& device, uint32_t gamePid)
  : m_device(device), m_gamePid(gamePid), m_package(std::make_unique<Package>()) {
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  m_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
  char value[8] = {};
  const DWORD size = GetEnvironmentVariableA("WARVK_D3D9_EVIDENCE", value, sizeof(value));
  if (size == 1 && value[0] == '0') {
    m_noRing = true;
    m_noRingReason = "frame evidence is switched off (WARVK_D3D9_EVIDENCE=0)";
  }
}

EvidenceRecorder::~EvidenceRecorder() {
  // The device is going: the package ends with the pictures written so far.
  m_stopWorker.store(true, std::memory_order_release);
  if (m_worker.joinable())
    m_worker.join();
  flushLogs();
  if (m_block) {
    m_block->evidenceState = diag::EvidenceOff;
    m_block->hostPid = 0;
  }
  if (m_block)
    UnmapViewOfFile(m_block);
  if (m_mapping)
    CloseHandle(static_cast<HANDLE>(m_mapping));
}

bool EvidenceRecorder::wantsAudit() const noexcept {
  // Not while a performance log is written: its numbers are the game's.
  return m_block != nullptr && !m_off && !m_noRing && !m_block->perfWanted;
}

void EvidenceRecorder::flushLogs() noexcept {
  try { perfStop(false); } catch (...) { }
}

void EvidenceRecorder::attach() {
  wchar_t name[64];
  diag::BlockName(name, 64, m_gamePid);
  const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
  if (!mapping)
    return;
  auto* block = diag::MapCompatibleBlock(mapping, m_gamePid);
  if (!block) {
    CloseHandle(mapping);
    m_off = true; // another build's block: nothing to agree on
    return;
  }
  m_mapping = mapping;
  m_block = block;
  m_requestSeen = block->evidenceRequest;
  m_perfSession = 0; // a recording the player started before this host is honoured
  block->evidenceError[0] = '\0';
  block->evidenceState = m_noRing ? diag::EvidenceOff : diag::EvidenceRecording;
  block->perfState = diag::PerfIdle;
  block->hostPid = int32_t(GetCurrentProcessId());
  std::fprintf(stderr, "D3D9 evidence: attached to the game's diagnostics block pid=%u\n", m_gamePid);
  std::fflush(stderr);
}

void EvidenceRecorder::fail(const char* reason) noexcept {
  std::fprintf(stderr, "D3D9 evidence: %s\n", reason);
  std::fflush(stderr);
  if (m_block) {
    std::snprintf(m_block->evidenceError, sizeof(m_block->evidenceError), "%s", reason);
    m_block->evidenceRequestSeen = m_requestSeen;
    m_block->evidenceState = diag::EvidenceFailed;
  }
}

void EvidenceRecorder::dropRing() noexcept {
  if (m_triggered && !m_exportDue)
    fail("the picture changed size before the frames were kept");
  if (m_exportDue)
    fail("the picture changed size before the frames were read");
  m_triggered = m_exportDue = false;
  m_postLeft = 0;
  m_slots.clear();
  m_package->ringBounds.clear();
  m_ringBytes = 0;
  m_head = m_filled = 0;
  m_sourceView = nullptr;
  m_sourceImage = nullptr;
  m_sourceWidth = m_sourceHeight = 0;
  if (m_block && m_block->evidenceState == diag::EvidenceTriggered)
    m_block->evidenceState = diag::EvidenceRecording;
}

bool EvidenceRecorder::ensureRing(uint32_t width, uint32_t height) {
  if (!m_slots.empty() && width == m_sourceWidth && height == m_sourceHeight)
    return true;
  dropRing();
  if (!width || !height)
    return false;
  m_sourceWidth = width; m_sourceHeight = height;
  // About a thousand pixels across, whatever the window: seconds matter more
  // here than sharpness.
  m_divisor = std::max(1u, (width + 999u) / 1000u);
  m_width = std::max(1u, width / m_divisor);
  m_height = std::max(1u, height / m_divisor);
  // At most an eighth of the video memory, for the small adapters.
  uint64_t budget = RingBytes, largestHeap = 0;
  const auto& memory = m_device->adapter()->memoryProperties();
  for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap)
    if (memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      largestHeap = std::max<uint64_t>(largestHeap, memory.memoryHeaps[heap].size);
  if (largestHeap)
    budget = std::min<uint64_t>(budget, largestHeap / 8u);
  const uint64_t bytes = uint64_t(m_width) * m_height * 4u;
  const uint32_t count = uint32_t(std::clamp<uint64_t>(budget / bytes, RingFramesLeast, RingFramesMost));
  m_slots.resize(count);
  m_package->ringBounds.resize(count);
  return true;
}

void EvidenceRecorder::reapWorker() {
  if (!m_worker.joinable() || !m_workerDone.load(std::memory_order_acquire))
    return;
  m_worker.join();
  m_workerDone.store(false, std::memory_order_release);
  m_package->writing.reset();
  m_exportBuffer = nullptr;
  ++m_packages;
}

void EvidenceRecorder::frame(dxvk::DxvkContext& ctx, const Rc<dxvk::DxvkImage>& color, EvidenceFrame facts,
    const char* const* waitNames, uint32_t waitCount) {
  reapWorker();
  if (m_off)
    return;
  if (!m_block) {
    if (m_attachCountdown == 0u) {
      attach();
      m_attachCountdown = 30u;
    } else {
      --m_attachCountdown;
    }
    if (!m_block)
      return;
  }
  diag::Block& block = *m_block;
  auto bounds = shadowhost::PresentedBounds();
  const auto completed = bounds;
  bounds.shadowMapGpuUs = bounds.shadowReceiverGpuUs = 0;
  m_package->finishRows(m_perfPending, bounds.timingId > 4 ? bounds.timingId - 4 : 0, completed);
  if (completed.completedTimingId) {
    for (unsigned age = 1; age <= std::min(m_filled, 4u); ++age) {
      const auto index = (m_head + m_slots.size() - age) % m_slots.size();
      auto& prior = m_package->ringBounds[index];
      if (prior.timingId == completed.completedTimingId) {
        prior.shadowMapGpuUs = completed.shadowMapGpuUs;
        prior.shadowReceiverGpuUs = completed.shadowReceiverGpuUs;
        break;
      }
    }
  }
  if (m_waitNames.empty())
    m_waitNames.assign(waitNames, waitNames + waitCount);
  facts.qpc = Qpc();
  facts.intervalUs = m_lastQpc ? uint32_t(std::min<uint64_t>((facts.qpc - m_lastQpc) * 1000000u / uint64_t(m_frequency), UINT32_MAX)) : 0u;
  m_lastQpc = facts.qpc;
  diag::GameFrame game = {};
  {
    const int32_t head = block.gameFrameHead;
    if (head > 0) {
      game = block.frames[uint32_t(head - 1) % diag::GameFrames];
      facts.gameFrame = game.frame; facts.gameIntervalUs = game.intervalUs; facts.gameFlags = game.flags;
      facts.gameConsidered = game.castersConsidered; facts.gameSent = game.castersSent; facts.gameOmitted = game.castersOmitted;
    }
  }

  publishStats(facts);

  // ---- the performance log ----
  const bool wanted = block.perfWanted != 0;
  if (wanted && !m_perfFile && block.perfSession != m_perfSession) {
    m_perfSession = block.perfSession;
    m_perfPath = std::wstring(block.outputDirectory) + L"\\perf-" + Stamp() + L".csv";
    const HANDLE file = CreateFileW(m_perfPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      block.perfSessionSeen = m_perfSession;
      block.perfState = diag::PerfFailed;
    } else {
      m_perfFile = file;
      m_perfRows = 0; m_perfBeginQpc = facts.qpc; m_perfIntervals.clear(); m_perfPending.clear();
      m_perfPending = "hostFrame,timeMs,intervalMs,draws,drawBatches,worldFrame,shadowRan,castersReceived,castersRefused,"
          "casterBytes,replayDraws,drawnCasters,mapExecuted,receiverExecuted,completeMap,reuseMap,replayRejected,"
          "cascade0,cascade1,cascade2,cascade3,auditChecked,auditOdd,deviceUsedMiB,deviceAllocatedMiB,"
          "gameFrame,gameIntervalMs,gameCastersConsidered,gameCastersSent,gameCastersOmitted";
      for (uint32_t i = 0; i < waitCount; ++i) { m_perfPending += ",waitUs"; m_perfPending += waitNames[i]; }
      m_perfPending += ",gameCasterDemandCasters,gameCasterDemandBytes,gameCasterAdmittedBytes"
          ",castersExactBounds,castersUnknownBounds,csmRemoved0,csmRemoved1,csmRemoved2,csmRemoved3"
          ",csmCompleteMask,csmHeldMask,csmUnavailableMask,castersOmittedExact,castersOmittedUnknown,hostCasterAdmittedBytes"
          ",shadowMapGpuUs,shadowReceiverGpuUs,casterVertexBytes,casterIndexBytes,casterIndexReusedBytes\r\n";
      block.perfSessionSeen = m_perfSession;
      block.perfRows = 0;
      block.perfState = diag::PerfRecording;
    }
  }
  if (m_perfFile) {
    if (wanted) {
      auto& row = m_package->rows[(m_package->head + m_package->count++) % m_package->rows.size()];
      row.id = bounds.timingId;
      row.vertex = bounds.casterVertexBytes;
      row.index = bounds.casterIndexBytes;
      row.reused = bounds.casterIndexReusedBytes;
      row.prefix.clear();
      row.prefix.swap(m_perfPending);
      perfRow(facts, waitNames, waitCount);
      Append(m_perfPending, ",%u,%llu,%u", game.demandCasters,
          static_cast<unsigned long long>(game.demandBytes), game.admittedBytes);
      Append(m_perfPending, ",%u,%u,%u,%u,%u,%u", bounds.exact, bounds.unknown,
          bounds.removed[0], bounds.removed[1], bounds.removed[2], bounds.removed[3]);
      Append(m_perfPending, ",%u,%u,%u,%u,%u,%llu", bounds.complete, bounds.held, bounds.unavailable,
          bounds.omittedExact, bounds.omittedUnknown, static_cast<unsigned long long>(bounds.admittedBytes));
      row.prefix.swap(m_perfPending);
      if (m_perfPending.size() >= 65536u) {
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(m_perfFile), m_perfPending.data(), DWORD(m_perfPending.size()), &written, nullptr))
          perfStop(true);
        m_perfPending.clear();
      }
    } else {
      perfStop(false);
    }
  }

  // ---- frame evidence ----
  const int32_t request = block.evidenceRequest;
  if (request != m_requestSeen && !m_triggered && !m_worker.joinable()) {
    m_requestSeen = request;
    block.evidenceRequestSeen = request;
    if (m_noRing) {
      fail(m_noRingReason);
    } else if (!m_filled) {
      fail("no frame of a map has been drawn yet");
    } else {
      m_triggered = true;
      m_postLeft = PostFrames;
      m_triggerFrame = facts.hostFrame;
      block.evidenceError[0] = '\0';
      block.evidenceSaved = 0; block.evidenceTotal = 0;
      block.evidenceState = diag::EvidenceTriggered;
    }
  }
  if (facts.beforeUi && color != nullptr && !m_exportDue && !m_noRing) {
    try {
      const auto& info = color->info();
      if (ensureRing(info.extent.width, info.extent.height)) {
        if (m_sourceImage != color) {
          dxvk::DxvkImageViewKey key;
          key.viewType = VK_IMAGE_VIEW_TYPE_2D;
          key.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
          key.format = info.format;
          key.layout = VK_IMAGE_LAYOUT_GENERAL;
          key.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
          key.mipCount = 1; key.layerCount = 1;
          m_sourceView = color->createView(key);
          m_sourceImage = color;
        }
        Slot& slot = m_slots[m_head];
        if (slot.image == nullptr) {
          dxvk::DxvkImageCreateInfo image;
          image.type = VK_IMAGE_TYPE_2D;
          image.format = info.format;
          image.sampleCount = VK_SAMPLE_COUNT_1_BIT;
          image.extent = { m_width, m_height, 1 };
          image.numLayers = 1; image.mipLevels = 1;
          image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
          image.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
          image.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
          image.layout = VK_IMAGE_LAYOUT_GENERAL;
          image.debugName = "WarVK frame evidence";
          slot.image = m_device->createImage(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
          ctx.initImage(slot.image, VK_IMAGE_LAYOUT_UNDEFINED);
          m_ringBytes += uint64_t(m_width) * m_height * 4u;
          dxvk::DxvkImageViewKey key;
          key.viewType = VK_IMAGE_VIEW_TYPE_2D;
          key.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
          key.format = info.format;
          key.layout = VK_IMAGE_LAYOUT_GENERAL;
          key.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
          key.mipCount = 1; key.layerCount = 1;
          slot.view = slot.image->createView(key);
        }
        const VkOffset3D source[2] = { { 0, 0, 0 }, { int32_t(m_sourceWidth), int32_t(m_sourceHeight), 1 } };
        const VkOffset3D target[2] = { { 0, 0, 0 }, { int32_t(m_width), int32_t(m_height), 1 } };
        ctx.blitImageView(slot.view, target, m_sourceView, source, m_divisor == 1u ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        slot.facts = facts;
        m_package->ringBounds[m_head] = bounds;
        slot.used = true;
        m_head = (m_head + 1u) % uint32_t(m_slots.size());
        m_filled = std::min<uint32_t>(m_filled + 1u, uint32_t(m_slots.size()));
      }
    } catch (...) {
      // No memory for the ring, most likely: the evidence ends, the frame does not.
      m_slots.clear();
      m_ringBytes = 0;
      m_head = m_filled = 0;
      m_sourceView = nullptr; m_sourceImage = nullptr;
      m_triggered = false;
      m_noRing = true;
      m_noRingReason = "the frame ring could not be created (video memory)";
      fail(m_noRingReason);
    }
  }
  // The frames after the request, whether or not they had a world scene.
  if (m_triggered && !m_exportDue && m_postLeft && --m_postLeft == 0u)
    m_exportDue = true;

  block.evidenceFrames = int32_t(m_filled);
  if (m_filled) {
    const uint32_t count = uint32_t(m_slots.size());
    const Slot& oldest = m_slots[(m_head + count - m_filled) % count];
    block.evidenceMillis = int32_t(std::min<uint64_t>((facts.qpc - oldest.facts.qpc) * 1000u / uint64_t(m_frequency), 600000u));
  } else {
    block.evidenceMillis = 0;
  }
  block.evidencePackages = int32_t(m_packages);
}

bool EvidenceRecorder::exportDue() noexcept {
  return m_exportDue && m_block && !m_off && !m_worker.joinable() && m_filled != 0u;
}

void EvidenceRecorder::recordExport(dxvk::DxvkContext& ctx) {
  m_exportBuffer = nullptr;
  m_package->writing.reset();
  try {
    const uint32_t count = uint32_t(m_slots.size());
    const uint64_t pictureBytes = uint64_t(m_width) * m_height * 4u;
    dxvk::DxvkBufferCreateInfo info;
    info.size = pictureBytes * m_filled;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    info.debugName = "WarVK frame evidence readback";
    Rc<dxvk::DxvkBuffer> buffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (buffer == nullptr || buffer->mapPtr(0) == nullptr)
      throw std::runtime_error("no mapping");
    auto package = std::make_unique<EvidenceExport>();
    package->frames.reserve(m_filled);
    const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    for (uint32_t i = 0; i < m_filled; ++i) {
      const uint32_t index = (m_head + count - m_filled + i) % count;
      const Slot& slot = m_slots[index];
      ctx.copyImageToBuffer(buffer, pictureBytes * i, 0, 0, slot.image->info().format, slot.image, layers,
          { 0, 0, 0 }, { m_width, m_height, 1 });
      package->frames.push_back({slot.facts, m_package->ringBounds[index]});
    }
    package->pixels = static_cast<const uint8_t*>(buffer->mapPtr(0));
    package->width = m_width; package->height = m_height; package->divisor = m_divisor;
    package->sourceWidth = m_sourceWidth; package->sourceHeight = m_sourceHeight;
    package->triggerFrame = m_triggerFrame; package->frequency = uint64_t(m_frequency);
    package->gamePid = m_gamePid; package->ordinal = m_packages + 1u; package->request = m_requestSeen;
    package->root = m_block->outputDirectory;
    package->gameFrames.assign(m_block->frames, m_block->frames + diag::GameFrames);
    package->waitNames = m_waitNames;
    package->block = m_block;
    m_exportBuffer = std::move(buffer);
    m_package->writing = std::move(package);
  } catch (...) {
    m_exportBuffer = nullptr;
    m_package->writing.reset();
  }
}

void EvidenceRecorder::exportReady() {
  m_exportDue = false;
  m_triggered = false;
  m_postLeft = 0;
  if (!m_package->writing || m_exportBuffer == nullptr) {
    fail("the frames could not be read back (memory)");
    if (m_block)
      m_block->evidenceState = diag::EvidenceFailed;
    return;
  }
  m_block->evidenceTotal = int32_t(m_package->writing->frames.size());
  m_block->evidenceSaved = 0;
  m_block->evidenceState = diag::EvidenceSaving;
  m_workerDone.store(false, std::memory_order_release);
  EvidenceExport* package = m_package->writing.get();
  try {
    m_worker = std::thread([this, package] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      diag::Block& block = *package->block;
      try {
      const std::wstring directory = package->root + L"\\evidence-" + Stamp() + L"-" + std::to_wstring(package->ordinal);
      bool ok = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
      const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
      IWICImagingFactory* factory = nullptr;
      if (com)
        CoCreateInstance(kWicFactory, nullptr, CLSCTX_INPROC_SERVER, kWicFactoryInterface, reinterpret_cast<void**>(&factory));
      const size_t pixels = size_t(package->width) * package->height;
      std::vector<uint8_t> bgr(pixels * 3u);
      uint32_t saved = 0;
      std::string json;
      SYSTEMTIME now{};
      GetLocalTime(&now);
      Append(json, "{\n\"schema\":\"warvk-proxy-frame-evidence/v1\",\n\"created\":\"%04u-%02u-%02u %02u:%02u:%02u\",\n",
          now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
      Append(json, "\"hostPid\":%lu,\"gamePid\":%u,\"request\":%d,\"package\":%u,\n", GetCurrentProcessId(),
          package->gamePid, package->request, package->ordinal);
      Append(json, "\"gameWidth\":%u,\"gameHeight\":%u,\"pictureWidth\":%u,\"pictureHeight\":%u,\"divisor\":%u,\n",
          package->sourceWidth, package->sourceHeight, package->width, package->height, package->divisor);
      Append(json, "\"triggerHostFrame\":%llu,\"qpcFrequency\":%llu,\"pictureFormat\":\"%s\",\n",
          static_cast<unsigned long long>(package->triggerFrame), static_cast<unsigned long long>(package->frequency),
          factory ? "jpg" : "bmp");
      json += "\"note\":\"frames are oldest first; msBeforeTrigger counts back from the frame the request arrived in; "
          "shadow.audit counts casters whose geometry looked wrong as it was copied, and is empty "
          "(checked 0) while a performance log is being written\",\n\"frames\":[\n";
      uint64_t triggerQpc = 0;
      for (const EvidenceFrame& frame : package->frames)
        if (frame.hostFrame <= package->triggerFrame)
          triggerQpc = frame.qpc;
      for (size_t i = 0; ok && i < package->frames.size() && !m_stopWorker.load(std::memory_order_acquire); ++i) {
        const RecordedFrame& f = package->frames[i];
        if (i)
          json += ",\n";
        const uint8_t* source = package->pixels + pixels * 4u * i;
        for (size_t p = 0; p < pixels; ++p) {
          bgr[p * 3u] = source[p * 4u]; bgr[p * 3u + 1u] = source[p * 4u + 1u]; bgr[p * 3u + 2u] = source[p * 4u + 2u];
        }
        wchar_t name[32];
        std::swprintf(name, 32, L"f%03u.%ls", unsigned(i), factory ? L"jpg" : L"bmp");
        const std::wstring path = directory + L"\\" + name;
        const bool written = factory ? SaveJpeg(factory, path, bgr.data(), package->width, package->height)
            : SaveBmp(path, bgr.data(), package->width, package->height);
        ok = ok && written;
        if (written)
          block.evidenceSaved = int32_t(++saved);
        const ShadowOutcome& s = f.shadow;
        const double msBefore = triggerQpc >= f.qpc ? double(triggerQpc - f.qpc) * 1000.0 / double(package->frequency)
            : -double(f.qpc - triggerQpc) * 1000.0 / double(package->frequency);
        Append(json, "{\"file\":\"%ls\",\"hostFrame\":%llu,\"qpc\":%llu,\"msBeforeTrigger\":%.2f,\"intervalUs\":%u,"
            "\"draws\":%u,\"drawBatches\":%u,\"worldFrame\":%d,\n",
            name, static_cast<unsigned long long>(f.hostFrame), static_cast<unsigned long long>(f.qpc), msBefore,
            f.intervalUs, f.draws, f.drawBatches, int(f.beforeUi));
        Append(json, " \"game\":{\"frame\":%u,\"intervalUs\":%u,\"flags\":%u,\"castersConsidered\":%u,\"castersSent\":%u,"
            "\"castersOmitted\":%u},\n", f.gameFrame, f.gameIntervalUs, f.gameFlags, f.gameConsidered, f.gameSent, f.gameOmitted);
        Append(json, " \"records\":{\"camera\":%d,\"lighting\":%d,\"shadowFrame\":%d,\"timeOfDay\":%.5f,\"casters\":%u,"
            "\"omitted\":%u,\"omissionReasons\":%u,\"sunDirection\":", int(f.hasCamera), int(f.hasLighting),
            int(f.hasShadowFrame), double(std::isfinite(f.timeOfDay) ? f.timeOfDay : -1.0f), f.recordCasters, f.recordOmitted,
            f.recordOmissionReasons);
        AppendFloats(json, f.sunDirection.data(), 4);
        json += ",\"view\":"; AppendFloats(json, f.view.data(), 16);
        json += ",\"projection\":"; AppendFloats(json, f.projection.data(), 16);
        json += "},\n";
        Append(json, " \"shadow\":{\"ran\":%d,\"received\":%u,\"refused\":%u,\"refusals\":[%u,%u,%u,%u],\"copied\":%u,"
            "\"copiedBytes\":%llu,\"casters\":%u,\"replayDraws\":%u,\"drawnCasters\":%u,\"cascades\":%u,"
            "\"cascadeDrawn\":[%u,%u,%u,%u],\n", int(s.ran), s.received, s.refused, s.refusals[0], s.refusals[1], s.refusals[2],
            s.refusals[3], s.copied, static_cast<unsigned long long>(s.copiedBytes), s.casters, s.replayDraws, s.drawnCasters,
            s.cascades, s.cascadeDrawn[0], s.cascadeDrawn[1], s.cascadeDrawn[2], s.cascadeDrawn[3]);
        Append(json, "  \"mapExecuted\":%u,\"receiverExecuted\":%u,\"completeMap\":%u,\"earlyReturn\":%u,\"inputReject\":%u,"
            "\"replayRejected\":%u,\"reuseMap\":%u,\"holdInvalidCsm\":%u,\"holdEmptyReplay\":%u,\"holdIdentityChurn\":%u,"
            "\"csmFallback\":%u,\"noCompleteMap\":%u,\"replayLastReason\":%u,\"partialPrevented\":%u,\"skippedNoResources\":%u,\n",
            s.mapExecuted, s.receiverExecuted, s.completeMap, s.earlyReturn, s.inputReject, s.replayRejected, s.reuseMap,
            s.holdInvalidCsm, s.holdEmptyReplay, s.holdIdentityChurn, s.csmFallback, s.noCompleteMap, s.replayLastReason,
            s.partialPrevented, s.skippedNoResources);
        Append(json, "  \"mapRenderSerial\":%llu,\"cameraHash\":\"%016llx\",\"sunHash\":\"%016llx\",\"csmHash\":\"%016llx\","
            "\"cameraDelta\":%llu,\"sunDelta\":%llu,\"csmDelta\":%llu,\"snappedCenterDelta\":%llu,\"texelSizeDelta\":%llu,"
            "\"taaMode\":%u,\"taaHistory\":%u,\"taaInvalidation\":%u,\"pointShadow\":%u,\"gameTime\":%.5f,\"resourceBytes\":%llu,\n",
            static_cast<unsigned long long>(s.mapRenderSerial), static_cast<unsigned long long>(s.cameraHash),
            static_cast<unsigned long long>(s.sunHash), static_cast<unsigned long long>(s.csmHash),
            static_cast<unsigned long long>(s.cameraDelta), static_cast<unsigned long long>(s.sunDelta),
            static_cast<unsigned long long>(s.csmDelta), static_cast<unsigned long long>(s.snappedCenterDelta),
            static_cast<unsigned long long>(s.texelSizeDelta), s.taaMode, s.taaHistory, s.taaInvalidation, s.pointShadow,
            double(std::isfinite(s.gameTime) ? s.gameTime : -1.0f), static_cast<unsigned long long>(s.resourceBytes));
        Append(json, "  \"audit\":{\"checked\":%u,\"indexOutsideRange\":%u,\"outsideBuffer\":%u,\"notNumber\":%u,"
            "\"farPosition\":%u,\"longEdge\":%u,\"longestEdge\":%.1f,\"largestExtent\":%.1f,\"odd\":",
            s.audit.checked, s.audit.indexOutsideRange, s.audit.outsideBuffer, s.audit.notNumber, s.audit.farPosition,
            s.audit.longEdge, double(s.audit.longestEdge), double(s.audit.largestExtent));
        json += JsonText(std::string(s.audit.odd.data(), strnlen(s.audit.odd.data(), s.audit.odd.size())));
        Append(json, "},\"castersExactBounds\":%u,\"castersUnknownBounds\":%u,\"csmRemoved\":[%u,%u,%u,%u],",
            f.bounds.exact, f.bounds.unknown, f.bounds.removed[0], f.bounds.removed[1], f.bounds.removed[2], f.bounds.removed[3]);
        Append(json, "\"csmCompleteMask\":%u,\"csmHeldMask\":%u,\"csmUnavailableMask\":%u,"
            "\"castersOmittedExact\":%u,\"castersOmittedUnknown\":%u,\"hostCasterAdmittedBytes\":%llu,"
            "\"shadowMapGpuUs\":%.3f,\"shadowReceiverGpuUs\":%.3f,"
            "\"casterVertexBytes\":%llu,\"casterIndexBytes\":%llu,\"casterIndexReusedBytes\":%llu},\n \"waitUs\":[",
            f.bounds.complete, f.bounds.held, f.bounds.unavailable, f.bounds.omittedExact, f.bounds.omittedUnknown,
            static_cast<unsigned long long>(f.bounds.admittedBytes), f.bounds.shadowMapGpuUs, f.bounds.shadowReceiverGpuUs,
            (unsigned long long)f.bounds.casterVertexBytes, (unsigned long long)f.bounds.casterIndexBytes,
            (unsigned long long)f.bounds.casterIndexReusedBytes);
        for (size_t k = 0; k < f.waitUs.size(); ++k)
          Append(json, k ? ",%u" : "%u", f.waitUs[k]);
        Append(json, "],\"deviceUsedBytes\":%llu,\"deviceAllocatedBytes\":%llu}",
            static_cast<unsigned long long>(f.deviceUsedBytes), static_cast<unsigned long long>(f.deviceAllocatedBytes));
      }
      json += "\n],\n\"waitKinds\":[";
      for (size_t k = 0; k < package->waitNames.size(); ++k)
        json += (k ? "," : "") + JsonText(package->waitNames[k]);
      json += "],\n\"gameFrames\":[\n";
      bool first = true;
      for (const diag::GameFrame& g : package->gameFrames) {
        if (!g.frame)
          continue;
        Append(json, "%s{\"frame\":%u,\"qpc\":%llu,\"intervalUs\":%u,\"flags\":%u,\"cameraTier\":%u,\"considered\":%u,\"sent\":%u,"
            "\"omitted\":%u,\"terrain\":%u,\"doodads\":%u,\"units\":%u,\"worldObjects\":%u,\"casterBytes\":%u,\"timeOfDay\":%.5f,"
            "\"demandCasters\":%u,\"demandBytes\":%llu,\"admittedBytes\":%u}",
            first ? "" : ",\n", g.frame, static_cast<unsigned long long>(g.qpc), g.intervalUs, g.flags, g.cameraTier,
            g.castersConsidered, g.castersSent, g.castersOmitted, g.casterTerrain, g.casterDoodads, g.casterUnits,
            g.casterWorldObjects, g.casterBytes, double(std::isfinite(g.timeOfDay) ? g.timeOfDay : -1.0f),
            g.demandCasters, static_cast<unsigned long long>(g.demandBytes), g.admittedBytes);
        first = false;
      }
      json += "\n]\n}\n";
      ok = WriteAll(directory + L"\\evidence.json", json) && ok;
      Drop(factory);
      if (com)
        CoUninitialize();
      std::wcsncpy(block.evidencePath, directory.c_str(), diag::PathChars - 1u);
      block.evidencePath[diag::PathChars - 1u] = L'\0';
      if (!ok)
        std::snprintf(block.evidenceError, sizeof(block.evidenceError), "%s",
            "the package could not be written completely (disk space or folder)");
      block.evidenceRequestSeen = package->request;
      block.evidenceState = ok ? diag::EvidenceSaved : diag::EvidenceFailed;
      std::fprintf(stderr, "D3D9 evidence: package %u frames=%zu saved=%u ok=%d\n", package->ordinal,
          package->frames.size(), saved, int(ok));
      std::fflush(stderr);
      } catch (...) {
        std::snprintf(block.evidenceError, sizeof(block.evidenceError), "%s", "the package writer ran out of memory");
        block.evidenceRequestSeen = package->request;
        block.evidenceState = diag::EvidenceFailed;
      }
      m_workerDone.store(true, std::memory_order_release);
    });
  } catch (...) {
    m_package->writing.reset();
    m_exportBuffer = nullptr;
    fail("the package writer could not be started");
  }
}

void EvidenceRecorder::publishStats(const EvidenceFrame& f) {
  diag::HostStats& out = m_block->stats;
  ++m_statFrames;
  m_statWorldFrames += f.beforeUi ? 1u : 0u;
  if (f.intervalUs)
    m_statAverageUs = m_statAverageUs ? (m_statAverageUs * 31u + f.intervalUs) / 32u : f.intervalUs;
  out.sequence = ++m_statSequence; // odd: being written
  MemoryBarrier();
  out.width = f.width; out.height = f.height;
  out.draws = f.draws; out.drawBatches = f.drawBatches;
  out.intervalUs = f.intervalUs; out.averageIntervalUs = uint32_t(m_statAverageUs);
  out.worldThisFrame = f.beforeUi ? 1u : 0u;
  out.frames = m_statFrames; out.worldFrames = m_statWorldFrames;
  out.evidenceRingBytes = m_ringBytes;
  if (f.beforeUi) {
    const ShadowOutcome& s = f.shadow;
    out.shadowRan = s.ran ? 1u : 0u; out.casters = s.received; out.refused = s.refused; out.copied = s.copied;
    out.replayDraws = s.replayDraws; out.drawnCasters = s.drawnCasters; out.cascades = s.cascades;
    for (size_t i = 0; i < 4; ++i)
      out.cascadeDrawn[i] = s.cascadeDrawn[i];
    out.mapExecuted = s.mapExecuted; out.receiverExecuted = s.receiverExecuted;
    out.completeMap = s.completeMap; out.reuseMap = s.reuseMap;
    out.earlyReturn = s.earlyReturn; out.inputReject = s.inputReject; out.replayRejected = s.replayRejected;
    out.replayLastReason = s.replayLastReason; out.partialPrevented = s.partialPrevented;
    out.holdInvalidCsm = s.holdInvalidCsm; out.holdEmptyReplay = s.holdEmptyReplay;
    out.holdIdentityChurn = s.holdIdentityChurn; out.csmFallback = s.csmFallback;
    out.noCompleteMap = s.noCompleteMap; out.skippedNoResources = s.skippedNoResources;
    out.taaMode = s.taaMode; out.taaHistory = s.taaHistory; out.pointShadow = s.pointShadow; out.outline = s.outline;
    out.volumetric = s.volumetric; out.volumetricComposite = s.volumetricComposite; out.alphaPrepared = s.alphaPrepared;
    out.effectsRecorded = f.effectsRecorded;
    out.recordCasters = f.recordCasters; out.recordOmitted = f.recordOmitted;
    out.auditChecked = s.audit.checked;
    out.auditOdd = s.audit.indexOutsideRange + s.audit.outsideBuffer + s.audit.notNumber + s.audit.farPosition + s.audit.longEdge;
    out.copiedBytes = s.copiedBytes; out.mapRenderSerial = s.mapRenderSerial;
    out.shadowBytes = s.resourceBytes; out.volumetricBytes = s.volumetricBytes; out.effectBytes = f.effectBytes;
  }
  if (m_statCountdown == 0u) {
    m_statCountdown = 20u;
    PROCESS_MEMORY_COUNTERS_EX memory = {};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
      out.processPrivate = memory.PrivateUsage;
      out.processPeakPrivate = memory.PeakPagefileUsage;
      out.processWorkingSet = memory.WorkingSetSize;
    }
    const auto heaps = m_device->adapter()->getMemoryHeapInfo();
    uint64_t local[5] = {}, shared[3] = {};
    for (uint32_t heap = 0; heap < heaps.heapCount; ++heap) {
      const auto& info = heaps.heaps[heap];
      const auto stats = m_device->getMemoryStats(heap);
      if (info.heapFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        local[0] += info.heapSize; local[1] += info.memoryBudget; local[2] += info.memoryCommitted;
        local[3] += stats.memoryAllocated; local[4] += stats.memoryUsed;
      } else {
        shared[0] += info.memoryCommitted; shared[1] += stats.memoryAllocated; shared[2] += stats.memoryUsed;
      }
    }
    out.localHeap = local[0]; out.localBudget = local[1]; out.localCommitted = local[2];
    out.localAllocated = local[3]; out.localUsed = local[4];
    out.sharedCommitted = shared[0]; out.sharedAllocated = shared[1]; out.sharedUsed = shared[2];
  } else {
    --m_statCountdown;
  }
  MemoryBarrier();
  out.sequence = ++m_statSequence;
}

void EvidenceRecorder::perfRow(const EvidenceFrame& f, const char* const* waitNames, uint32_t waitCount) {
  const ShadowOutcome& s = f.shadow;
  const double timeMs = double(f.qpc - m_perfBeginQpc) * 1000.0 / double(m_frequency);
  Append(m_perfPending, "%llu,%.3f,%.3f,%u,%u,%d,%d,%u,%u,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.1f,%.1f,%u,%.3f,%u,%u,%u",
      static_cast<unsigned long long>(f.hostFrame), timeMs, double(f.intervalUs) / 1000.0, f.draws, f.drawBatches,
      int(f.beforeUi), int(s.ran), s.received, s.refused, static_cast<unsigned long long>(s.copiedBytes), s.replayDraws,
      s.drawnCasters, s.mapExecuted, s.receiverExecuted, s.completeMap, s.reuseMap, s.replayRejected, s.cascadeDrawn[0],
      s.cascadeDrawn[1], s.cascadeDrawn[2], s.cascadeDrawn[3], s.audit.checked,
      s.audit.indexOutsideRange + s.audit.outsideBuffer + s.audit.notNumber + s.audit.farPosition + s.audit.longEdge,
      double(f.deviceUsedBytes) / 1048576.0, double(f.deviceAllocatedBytes) / 1048576.0, f.gameFrame,
      double(f.gameIntervalUs) / 1000.0, f.gameConsidered, f.gameSent, f.gameOmitted);
  for (uint32_t i = 0; i < waitCount && i < f.waitUs.size(); ++i)
    Append(m_perfPending, ",%u", f.waitUs[i]);
  (void)waitNames;
  ++m_perfRows;
  if (f.intervalUs && m_perfIntervals.size() < 4000000u)
    m_perfIntervals.push_back(f.intervalUs);
  if (m_block)
    m_block->perfRows = int32_t(m_perfRows);
}

void EvidenceRecorder::perfStop(bool failed) {
  if (!m_perfFile)
    return;
  m_package->finishRows(m_perfPending, UINT64_MAX, {});
  DWORD written = 0;
  if (!m_perfPending.empty() && !WriteFile(static_cast<HANDLE>(m_perfFile), m_perfPending.data(), DWORD(m_perfPending.size()), &written, nullptr))
    failed = true;
  m_perfPending.clear();
  CloseHandle(static_cast<HANDLE>(m_perfFile));
  m_perfFile = nullptr;
  // The summary a person reads first.
  std::string summary;
  std::vector<uint32_t>& t = m_perfIntervals;
  if (!t.empty()) {
    uint64_t total = 0;
    for (const uint32_t us : t) total += us;
    std::sort(t.begin(), t.end());
    const auto at = [&](double q) { return double(t[std::min(t.size() - 1u, size_t(q * double(t.size())))]) / 1000.0; };
    const double seconds = double(total) / 1000000.0;
    size_t over33 = 0, over50 = 0, over100 = 0;
    for (const uint32_t us : t) { over33 += us > 33333u; over50 += us > 50000u; over100 += us > 100000u; }
    Append(summary, "WarVK proxy mode performance log\r\nframes %zu  seconds %.1f  average fps %.1f\r\n", t.size(), seconds,
        seconds > 0.0 ? double(t.size()) / seconds : 0.0);
    Append(summary, "frame time ms: median %.2f  p95 %.2f  p99 %.2f  p99.9 %.2f  longest %.2f\r\n", at(0.5), at(0.95), at(0.99),
        at(0.999), double(t.back()) / 1000.0);
    Append(summary, "1%% low fps %.1f  0.1%% low fps %.1f\r\n", 1000.0 / std::max(at(0.99), 0.001), 1000.0 / std::max(at(0.999), 0.001));
    Append(summary, "frames longer than 33 ms: %zu  50 ms: %zu  100 ms: %zu\r\n", over33, over50, over100);
    Append(summary, "rows are in the .csv beside this file: one per presented frame\r\n");
    std::wstring path = m_perfPath;
    if (path.size() > 4u)
      path.replace(path.size() - 4u, 4u, L"-summary.txt");
    WriteAll(path, summary);
  }
  t.clear();
  if (m_block) {
    std::wcsncpy(m_block->perfPath, m_perfPath.c_str(), diag::PathChars - 1u);
    m_block->perfPath[diag::PathChars - 1u] = L'\0';
    m_block->perfState = failed ? diag::PerfFailed : diag::PerfWritten;
  }
}

} // namespace warvk::host::dxvkcore
