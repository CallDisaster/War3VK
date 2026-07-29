#include "d3d9_mem.h"
#include "../util/util_string.h"
#include "../util/util_math.h"
#include "../util/log/log.h"
#include "../util/util_likely.h"
#include <utility>
#include <algorithm>
#include <limits>

#ifdef D3D9_ALLOW_UNMAPPING
#include <sysinfoapi.h>
#else
#include <stdlib.h>
#endif

namespace dxvk {

  namespace {

#ifndef D3D9_ALLOW_UNMAPPING
    void AdvanceAtomicDiagnosticGeneration(
            std::atomic<uint64_t>& generation) noexcept {
      uint64_t current = generation.load(std::memory_order_relaxed);
      while (current != std::numeric_limits<uint64_t>::max()
          && !generation.compare_exchange_weak(
              current, current + 1,
              std::memory_order_relaxed,
              std::memory_order_relaxed)) {
      }
    }
#endif

#ifdef D3D9_ALLOW_UNMAPPING
    std::atomic<uint64_t> g_nextD3D9MemoryChunkId { 1 };

    uint64_t AllocateD3D9MemoryChunkId() noexcept {
      uint64_t current =
          g_nextD3D9MemoryChunkId.load(std::memory_order_relaxed);
      while (current != 0) {
        const uint64_t next =
            current == std::numeric_limits<uint64_t>::max()
                ? 0
                : current + 1;
        if (g_nextD3D9MemoryChunkId.compare_exchange_weak(
                current, next,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
          return current;
      }
      return 0;
    }
#endif

  }

#ifdef D3D9_ALLOW_UNMAPPING
  D3D9MemoryAllocator::D3D9MemoryAllocator() {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    m_allocationGranularity = sysInfo.dwAllocationGranularity;
    m_mappingGranularity = m_allocationGranularity * 16;
  }

  D3D9Memory D3D9MemoryAllocator::Alloc(uint32_t Size) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    uint32_t alignedSize = align(Size, CACHE_LINE_SIZE);
    for (auto& chunk : m_chunks) {
      D3D9Memory memory = chunk->AllocLocked(alignedSize);
      if (memory) {
        m_usedMemory += memory.GetSize();
        return memory;
      }
    }

    const uint64_t chunkId = AllocateD3D9MemoryChunkId();
    if (unlikely(chunkId == 0)) {
      Logger::err("D3D9MemoryAllocator: Diagnostic chunk ID exhausted");
    }

    uint32_t chunkSize = std::max(D3D9ChunkSize, alignedSize);
    m_allocatedMemory += chunkSize;

    D3D9MemoryChunk* chunk =
        new D3D9MemoryChunk(this, chunkId, chunkSize);
    std::unique_ptr<D3D9MemoryChunk> uniqueChunk(chunk);
    D3D9Memory memory = uniqueChunk->AllocLocked(alignedSize);
    m_usedMemory += memory.GetSize();

    m_chunks.push_back(std::move(uniqueChunk));
    return memory;
  }

  void D3D9MemoryAllocator::Free(D3D9Memory *Memory) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    D3D9MemoryChunk* chunk = Memory->GetChunk();
    chunk->FreeLocked(Memory);
    m_usedMemory -= Memory->GetSize();
    if (chunk->IsEmpty())
      FreeChunk(chunk);
  }

  void D3D9MemoryAllocator::FreeChunk(D3D9MemoryChunk *Chunk) {
    // 必须在持锁状态下调用。

    m_allocatedMemory -= Chunk->Size();

    m_chunks.erase(std::remove_if(m_chunks.begin(), m_chunks.end(), [&](auto& item) {
        return item.get() == Chunk;
    }), m_chunks.end());
  }

  void* D3D9MemoryAllocator::Map(D3D9Memory* Memory) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    D3D9MemoryChunk* chunk = Memory->GetChunk();
    uint32_t memoryMapped;
    void* ptr = chunk->MapLocked(Memory, memoryMapped);
    m_mappedMemory += memoryMapped;
    return ptr;
  }

  void D3D9MemoryAllocator::Unmap(D3D9Memory* Memory) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    D3D9MemoryChunk* chunk = Memory->GetChunk();
    m_mappedMemory -= chunk->UnmapLocked(Memory);
  }

  uint32_t D3D9MemoryAllocator::MappedMemory() const {
    return m_mappedMemory.load();
  }

  uint32_t D3D9MemoryAllocator::UsedMemory() const {
    return m_usedMemory.load();
  }

  uint32_t D3D9MemoryAllocator::AllocatedMemory() const {
    return m_allocatedMemory.load();
  }

  void D3D9MemoryAllocator::AdvanceDiagnosticMutationLocked() noexcept {
    // 必须在持有 allocator 锁时调用；饱和后不回绕，避免旧快照与新状态发生 ABA。

    if (m_mutationGeneration == std::numeric_limits<uint64_t>::max()) {
      m_mutationGenerationSaturated = true;
      return;
    }
    m_mutationGeneration++;
    if (m_mutationGeneration == std::numeric_limits<uint64_t>::max())
      m_mutationGenerationSaturated = true;
  }

  D3D9MemoryAllocatorDiagnosticSnapshot
  D3D9MemoryAllocator::CaptureDiagnosticSnapshot() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    D3D9MemoryAllocatorDiagnosticSnapshot snapshot;
    snapshot.chunkBacked = true;
    snapshot.mutationGeneration = m_mutationGeneration;
    snapshot.mutationGenerationSaturated =
        m_mutationGenerationSaturated;
    snapshot.chunks.reserve(m_chunks.size());
    uint64_t snapshotDetectedMappingStateFaultCount = 0;

    for (const auto& chunk : m_chunks) {
      D3D9MemoryChunkDiagnosticSnapshot value;
      value.chunkId = chunk->m_chunkId;
      value.reserveBytes = chunk->m_size;
      value.freeRangeCount = chunk->m_freeRanges.size();

      for (const D3D9MemoryRange& range : chunk->m_freeRanges)
        value.freePayloadBytes += range.length;

      if (value.freePayloadBytes <= value.reserveBytes) {
        value.chunkOccupiedBytes =
            value.reserveBytes - value.freePayloadBytes;
      } else {
        value.mappingStateFaultCount++;
        snapshotDetectedMappingStateFaultCount++;
      }

      for (const D3D9MappingRange& range : chunk->m_mappingRanges) {
        value.sharedMappedRefs += range.refCount;
        if (range.ptr != nullptr)
          value.sharedMappedBytes += m_mappingGranularity;
        if ((range.refCount == 0) != (range.ptr == nullptr)) {
          value.mappingStateFaultCount++;
          snapshotDetectedMappingStateFaultCount++;
        }
      }

      value.standaloneMappedRefs = chunk->m_standaloneMappedRefs;
      value.standaloneMappedBytes = chunk->m_standaloneMappedBytes;
      value.mapFailureCount = chunk->m_mapFailureCount;
      value.unmapFailureCount = chunk->m_unmapFailureCount;
      value.mappingStateFaultCount += chunk->m_mappingStateFaultCount;

      snapshot.reserveBytes += value.reserveBytes;
      snapshot.chunkOccupiedBytes += value.chunkOccupiedBytes;
      snapshot.freePayloadBytes += value.freePayloadBytes;
      snapshot.sharedMappedRefs += value.sharedMappedRefs;
      snapshot.sharedMappedBytes += value.sharedMappedBytes;
      snapshot.standaloneMappedRefs += value.standaloneMappedRefs;
      snapshot.standaloneMappedBytes += value.standaloneMappedBytes;
      snapshot.chunks.push_back(value);
    }

    // allocator 级计数保留已退休 chunk 的故障历史，不能用当前 chunk 求和替代。
    snapshot.mapFailureCount = m_mapFailureCount;
    snapshot.unmapFailureCount = m_unmapFailureCount;
    snapshot.mappingStateFaultCount =
        m_mappingStateFaultCount + snapshotDetectedMappingStateFaultCount;
    snapshot.allocatorUsedPayloadBytes = m_usedMemory.load();
    if (snapshot.chunkOccupiedBytes >=
        snapshot.allocatorUsedPayloadBytes) {
      snapshot.internalFragmentationBytes =
          snapshot.chunkOccupiedBytes -
          snapshot.allocatorUsedPayloadBytes;
    } else {
      snapshot.mappingStateFaultCount++;
    }

    snapshot.mappedRefs =
        snapshot.sharedMappedRefs + snapshot.standaloneMappedRefs;
    snapshot.mappedBytes =
        snapshot.sharedMappedBytes + snapshot.standaloneMappedBytes;
    snapshot.accountingClosure =
        snapshot.mappingStateFaultCount == 0
        && snapshot.unmapFailureCount == 0
        && snapshot.reserveBytes == m_allocatedMemory.load()
        && snapshot.chunkOccupiedBytes ==
            snapshot.allocatorUsedPayloadBytes +
                snapshot.internalFragmentationBytes
        && snapshot.mappedBytes == m_mappedMemory.load();
    return snapshot;
  }

  D3D9MemoryChunk::D3D9MemoryChunk(
          D3D9MemoryAllocator* Allocator,
          uint64_t ChunkId,
          uint32_t Size)
    : m_allocator ( Allocator )
    , m_chunkId ( ChunkId )
    , m_size ( Size )
    , m_mapping ( CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, Size, nullptr) ) {
    m_freeRanges.push_back({ 0, Size });
    uint32_t mappingGranularity = Allocator->MappingGranularity();
    m_mappingRanges.resize(((Size + mappingGranularity - 1) / mappingGranularity));
  }

  D3D9MemoryChunk::~D3D9MemoryChunk() {
    // 必须由 allocator 锁保护。

    CloseHandle(m_mapping);
  }

  void* D3D9MemoryChunk::MapLocked(D3D9Memory* Memory, uint32_t& mappedSize) {
    // 必须由 allocator 锁保护。

    mappedSize = 0;
    uint32_t mappingGranularity = m_allocator->MappingGranularity();

    uint32_t alignedOffset = alignDown(Memory->GetOffset(), mappingGranularity);
    uint32_t alignmentDelta = Memory->GetOffset() - alignedOffset;
    uint32_t alignedSize = Memory->GetSize() + alignmentDelta;
    if (alignedSize > mappingGranularity) {
      // 该分配跨越了所属内部映射页的边界，因此单独建立映射。
      alignedOffset = alignDown(Memory->GetOffset(), m_allocator->AllocationGranularity());
      alignmentDelta = Memory->GetOffset() - alignedOffset;
      alignedSize = Memory->GetSize() + alignmentDelta;

      uint8_t* basePtr = static_cast<uint8_t*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, alignedOffset, alignedSize));
      if (unlikely(basePtr == nullptr)) {
        m_mapFailureCount++;
        m_allocator->m_mapFailureCount++;
        m_allocator->AdvanceDiagnosticMutationLocked();
        DWORD error = GetLastError();
        Logger::err(str::format("Mapping non-persisted file failed: ", error, ", Mapped memory: ", m_allocator->MappedMemory()));
        return nullptr;
      }
      m_standaloneMappedRefs++;
      m_standaloneMappedBytes += alignedSize;
      m_allocator->AdvanceDiagnosticMutationLocked();
      mappedSize = alignedSize;
      return basePtr + alignmentDelta;
    }

    // 小分配直接映射整个映射页，减少将偏移对齐到 64 KiB 带来的额外开销，
    // 同时尽量减少微小分配触发 MapViewOfFile 的次数。
    auto& mappingRange = m_mappingRanges[Memory->GetOffset() / mappingGranularity];
    if (unlikely(mappingRange.refCount == 0)) {
      mappingRange.ptr = static_cast<uint8_t*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, alignedOffset, m_allocator->MappingGranularity()));
      if (unlikely(mappingRange.ptr == nullptr)) {
        m_mapFailureCount++;
        m_allocator->m_mapFailureCount++;
        m_allocator->AdvanceDiagnosticMutationLocked();
        DWORD error = GetLastError();
        LPTSTR buffer = nullptr;
        FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), (LPTSTR)&buffer, 0, nullptr);
        Logger::err(str::format("Mapping non-persisted file failed: ", error, ", Mapped memory: ", m_allocator->MappedMemory(), ", Msg: ", buffer));
        if (buffer) {
          LocalFree(buffer);
        }
        return nullptr;
      }
      mappedSize = mappingGranularity;
    }
    mappingRange.refCount++;
    m_allocator->AdvanceDiagnosticMutationLocked();
    uint8_t* basePtr = static_cast<uint8_t*>(mappingRange.ptr);
    return basePtr + alignmentDelta;
  }

  uint32_t D3D9MemoryChunk::UnmapLocked(D3D9Memory* Memory) {
    // 必须由 allocator 锁保护。

    uint32_t mappingGranularity = m_allocator->MappingGranularity();

    uint32_t alignedOffset = alignDown(Memory->GetOffset(), mappingGranularity);
    uint32_t alignmentDelta = Memory->GetOffset() - alignedOffset;
    uint32_t alignedSize = Memory->GetSize() + alignmentDelta;
    if (alignedSize > mappingGranularity) {
      // 单次使用的独立映射。
      alignedOffset = alignDown(Memory->GetOffset(), m_allocator->AllocationGranularity());
      alignmentDelta = Memory->GetOffset() - alignedOffset;
      alignedSize = Memory->GetSize() + alignmentDelta;

      uint8_t* basePtr = static_cast<uint8_t*>(Memory->Ptr()) - alignmentDelta;
      if (unlikely(!UnmapViewOfFile(basePtr))) {
        m_unmapFailureCount++;
        m_allocator->m_unmapFailureCount++;
      }
      if (likely(m_standaloneMappedRefs != 0
              && m_standaloneMappedBytes >= alignedSize)) {
        m_standaloneMappedRefs--;
        m_standaloneMappedBytes -= alignedSize;
      } else {
        m_mappingStateFaultCount++;
        m_allocator->m_mappingStateFaultCount++;
        m_standaloneMappedRefs = 0;
        m_standaloneMappedBytes = 0;
      }
      m_allocator->AdvanceDiagnosticMutationLocked();
      return alignedSize;
    }
    auto& mappingRange = m_mappingRanges[Memory->GetOffset() / mappingGranularity];
    if (unlikely(mappingRange.refCount == 0)) {
      m_mappingStateFaultCount++;
      m_allocator->m_mappingStateFaultCount++;
      m_allocator->AdvanceDiagnosticMutationLocked();
      return 0;
    }
    mappingRange.refCount--;
    if (unlikely(mappingRange.refCount == 0)) {
      if (unlikely(mappingRange.ptr == nullptr
                || !UnmapViewOfFile(mappingRange.ptr))) {
        m_unmapFailureCount++;
        m_allocator->m_unmapFailureCount++;
      }
      mappingRange.ptr = nullptr;
      m_allocator->AdvanceDiagnosticMutationLocked();
      return mappingGranularity;
    }
    m_allocator->AdvanceDiagnosticMutationLocked();
    return 0;
  }

  D3D9Memory D3D9MemoryChunk::AllocLocked(uint32_t Size) {
    // 必须由 allocator 锁保护。

    uint32_t offset = 0;
    uint32_t size = 0;

    for (auto range = m_freeRanges.begin(); range != m_freeRanges.end(); range++) {
      if (range->length >= Size) {
        offset = range->offset;
        size = Size;
        range->offset += Size;
        range->length -= Size;
        // 保留既有 allocator 行为：小于 4 KiB 的尾段计入 chunk 占用，
        // 但 D3D9Memory 的 payload 大小仍保持调用方请求值。
        if (range->length < (4 << 10)) {
          size += range->length;
          m_freeRanges.erase(range);
        }
        break;
      }
    }

    if (size != 0) {
      m_allocator->AdvanceDiagnosticMutationLocked();
      return D3D9Memory(this, offset, Size);
    }

    return {};
  }

  void D3D9MemoryChunk::FreeLocked(D3D9Memory *Memory) {
    // 必须由 allocator 锁保护。

    uint32_t offset = Memory->GetOffset();
    uint32_t size = Memory->GetSize();

    auto curr = m_freeRanges.begin();

    // 该合并逻辑沿用 dxvk_memory.cpp 的实现。
    while (curr != m_freeRanges.end()) {
      if (curr->offset == offset + size) {
        size += curr->length;
        curr = m_freeRanges.erase(curr);
      } else if (curr->offset + curr->length == offset) {
        offset -= curr->length;
        size += curr->length;
        curr = m_freeRanges.erase(curr);
      } else {
        curr++;
      }
    }

    m_freeRanges.push_back({ offset, size });
    m_allocator->AdvanceDiagnosticMutationLocked();
  }

  bool D3D9MemoryChunk::IsEmpty() const {
    // 必须由 allocator 锁保护。

    return m_freeRanges.size() == 1
        && m_freeRanges[0].length == m_size;
  }

  D3D9MemoryAllocator* D3D9MemoryChunk::Allocator() const {
    return m_allocator;
  }


  D3D9Memory::D3D9Memory(D3D9MemoryChunk* Chunk, size_t Offset, size_t Size)
    : m_chunk(Chunk), m_offset(Offset), m_size(Size) {}

  D3D9Memory::D3D9Memory(D3D9Memory&& other)
    : m_chunk(std::exchange(other.m_chunk, nullptr)),
      m_ptr(std::exchange(other.m_ptr, nullptr)),
      m_offset(std::exchange(other.m_offset, 0)),
      m_size(std::exchange(other.m_size, 0)) {}

  D3D9Memory::~D3D9Memory() {
    this->Free();
  }

  D3D9Memory& D3D9Memory::operator = (D3D9Memory&& other) {
    this->Free();

    m_chunk = std::exchange(other.m_chunk, nullptr);
    m_ptr = std::exchange(other.m_ptr, nullptr);
    m_offset = std::exchange(other.m_offset, 0);
    m_size = std::exchange(other.m_size, 0);
    return *this;
  }

  void D3D9Memory::Free() {
    if (unlikely(m_chunk == nullptr))
      return;

    if (m_ptr != nullptr)
      Unmap();

    m_chunk->Allocator()->Free(this);
    m_chunk = nullptr;
  }

  void D3D9Memory::Map() {
    if (unlikely(m_ptr != nullptr))
      return;

    if (unlikely(m_chunk == nullptr))
      return;

    m_ptr = m_chunk->Allocator()->Map(this);
  }

  void D3D9Memory::Unmap() {
    if (unlikely(m_ptr == nullptr))
      return;

    m_chunk->Allocator()->Unmap(this);
    m_ptr = nullptr;
  }

  void* D3D9Memory::Ptr() {
    return m_ptr;
  }

  D3D9MemoryDiagnosticBinding
  D3D9Memory::GetDiagnosticBinding() const noexcept {
    if (m_chunk == nullptr)
      return {};
    return {
      m_chunk->ChunkId(),
      static_cast<uint64_t>(m_offset),
      static_cast<uint64_t>(m_size),
      m_ptr != nullptr,
    };
  }

#else

  D3D9Memory D3D9MemoryAllocator::Alloc(uint32_t Size) {
    D3D9Memory memory(this, Size);
    m_allocatedMemory += Size;
    AdvanceAtomicDiagnosticGeneration(m_mutationGeneration);
    return memory;
  }

  uint32_t D3D9MemoryAllocator::MappedMemory() const {
    return m_allocatedMemory.load();
  }

  uint32_t D3D9MemoryAllocator::UsedMemory() const {
    return m_allocatedMemory.load();
  }

  uint32_t D3D9MemoryAllocator::AllocatedMemory() const {
    return m_allocatedMemory.load();
  }

  void D3D9MemoryAllocator::NotifyFreed(uint32_t Size) {
    m_allocatedMemory -= Size;
    AdvanceAtomicDiagnosticGeneration(m_mutationGeneration);
  }

  D3D9MemoryAllocatorDiagnosticSnapshot
  D3D9MemoryAllocator::CaptureDiagnosticSnapshot() {
    D3D9MemoryAllocatorDiagnosticSnapshot snapshot;
    snapshot.chunkBacked = false;
    snapshot.mutationGeneration =
        m_mutationGeneration.load(std::memory_order_relaxed);
    snapshot.mutationGenerationSaturated =
        snapshot.mutationGeneration == std::numeric_limits<uint64_t>::max();
    snapshot.reserveBytes = m_allocatedMemory.load();
    snapshot.allocatorUsedPayloadBytes = snapshot.reserveBytes;
    snapshot.chunkOccupiedBytes = snapshot.reserveBytes;
    snapshot.mappedBytes = snapshot.reserveBytes;
    // malloc 后备没有 chunk/free-range 账本，不能声明 chunk closure。
    snapshot.accountingClosure = false;
    return snapshot;
  }

  D3D9Memory::D3D9Memory(D3D9MemoryAllocator* pAllocator, size_t Size)
    : m_allocator (pAllocator),
      m_ptr       (malloc(Size)),
      m_size      (Size) {}

  D3D9Memory::D3D9Memory(D3D9Memory&& other)
    : m_allocator(std::exchange(other.m_allocator, nullptr)),
      m_ptr(std::exchange(other.m_ptr, nullptr)),
      m_size(std::exchange(other.m_size, 0)) {}

  D3D9Memory::~D3D9Memory() {
    this->Free();
  }

  D3D9Memory& D3D9Memory::operator = (D3D9Memory&& other) {
    this->Free();

    m_allocator = std::exchange(other.m_allocator, nullptr);
    m_ptr = std::exchange(other.m_ptr, nullptr);
    m_size = std::exchange(other.m_size, 0);
    return *this;
  }

  void D3D9Memory::Free() {
    if (m_ptr == nullptr)
      return;

    free(m_ptr);
    m_ptr = nullptr;
    m_allocator->NotifyFreed(m_size);
  }

  D3D9MemoryDiagnosticBinding
  D3D9Memory::GetDiagnosticBinding() const noexcept {
    return {
      0,
      0,
      static_cast<uint64_t>(m_size),
      m_ptr != nullptr,
    };
  }


#endif

}
