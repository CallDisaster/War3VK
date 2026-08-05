#include "war3_cpu_skin_mt.h"

#include "war3_gpu_skin_compute.h"

#include "../../../util/thread.h"
#include "../../../util/util_env.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if defined(__SSE__) || defined(_M_IX86) || defined(_M_X64)
#include <xmmintrin.h>
#define DXVK_WAR3_CPU_SKIN_HAS_SSE 1
#else
#define DXVK_WAR3_CPU_SKIN_HAS_SSE 0
#endif

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace dxvk::war3::gpu_skin {
namespace {

constexpr uint32_t kMaxNativeVertexCount = 16384u;
constexpr uint32_t kMaxPaletteMatrixCount = 256u;
constexpr uint32_t kOutputAlignment = 16u;
constexpr uint32_t kMxcsrControlMask = 0xffc0u;
constexpr uint32_t kMxcsrExceptionMask = 0x1f80u;

uint32_t CurrentFloatingPointControl() noexcept {
#if DXVK_WAR3_CPU_SKIN_HAS_SSE
  return _mm_getcsr() & kMxcsrControlMask;
#else
  return 0u;
#endif
}

bool HasSafeFloatingPointControl(uint32_t control) noexcept {
#if DXVK_WAR3_CPU_SKIN_HAS_SSE
  return (control & ~kMxcsrControlMask) == 0u &&
      (control & kMxcsrExceptionMask) == kMxcsrExceptionMask;
#else
  // The proven Game.dll path is SSE. A build without MXCSR control has no
  // independent parity authority and must keep the original native kernel.
  (void)control;
  return false;
#endif
}

class ScopedFloatingPointControl {
public:
  explicit ScopedFloatingPointControl(uint32_t control) noexcept {
#if DXVK_WAR3_CPU_SKIN_HAS_SSE
    m_previous = _mm_getcsr();
    // Preserve this thread's sticky exception status. Only the render-thread
    // control contract is installed for the pure SSE calculation.
    _mm_setcsr((m_previous & ~kMxcsrControlMask) |
               (control & kMxcsrControlMask));
#else
    (void)control;
#endif
  }

  ~ScopedFloatingPointControl() {
#if DXVK_WAR3_CPU_SKIN_HAS_SSE
    _mm_setcsr(m_previous);
#endif
  }

  ScopedFloatingPointControl(const ScopedFloatingPointControl&) = delete;
  ScopedFloatingPointControl& operator=(
      const ScopedFloatingPointControl&) = delete;

private:
#if DXVK_WAR3_CPU_SKIN_HAS_SSE
  uint32_t m_previous = 0u;
#endif
};

template<typename T>
bool CheckedAdd(T lhs, T rhs, T& result) noexcept {
  if (rhs > std::numeric_limits<T>::max() - lhs)
    return false;
  result = lhs + rhs;
  return true;
}

template<typename T>
bool CheckedMul(T lhs, T rhs, T& result) noexcept {
  if (lhs != 0 && rhs > std::numeric_limits<T>::max() / lhs)
    return false;
  result = lhs * rhs;
  return true;
}

bool CheckedAlign16(uint32_t value, uint32_t& result) noexcept {
  if (value > std::numeric_limits<uint32_t>::max() -
                  (kOutputAlignment - 1u))
    return false;
  result = (value + (kOutputAlignment - 1u)) &
      ~(kOutputAlignment - 1u);
  return true;
}

bool StreamCovers(const CpuSkinMtStreamView& stream,
                  uint32_t vertexCount,
                  uint32_t elementByteSize,
                  bool optional = false) noexcept {
  if (stream.data == nullptr)
    return optional && stream.byteSize == 0u && stream.stride == 0u;
  if (stream.stride < elementByteSize || vertexCount == 0u)
    return false;

  size_t lastOffset = 0u;
  if (!CheckedMul<size_t>(size_t(vertexCount - 1u), stream.stride,
                          lastOffset))
    return false;
  size_t required = 0u;
  return CheckedAdd(lastOffset, size_t(elementByteSize), required) &&
      required <= stream.byteSize;
}

void CopyStrided(uint8_t* destination,
                 const CpuSkinMtStreamView& source,
                 uint32_t vertexCount,
                 uint32_t elementByteSize) noexcept {
  const auto* input = static_cast<const uint8_t*>(source.data);
  for (uint32_t i = 0u; i < vertexCount; ++i) {
    std::memcpy(destination + size_t(i) * elementByteSize,
                input + size_t(i) * source.stride,
                elementByteSize);
  }
}

uint64_t NextGeneration(std::atomic<uint64_t>& generation) noexcept {
  uint64_t current = generation.load(std::memory_order_relaxed);
  for (;;) {
    uint64_t next = current + 1u;
    if (next == 0u)
      next = 1u;
    if (generation.compare_exchange_weak(
            current, next, std::memory_order_acq_rel,
            std::memory_order_relaxed))
      return next;
  }
}

template<typename T>
void AtomicMax(std::atomic<T>& destination, T value) noexcept {
  T current = destination.load(std::memory_order_relaxed);
  while (current < value &&
         !destination.compare_exchange_weak(
             current, value, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

struct AtomicCpuSkinMtDiagnostics {
  std::atomic<uint64_t> submittedBatches = { 0u };
  std::atomic<uint64_t> rejectedBatches = { 0u };
  std::atomic<uint64_t> submittedJobs = { 0u };
  std::atomic<uint64_t> readyJobs = { 0u };
  std::atomic<uint64_t> cpuFallbackJobs = { 0u };
  std::atomic<uint64_t> cancelledJobs = { 0u };
  std::atomic<uint64_t> failedJobs = { 0u };
  std::atomic<uint64_t> workerTasks = { 0u };
  std::atomic<uint64_t> ownerAssistTasks = { 0u };
  std::atomic<uint64_t> synchronousCalls = { 0u };
  std::atomic<uint64_t> synchronousCompleted = { 0u };
  std::atomic<uint64_t> queuedTaskHighWater = { 0u };
  std::atomic<uint64_t> paletteBytesPinned = { 0u };
  std::atomic<uint64_t> staticBytesPinned = { 0u };
  std::atomic<uint64_t> outputBytesOwned = { 0u };
  std::atomic<uint64_t> verticesCompleted = { 0u };
  std::atomic<uint64_t> queueBackpressure = { 0u };
  std::atomic<uint64_t> memoryBackpressure = { 0u };
  std::atomic<uint64_t> staleLeaseRejects = { 0u };
  std::atomic<uint64_t> floatingPointEnvironmentRejects = { 0u };
  std::atomic<uint64_t> synchronousReadyBytes = { 0u };
  std::atomic<uint64_t> resets = { 0u };
  std::atomic<uint64_t> joinedWorkers = { 0u };
};

struct CpuSkinMtControlState {
  explicit CpuSkinMtControlState(const CpuSkinMtConfig& input)
      : config(input) {
  }

  bool reserveOwnedBytes(size_t bytes) noexcept {
    size_t currentBytes = ownedBytes.load(std::memory_order_relaxed);
    for (;;) {
      if (bytes > config.maxOwnedBytes ||
          currentBytes > config.maxOwnedBytes - bytes)
        return false;
      if (ownedBytes.compare_exchange_weak(
              currentBytes, currentBytes + bytes,
              std::memory_order_acq_rel,
              std::memory_order_relaxed))
        return true;
    }
  }

  void releaseOwnedBytes(size_t bytes) noexcept {
    ownedBytes.fetch_sub(bytes, std::memory_order_release);
  }

  bool reserveBatch(size_t bytes) noexcept {
    uint32_t batchCount = pendingBatches.load(std::memory_order_relaxed);
    for (;;) {
      if (batchCount >= config.maxPendingBatches)
        return false;
      if (pendingBatches.compare_exchange_weak(
              batchCount, batchCount + 1u,
              std::memory_order_acq_rel,
              std::memory_order_relaxed))
        break;
    }

    if (reserveOwnedBytes(bytes))
      return true;
    pendingBatches.fetch_sub(1u, std::memory_order_release);
    return false;
  }

  void releaseBatch(size_t bytes) noexcept {
    releaseOwnedBytes(bytes);
    pendingBatches.fetch_sub(1u, std::memory_order_release);
  }

  CpuSkinMtConfig config;
  std::atomic<uint64_t> generation = { 1u };
  std::atomic<bool> stopping = { false };
  std::atomic<size_t> ownedBytes = { 0u };
  std::atomic<uint32_t> pendingBatches = { 0u };
  AtomicCpuSkinMtDiagnostics diagnostics;
};

struct CpuSkinMtBatchJobRecord {
  CpuSkinMtJobDesc desc;
  uint32_t outputOffset = 0u;
  uint32_t outputByteSize = 0u;
  uint32_t outputStride = 0u;
  uint32_t outputFormat = 0u;
};

struct CpuSkinMtBatchTaskRange {
  uint32_t firstJob = 0u;
  uint32_t jobCount = 0u;
};

}  // namespace

class CpuSkinMtStaticSnapshot::Impl {
public:
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  uint64_t contentHash = 0u;
  uintptr_t geosetData = 0u;
  uint32_t layoutGeneration = 0u;
  uint32_t vertexCount = 0u;
  uint32_t matrixGroupCount = 0u;
  uint32_t uvLayerCount = 0u;
  uint32_t maxGroupSlot = 0u;
  GpuSkinStaticSourceLayout layout;
  std::vector<uint8_t> packedSource;
  std::vector<uint32_t> diffuse;
};

class CpuSkinMtFrozenKernel::Impl {
public:
  std::shared_ptr<const CpuSkinMtStaticSnapshot> staticSnapshot;
  std::vector<uint8_t> palette;
  uint32_t outputFormat = 0u;
  uint32_t outputStride = 0u;
  uint32_t outputByteSize = 0u;
  uint32_t floatingPointControl = 0u;
};

class CpuSkinMtBatchStateData {
public:
  ~CpuSkinMtBatchStateData() {
    if (accounted && control != nullptr)
      control->releaseBatch(accountedBytes);
  }

  std::shared_ptr<CpuSkinMtControlState> control;
  CpuSkinMtBatchDesc desc;
  uint64_t generation = 0u;
  std::vector<CpuSkinMtBatchJobRecord> jobs;
  std::vector<CpuSkinMtBatchTaskRange> tasks;
  std::unique_ptr<std::atomic<uint8_t>[]> jobStates;
  std::unique_ptr<std::atomic<bool>[]> jobCancel;
  std::unique_ptr<uint8_t[]> output;
  uint32_t outputByteSize = 0u;
  std::atomic<uint8_t> batchState = {
      uint8_t(CpuSkinMtBatchState::Invalid) };
  std::atomic<uint32_t> remainingTasks = { 0u };
  std::atomic<bool> cancelled = { false };
  size_t accountedBytes = 0u;
  bool accounted = false;
};

class CpuSkinMtSynchronousOutputState {
public:
  ~CpuSkinMtSynchronousOutputState() {
    if (accounted && control != nullptr)
      control->releaseOwnedBytes(accountedBytes);
  }

  std::shared_ptr<CpuSkinMtControlState> control;
  std::shared_ptr<const CpuSkinMtFrozenKernel> kernel;
  uint64_t generation = 0u;
  uint32_t ownerThread = 0u;
  std::unique_ptr<uint8_t[]> staging;
  uint32_t outputByteSize = 0u;
  size_t accountedBytes = 0u;
  bool accounted = false;
  std::atomic<uint32_t> remainingTasks = { 0u };
  std::atomic<bool> cancelled = { false };
  std::atomic<bool> failed = { false };
  dxvk::mutex completionMutex;
  dxvk::condition_variable completionCv;
};

namespace {

enum class CpuSkinMtQueueTaskKind : uint8_t {
  Batch,
  Synchronous,
};

struct CpuSkinMtQueueTask {
  CpuSkinMtQueueTaskKind kind = CpuSkinMtQueueTaskKind::Batch;
  std::shared_ptr<CpuSkinMtBatchStateData> batch;
  std::shared_ptr<CpuSkinMtSynchronousOutputState> synchronous;
  uint32_t taskIndex = 0u;
  uint32_t firstVertex = 0u;
  uint32_t vertexCount = 0u;
};

void SetReject(CpuSkinMtRejectReason* output,
               CpuSkinMtRejectReason value) noexcept {
  if (output != nullptr)
    *output = value;
}

void TransformPositionAndNormal(
    const float matrix[12],
    const float position[3],
    const float normal[3],
    float transformedPosition[3],
    float transformedNormal[3]) noexcept {
#if DXVK_WAR3_CPU_SKIN_HAS_SSE
  // position/normal 共用同一组三列，但各自仍保持 native 的
  // ((column0*x)+(column1*y))+(column2*z) 分段顺序；normal 不加平移。
  // 显式 SSE intrinsic 与正式反汇编门共同保证目标 x86 路径无 FMA。
  const __m128 column0 = _mm_set_ps(
      0.0f, matrix[2], matrix[1], matrix[0]);
  const __m128 column1 = _mm_set_ps(
      0.0f, matrix[5], matrix[4], matrix[3]);
  const __m128 column2 = _mm_set_ps(
      0.0f, matrix[8], matrix[7], matrix[6]);
  __m128 positionValue = _mm_mul_ps(
      column0, _mm_set1_ps(position[0]));
  positionValue = _mm_add_ps(
      positionValue, _mm_mul_ps(column1, _mm_set1_ps(position[1])));
  positionValue = _mm_add_ps(
      positionValue, _mm_mul_ps(column2, _mm_set1_ps(position[2])));
  positionValue = _mm_add_ps(positionValue, _mm_set_ps(
      0.0f, matrix[11], matrix[10], matrix[9]));

  __m128 normalValue = _mm_mul_ps(
      column0, _mm_set1_ps(normal[0]));
  normalValue = _mm_add_ps(
      normalValue, _mm_mul_ps(column1, _mm_set1_ps(normal[1])));
  normalValue = _mm_add_ps(
      normalValue, _mm_mul_ps(column2, _mm_set1_ps(normal[2])));

  alignas(16) float positionTemporary[4];
  alignas(16) float normalTemporary[4];
  _mm_store_ps(positionTemporary, positionValue);
  _mm_store_ps(normalTemporary, normalValue);
  std::memcpy(transformedPosition, positionTemporary,
              3u * sizeof(float));
  std::memcpy(transformedNormal, normalTemporary,
              3u * sizeof(float));
#else
  const float positionX01 =
      matrix[0] * position[0] + matrix[3] * position[1];
  const float positionY01 =
      matrix[1] * position[0] + matrix[4] * position[1];
  const float positionZ01 =
      matrix[2] * position[0] + matrix[5] * position[1];
  transformedPosition[0] = positionX01 + matrix[6] * position[2];
  transformedPosition[1] = positionY01 + matrix[7] * position[2];
  transformedPosition[2] = positionZ01 + matrix[8] * position[2];
  transformedPosition[0] += matrix[9];
  transformedPosition[1] += matrix[10];
  transformedPosition[2] += matrix[11];

  const float normalX01 =
      matrix[0] * normal[0] + matrix[3] * normal[1];
  const float normalY01 =
      matrix[1] * normal[0] + matrix[4] * normal[1];
  const float normalZ01 =
      matrix[2] * normal[0] + matrix[5] * normal[1];
  transformedNormal[0] = normalX01 + matrix[6] * normal[2];
  transformedNormal[1] = normalY01 + matrix[7] * normal[2];
  transformedNormal[2] = normalZ01 + matrix[8] * normal[2];
#endif
}

}  // namespace

CpuSkinMtStaticSnapshot::CpuSkinMtStaticSnapshot(
    std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {
}

CpuSkinMtStaticSnapshot::~CpuSkinMtStaticSnapshot() = default;

std::shared_ptr<const CpuSkinMtStaticSnapshot>
CpuSkinMtStaticSnapshot::Create(
    const CpuSkinMtStaticSnapshotDesc& desc,
    CpuSkinMtRejectReason* reject) noexcept {
  SetReject(reject, CpuSkinMtRejectReason::InvalidStaticSnapshot);
  if (desc.mapEpoch == 0u || desc.deviceEpoch == 0u ||
      desc.contentHash == 0u || desc.geosetData == 0u ||
      desc.layoutGeneration == 0u || desc.vertexCount == 0u ||
      desc.vertexCount > kMaxNativeVertexCount ||
      desc.matrixGroupCount == 0u ||
      desc.matrixGroupCount > kMaxPaletteMatrixCount ||
      desc.uvLayerCount > 2u ||
      !StreamCovers(desc.positions, desc.vertexCount, 12u) ||
      !StreamCovers(desc.normals, desc.vertexCount, 12u) ||
      !StreamCovers(desc.groupSlots, desc.vertexCount, 1u) ||
      (desc.uvLayerCount >= 1u &&
       !StreamCovers(desc.texcoord0, desc.vertexCount, 8u)) ||
      (desc.uvLayerCount >= 2u &&
       !StreamCovers(desc.texcoord1, desc.vertexCount, 8u)) ||
      !StreamCovers(desc.diffuse, desc.vertexCount, 4u, true)) {
    return nullptr;
  }

  const GpuSkinStaticSourceLayout layout =
      GetGpuSkinStaticSourceLayout(desc.vertexCount, desc.uvLayerCount);
  if (layout.byteSize == 0u)
    return nullptr;

  try {
    auto impl = std::make_unique<Impl>();
    impl->mapEpoch = desc.mapEpoch;
    impl->deviceEpoch = desc.deviceEpoch;
    impl->contentHash = desc.contentHash;
    impl->geosetData = desc.geosetData;
    impl->layoutGeneration = desc.layoutGeneration;
    impl->vertexCount = desc.vertexCount;
    impl->matrixGroupCount = desc.matrixGroupCount;
    impl->uvLayerCount = desc.uvLayerCount;
    impl->layout = layout;
    impl->packedSource.resize(layout.byteSize);

    CopyStrided(impl->packedSource.data() + layout.positionOffset,
                desc.positions, desc.vertexCount, 12u);
    CopyStrided(impl->packedSource.data() + layout.normalOffset,
                desc.normals, desc.vertexCount, 12u);
    CopyStrided(impl->packedSource.data() + layout.groupSlotOffset,
                desc.groupSlots, desc.vertexCount, 1u);
    if (desc.uvLayerCount >= 1u) {
      CopyStrided(impl->packedSource.data() + layout.texcoord0Offset,
                  desc.texcoord0, desc.vertexCount, 8u);
    }
    if (desc.uvLayerCount >= 2u) {
      CopyStrided(impl->packedSource.data() + layout.texcoord1Offset,
                  desc.texcoord1, desc.vertexCount, 8u);
    }
    if (desc.diffuse.data != nullptr) {
      impl->diffuse.resize(desc.vertexCount);
      CopyStrided(reinterpret_cast<uint8_t*>(impl->diffuse.data()),
                  desc.diffuse, desc.vertexCount, 4u);
    }

    const uint8_t* groupSlots =
        impl->packedSource.data() + layout.groupSlotOffset;
    for (uint32_t i = 0u; i < desc.vertexCount; ++i)
      impl->maxGroupSlot = std::max<uint32_t>(
          impl->maxGroupSlot, groupSlots[i]);
    if (impl->maxGroupSlot >= impl->matrixGroupCount)
      return nullptr;

    SetReject(reject, CpuSkinMtRejectReason::None);
    return std::shared_ptr<const CpuSkinMtStaticSnapshot>(
        new CpuSkinMtStaticSnapshot(std::move(impl)));
  } catch (...) {
    return nullptr;
  }
}

uint64_t CpuSkinMtStaticSnapshot::mapEpoch() const noexcept {
  return m_impl != nullptr ? m_impl->mapEpoch : 0u;
}

uint64_t CpuSkinMtStaticSnapshot::deviceEpoch() const noexcept {
  return m_impl != nullptr ? m_impl->deviceEpoch : 0u;
}

uint64_t CpuSkinMtStaticSnapshot::contentHash() const noexcept {
  return m_impl != nullptr ? m_impl->contentHash : 0u;
}

uintptr_t CpuSkinMtStaticSnapshot::geosetData() const noexcept {
  return m_impl != nullptr ? m_impl->geosetData : 0u;
}

uint32_t CpuSkinMtStaticSnapshot::layoutGeneration() const noexcept {
  return m_impl != nullptr ? m_impl->layoutGeneration : 0u;
}

uint32_t CpuSkinMtStaticSnapshot::vertexCount() const noexcept {
  return m_impl != nullptr ? m_impl->vertexCount : 0u;
}

uint32_t CpuSkinMtStaticSnapshot::matrixGroupCount() const noexcept {
  return m_impl != nullptr ? m_impl->matrixGroupCount : 0u;
}

uint32_t CpuSkinMtStaticSnapshot::uvLayerCount() const noexcept {
  return m_impl != nullptr ? m_impl->uvLayerCount : 0u;
}

size_t CpuSkinMtStaticSnapshot::ownedByteSize() const noexcept {
  return m_impl != nullptr
      ? m_impl->packedSource.size() +
          m_impl->diffuse.size() * sizeof(uint32_t)
      : 0u;
}

CpuSkinMtFrozenKernel::CpuSkinMtFrozenKernel(
    std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {
}

CpuSkinMtFrozenKernel::~CpuSkinMtFrozenKernel() = default;

std::shared_ptr<const CpuSkinMtFrozenKernel>
CpuSkinMtFrozenKernel::Create(
    std::shared_ptr<const CpuSkinMtStaticSnapshot> staticSnapshot,
    const void* paletteData,
    size_t paletteByteSize,
    uint32_t outputFormat,
    CpuSkinMtRejectReason* reject) noexcept {
  SetReject(reject, CpuSkinMtRejectReason::InvalidPalette);
  if (staticSnapshot == nullptr || staticSnapshot->m_impl == nullptr ||
      paletteData == nullptr)
    return nullptr;

  const uint32_t outputStride = GetGpuSkinFvfStride(outputFormat);
  const uint32_t requiredUvLayers =
      GetGpuSkinFvfUvLayerCount(outputFormat);
  if (outputStride == 0u ||
      requiredUvLayers > staticSnapshot->uvLayerCount()) {
    SetReject(reject, CpuSkinMtRejectReason::InvalidOutputFormat);
    return nullptr;
  }

  const uint32_t floatingPointControl =
      CurrentFloatingPointControl();
  if (!HasSafeFloatingPointControl(floatingPointControl)) {
    SetReject(reject,
              CpuSkinMtRejectReason::FloatingPointEnvironmentMismatch);
    return nullptr;
  }

  size_t requiredPaletteBytes = 0u;
  if (!CheckedMul<size_t>(
          staticSnapshot->matrixGroupCount(), size_t(48u),
          requiredPaletteBytes) ||
      requiredPaletteBytes == 0u ||
      paletteByteSize < requiredPaletteBytes) {
    return nullptr;
  }

  uint32_t outputByteSize = 0u;
  if (!CheckedMul<uint32_t>(staticSnapshot->vertexCount(), outputStride,
                            outputByteSize) ||
      outputByteSize == 0u) {
    SetReject(reject, CpuSkinMtRejectReason::InvalidOutputFormat);
    return nullptr;
  }

  try {
    auto impl = std::make_unique<Impl>();
    impl->staticSnapshot = std::move(staticSnapshot);
    impl->palette.resize(requiredPaletteBytes);
    std::memcpy(impl->palette.data(), paletteData, requiredPaletteBytes);
    impl->outputFormat = outputFormat;
    impl->outputStride = outputStride;
    impl->outputByteSize = outputByteSize;
    impl->floatingPointControl = floatingPointControl;
    SetReject(reject, CpuSkinMtRejectReason::None);
    return std::shared_ptr<const CpuSkinMtFrozenKernel>(
        new CpuSkinMtFrozenKernel(std::move(impl)));
  } catch (...) {
    return nullptr;
  }
}

const std::shared_ptr<const CpuSkinMtStaticSnapshot>&
CpuSkinMtFrozenKernel::staticSnapshot() const noexcept {
  static const std::shared_ptr<const CpuSkinMtStaticSnapshot> kEmpty;
  return m_impl != nullptr ? m_impl->staticSnapshot : kEmpty;
}

uint32_t CpuSkinMtFrozenKernel::vertexCount() const noexcept {
  return m_impl != nullptr && m_impl->staticSnapshot != nullptr
      ? m_impl->staticSnapshot->vertexCount()
      : 0u;
}

uint32_t CpuSkinMtFrozenKernel::outputFormat() const noexcept {
  return m_impl != nullptr ? m_impl->outputFormat : 0u;
}

uint32_t CpuSkinMtFrozenKernel::outputStride() const noexcept {
  return m_impl != nullptr ? m_impl->outputStride : 0u;
}

uint32_t CpuSkinMtFrozenKernel::outputByteSize() const noexcept {
  return m_impl != nullptr ? m_impl->outputByteSize : 0u;
}

uint32_t CpuSkinMtFrozenKernel::floatingPointControl() const noexcept {
  return m_impl != nullptr ? m_impl->floatingPointControl : 0u;
}

size_t CpuSkinMtFrozenKernel::ownedPaletteByteSize() const noexcept {
  return m_impl != nullptr ? m_impl->palette.size() : 0u;
}

bool CpuSkinMtFrozenKernel::runRange(
    uint32_t firstVertex,
    uint32_t rangeVertexCount,
    void* outputBase,
    size_t outputByteSize) const noexcept {
  if (m_impl == nullptr ||
      !HasSafeFloatingPointControl(m_impl->floatingPointControl))
    return false;
  const ScopedFloatingPointControl floatingPointScope(
      m_impl->floatingPointControl);
  uint32_t cachedGroupSlot = std::numeric_limits<uint32_t>::max();
  float cachedMatrix[12];
  return runRangeWithInstalledFloatingPointControl(
      firstVertex, rangeVertexCount, outputBase, outputByteSize,
      &cachedGroupSlot, cachedMatrix);
}

bool CpuSkinMtFrozenKernel::runRangeWithInstalledFloatingPointControl(
    uint32_t firstVertex,
    uint32_t rangeVertexCount,
    void* outputBase,
    size_t outputByteSize,
    uint32_t* cachedGroupSlot,
    float* cachedMatrix) const noexcept {
  if (m_impl == nullptr || m_impl->staticSnapshot == nullptr ||
      m_impl->staticSnapshot->m_impl == nullptr || outputBase == nullptr ||
      cachedGroupSlot == nullptr || cachedMatrix == nullptr ||
      rangeVertexCount == 0u || firstVertex >= vertexCount() ||
      rangeVertexCount > vertexCount() - firstVertex ||
      outputByteSize < m_impl->outputByteSize)
    return false;

  const auto& snapshot = *m_impl->staticSnapshot->m_impl;
  if (snapshot.matrixGroupCount == 0u ||
      m_impl->palette.size() != size_t(snapshot.matrixGroupCount) * 48u ||
      snapshot.packedSource.size() != snapshot.layout.byteSize)
    return false;

  if (!HasSafeFloatingPointControl(m_impl->floatingPointControl) ||
      CurrentFloatingPointControl() != m_impl->floatingPointControl)
    return false;

  const uint8_t* positions =
      snapshot.packedSource.data() + snapshot.layout.positionOffset;
  const uint8_t* normals =
      snapshot.packedSource.data() + snapshot.layout.normalOffset;
  const uint8_t* groupSlots =
      snapshot.packedSource.data() + snapshot.layout.groupSlotOffset;
  const uint8_t* texcoord0 = snapshot.uvLayerCount >= 1u
      ? snapshot.packedSource.data() + snapshot.layout.texcoord0Offset
      : nullptr;
  const uint8_t* texcoord1 = snapshot.uvLayerCount >= 2u
      ? snapshot.packedSource.data() + snapshot.layout.texcoord1Offset
      : nullptr;
  auto* output = static_cast<uint8_t*>(outputBase);
  const uint32_t requiredUvLayers =
      GetGpuSkinFvfUvLayerCount(m_impl->outputFormat);
  const bool hasDiffuse = (m_impl->outputFormat & 1u) != 0u;

  for (uint32_t i = firstVertex;
       i < firstVertex + rangeVertexCount; ++i) {
    const uint32_t groupSlot = groupSlots[i];
    if (groupSlot >= snapshot.matrixGroupCount)
      return false;

    float position[3];
    float normal[3];
    float transformedPosition[3];
    float transformedNormal[3];
    if (groupSlot != *cachedGroupSlot) {
      std::memcpy(cachedMatrix,
                  m_impl->palette.data() + size_t(groupSlot) * 48u,
                  12u * sizeof(float));
      *cachedGroupSlot = groupSlot;
    }
    std::memcpy(position, positions + size_t(i) * 12u,
                sizeof(position));
    std::memcpy(normal, normals + size_t(i) * 12u,
                sizeof(normal));
    TransformPositionAndNormal(
        cachedMatrix, position, normal,
        transformedPosition, transformedNormal);

    uint8_t* vertexOutput =
        output + size_t(i) * m_impl->outputStride;
    std::memcpy(vertexOutput, transformedPosition,
                sizeof(transformedPosition));
    std::memcpy(vertexOutput + 12u, transformedNormal,
                sizeof(transformedNormal));
    uint32_t texcoordOutputOffset = 24u;
    if (hasDiffuse) {
      const uint32_t diffuse = snapshot.diffuse.empty()
          ? 0xffffffffu : snapshot.diffuse[i];
      std::memcpy(vertexOutput + 24u, &diffuse, sizeof(diffuse));
      texcoordOutputOffset = 28u;
    }
    if (requiredUvLayers >= 1u) {
      std::memcpy(vertexOutput + texcoordOutputOffset,
                  texcoord0 + size_t(i) * 8u, 8u);
    }
    if (requiredUvLayers >= 2u) {
      std::memcpy(vertexOutput + texcoordOutputOffset + 8u,
                  texcoord1 + size_t(i) * 8u, 8u);
    }
  }
  return true;
}

CpuSkinMtBatchHandle::CpuSkinMtBatchHandle(
    std::shared_ptr<CpuSkinMtBatchStateData> state) noexcept
    : m_state(std::move(state)) {
}

CpuSkinMtBatchHandle::operator bool() const noexcept {
  return m_state != nullptr;
}

uint64_t CpuSkinMtBatchHandle::batchId() const noexcept {
  return m_state != nullptr ? m_state->desc.batchId : 0u;
}

uint64_t CpuSkinMtBatchHandle::generation() const noexcept {
  return m_state != nullptr ? m_state->generation : 0u;
}

CpuSkinMtBatchState CpuSkinMtBatchHandle::state() const noexcept {
  return m_state != nullptr
      ? CpuSkinMtBatchState(
            m_state->batchState.load(std::memory_order_acquire))
      : CpuSkinMtBatchState::Invalid;
}

void CpuSkinMtBatchHandle::reset() noexcept {
  m_state.reset();
}

CpuSkinMtOutputLease::CpuSkinMtOutputLease(
    std::shared_ptr<const CpuSkinMtBatchStateData> state,
    uint32_t jobIndex) noexcept
    : m_state(std::move(state)), m_jobIndex(jobIndex) {
}

CpuSkinMtOutputLease::operator bool() const noexcept {
  if (m_state == nullptr || m_state->control == nullptr ||
      m_jobIndex >= m_state->jobs.size() ||
      m_state->output == nullptr ||
      m_state->jobs[m_jobIndex].desc.kernel == nullptr ||
      m_state->cancelled.load(std::memory_order_acquire) ||
      m_state->control->stopping.load(std::memory_order_acquire) ||
      m_state->control->generation.load(std::memory_order_acquire) !=
          m_state->generation)
    return false;
  const uint32_t currentFloatingPointControl =
      CurrentFloatingPointControl();
  if (!HasSafeFloatingPointControl(currentFloatingPointControl) ||
      m_state->jobs[m_jobIndex].desc.kernel->floatingPointControl() !=
          currentFloatingPointControl)
    return false;
  return CpuSkinMtJobState(
      m_state->jobStates[m_jobIndex].load(std::memory_order_acquire)) ==
      CpuSkinMtJobState::Ready;
}

const uint8_t* CpuSkinMtOutputLease::data() const noexcept {
  return static_cast<bool>(*this)
      ? m_state->output.get() + m_state->jobs[m_jobIndex].outputOffset
      : nullptr;
}

uint32_t CpuSkinMtOutputLease::byteSize() const noexcept {
  return static_cast<bool>(*this)
      ? m_state->jobs[m_jobIndex].outputByteSize : 0u;
}

uint32_t CpuSkinMtOutputLease::token() const noexcept {
  return static_cast<bool>(*this)
      ? m_state->jobs[m_jobIndex].desc.token : 0u;
}

uint32_t CpuSkinMtOutputLease::outputFormat() const noexcept {
  return static_cast<bool>(*this)
      ? m_state->jobs[m_jobIndex].outputFormat : 0u;
}

OutputLeaseDesc CpuSkinMtOutputLease::makeOutputLeaseDesc(
    uint32_t gpuByteOffset) const noexcept {
  OutputLeaseDesc result;
  if (!static_cast<bool>(*this))
    return result;
  const CpuSkinMtBatchJobRecord& job = m_state->jobs[m_jobIndex];
  result.mapEpoch = m_state->desc.mapEpoch;
  result.deviceEpoch = m_state->desc.deviceEpoch;
  result.frameTag = m_state->desc.frameTag;
  result.token = job.desc.token;
  result.dispatchEpoch = job.desc.dispatchEpoch;
  result.uploadEpoch = job.desc.uploadEpoch;
  result.byteOffset = gpuByteOffset;
  result.byteLength = job.outputByteSize;
  result.vertexStride = job.outputStride;
  result.vertexCount = job.desc.kernel->vertexCount();
  result.dipOrdinal = job.desc.dipOrdinal;
  result.consumerBits = job.desc.consumerBits;
  return result;
}

void CpuSkinMtOutputLease::reset() noexcept {
  m_state.reset();
  m_jobIndex = 0u;
}

CpuSkinMtSynchronousOutput::CpuSkinMtSynchronousOutput(
    std::shared_ptr<const CpuSkinMtSynchronousOutputState> state) noexcept
    : m_state(std::move(state)) {
}

CpuSkinMtSynchronousOutput::operator bool() const noexcept {
  if (m_state == nullptr || m_state->control == nullptr ||
      m_state->kernel == nullptr || m_state->staging == nullptr ||
      m_state->ownerThread != dxvk::this_thread::get_id() ||
      m_state->remainingTasks.load(std::memory_order_acquire) != 0u ||
      m_state->cancelled.load(std::memory_order_acquire) ||
      m_state->failed.load(std::memory_order_acquire) ||
      m_state->control->stopping.load(std::memory_order_acquire) ||
      m_state->control->generation.load(std::memory_order_acquire) !=
          m_state->generation)
    return false;
  const uint32_t currentFloatingPointControl =
      CurrentFloatingPointControl();
  return HasSafeFloatingPointControl(currentFloatingPointControl) &&
      m_state->kernel->floatingPointControl() ==
          currentFloatingPointControl;
}

const uint8_t* CpuSkinMtSynchronousOutput::data() const noexcept {
  return static_cast<bool>(*this) ? m_state->staging.get() : nullptr;
}

uint32_t CpuSkinMtSynchronousOutput::byteSize() const noexcept {
  return static_cast<bool>(*this) ? m_state->outputByteSize : 0u;
}

uint64_t CpuSkinMtSynchronousOutput::generation() const noexcept {
  return m_state != nullptr ? m_state->generation : 0u;
}

uint32_t CpuSkinMtSynchronousOutput::floatingPointControl() const noexcept {
  return m_state != nullptr && m_state->kernel != nullptr
      ? m_state->kernel->floatingPointControl() : 0u;
}

void CpuSkinMtSynchronousOutput::reset() noexcept {
  m_state.reset();
}

namespace {

CpuSkinMtConfig NormalizeConfig(CpuSkinMtConfig config) noexcept {
  config.maxWorkerCount = std::max(config.maxWorkerCount, 1u);
  const uint32_t hardwareThreads =
      std::max(dxvk::thread::hardware_concurrency(), 1u);
  const uint32_t automaticWorkers = hardwareThreads > 1u
      ? hardwareThreads - 1u : 1u;
  if (config.workerCount == 0u) {
    config.workerCount = std::min(
        automaticWorkers, config.maxWorkerCount);
  } else {
    config.workerCount = std::min(
        std::max(config.workerCount, 1u), config.maxWorkerCount);
  }
  config.maxQueuedTasks = std::max(config.maxQueuedTasks,
                                   config.workerCount + 1u);
  config.maxPendingBatches = std::max(config.maxPendingBatches, 1u);
  config.maxJobsPerBatch = std::max(config.maxJobsPerBatch, 1u);
  config.targetVerticesPerTask = std::max(
      config.targetVerticesPerTask, 1u);
  config.cancelCheckPeriodVertices = std::clamp(
      config.cancelCheckPeriodVertices, 1u, 64u);
  config.minAsyncVerticesPerJob = std::max(
      config.minAsyncVerticesPerJob, 1u);
  config.maxAsyncVerticesPerJob = std::max(
      config.maxAsyncVerticesPerJob,
      config.minAsyncVerticesPerJob);
  config.minAsyncVerticesPerBatch = std::max(
      config.minAsyncVerticesPerBatch,
      config.minAsyncVerticesPerJob);
  config.synchronousChunkVertices = std::max(
      config.synchronousChunkVertices, 1u);
  config.minSynchronousVertices = std::max(
      config.minSynchronousVertices,
      config.synchronousChunkVertices);
  config.maxOwnedBytes = std::max<size_t>(config.maxOwnedBytes, 1u);
  config.maxPinnedStaticBytesPerBatch = std::max<size_t>(
      config.maxPinnedStaticBytesPerBatch, 1u);
  config.maxOutputBytesPerBatch = std::max<size_t>(
      config.maxOutputBytesPerBatch, 1u);
  return config;
}

uint32_t FindJobIndex(const CpuSkinMtBatchStateData& batch,
                      uint32_t token) noexcept {
  for (uint32_t i = 0u; i < batch.jobs.size(); ++i) {
    if (batch.jobs[i].desc.token == token)
      return i;
  }
  return std::numeric_limits<uint32_t>::max();
}

bool IsTerminalJobState(CpuSkinMtJobState state) noexcept {
  return state == CpuSkinMtJobState::Ready ||
      state == CpuSkinMtJobState::CpuFallback ||
      state == CpuSkinMtJobState::Cancelled ||
      state == CpuSkinMtJobState::Failed;
}

}  // namespace

class War3CpuSkinMtProducer::Impl {
public:
  explicit Impl(const CpuSkinMtConfig& inputConfig)
      : m_config(NormalizeConfig(inputConfig)),
        m_control(std::make_shared<CpuSkinMtControlState>(m_config)),
        m_ownerThread(dxvk::this_thread::get_id()) {
    try {
      m_workers.reserve(m_config.workerCount);
      for (uint32_t i = 0u; i < m_config.workerCount; ++i) {
        m_workers.emplace_back(dxvk::thread(
            [this, i] { workerLoop(i); }));
      }
    } catch (...) {
      shutdown();
      throw;
    }
  }

  ~Impl() {
    // All mutating producer lifetime is render-owner affine. Destroying the
    // object on another thread cannot be made safe after its C++ lifetime has
    // already begun to end, so expose the integration error deterministically.
    if (!isOwnerThread())
      std::terminate();
    shutdown();
  }

  CpuSkinMtRejectReason submit(
      const CpuSkinMtBatchDesc& batchDesc,
      const CpuSkinMtJobDesc* jobDescs,
      size_t jobCount,
      CpuSkinMtBatchHandle& outputHandle) noexcept {
    outputHandle.reset();
    const auto reject = [this](CpuSkinMtRejectReason reason) {
      m_control->diagnostics.rejectedBatches.fetch_add(
          1u, std::memory_order_relaxed);
      return reason;
    };

    if (!isOwnerThread())
      return reject(CpuSkinMtRejectReason::WrongThread);
    if (m_control->stopping.load(std::memory_order_acquire))
      return reject(CpuSkinMtRejectReason::Stopping);
    if (batchDesc.batchId == 0u || batchDesc.mapEpoch == 0u ||
        batchDesc.deviceEpoch == 0u || batchDesc.frameTag == 0u ||
        batchDesc.flushEpoch == 0u)
      return reject(CpuSkinMtRejectReason::InvalidEpoch);
    if (jobDescs == nullptr || jobCount == 0u ||
        jobCount > m_config.maxJobsPerBatch ||
        jobCount > std::numeric_limits<uint32_t>::max())
      return reject(CpuSkinMtRejectReason::InvalidJob);

    const uint32_t currentFloatingPointControl =
        CurrentFloatingPointControl();
    if (!HasSafeFloatingPointControl(currentFloatingPointControl)) {
      m_control->diagnostics.floatingPointEnvironmentRejects.fetch_add(
          1u, std::memory_order_relaxed);
      return reject(
          CpuSkinMtRejectReason::FloatingPointEnvironmentMismatch);
    }

    uint64_t totalVertices = 0u;
    uint32_t outputCursor = 0u;
    size_t pinnedStaticBytes = 0u;
    size_t pinnedPaletteBytes = 0u;
    std::vector<const CpuSkinMtStaticSnapshot*> uniqueSnapshots;
    std::vector<const CpuSkinMtFrozenKernel*> uniqueKernels;
    try {
      uniqueSnapshots.reserve(jobCount);
      uniqueKernels.reserve(jobCount);
    } catch (...) {
      return reject(CpuSkinMtRejectReason::MemoryBackpressure);
    }

    for (size_t i = 0u; i < jobCount; ++i) {
      const CpuSkinMtJobDesc& job = jobDescs[i];
      if (job.kernel == nullptr || job.token == 0u ||
          job.kernel->staticSnapshot() == nullptr ||
          job.kernel->vertexCount() == 0u ||
          job.kernel->outputByteSize() == 0u ||
          job.kernel->staticSnapshot()->mapEpoch() != batchDesc.mapEpoch ||
          job.kernel->staticSnapshot()->deviceEpoch() !=
              batchDesc.deviceEpoch) {
        return reject(CpuSkinMtRejectReason::InvalidJob);
      }
      if (job.kernel->floatingPointControl() !=
          currentFloatingPointControl) {
        m_control->diagnostics.floatingPointEnvironmentRejects.fetch_add(
            1u, std::memory_order_relaxed);
        return reject(
            CpuSkinMtRejectReason::FloatingPointEnvironmentMismatch);
      }
      if ((job.kernel->outputFormat() & 1u) != 0u ||
          job.kernel->outputStride() !=
              GetGpuSkinFvfStride(job.kernel->outputFormat())) {
        return reject(CpuSkinMtRejectReason::InvalidOutputFormat);
      }
      if (job.kernel->vertexCount() < m_config.minAsyncVerticesPerJob ||
          job.kernel->vertexCount() > m_config.maxAsyncVerticesPerJob) {
        return reject(CpuSkinMtRejectReason::JobOutsideRoute);
      }
      for (size_t j = 0u; j < i; ++j) {
        if (jobDescs[j].token == job.token)
          return reject(CpuSkinMtRejectReason::DuplicateToken);
      }
      totalVertices += job.kernel->vertexCount();

      uint32_t alignedOutputCursor = 0u;
      uint32_t outputEnd = 0u;
      if (!CheckedAlign16(outputCursor, alignedOutputCursor) ||
          !CheckedAdd<uint32_t>(alignedOutputCursor,
                                job.kernel->outputByteSize(), outputEnd)) {
        return reject(CpuSkinMtRejectReason::MemoryBackpressure);
      }
      outputCursor = outputEnd;

      const CpuSkinMtStaticSnapshot* snapshot =
          job.kernel->staticSnapshot().get();
      if (std::find(uniqueSnapshots.begin(), uniqueSnapshots.end(),
                    snapshot) == uniqueSnapshots.end()) {
        uniqueSnapshots.push_back(snapshot);
        size_t nextPinned = 0u;
        if (!CheckedAdd(pinnedStaticBytes, snapshot->ownedByteSize(),
                        nextPinned))
          return reject(CpuSkinMtRejectReason::MemoryBackpressure);
        pinnedStaticBytes = nextPinned;
      }
      if (std::find(uniqueKernels.begin(), uniqueKernels.end(),
                    job.kernel.get()) == uniqueKernels.end()) {
        uniqueKernels.push_back(job.kernel.get());
        size_t nextPinned = 0u;
        if (!CheckedAdd(pinnedPaletteBytes,
                        job.kernel->ownedPaletteByteSize(), nextPinned))
          return reject(CpuSkinMtRejectReason::MemoryBackpressure);
        pinnedPaletteBytes = nextPinned;
      }
    }

    if (totalVertices < m_config.minAsyncVerticesPerBatch)
      return reject(CpuSkinMtRejectReason::BatchTooSmall);
    if (pinnedStaticBytes > m_config.maxPinnedStaticBytesPerBatch ||
        outputCursor > m_config.maxOutputBytesPerBatch)
      return reject(CpuSkinMtRejectReason::MemoryBackpressure);

    uint32_t taskCount = 0u;
    uint64_t taskVertices = 0u;
    for (size_t i = 0u; i < jobCount; ++i) {
      taskVertices += jobDescs[i].kernel->vertexCount();
      if (taskVertices >= m_config.targetVerticesPerTask ||
          i + 1u == jobCount) {
        ++taskCount;
        taskVertices = 0u;
      }
    }
    if (taskCount == 0u || taskCount > m_config.maxQueuedTasks)
      return reject(CpuSkinMtRejectReason::QueueBackpressure);

    size_t accountedBytes = outputCursor;
    size_t temporaryBytes = 0u;
    if (!CheckedMul(jobCount, sizeof(CpuSkinMtBatchJobRecord),
                    temporaryBytes) ||
        !CheckedAdd(accountedBytes, temporaryBytes, accountedBytes) ||
        !CheckedMul<size_t>(taskCount, sizeof(CpuSkinMtBatchTaskRange),
                            temporaryBytes) ||
        !CheckedAdd(accountedBytes, temporaryBytes, accountedBytes) ||
        !CheckedMul(jobCount,
                    sizeof(std::atomic<uint8_t>) +
                        sizeof(std::atomic<bool>),
                    temporaryBytes) ||
        !CheckedAdd(accountedBytes, temporaryBytes, accountedBytes) ||
        !CheckedAdd(accountedBytes, pinnedPaletteBytes,
                    accountedBytes)) {
      return reject(CpuSkinMtRejectReason::MemoryBackpressure);
    }

    std::shared_ptr<CpuSkinMtBatchStateData> state;
    try {
      state = std::make_shared<CpuSkinMtBatchStateData>();
      state->control = m_control;
      state->desc = batchDesc;
      state->generation = m_control->generation.load(
          std::memory_order_acquire);
      state->outputByteSize = outputCursor;
      state->jobs.reserve(jobCount);
      state->tasks.reserve(taskCount);
      state->output.reset(new (std::nothrow) uint8_t[outputCursor]);
      state->jobStates.reset(
          new (std::nothrow) std::atomic<uint8_t>[jobCount]);
      state->jobCancel.reset(
          new (std::nothrow) std::atomic<bool>[jobCount]);
      if (state->output == nullptr || state->jobStates == nullptr ||
          state->jobCancel == nullptr)
        return reject(CpuSkinMtRejectReason::MemoryBackpressure);

      uint32_t cursor = 0u;
      for (size_t i = 0u; i < jobCount; ++i) {
        uint32_t alignedCursor = 0u;
        if (!CheckedAlign16(cursor, alignedCursor))
          return reject(CpuSkinMtRejectReason::MemoryBackpressure);
        CpuSkinMtBatchJobRecord job;
        job.desc = jobDescs[i];
        job.outputOffset = alignedCursor;
        job.outputByteSize = job.desc.kernel->outputByteSize();
        job.outputStride = job.desc.kernel->outputStride();
        job.outputFormat = job.desc.kernel->outputFormat();
        state->jobs.push_back(std::move(job));
        state->jobStates[i].store(
            uint8_t(CpuSkinMtJobState::Queued),
            std::memory_order_relaxed);
        state->jobCancel[i].store(false, std::memory_order_relaxed);
        cursor = alignedCursor + state->jobs.back().outputByteSize;
      }

      uint32_t firstJob = 0u;
      uint64_t groupedVertices = 0u;
      for (uint32_t i = 0u; i < jobCount; ++i) {
        groupedVertices += state->jobs[i].desc.kernel->vertexCount();
        if (groupedVertices >= m_config.targetVerticesPerTask ||
            i + 1u == jobCount) {
          state->tasks.push_back({ firstJob, i - firstJob + 1u });
          firstJob = i + 1u;
          groupedVertices = 0u;
        }
      }
      if (state->tasks.size() != taskCount)
        return reject(CpuSkinMtRejectReason::InvalidJob);

      if (!m_control->reserveBatch(accountedBytes)) {
        m_control->diagnostics.memoryBackpressure.fetch_add(
            1u, std::memory_order_relaxed);
        return reject(CpuSkinMtRejectReason::MemoryBackpressure);
      }
      state->accounted = true;
      state->accountedBytes = accountedBytes;
      state->remainingTasks.store(taskCount, std::memory_order_relaxed);
      state->batchState.store(
          uint8_t(CpuSkinMtBatchState::Queued),
          std::memory_order_release);

      {
        std::unique_lock<dxvk::mutex> lock(m_queueMutex);
        if (m_control->stopping.load(std::memory_order_acquire) ||
            m_tasks.size() > m_config.maxQueuedTasks - taskCount) {
          state->cancelled.store(true, std::memory_order_release);
          m_control->diagnostics.queueBackpressure.fetch_add(
              1u, std::memory_order_relaxed);
          return reject(CpuSkinMtRejectReason::QueueBackpressure);
        }
        const size_t oldSize = m_tasks.size();
        try {
          for (uint32_t i = 0u; i < taskCount; ++i) {
            CpuSkinMtQueueTask task;
            task.kind = CpuSkinMtQueueTaskKind::Batch;
            task.batch = state;
            task.taskIndex = i;
            m_tasks.push_back(std::move(task));
          }
        } catch (...) {
          while (m_tasks.size() > oldSize)
            m_tasks.pop_back();
          state->cancelled.store(true, std::memory_order_release);
          m_control->diagnostics.queueBackpressure.fetch_add(
              1u, std::memory_order_relaxed);
          return reject(CpuSkinMtRejectReason::QueueBackpressure);
        }
        AtomicMax<uint64_t>(
            m_control->diagnostics.queuedTaskHighWater,
            uint64_t(m_tasks.size()));
      }
    } catch (...) {
      return reject(CpuSkinMtRejectReason::MemoryBackpressure);
    }

    m_control->diagnostics.submittedBatches.fetch_add(
        1u, std::memory_order_relaxed);
    m_control->diagnostics.submittedJobs.fetch_add(
        jobCount, std::memory_order_relaxed);
    m_control->diagnostics.paletteBytesPinned.fetch_add(
        pinnedPaletteBytes, std::memory_order_relaxed);
    m_control->diagnostics.staticBytesPinned.fetch_add(
        pinnedStaticBytes, std::memory_order_relaxed);
    m_control->diagnostics.outputBytesOwned.fetch_add(
        outputCursor, std::memory_order_relaxed);
    outputHandle = War3CpuSkinMtProducer::makeBatchHandle(state);
    m_queueCv.notify_all();
    return CpuSkinMtRejectReason::None;
  }

  bool tryAcquireOutput(const CpuSkinMtBatchHandle& batch,
                        uint32_t token,
                        CpuSkinMtOutputLease& output) noexcept {
    output.reset();
    const auto& state = War3CpuSkinMtProducer::batchState(batch);
    if (!isOwnerThread() || state == nullptr ||
        state->control.get() != m_control.get())
      return false;
    const uint32_t jobIndex = FindJobIndex(*state, token);
    if (jobIndex == std::numeric_limits<uint32_t>::max())
      return false;
    if (state->generation !=
            m_control->generation.load(std::memory_order_acquire) ||
        m_control->stopping.load(std::memory_order_acquire)) {
      m_control->diagnostics.staleLeaseRejects.fetch_add(
          1u, std::memory_order_relaxed);
      return false;
    }
    if (CpuSkinMtJobState(
            state->jobStates[jobIndex].load(
                std::memory_order_acquire)) != CpuSkinMtJobState::Ready)
      return false;
    const uint32_t currentFloatingPointControl =
        CurrentFloatingPointControl();
    if (!HasSafeFloatingPointControl(currentFloatingPointControl) ||
        state->jobs[jobIndex].desc.kernel->floatingPointControl() !=
            currentFloatingPointControl) {
      m_control->diagnostics.floatingPointEnvironmentRejects.fetch_add(
          1u, std::memory_order_relaxed);
      return false;
    }
    output = War3CpuSkinMtProducer::makeOutputLease(state, jobIndex);
    return static_cast<bool>(output);
  }

  bool cancelJob(const CpuSkinMtBatchHandle& batch,
                 uint32_t token) noexcept {
    const auto& batchState = War3CpuSkinMtProducer::batchState(batch);
    if (!isOwnerThread() || batchState == nullptr ||
        batchState->control.get() != m_control.get())
      return false;
    const uint32_t jobIndex = FindJobIndex(*batchState, token);
    if (jobIndex == std::numeric_limits<uint32_t>::max())
      return false;
    const bool cancelled = finishJob(
        batchState, jobIndex, CpuSkinMtJobState::CpuFallback, 0u);
    if (cancelled) {
      batchState->jobCancel[jobIndex].store(
          true, std::memory_order_release);
    }
    return cancelled;
  }

  void cancelBatch(const CpuSkinMtBatchHandle& batch) noexcept {
    const auto& state = War3CpuSkinMtProducer::batchState(batch);
    if (!isOwnerThread() || state == nullptr ||
        state->control.get() != m_control.get())
      return;
    state->cancelled.store(true, std::memory_order_release);
    for (uint32_t i = 0u; i < state->jobs.size(); ++i) {
      state->jobCancel[i].store(true, std::memory_order_release);
      finishJob(state, i, CpuSkinMtJobState::Cancelled, 0u);
    }
  }

  uint32_t assist(const CpuSkinMtBatchHandle& batch,
                  uint32_t maxTasks) noexcept {
    const auto& state = War3CpuSkinMtProducer::batchState(batch);
    if (!isOwnerThread() || state == nullptr || maxTasks == 0u ||
        state->control.get() != m_control.get())
      return 0u;
    uint32_t completed = 0u;
    while (completed < maxTasks) {
      CpuSkinMtQueueTask task;
      if (!popTaskForBatch(state, task))
        break;
      executeTask(task, true);
      ++completed;
    }
    return completed;
  }

  CpuSkinMtSynchronousResult runSynchronous(
      const std::shared_ptr<const CpuSkinMtFrozenKernel>& kernel,
      CpuSkinMtSynchronousOutput& output,
      CpuSkinMtRejectReason* reject) noexcept {
    output.reset();
    SetReject(reject, CpuSkinMtRejectReason::None);
    m_control->diagnostics.synchronousCalls.fetch_add(
        1u, std::memory_order_relaxed);
    if (!isOwnerThread()) {
      SetReject(reject, CpuSkinMtRejectReason::WrongThread);
      return CpuSkinMtSynchronousResult::Rejected;
    }
    if (m_control->stopping.load(std::memory_order_acquire)) {
      SetReject(reject, CpuSkinMtRejectReason::Stopping);
      return CpuSkinMtSynchronousResult::Rejected;
    }
    if (kernel == nullptr) {
      SetReject(reject, CpuSkinMtRejectReason::InvalidJob);
      return CpuSkinMtSynchronousResult::Rejected;
    }
    if ((kernel->outputFormat() & 1u) != 0u) {
      SetReject(reject, CpuSkinMtRejectReason::InvalidOutputFormat);
      return CpuSkinMtSynchronousResult::Rejected;
    }
    const uint32_t currentFloatingPointControl =
        CurrentFloatingPointControl();
    if (!HasSafeFloatingPointControl(currentFloatingPointControl) ||
        kernel->floatingPointControl() != currentFloatingPointControl) {
      m_control->diagnostics.floatingPointEnvironmentRejects.fetch_add(
          1u, std::memory_order_relaxed);
      SetReject(reject,
                CpuSkinMtRejectReason::FloatingPointEnvironmentMismatch);
      return CpuSkinMtSynchronousResult::Rejected;
    }
    if (kernel->vertexCount() < m_config.minSynchronousVertices) {
      SetReject(reject, CpuSkinMtRejectReason::BatchTooSmall);
      return CpuSkinMtSynchronousResult::Rejected;
    }
    if (kernel->outputByteSize() > m_config.maxOutputBytesPerBatch ||
        kernel->outputByteSize() > m_config.maxOwnedBytes) {
      m_control->diagnostics.memoryBackpressure.fetch_add(
          1u, std::memory_order_relaxed);
      SetReject(reject, CpuSkinMtRejectReason::MemoryBackpressure);
      return CpuSkinMtSynchronousResult::Rejected;
    }

    const uint32_t taskCount =
        (kernel->vertexCount() - 1u) /
            m_config.synchronousChunkVertices + 1u;
    if (taskCount == 0u || taskCount > m_config.maxQueuedTasks) {
      SetReject(reject, CpuSkinMtRejectReason::QueueBackpressure);
      return CpuSkinMtSynchronousResult::Rejected;
    }

    std::shared_ptr<CpuSkinMtSynchronousOutputState> state;
    try {
      state = std::make_shared<CpuSkinMtSynchronousOutputState>();
      state->control = m_control;
      state->kernel = kernel;
      state->generation = m_control->generation.load(
          std::memory_order_acquire);
      state->ownerThread = m_ownerThread;
      state->outputByteSize = kernel->outputByteSize();
      if (!m_control->reserveOwnedBytes(state->outputByteSize)) {
        m_control->diagnostics.memoryBackpressure.fetch_add(
            1u, std::memory_order_relaxed);
        SetReject(reject, CpuSkinMtRejectReason::MemoryBackpressure);
        return CpuSkinMtSynchronousResult::Rejected;
      }
      state->accountedBytes = state->outputByteSize;
      state->accounted = true;
      state->staging.reset(new (std::nothrow)
          uint8_t[state->outputByteSize]);
      if (state->staging == nullptr) {
        SetReject(reject, CpuSkinMtRejectReason::MemoryBackpressure);
        return CpuSkinMtSynchronousResult::Rejected;
      }
      state->remainingTasks.store(taskCount, std::memory_order_relaxed);

      std::unique_lock<dxvk::mutex> lock(m_queueMutex);
      if (m_control->stopping.load(std::memory_order_acquire) ||
          m_tasks.size() > m_config.maxQueuedTasks - taskCount) {
        m_control->diagnostics.queueBackpressure.fetch_add(
            1u, std::memory_order_relaxed);
        SetReject(reject, CpuSkinMtRejectReason::QueueBackpressure);
        return CpuSkinMtSynchronousResult::Rejected;
      }
      const size_t oldSize = m_tasks.size();
      try {
        for (uint32_t i = 0u; i < taskCount; ++i) {
          CpuSkinMtQueueTask task;
          task.kind = CpuSkinMtQueueTaskKind::Synchronous;
          task.synchronous = state;
          task.firstVertex = i * m_config.synchronousChunkVertices;
          task.vertexCount = std::min(
              m_config.synchronousChunkVertices,
              kernel->vertexCount() - task.firstVertex);
          m_tasks.push_back(std::move(task));
        }
      } catch (...) {
        while (m_tasks.size() > oldSize)
          m_tasks.pop_back();
        SetReject(reject, CpuSkinMtRejectReason::QueueBackpressure);
        return CpuSkinMtSynchronousResult::Rejected;
      }
      AtomicMax<uint64_t>(
          m_control->diagnostics.queuedTaskHighWater,
          uint64_t(m_tasks.size()));
    } catch (...) {
      SetReject(reject, CpuSkinMtRejectReason::MemoryBackpressure);
      return CpuSkinMtSynchronousResult::Rejected;
    }

    m_queueCv.notify_all();
    while (state->remainingTasks.load(std::memory_order_acquire) != 0u) {
      CpuSkinMtQueueTask task;
      if (popTaskForSynchronous(state, task)) {
        executeTask(task, true);
        continue;
      }
      std::unique_lock<dxvk::mutex> lock(state->completionMutex);
      state->completionCv.wait(lock, [&] {
        return state->remainingTasks.load(std::memory_order_acquire) == 0u;
      });
    }

    if (state->cancelled.load(std::memory_order_acquire) ||
        state->generation !=
            m_control->generation.load(std::memory_order_acquire)) {
      SetReject(reject, CpuSkinMtRejectReason::Cancelled);
      return CpuSkinMtSynchronousResult::Cancelled;
    }
    if (state->failed.load(std::memory_order_acquire)) {
      SetReject(reject, CpuSkinMtRejectReason::KernelFailure);
      return CpuSkinMtSynchronousResult::Failed;
    }
    if (CurrentFloatingPointControl() != currentFloatingPointControl) {
      m_control->diagnostics.floatingPointEnvironmentRejects.fetch_add(
          1u, std::memory_order_relaxed);
      SetReject(reject,
                CpuSkinMtRejectReason::FloatingPointEnvironmentMismatch);
      return CpuSkinMtSynchronousResult::Rejected;
    }

    output = War3CpuSkinMtProducer::makeSynchronousOutput(state);
    if (!static_cast<bool>(output)) {
      output.reset();
      SetReject(reject,
                CpuSkinMtRejectReason::FloatingPointEnvironmentMismatch);
      return CpuSkinMtSynchronousResult::Rejected;
    }
    m_control->diagnostics.synchronousReadyBytes.fetch_add(
        state->outputByteSize, std::memory_order_relaxed);
    m_control->diagnostics.synchronousCompleted.fetch_add(
        1u, std::memory_order_relaxed);
    return CpuSkinMtSynchronousResult::Completed;
  }

  uint64_t reset() noexcept {
    if (!isOwnerThread())
      return 0u;
    const uint64_t nextGeneration = NextGeneration(m_control->generation);
    m_control->diagnostics.resets.fetch_add(
        1u, std::memory_order_relaxed);
    drainQueuedTasks();
    m_queueCv.notify_all();
    return nextGeneration;
  }

  bool shutdown() noexcept {
    if (!isOwnerThread())
      return false;
    if (m_control == nullptr ||
        m_control->stopping.exchange(true, std::memory_order_acq_rel))
      return true;
    NextGeneration(m_control->generation);
    drainQueuedTasks();
    m_queueCv.notify_all();
    for (dxvk::thread& worker : m_workers) {
      if (!worker.joinable())
        continue;
      try {
        worker.join();
        m_control->diagnostics.joinedWorkers.fetch_add(
            1u, std::memory_order_relaxed);
      } catch (...) {
        // DXVK thread 析构遇到 joinable 会 terminate。这里显式终止比
        // 静默 detach 或遗留线程更安全，也不会伪造 clean shutdown。
        std::terminate();
      }
    }
    m_workers.clear();
    return true;
  }

  uint64_t generation() const noexcept {
    return m_control->generation.load(std::memory_order_acquire);
  }

  CpuSkinMtDiagnostics snapshotDiagnostics() const noexcept {
    CpuSkinMtDiagnostics result;
    const auto& source = m_control->diagnostics;
    result.generation = generation();
    result.submittedBatches = source.submittedBatches.load();
    result.rejectedBatches = source.rejectedBatches.load();
    result.submittedJobs = source.submittedJobs.load();
    result.readyJobs = source.readyJobs.load();
    result.cpuFallbackJobs = source.cpuFallbackJobs.load();
    result.cancelledJobs = source.cancelledJobs.load();
    result.failedJobs = source.failedJobs.load();
    result.workerTasks = source.workerTasks.load();
    result.ownerAssistTasks = source.ownerAssistTasks.load();
    result.synchronousCalls = source.synchronousCalls.load();
    result.synchronousCompleted = source.synchronousCompleted.load();
    result.queuedTaskHighWater = source.queuedTaskHighWater.load();
    result.paletteBytesPinned = source.paletteBytesPinned.load();
    result.staticBytesPinned = source.staticBytesPinned.load();
    result.outputBytesOwned = source.outputBytesOwned.load();
    result.verticesCompleted = source.verticesCompleted.load();
    result.queueBackpressure = source.queueBackpressure.load();
    result.memoryBackpressure = source.memoryBackpressure.load();
    result.staleLeaseRejects = source.staleLeaseRejects.load();
    result.floatingPointEnvironmentRejects =
        source.floatingPointEnvironmentRejects.load();
    result.synchronousReadyBytes =
        source.synchronousReadyBytes.load();
    result.resets = source.resets.load();
    result.joinedWorkers = source.joinedWorkers.load();
    result.workerCount = m_control->stopping.load(
        std::memory_order_acquire) ? 0u : m_config.workerCount;
    {
      std::unique_lock<dxvk::mutex> lock(m_queueMutex);
      result.pendingTasks = uint32_t(std::min<size_t>(
          m_tasks.size(), std::numeric_limits<uint32_t>::max()));
    }
    return result;
  }

private:
  bool isOwnerThread() const noexcept {
    return dxvk::this_thread::get_id() == m_ownerThread;
  }

  void workerLoop(uint32_t workerIndex) noexcept {
    try {
      env::setThreadName(
          std::string("war3-cpu-skin-") + std::to_string(workerIndex));
    } catch (...) {
      // 线程命名只用于诊断，失败不能让 persistent worker 退出。
    }
    for (;;) {
      CpuSkinMtQueueTask task;
      {
        std::unique_lock<dxvk::mutex> lock(m_queueMutex);
        m_queueCv.wait(lock, [this] {
          return m_control->stopping.load(std::memory_order_acquire) ||
              !m_tasks.empty();
        });
        if (m_tasks.empty()) {
          if (m_control->stopping.load(std::memory_order_acquire))
            return;
          continue;
        }
        task = std::move(m_tasks.front());
        m_tasks.pop_front();
      }
      executeTask(task, false);
    }
  }

  void executeTask(const CpuSkinMtQueueTask& task,
                   bool ownerAssist) noexcept {
    if (ownerAssist) {
      m_control->diagnostics.ownerAssistTasks.fetch_add(
          1u, std::memory_order_relaxed);
    } else {
      m_control->diagnostics.workerTasks.fetch_add(
          1u, std::memory_order_relaxed);
    }
    if (task.kind == CpuSkinMtQueueTaskKind::Batch)
      executeBatchTask(task);
    else
      executeSynchronousTask(task);
  }

  void executeBatchTask(const CpuSkinMtQueueTask& task) noexcept {
    const auto& batch = task.batch;
    if (batch == nullptr || task.taskIndex >= batch->tasks.size())
      return;
    uint8_t expected = uint8_t(CpuSkinMtBatchState::Queued);
    batch->batchState.compare_exchange_strong(
        expected, uint8_t(CpuSkinMtBatchState::Running),
        std::memory_order_acq_rel, std::memory_order_relaxed);

    const CpuSkinMtBatchTaskRange range = batch->tasks[task.taskIndex];
    if (range.firstJob > batch->jobs.size() ||
        range.jobCount > batch->jobs.size() - range.firstJob) {
      batch->cancelled.store(true, std::memory_order_release);
      finishBatchTask(batch);
      return;
    }

    for (uint32_t local = 0u; local < range.jobCount; ++local) {
      const uint32_t jobIndex = range.firstJob + local;
      CpuSkinMtBatchJobRecord& job = batch->jobs[jobIndex];
      const bool generationCancelled =
          batch->generation !=
              batch->control->generation.load(std::memory_order_acquire) ||
          batch->control->stopping.load(std::memory_order_acquire) ||
          batch->cancelled.load(std::memory_order_acquire);
      if (generationCancelled) {
        finishJob(batch, jobIndex, CpuSkinMtJobState::Cancelled, 0u);
        continue;
      }
      if (batch->jobCancel[jobIndex].load(std::memory_order_acquire)) {
        finishJob(batch, jobIndex, CpuSkinMtJobState::CpuFallback, 0u);
        continue;
      }

      uint8_t expectedState = uint8_t(CpuSkinMtJobState::Queued);
      if (!batch->jobStates[jobIndex].compare_exchange_strong(
              expectedState, uint8_t(CpuSkinMtJobState::Running),
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        // cancelJob 可在上面的 flag 检查后先把 Queued 结算为
        // CpuFallback。worker 只能 claim Queued，绝不能把 terminal
        // state 覆盖回 Running；同一 job 不会被切到两个 batch task。
        continue;
      }
      bool success = true;
      uint32_t completedVertices = 0u;
      const uint32_t vertexCount = job.desc.kernel->vertexCount();
      const uint32_t floatingPointControl =
          job.desc.kernel->floatingPointControl();
      if (!HasSafeFloatingPointControl(floatingPointControl)) {
        success = false;
      } else {
        const ScopedFloatingPointControl floatingPointScope(
            floatingPointControl);
        uint32_t cachedGroupSlot =
            std::numeric_limits<uint32_t>::max();
        float cachedMatrix[12];
        while (completedVertices < vertexCount) {
          if (batch->jobCancel[jobIndex].load(std::memory_order_acquire) ||
              batch->cancelled.load(std::memory_order_acquire) ||
              batch->generation != batch->control->generation.load(
                  std::memory_order_acquire) ||
              batch->control->stopping.load(std::memory_order_acquire)) {
            success = false;
            break;
          }
          const uint32_t chunk = std::min(
              m_config.cancelCheckPeriodVertices,
              vertexCount - completedVertices);
          success = War3CpuSkinMtProducer::
              runFrozenRangeWithInstalledFloatingPointControl(
                  *job.desc.kernel, completedVertices, chunk,
                  batch->output.get() + job.outputOffset,
                  job.outputByteSize, &cachedGroupSlot, cachedMatrix);
          if (!success)
            break;
          completedVertices += chunk;
        }
      }

      if (batch->jobCancel[jobIndex].load(std::memory_order_acquire)) {
        finishJob(batch, jobIndex, CpuSkinMtJobState::CpuFallback, 0u);
      } else if (batch->cancelled.load(std::memory_order_acquire) ||
                 batch->generation != batch->control->generation.load(
                     std::memory_order_acquire) ||
                 batch->control->stopping.load(std::memory_order_acquire)) {
        finishJob(batch, jobIndex, CpuSkinMtJobState::Cancelled, 0u);
      } else if (!success || completedVertices != vertexCount) {
        finishJob(batch, jobIndex, CpuSkinMtJobState::Failed, 0u);
      } else {
        finishJob(batch, jobIndex, CpuSkinMtJobState::Ready,
                  vertexCount);
      }
    }
    finishBatchTask(batch);
  }

  void executeSynchronousTask(const CpuSkinMtQueueTask& task) noexcept {
    const auto& state = task.synchronous;
    if (state == nullptr || state->kernel == nullptr) {
      return;
    }
    bool success = true;
    uint32_t completedVertices = 0u;
    const uint32_t floatingPointControl =
        state->kernel->floatingPointControl();
    if (!HasSafeFloatingPointControl(floatingPointControl)) {
      success = false;
    } else {
      const ScopedFloatingPointControl floatingPointScope(
          floatingPointControl);
      uint32_t cachedGroupSlot =
          std::numeric_limits<uint32_t>::max();
      float cachedMatrix[12];
      while (completedVertices < task.vertexCount) {
        if (state->cancelled.load(std::memory_order_acquire) ||
            state->control->stopping.load(std::memory_order_acquire) ||
            state->generation != state->control->generation.load(
                std::memory_order_acquire)) {
          state->cancelled.store(true, std::memory_order_release);
          success = false;
          break;
        }
        const uint32_t chunk = std::min(
            m_config.cancelCheckPeriodVertices,
            task.vertexCount - completedVertices);
        success = War3CpuSkinMtProducer::
            runFrozenRangeWithInstalledFloatingPointControl(
                *state->kernel,
                task.firstVertex + completedVertices, chunk,
                state->staging.get(), state->outputByteSize,
                &cachedGroupSlot, cachedMatrix);
        if (!success)
          break;
        completedVertices += chunk;
      }
    }
    if (!success &&
        !state->cancelled.load(std::memory_order_acquire))
      state->failed.store(true, std::memory_order_release);
    if (success) {
      state->control->diagnostics.verticesCompleted.fetch_add(
          completedVertices, std::memory_order_relaxed);
    }
    finishSynchronousTask(state);
  }

  bool finishJob(const std::shared_ptr<CpuSkinMtBatchStateData>& batch,
                 uint32_t jobIndex,
                 CpuSkinMtJobState finalState,
                 uint32_t completedVertices) noexcept {
    if (batch == nullptr || jobIndex >= batch->jobs.size() ||
        !IsTerminalJobState(finalState))
      return false;
    uint8_t current = batch->jobStates[jobIndex].load(
        std::memory_order_acquire);
    for (;;) {
      if (IsTerminalJobState(CpuSkinMtJobState(current)))
        return false;
      if (batch->jobStates[jobIndex].compare_exchange_weak(
              current, uint8_t(finalState),
              std::memory_order_acq_rel,
              std::memory_order_acquire))
        break;
    }
    switch (finalState) {
      case CpuSkinMtJobState::Ready:
        batch->control->diagnostics.readyJobs.fetch_add(
            1u, std::memory_order_relaxed);
        batch->control->diagnostics.verticesCompleted.fetch_add(
            completedVertices, std::memory_order_relaxed);
        break;
      case CpuSkinMtJobState::CpuFallback:
        batch->control->diagnostics.cpuFallbackJobs.fetch_add(
            1u, std::memory_order_relaxed);
        break;
      case CpuSkinMtJobState::Cancelled:
        batch->control->diagnostics.cancelledJobs.fetch_add(
            1u, std::memory_order_relaxed);
        break;
      case CpuSkinMtJobState::Failed:
        batch->control->diagnostics.failedJobs.fetch_add(
            1u, std::memory_order_relaxed);
        break;
      default:
        break;
    }
    return true;
  }

  void finishBatchTask(
      const std::shared_ptr<CpuSkinMtBatchStateData>& batch) noexcept {
    if (batch->remainingTasks.fetch_sub(
            1u, std::memory_order_acq_rel) != 1u)
      return;
    uint32_t ready = 0u;
    uint32_t failed = 0u;
    uint32_t cancelled = 0u;
    for (uint32_t i = 0u; i < batch->jobs.size(); ++i) {
      switch (CpuSkinMtJobState(batch->jobStates[i].load(
          std::memory_order_acquire))) {
        case CpuSkinMtJobState::Ready:
          ++ready;
          break;
        case CpuSkinMtJobState::Failed:
          ++failed;
          break;
        case CpuSkinMtJobState::Cancelled:
          ++cancelled;
          break;
        default:
          break;
      }
    }
    CpuSkinMtBatchState finalState = CpuSkinMtBatchState::Partial;
    if (failed != 0u)
      finalState = CpuSkinMtBatchState::Failed;
    else if (ready == batch->jobs.size())
      finalState = CpuSkinMtBatchState::Ready;
    else if (ready == 0u &&
             (cancelled != 0u ||
              batch->cancelled.load(std::memory_order_acquire) ||
              batch->generation != batch->control->generation.load(
                  std::memory_order_acquire)))
      finalState = CpuSkinMtBatchState::Cancelled;
    batch->batchState.store(uint8_t(finalState),
                            std::memory_order_release);
  }

  void finishSynchronousTask(
      const std::shared_ptr<CpuSkinMtSynchronousOutputState>& state) noexcept {
    if (state->remainingTasks.fetch_sub(
            1u, std::memory_order_acq_rel) == 1u) {
      std::unique_lock<dxvk::mutex> lock(state->completionMutex);
      state->completionCv.notify_all();
    }
  }

  bool popTaskForBatch(
      const std::shared_ptr<CpuSkinMtBatchStateData>& batch,
      CpuSkinMtQueueTask& output) noexcept {
    std::unique_lock<dxvk::mutex> lock(m_queueMutex);
    for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it) {
      if (it->kind == CpuSkinMtQueueTaskKind::Batch &&
          it->batch == batch) {
        output = std::move(*it);
        m_tasks.erase(it);
        return true;
      }
    }
    return false;
  }

  bool popTaskForSynchronous(
      const std::shared_ptr<CpuSkinMtSynchronousOutputState>& synchronous,
      CpuSkinMtQueueTask& output) noexcept {
    std::unique_lock<dxvk::mutex> lock(m_queueMutex);
    for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it) {
      if (it->kind == CpuSkinMtQueueTaskKind::Synchronous &&
          it->synchronous == synchronous) {
        output = std::move(*it);
        m_tasks.erase(it);
        return true;
      }
    }
    return false;
  }

  void cancelQueuedTask(const CpuSkinMtQueueTask& task) noexcept {
    if (task.kind == CpuSkinMtQueueTaskKind::Synchronous) {
      if (task.synchronous != nullptr) {
        task.synchronous->cancelled.store(true, std::memory_order_release);
        finishSynchronousTask(task.synchronous);
      }
      return;
    }
    if (task.batch == nullptr ||
        task.taskIndex >= task.batch->tasks.size())
      return;
    task.batch->cancelled.store(true, std::memory_order_release);
    const CpuSkinMtBatchTaskRange range =
        task.batch->tasks[task.taskIndex];
    if (range.firstJob <= task.batch->jobs.size() &&
        range.jobCount <= task.batch->jobs.size() - range.firstJob) {
      for (uint32_t i = 0u; i < range.jobCount; ++i) {
        finishJob(task.batch, range.firstJob + i,
                  CpuSkinMtJobState::Cancelled, 0u);
      }
    }
    finishBatchTask(task.batch);
  }

  void drainQueuedTasks() noexcept {
    std::deque<CpuSkinMtQueueTask> cancelled;
    {
      std::unique_lock<dxvk::mutex> lock(m_queueMutex);
      cancelled.swap(m_tasks);
    }
    for (const CpuSkinMtQueueTask& task : cancelled)
      cancelQueuedTask(task);
  }

  CpuSkinMtConfig m_config;
  std::shared_ptr<CpuSkinMtControlState> m_control;
  uint32_t m_ownerThread = 0u;
  mutable dxvk::mutex m_queueMutex;
  dxvk::condition_variable m_queueCv;
  std::deque<CpuSkinMtQueueTask> m_tasks;
  std::vector<dxvk::thread> m_workers;
};

War3CpuSkinMtProducer::War3CpuSkinMtProducer(
    const CpuSkinMtConfig& config)
    : m_impl(std::make_unique<Impl>(config)) {
}

War3CpuSkinMtProducer::~War3CpuSkinMtProducer() = default;

const std::shared_ptr<CpuSkinMtBatchStateData>&
War3CpuSkinMtProducer::batchState(
    const CpuSkinMtBatchHandle& batch) noexcept {
  return batch.m_state;
}

CpuSkinMtBatchHandle War3CpuSkinMtProducer::makeBatchHandle(
    std::shared_ptr<CpuSkinMtBatchStateData> state) noexcept {
  return CpuSkinMtBatchHandle(std::move(state));
}

CpuSkinMtOutputLease War3CpuSkinMtProducer::makeOutputLease(
    std::shared_ptr<const CpuSkinMtBatchStateData> state,
    uint32_t jobIndex) noexcept {
  return CpuSkinMtOutputLease(std::move(state), jobIndex);
}

CpuSkinMtSynchronousOutput War3CpuSkinMtProducer::makeSynchronousOutput(
    std::shared_ptr<const CpuSkinMtSynchronousOutputState> state) noexcept {
  return CpuSkinMtSynchronousOutput(std::move(state));
}

bool War3CpuSkinMtProducer::
runFrozenRangeWithInstalledFloatingPointControl(
    const CpuSkinMtFrozenKernel& kernel,
    uint32_t firstVertex,
    uint32_t vertexCount,
    void* outputBase,
    size_t outputByteSize,
    uint32_t* cachedGroupSlot,
    float* cachedMatrix) noexcept {
  return kernel.runRangeWithInstalledFloatingPointControl(
      firstVertex, vertexCount, outputBase, outputByteSize,
      cachedGroupSlot, cachedMatrix);
}

CpuSkinMtRejectReason War3CpuSkinMtProducer::submit(
    const CpuSkinMtBatchDesc& batch,
    const CpuSkinMtJobDesc* jobs,
    size_t jobCount,
    CpuSkinMtBatchHandle& output) noexcept {
  return m_impl != nullptr
      ? m_impl->submit(batch, jobs, jobCount, output)
      : CpuSkinMtRejectReason::Stopping;
}

bool War3CpuSkinMtProducer::tryAcquireOutput(
    const CpuSkinMtBatchHandle& batch,
    uint32_t token,
    CpuSkinMtOutputLease& output) noexcept {
  return m_impl != nullptr &&
      m_impl->tryAcquireOutput(batch, token, output);
}

bool War3CpuSkinMtProducer::cancelJob(
    const CpuSkinMtBatchHandle& batch,
    uint32_t token) noexcept {
  return m_impl != nullptr && m_impl->cancelJob(batch, token);
}

void War3CpuSkinMtProducer::cancelBatch(
    const CpuSkinMtBatchHandle& batch) noexcept {
  if (m_impl != nullptr)
    m_impl->cancelBatch(batch);
}

uint32_t War3CpuSkinMtProducer::assist(
    const CpuSkinMtBatchHandle& batch,
    uint32_t maxTasks) noexcept {
  return m_impl != nullptr ? m_impl->assist(batch, maxTasks) : 0u;
}

CpuSkinMtSynchronousResult War3CpuSkinMtProducer::runSynchronous(
    const std::shared_ptr<const CpuSkinMtFrozenKernel>& kernel,
    CpuSkinMtSynchronousOutput& output,
    CpuSkinMtRejectReason* reject) noexcept {
  output.reset();
  if (m_impl == nullptr) {
    SetReject(reject, CpuSkinMtRejectReason::Stopping);
    return CpuSkinMtSynchronousResult::Rejected;
  }
  return m_impl->runSynchronous(
      kernel, output, reject);
}

uint64_t War3CpuSkinMtProducer::reset() noexcept {
  return m_impl != nullptr ? m_impl->reset() : 0u;
}

bool War3CpuSkinMtProducer::shutdown() noexcept {
  return m_impl == nullptr || m_impl->shutdown();
}

uint64_t War3CpuSkinMtProducer::generation() const noexcept {
  return m_impl != nullptr ? m_impl->generation() : 0u;
}

CpuSkinMtDiagnostics
War3CpuSkinMtProducer::snapshotDiagnostics() const noexcept {
  return m_impl != nullptr
      ? m_impl->snapshotDiagnostics()
      : CpuSkinMtDiagnostics{};
}

}  // namespace dxvk::war3::gpu_skin
