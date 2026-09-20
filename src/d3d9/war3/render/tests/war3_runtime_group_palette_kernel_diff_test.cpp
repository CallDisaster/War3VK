// =============================================================================
// 2026-09-17 S2（收窄版）可复现随机差分 + 适配层派生 / 成本口径测试
//
// 对应上级 S2 复核要求：
//   * "随机差分程序已删除，30 万次零差异目前不可复现。应保留程序、种子、
//     旧实现身份及输出，且覆盖适配层，不只覆盖内核。"
//     -> 本文件把程序永久入库；固定种子；旧实现身份（pre-S2 副本 SHA-256）
//        与逐语句抽取的旧实现一起入库；每次运行都打印摘要（种子 / 两组输入数 /
//        各自 mismatch 数 / 旧实现副本 SHA-256），任何不匹配即非零退出。
//   * "数值等价可以作为进展，不能据此宣称热路径成本等价。"
//     -> 本文件只报告分配次数 / 写入元素数 / 槽位扫描趟数的实测口径与不确定性，
//        并在输出里显式声明"不宣称热路径成本等价"。
//
// -----------------------------------------------------------------------------
// 旧实现身份（pre-S2 抽取前源码副本，权威来源）
// -----------------------------------------------------------------------------
//   C:\Windows\Temp\warvk_s2_backup\war3_shadow_renderer_core.cpp
//     SHA-256 = 0F7B720E1FAF5B7065CE40DFCE4D7462A5AC14D3217DDA6716A395228030D8F9
//     size    = 402159 bytes
//     抽取点  = TryBuildRuntimeGroupPalette（5 步 fallback 集合：direct / sparse /
//               directPose / sparsePose / uniformRoot）
//     副本位置= 本文件 legacy::TryBuildRuntimeGroupPaletteFiveStep
//   C:\Windows\Temp\warvk_s2_backup\war3_upper_layer_shadow.cpp
//     SHA-256 = 61EB0C8FFDF24F36016B1651A7C677BE0C47E805680C5278F0CE18BD950517A9
//     size    = 17763 bytes
//     抽取点  = TryBuildRuntimeGroupPalette（4 步 fallback 集合：direct / sparse /
//               directPose / sparsePose）
//     副本位置= 本文件 legacy::TryBuildRuntimeGroupPaletteFourStep
//
// 两个副本的计算语句逐条照抄，只做三类机械替换（全部在注释里标注）：
//   (1) 记录类型换成同字段名的 legacy 局部替身（d3d9 记录类型在宿主机不可链接）；
//   (2) 诊断面 NoteRuntimeGroupPaletteMiss / logFailure 换成 legacy 局部同构实现；
//   (3) 4 步副本为取得旧实现的失败出口，按 pre-S2 core 自己的
//       "return (Note...(...), false);" 惯用法插桩，不新增/删除/重排任何计算语句。
//   旧实现里的来源链（producer 快照 / +0x08 槽位 / 记忆槽位 / Game.dll arena 读）
//   与日志面依赖运行时内存，本文件不复现也不覆盖（抽取点之后的纯计算段才是
//   S2 的替换范围）。
//
// -----------------------------------------------------------------------------
// 固定种子与生成算法
// -----------------------------------------------------------------------------
//   PRNG = SplitMix64（Steele / Lea / Flood, "Fast Splittable Pseudorandom Number
//   Generators", 2014）：每次 next() 先 state += 0x9E3779B97F4A7C15ull，再做
//     z = state; z ^= z >> 30; z *= 0xBF58476D1CE4E5B9ull;
//     z ^= z >> 27; z *= 0x94D049BB133111EBull; return z ^ (z >> 31);
//   种子常量（写进输出）：
//     kSeedCoreGroup  = 0x5332464600000001  ("S2FF" + 组号 1, core 适配层 + 5 步)
//     kSeedUpperGroup = 0x5332464600000002  ("S2FF" + 组号 2, upper 适配层 + 4 步)
//   每组迭代 kGroupIterations = 300000 次；不读时钟、不读随机设备，结果与机器无关。
//
// -----------------------------------------------------------------------------
// 适配层覆盖方式（为什么是派生建模 + 内核差分）
// -----------------------------------------------------------------------------
//   两个生产 TryBuildRuntimeGroupPalette 分别在
//     src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp
//     src/d3d9/war3/render/war3_upper_layer_shadow.cpp
//   它们依赖 d3d9 记录类型（ShadowModelResourceRecord / model::ShadowGeosetResourceRecord）
//   与 TU 内部状态（ShadowPoseStore、来源链、Game.dll arena 读、语义日志面），
//   无法在宿主机链接，因此本文件用：
//     (a) DeriveCoreAdapterInputs / DeriveUpperAdapterInputs 显式建模两侧适配层的
//         输入派生（core：RuntimeVertexGroupSlotCount 的 min 链 + 128Ki clamp、
//         groupCount = matrixGroupSizes.size()；upper：min(vertexGroupCount, size)、
//         min(matrixGroupCount, size)）与三条前置校验（hasSkinningData / 空 pose /
//         空 vertexGroups ⇒ 适配层早退，内核不参与）；
//     (b) 把 (a) 派生出的只读视图交给共享内核，与逐语句抽取的旧实现逐输入比较。
//   适配层文本一致性（内核为唯一来源、来源链未被搬走、两侧 fallback 集合未合并）
//   由静态门禁 AutoTest/test_runtime_group_palette_kernel_single_source_static.py 负责。
//
// -----------------------------------------------------------------------------
// 未做到 / 不确定（不得据此宣称等价）
// -----------------------------------------------------------------------------
//   * 只覆盖来源链之后的纯计算段；来源链本身（producer 快照 / 槽位缓存 /
//     Game.dll arena）在本 TU 不可复现；
//   * 适配层派生建模是对源码的复刻，其正确性由静态门禁 + 与旧实现副本
//     RuntimeVertexGroupSlotCount 的逐输入相等断言共同约束，不等于链接了生产函数；
//   * 分配计数只统计本 TU 重载的全局 operator new/delete，不含 CRT / Vulkan / 驱动侧；
//   * 时间比例测量只用于佐证内核扫描趟数，噪声未建模，不作为等价结论。
// =============================================================================

#include "../war3_runtime_group_palette_kernel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------------
// A. 分配计数：只在本测试 TU 内重载全局 operator new / operator delete。
//    产品代码零改动；重载只对本可执行文件生效（目标独立，不链接 d3d9 源）。
// -----------------------------------------------------------------------------
namespace s2alloc {

constexpr uint64_t kMagic = 0x5332414C4C4F4331ull;  // "S2ALLOC1"
constexpr std::size_t kMinAlign = 16u;               // = __STDCPP_DEFAULT_NEW_ALIGNMENT__

// 头部保存原始指针与实际请求大小；返回给调用方的指针手工对齐到 max(16, align)。
struct Header {
  uint64_t magic = 0u;
  uint64_t size = 0u;
  uint64_t raw = 0u;
  uint64_t align = 0u;
};

uint64_t    g_totalAllocs = 0u;
uint64_t    g_totalFrees = 0u;
uint64_t    g_totalBytes = 0u;
std::size_t g_liveBytes = 0u;
std::size_t g_windowPeakLive = 0u;
std::size_t g_windowMaxSingle = 0u;
bool        g_tracking = false;

inline void* allocate(std::size_t n, std::size_t align) {
  if (align < kMinAlign)
    align = kMinAlign;
  const std::size_t total = n + align + sizeof(Header);
  if (total < n)
    throw std::bad_alloc();
  void* raw = std::malloc(total);
  if (raw == nullptr)
    throw std::bad_alloc();

  const uintptr_t base = reinterpret_cast<uintptr_t>(raw) + sizeof(Header);
  const uintptr_t aligned = (base + (align - 1u)) & ~uintptr_t(align - 1u);
  Header* header = reinterpret_cast<Header*>(aligned) - 1;
  header->magic = kMagic;
  header->size = uint64_t(n);
  header->raw = uint64_t(reinterpret_cast<uintptr_t>(raw));
  header->align = uint64_t(align);

  ++g_totalAllocs;
  g_totalBytes += uint64_t(n);
  g_liveBytes += n;
  if (g_tracking) {
    if (g_liveBytes > g_windowPeakLive)
      g_windowPeakLive = g_liveBytes;
    if (n > g_windowMaxSingle)
      g_windowMaxSingle = n;
  }
  return reinterpret_cast<void*>(aligned);
}

inline void release(void* p) noexcept {
  if (p == nullptr)
    return;
  Header* header = reinterpret_cast<Header*>(p) - 1;
  if (header->magic != kMagic) {
    std::fprintf(stderr,
                 "war3_runtime_group_palette_kernel_diff_test: FATAL bad free "
                 "(pointer not produced by this test allocator)\n");
    std::abort();
  }
  const std::size_t n = std::size_t(header->size);
  void* raw = reinterpret_cast<void*>(uintptr_t(header->raw));
  header->magic = 0u;
  ++g_totalFrees;
  g_liveBytes = (g_liveBytes >= n) ? (g_liveBytes - n) : 0u;
  std::free(raw);
}

}  // namespace s2alloc

void* operator new(std::size_t n) { return s2alloc::allocate(n, s2alloc::kMinAlign); }
void* operator new[](std::size_t n) { return s2alloc::allocate(n, s2alloc::kMinAlign); }
void* operator new(std::size_t n, std::align_val_t a) {
  return s2alloc::allocate(n, std::size_t(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return s2alloc::allocate(n, std::size_t(a));
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return s2alloc::allocate(n, s2alloc::kMinAlign);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return s2alloc::allocate(n, s2alloc::kMinAlign);
  } catch (...) {
    return nullptr;
  }
}
void* operator new(std::size_t n, std::align_val_t a,
                   const std::nothrow_t&) noexcept {
  try {
    return s2alloc::allocate(n, std::size_t(a));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, std::align_val_t a,
                     const std::nothrow_t&) noexcept {
  try {
    return s2alloc::allocate(n, std::size_t(a));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* p) noexcept { s2alloc::release(p); }
void operator delete[](void* p) noexcept { s2alloc::release(p); }
void operator delete(void* p, std::size_t) noexcept { s2alloc::release(p); }
void operator delete[](void* p, std::size_t) noexcept { s2alloc::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { s2alloc::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { s2alloc::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  s2alloc::release(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  s2alloc::release(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept { s2alloc::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { s2alloc::release(p); }
void operator delete(void* p, const std::nothrow_t&, std::align_val_t) noexcept {
  s2alloc::release(p);
}
void operator delete[](void* p, const std::nothrow_t&, std::align_val_t) noexcept {
  s2alloc::release(p);
}

// -----------------------------------------------------------------------------
// B. 旧实现副本（pre-S2 逐语句抽取），独立 legacy 命名空间
// -----------------------------------------------------------------------------
namespace legacy {

using dxvk::Matrix4;

// pre-S2 core 的 RuntimeGroupPaletteMissReason（war3_shadow_renderer_core.cpp:2751-2760）
// 与 RuntimeGroupPaletteMissDetail（2762-2770）/ NoteRuntimeGroupPaletteMiss（2772-2789）。
enum class MissReason : uint32_t {
  None = 0,
  NoSkinningData,
  NoPosePalette,
  NoVertexGroups,
  InvalidGroupTable,
  MatrixIndexOutOfRange,
  VertexGroupOutOfRange,
  FallbacksFailed,
};

struct MissDetail {
  MissReason reason = MissReason::None;
  uint32_t group = UINT32_MAX;
  uint32_t matrixIndex = UINT32_MAX;
  uint32_t poseCount = 0u;
  uint32_t groupCount = 0u;
  uint32_t maxVertexGroupSlot = 0u;
  uint32_t matrixIndexCount = 0u;
};

void NoteMiss(MissDetail* outDetail, MissReason reason, uint32_t poseCount = 0u,
              uint32_t groupCount = 0u, uint32_t maxVertexGroupSlot = 0u,
              uint32_t matrixIndexCount = 0u, uint32_t group = UINT32_MAX,
              uint32_t matrixIndex = UINT32_MAX) {
  if (outDetail == nullptr)
    return;
  outDetail->reason = reason;
  outDetail->group = group;
  outDetail->matrixIndex = matrixIndex;
  outDetail->poseCount = poseCount;
  outDetail->groupCount = groupCount;
  outDetail->maxVertexGroupSlot = maxVertexGroupSlot;
  outDetail->matrixIndexCount = matrixIndexCount;
}

// 4 步副本的出口插桩只需要 reason（upper production 侧不产出 miss detail），
// 计算语句不变，只是把旧的失败出口记下来供差分比较。
void NoteMiss(MissReason* outLastExit, MissReason reason) {
  if (outLastExit != nullptr)
    *outLastExit = reason;
}

// ---- 记录替身：字段名与生产记录一致，使副本的计算语句无需改写 ----
// ShadowModelResourceRecord（war3_shadow_runtime_contract.h:78-108）中本函数用到的字段。
struct CoreResourceRecord {
  std::vector<float> positions;
  std::vector<uint8_t> vertexGroupIndices;
  uint32_t vertexCount = 0u;
  std::vector<uint32_t> matrixGroupSizes;
  std::vector<uint32_t> matrixIndices;

  // 与 war3_shadow_runtime_contract.h:104-107 逐语句相同
  bool hasSkinningData() const {
    return !vertexGroupIndices.empty() &&
           (!matrixGroupSizes.empty() || !matrixIndices.empty());
  }
};

// ShadowPoseRecord（war3_shadow_runtime_contract.h:178-188）中本函数用到的字段。
struct CorePoseRecord {
  uint32_t matrixCount = 0u;
  std::vector<Matrix4> matrixPalette;
};

// model::ShadowGeosetResourceRecord（war3_model_resource_cache.h:54/73）中本函数用到的字段。
struct UpperResourceRecord {
  uint32_t vertexGroupCount = 0u;
  std::vector<uint8_t> vertexGroupIndices;
  uint32_t matrixGroupCount = 0u;
  std::vector<uint32_t> matrixGroupSizes;
  std::vector<uint32_t> matrixIndices;

  // 与 war3_model_resource_cache.h:111-114 逐语句相同
  bool hasSkinningData() const {
    return !vertexGroupIndices.empty() &&
           (!matrixGroupSizes.empty() || !matrixIndices.empty());
  }
};

// model::PoseRecord（war3_model_registry.h:162+）中本函数用到的字段。
struct UpperPoseRecord {
  uint32_t matrixCount = 0u;
  std::vector<Matrix4> matrixPalette;
};

// RuntimeVertexGroupSlotCount：war3_shadow_renderer_core.cpp:6486-6493（pre-S2 与当前
// 逐语句相同，S2 未改动该函数）的逐语句副本。
std::size_t RuntimeVertexGroupSlotCount(const CoreResourceRecord& resource) {
  std::size_t count = resource.vertexGroupIndices.size();
  const uint32_t vertexCount =
      resource.vertexCount != 0u
          ? resource.vertexCount
          : uint32_t(resource.positions.size() / 3u);
  if (vertexCount != 0u)
    count = (std::min)(count, std::size_t(vertexCount));

  // A Warcraft III geoset should be far below this; this guard prevents a bad
  // upstream sideband slice from turning semantic preview into an unbounded scan.
  constexpr std::size_t kMaxSemanticVertexGroupSlots = 128u * 1024u;
  return (std::min)(count, kMaxSemanticVertexGroupSlots);
}

// logFailure（pre-S2 core.cpp:6745-7008）只做诊断采样与打印，实测不写 outPalette /
// outMissDetail（已逐行核对 6745-7008 无 outPalette / outMissDetail 写入）。因此副本里
// 用同构空实现替代，且不改变任何计算语句与短路顺序。
void logFailureStub() {}

// =============================================================================
// 5 步副本：pre-S2 war3_shadow_renderer_core.cpp:6577-7166 的逐语句抽取。
// 唯一删除的是 6620-6723 的 tryEngineDirectPosePalette（来源链）与其调用，
// 以及来源链命中路径上的第一次 Note None。其余行结构、常量、短路顺序与 pre-S2 一致。
// =============================================================================
bool TryBuildRuntimeGroupPaletteFiveStep(
    const CoreResourceRecord& resource, const CorePoseRecord& pose,
    std::vector<Matrix4>& outPalette, uint32_t& outMaxVertexGroupSlot,
    bool& outUsesAveraging, MissDetail* outMissDetail = nullptr) {
  outPalette.clear();
  outMaxVertexGroupSlot = 0u;
  outUsesAveraging = false;
  if (outMissDetail != nullptr)
    *outMissDetail = {};

  if (!resource.hasSkinningData())
    return (NoteMiss(outMissDetail, MissReason::NoSkinningData, pose.matrixCount,
                     uint32_t(resource.matrixGroupSizes.size()), 0u,
                     uint32_t(resource.matrixIndices.size())),
            false);
  if (pose.matrixPalette.empty() || pose.matrixCount == 0u)
    return (NoteMiss(outMissDetail, MissReason::NoPosePalette, pose.matrixCount,
                     uint32_t(resource.matrixGroupSizes.size()), 0u,
                     uint32_t(resource.matrixIndices.size())),
            false);
  const std::size_t vertexGroupSlotCount = RuntimeVertexGroupSlotCount(resource);
  if (vertexGroupSlotCount == 0u)
    return (NoteMiss(outMissDetail, MissReason::NoVertexGroups, pose.matrixCount,
                     uint32_t(resource.matrixGroupSizes.size()), 0u,
                     uint32_t(resource.matrixIndices.size())),
            false);

  for (std::size_t i = 0u; i < vertexGroupSlotCount; ++i) {
    const uint8_t groupSlot = resource.vertexGroupIndices[i];
    outMaxVertexGroupSlot =
        (std::max)(outMaxVertexGroupSlot, uint32_t(groupSlot));
  }

  // [来源链 tryEngineDirectPosePalette 在此处，本 TU 不可复现，已删除]
  NoteMiss(outMissDetail, MissReason::None, pose.matrixCount,
           uint32_t(resource.matrixGroupSizes.size()), outMaxVertexGroupSlot,
           uint32_t(resource.matrixIndices.size()));

  if (!resource.hasSkinningData())
    return false;

  std::vector<uint32_t> uniqueGroupSlots;
  uniqueGroupSlots.reserve(outMaxVertexGroupSlot + 1u);
  std::array<bool, 256> seenGroupSlots = {};
  for (std::size_t i = 0u; i < vertexGroupSlotCount; ++i) {
    const uint8_t groupSlot = resource.vertexGroupIndices[i];
    if (!seenGroupSlots[groupSlot]) {
      seenGroupSlots[groupSlot] = true;
      uniqueGroupSlots.push_back(uint32_t(groupSlot));
    }
  }

  auto logFailure = [&]() { logFailureStub(); };

  auto buildDirectMatrixRemap = [&]() -> bool {
    if (resource.matrixIndices.empty() ||
        outMaxVertexGroupSlot >= resource.matrixIndices.size()) {
      return false;
    }

    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group) {
      const uint32_t matrixIndex = resource.matrixIndices[group];
      if (matrixIndex >= pose.matrixCount ||
          matrixIndex >= pose.matrixPalette.size()) {
        return false;
      }
      outPalette[group] = pose.matrixPalette[matrixIndex];
    }
    return true;
  };

  auto buildSparseMatrixRemap = [&]() -> bool {
    if (resource.matrixIndices.empty() || uniqueGroupSlots.empty() ||
        uniqueGroupSlots.size() > resource.matrixIndices.size()) {
      return false;
    }

    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (std::size_t i = 0; i < uniqueGroupSlots.size(); ++i) {
      const uint32_t matrixIndex = resource.matrixIndices[i];
      if (matrixIndex >= pose.matrixCount ||
          matrixIndex >= pose.matrixPalette.size()) {
        return false;
      }
      outPalette[uniqueGroupSlots[i]] = pose.matrixPalette[matrixIndex];
    }
    return true;
  };

  auto buildDirectPosePalette = [&]() -> bool {
    if (outMaxVertexGroupSlot >= pose.matrixCount ||
        outMaxVertexGroupSlot >= pose.matrixPalette.size()) {
      return false;
    }

    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group)
      outPalette[group] = pose.matrixPalette[group];
    return true;
  };

  auto buildSparsePosePalette = [&]() -> bool {
    if (uniqueGroupSlots.empty() || uniqueGroupSlots.size() > pose.matrixCount ||
        uniqueGroupSlots.size() > pose.matrixPalette.size()) {
      return false;
    }

    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (std::size_t i = 0; i < uniqueGroupSlots.size(); ++i)
      outPalette[uniqueGroupSlots[i]] = pose.matrixPalette[i];
    return true;
  };

  auto buildUniformPosePalette = [&]() -> bool {
    if (pose.matrixCount == 0u || pose.matrixPalette.empty())
      return false;

    const uint32_t paletteCount =
        (std::max)(outMaxVertexGroupSlot + 1u,
                   uint32_t(resource.matrixGroupSizes.size()));
    if (paletteCount == 0u)
      return false;

    // Some runtime models publish only the final root matrix even though the
    // static geoset still carries vertex-group metadata. Broadcasting that root
    // matrix gives us a semantic rigid-palette packet instead of dropping back
    // to the old draw-time capture path.
    outPalette.assign(paletteCount, pose.matrixPalette.front());
    return true;
  };

  auto tryFallbacks = [&]() -> bool {
    const bool ok = buildDirectMatrixRemap() || buildSparseMatrixRemap() ||
                    buildDirectPosePalette() || buildSparsePosePalette() ||
                    buildUniformPosePalette();
    if (!ok) {
      NoteMiss(outMissDetail, MissReason::FallbacksFailed, pose.matrixCount,
               uint32_t(resource.matrixGroupSizes.size()), outMaxVertexGroupSlot,
               uint32_t(resource.matrixIndices.size()));
      logFailure();
    }
    return ok;
  };

  const uint32_t groupCount = uint32_t(resource.matrixGroupSizes.size());
  if (groupCount == 0u) {
    return tryFallbacks();
  }

  std::vector<uint32_t> prefix(groupCount, 0u);
  uint32_t running = 0u;
  for (uint32_t i = 0u; i < groupCount; ++i) {
    prefix[i] = running;
    running += resource.matrixGroupSizes[i];
  }
  if (running > resource.matrixIndices.size()) {
    NoteMiss(outMissDetail, MissReason::InvalidGroupTable, pose.matrixCount,
             groupCount, outMaxVertexGroupSlot,
             uint32_t(resource.matrixIndices.size()));
    return tryFallbacks();
  }

  outPalette.resize(groupCount);
  for (uint32_t group = 0u; group < groupCount; ++group) {
    const uint32_t groupSize = resource.matrixGroupSizes[group];
    const uint32_t groupBase = prefix[group];
    if (groupSize == 0u ||
        (groupBase + groupSize) > resource.matrixIndices.size()) {
      NoteMiss(outMissDetail, MissReason::InvalidGroupTable, pose.matrixCount,
               groupCount, outMaxVertexGroupSlot,
               uint32_t(resource.matrixIndices.size()), group);
      return tryFallbacks();
    }

    Matrix4 accum(0.0f);
    for (uint32_t i = 0u; i < groupSize; ++i) {
      const uint32_t matrixIndex = resource.matrixIndices[groupBase + i];
      if (matrixIndex >= pose.matrixCount ||
          matrixIndex >= pose.matrixPalette.size()) {
        NoteMiss(outMissDetail, MissReason::MatrixIndexOutOfRange,
                 pose.matrixCount, groupCount, outMaxVertexGroupSlot,
                 uint32_t(resource.matrixIndices.size()), group, matrixIndex);
        return tryFallbacks();
      }
      accum += pose.matrixPalette[matrixIndex];
    }

    if (groupSize > 1u)
      outUsesAveraging = true;
    outPalette[group] =
        groupSize == 1u ? accum : (accum / float(groupSize));
  }

  for (std::size_t i = 0u; i < vertexGroupSlotCount; ++i) {
    const uint8_t groupSlot = resource.vertexGroupIndices[i];
    if (uint32_t(groupSlot) >= groupCount) {
      NoteMiss(outMissDetail, MissReason::VertexGroupOutOfRange,
               pose.matrixCount, groupCount, outMaxVertexGroupSlot,
               uint32_t(resource.matrixIndices.size()), uint32_t(groupSlot));
      return tryFallbacks();
    }
  }

  return true;
}

// =============================================================================
// 4 步副本：pre-S2 war3_upper_layer_shadow.cpp:98-242 的逐语句抽取。
// 计算语句逐条未变；为取得旧实现的失败出口（production 侧不产出 reason），
// 按 pre-S2 core 自己的 "return (Note...(...), false);" 惯用法在既有 return 点插桩，
// 并把四处 "return a || b || c || d;" 包成同序的 tryFallbacks() lambda
// （|| 链文本原样保留），以便 chain 失败时按 core 的语义记 FallbacksFailed。
// =============================================================================
bool TryBuildRuntimeGroupPaletteFourStep(
    const UpperResourceRecord& geoset, const UpperPoseRecord& pose,
    std::vector<Matrix4>& outPalette, uint32_t& outMaxVertexGroupSlot,
    bool& outUsesAveraging, MissReason* outLastExit = nullptr) {
  outPalette.clear();
  outMaxVertexGroupSlot = 0u;
  outUsesAveraging = false;
  if (outLastExit != nullptr)
    *outLastExit = MissReason::None;

  if (!geoset.hasSkinningData())
    return (NoteMiss(outLastExit, MissReason::NoSkinningData), false);
  if (pose.matrixPalette.empty() || pose.matrixCount == 0)
    return (NoteMiss(outLastExit, MissReason::NoPosePalette), false);
  const uint32_t vertexGroupCount = std::min<uint32_t>(
      geoset.vertexGroupCount, uint32_t(geoset.vertexGroupIndices.size()));
  if (vertexGroupCount == 0u)
    return (NoteMiss(outLastExit, MissReason::NoVertexGroups), false);

  for (uint32_t i = 0; i < vertexGroupCount; ++i)
    outMaxVertexGroupSlot =
        std::max(outMaxVertexGroupSlot, uint32_t(geoset.vertexGroupIndices[i]));

  std::vector<uint32_t> uniqueGroupSlots;
  uniqueGroupSlots.reserve(outMaxVertexGroupSlot + 1u);
  std::array<bool, 256> seenGroupSlots = {};
  for (uint32_t i = 0; i < vertexGroupCount; ++i) {
    const uint8_t groupSlot = geoset.vertexGroupIndices[i];
    if (!seenGroupSlots[groupSlot]) {
      seenGroupSlots[groupSlot] = true;
      uniqueGroupSlots.push_back(uint32_t(groupSlot));
    }
  }

  auto buildDirectMatrixRemap = [&]() -> bool {
    if (geoset.matrixIndices.empty() ||
        outMaxVertexGroupSlot >= geoset.matrixIndices.size()) {
      return false;
    }

    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group) {
      const uint32_t matrixIndex = geoset.matrixIndices[group];
      if (matrixIndex >= pose.matrixCount ||
          matrixIndex >= pose.matrixPalette.size()) {
        return false;
      }
      outPalette[group] = pose.matrixPalette[matrixIndex];
    }
    return true;
  };

  auto buildSparseMatrixRemap = [&]() -> bool {
    if (geoset.matrixIndices.empty() || uniqueGroupSlots.empty() ||
        uniqueGroupSlots.size() > geoset.matrixIndices.size()) {
      return false;
    }

    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (std::size_t i = 0; i < uniqueGroupSlots.size(); ++i) {
      const uint32_t matrixIndex = geoset.matrixIndices[i];
      if (matrixIndex >= pose.matrixCount ||
          matrixIndex >= pose.matrixPalette.size()) {
        return false;
      }
      outPalette[uniqueGroupSlots[i]] = pose.matrixPalette[matrixIndex];
    }
    return true;
  };

  auto buildDirectPosePalette = [&]() -> bool {
    if (outMaxVertexGroupSlot >= pose.matrixCount ||
        outMaxVertexGroupSlot >= pose.matrixPalette.size()) {
      return false;
    }

    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group)
      outPalette[group] = pose.matrixPalette[group];
    return true;
  };

  auto buildSparsePosePalette = [&]() -> bool {
    if (uniqueGroupSlots.empty() || uniqueGroupSlots.size() > pose.matrixCount ||
        uniqueGroupSlots.size() > pose.matrixPalette.size()) {
      return false;
    }

    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (std::size_t i = 0; i < uniqueGroupSlots.size(); ++i)
      outPalette[uniqueGroupSlots[i]] = pose.matrixPalette[i];
    return true;
  };

  // 机械抽出的 4 步 chain（表达式与 pre-S2 四处 return 完全相同）。
  auto tryFallbacks = [&]() -> bool {
    const bool ok = buildDirectMatrixRemap() || buildSparseMatrixRemap() ||
                    buildDirectPosePalette() || buildSparsePosePalette();
    if (!ok)
      NoteMiss(outLastExit, MissReason::FallbacksFailed);
    return ok;
  };

  const uint32_t groupCount = std::min<uint32_t>(
      geoset.matrixGroupCount, uint32_t(geoset.matrixGroupSizes.size()));
  if (groupCount == 0u)
    return tryFallbacks();

  std::vector<uint32_t> prefix(groupCount, 0u);
  uint32_t running = 0u;
  for (uint32_t i = 0; i < groupCount; ++i) {
    prefix[i] = running;
    running += geoset.matrixGroupSizes[i];
  }

  if (running > geoset.matrixIndices.size()) {
    NoteMiss(outLastExit, MissReason::InvalidGroupTable);
    return tryFallbacks();
  }

  outPalette.resize(groupCount);
  for (uint32_t group = 0; group < groupCount; ++group) {
    const uint32_t groupSize = geoset.matrixGroupSizes[group];
    const uint32_t groupBase = prefix[group];
    if (groupSize == 0u || (groupBase + groupSize) > geoset.matrixIndices.size()) {
      NoteMiss(outLastExit, MissReason::InvalidGroupTable);
      return tryFallbacks();
    }

    Matrix4 accum(0.0f);
    for (uint32_t i = 0; i < groupSize; ++i) {
      const uint32_t matrixIndex = geoset.matrixIndices[groupBase + i];
      if (matrixIndex >= pose.matrixCount ||
          matrixIndex >= pose.matrixPalette.size()) {
        NoteMiss(outLastExit, MissReason::MatrixIndexOutOfRange);
        return tryFallbacks();
      }

      accum += pose.matrixPalette[matrixIndex];
    }

    if (groupSize > 1u)
      outUsesAveraging = true;
    outPalette[group] =
        groupSize == 1u ? accum : (accum / float(groupSize));
  }

  for (uint32_t i = 0; i < vertexGroupCount; ++i) {
    const uint32_t groupSlot = geoset.vertexGroupIndices[i];
    if (groupSlot >= groupCount) {
      NoteMiss(outLastExit, MissReason::VertexGroupOutOfRange);
      return tryFallbacks();
    }
  }

  return true;
}

}  // namespace legacy

// 内核 Miss 枚举与 pre-S2 core 的 RuntimeGroupPaletteMissReason 逐值相同。
// 下面 8 条 static_assert 把这个映射钉成编译期事实；core.cpp 的同名枚举在本 TU
// 不可链接，其声明顺序由静态门禁对源码文本校验。
static_assert(uint32_t(dxvk::war3::render::RuntimeGroupPaletteKernelMiss::None) ==
                  uint32_t(legacy::MissReason::None),
              "miss enum mismatch: None");
static_assert(
    uint32_t(dxvk::war3::render::RuntimeGroupPaletteKernelMiss::NoSkinningData) ==
        uint32_t(legacy::MissReason::NoSkinningData),
    "miss enum mismatch: NoSkinningData");
static_assert(
    uint32_t(dxvk::war3::render::RuntimeGroupPaletteKernelMiss::NoPosePalette) ==
        uint32_t(legacy::MissReason::NoPosePalette),
    "miss enum mismatch: NoPosePalette");
static_assert(
    uint32_t(dxvk::war3::render::RuntimeGroupPaletteKernelMiss::NoVertexGroups) ==
        uint32_t(legacy::MissReason::NoVertexGroups),
    "miss enum mismatch: NoVertexGroups");
static_assert(
    uint32_t(
        dxvk::war3::render::RuntimeGroupPaletteKernelMiss::InvalidGroupTable) ==
        uint32_t(legacy::MissReason::InvalidGroupTable),
    "miss enum mismatch: InvalidGroupTable");
static_assert(
    uint32_t(dxvk::war3::render::RuntimeGroupPaletteKernelMiss::
                 MatrixIndexOutOfRange) ==
        uint32_t(legacy::MissReason::MatrixIndexOutOfRange),
    "miss enum mismatch: MatrixIndexOutOfRange");
static_assert(
    uint32_t(dxvk::war3::render::RuntimeGroupPaletteKernelMiss::
                 VertexGroupOutOfRange) ==
        uint32_t(legacy::MissReason::VertexGroupOutOfRange),
    "miss enum mismatch: VertexGroupOutOfRange");
static_assert(
    uint32_t(dxvk::war3::render::RuntimeGroupPaletteKernelMiss::FallbacksFailed) ==
        uint32_t(legacy::MissReason::FallbacksFailed),
    "miss enum mismatch: FallbacksFailed");

// =============================================================================
// C. 测试骨架
// =============================================================================
namespace {

using dxvk::Matrix4;
using dxvk::war3::render::RuntimeGroupPaletteFallbackSet;
using dxvk::war3::render::RuntimeGroupPaletteInput;
using dxvk::war3::render::RuntimeGroupPaletteKernelDetail;
using dxvk::war3::render::RuntimeGroupPaletteKernelMiss;
using dxvk::war3::render::RuntimeGroupPaletteOutput;
using dxvk::war3::render::FindRuntimeGroupPaletteMaxSlot;
using dxvk::war3::render::RuntimeGroupPaletteSlotScan;
using dxvk::war3::render::ScanRuntimeGroupPaletteSlots;
using dxvk::war3::render::TryBuildRuntimeGroupPaletteKernel;

constexpr uint64_t kSeedCoreGroup = 0x5332464600000001ull;
constexpr uint64_t kSeedUpperGroup = 0x5332464600000002ull;
constexpr uint64_t kGroupIterations = 300000ull;  // 每组 >= 300,000
constexpr uint64_t kMismatchCaseLimit = 8ull;     // 每组最多打印的反例数

constexpr const char* kLegacyCorePath =
    "C:\\Windows\\Temp\\warvk_s2_backup\\war3_shadow_renderer_core.cpp";
constexpr const char* kLegacyUpperPath =
    "C:\\Windows\\Temp\\warvk_s2_backup\\war3_upper_layer_shadow.cpp";
constexpr const char* kLegacyCoreSha256 =
    "0F7B720E1FAF5B7065CE40DFCE4D7462A5AC14D3217DDA6716A395228030D8F9";
constexpr const char* kLegacyUpperSha256 =
    "61EB0C8FFDF24F36016B1651A7C677BE0C47E805680C5278F0CE18BD950517A9";

uint32_t g_failures = 0u;
volatile uint64_t g_sink = 0u;

bool require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "war3_runtime_group_palette_kernel_diff_test: FAIL " << message
              << '\n';
    ++g_failures;
  }
  return condition;
}

// ---- 固定种子 PRNG（SplitMix64）----
struct SplitMix64 {
  uint64_t state = 0u;

  explicit SplitMix64(uint64_t seed) : state(seed) {}

  uint64_t next() {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  // bound 必须非 0
  uint32_t nextU32(uint32_t bound) { return uint32_t(next() % uint64_t(bound)); }
};

std::string Hex64(uint64_t value) {
  static const char* kDigits = "0123456789ABCDEF";
  std::string text = "0x";
  bool started = false;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const uint32_t nibble = uint32_t((value >> shift) & 0xFull);
    if (!started && nibble == 0u && shift != 0)
      continue;
    started = true;
    text.push_back(kDigits[nibble]);
  }
  return text;
}

// 不含 NaN/Inf 的矩阵；两侧同源，逐位比较有意义。
Matrix4 MakePoseMatrix(uint64_t iteration, uint32_t index) {
  Matrix4 m(0.0f);
  const float base = float(index) + float(iteration % 251ull) * 0.125f + 1.0f;
  for (uint32_t row = 0u; row < 4u; ++row) {
    for (uint32_t column = 0u; column < 4u; ++column)
      m[row][column] = base + float(row) * 0.25f + float(column) * 0.0625f;
  }
  return m;
}

// 逐位（bitwise）比较：比对 16 个 float lane 的位模式，不依赖 padding。
bool MatrixBitEqual(const Matrix4& a, const Matrix4& b, uint32_t* firstLane) {
  for (uint32_t row = 0u; row < 4u; ++row) {
    for (uint32_t column = 0u; column < 4u; ++column) {
      uint32_t lhs = 0u;
      uint32_t rhs = 0u;
      const float av = a[row][column];
      const float bv = b[row][column];
      std::memcpy(&lhs, &av, sizeof(lhs));
      std::memcpy(&rhs, &bv, sizeof(rhs));
      if (lhs != rhs) {
        if (firstLane != nullptr)
          *firstLane = row * 4u + column;
        return false;
      }
    }
  }
  return true;
}

// ---- 结果对比 ----
struct Outcome {
  bool ok = false;
  uint32_t maxSlot = 0u;
  bool usesAveraging = false;
  std::vector<Matrix4> palette;
  legacy::MissReason reason = legacy::MissReason::None;
  // 仅 core 组比较：miss detail 的其余字段（适配层经内核映射后逐字段一致）
  uint32_t detailGroup = UINT32_MAX;
  uint32_t detailMatrixIndex = UINT32_MAX;
  uint32_t detailPoseCount = 0u;
  uint32_t detailGroupCount = 0u;
  uint32_t detailMaxSlot = 0u;
  uint32_t detailMatrixIndexCount = 0u;
};

void OutcomeFromKernel(const RuntimeGroupPaletteOutput& out,
                       const RuntimeGroupPaletteKernelDetail& detail,
                       Outcome& target) {
  target.maxSlot = out.maxVertexGroupSlot;
  target.usesAveraging = out.usesAveraging;
  target.palette = out.palette;
  target.reason = legacy::MissReason(uint32_t(detail.reason));
  target.detailGroup = detail.group;
  target.detailMatrixIndex = detail.matrixIndex;
  target.detailPoseCount = detail.poseCount;
  target.detailGroupCount = detail.groupCount;
  target.detailMaxSlot = detail.maxVertexGroupSlot;
  target.detailMatrixIndexCount = detail.matrixIndexCount;
}

bool OutcomeDiffers(const Outcome& legacyOut, const Outcome& kernelOut,
                    bool compareDetail, std::string& why) {
  if (legacyOut.ok != kernelOut.ok) {
    why = "ok";
    return true;
  }
  if (legacyOut.usesAveraging != kernelOut.usesAveraging) {
    why = "usesAveraging";
    return true;
  }
  if (legacyOut.maxSlot != kernelOut.maxSlot) {
    why = "maxVertexGroupSlot";
    return true;
  }
  if (legacyOut.palette.size() != kernelOut.palette.size()) {
    why = "palette.size()";
    return true;
  }
  for (std::size_t i = 0u; i < legacyOut.palette.size(); ++i) {
    uint32_t lane = 0u;
    if (!MatrixBitEqual(legacyOut.palette[i], kernelOut.palette[i], &lane)) {
      why = "palette[" + std::to_string(i) + "].lane" + std::to_string(lane) +
            " bitwise";
      return true;
    }
  }
  if (legacyOut.reason != kernelOut.reason) {
    why = "miss reason (legacy=" + std::to_string(uint32_t(legacyOut.reason)) +
          " kernel=" + std::to_string(uint32_t(kernelOut.reason)) + ")";
    return true;
  }
  if (compareDetail) {
    if (legacyOut.detailGroup != kernelOut.detailGroup) {
      why = "detail.group";
      return true;
    }
    if (legacyOut.detailMatrixIndex != kernelOut.detailMatrixIndex) {
      why = "detail.matrixIndex";
      return true;
    }
    if (legacyOut.detailPoseCount != kernelOut.detailPoseCount) {
      why = "detail.poseCount";
      return true;
    }
    if (legacyOut.detailGroupCount != kernelOut.detailGroupCount) {
      why = "detail.groupCount";
      return true;
    }
    if (legacyOut.detailMaxSlot != kernelOut.detailMaxSlot) {
      why = "detail.maxVertexGroupSlot";
      return true;
    }
    if (legacyOut.detailMatrixIndexCount != kernelOut.detailMatrixIndexCount) {
      why = "detail.matrixIndexCount";
      return true;
    }
  }
  return false;
}

struct GroupStats {
  uint64_t inputs = 0u;
  uint64_t mismatches = 0u;
  uint64_t printedMismatches = 0u;
  uint64_t adapterEarlyExit = 0u;
  uint64_t earlyNoSkinningData = 0u;
  uint64_t earlyNoPosePalette = 0u;
  uint64_t earlyNoVertexGroups = 0u;
  uint64_t kernelInvoked = 0u;
  uint64_t kernelOk = 0u;
  uint64_t kernelOkAfterMainPathFailure = 0u;
  uint64_t kernelInvalidGroupTable = 0u;
  uint64_t kernelMatrixIndexOutOfRange = 0u;
  uint64_t kernelVertexGroupOutOfRange = 0u;
  uint64_t kernelFallbacksFailed = 0u;
  uint64_t usesAveragingTrue = 0u;
  uint64_t maxSlotIs255 = 0u;
  uint64_t boundaryCases = 0u;
};

std::string ReasonName(legacy::MissReason reason) {
  switch (reason) {
    case legacy::MissReason::None:
      return "None";
    case legacy::MissReason::NoSkinningData:
      return "NoSkinningData";
    case legacy::MissReason::NoPosePalette:
      return "NoPosePalette";
    case legacy::MissReason::NoVertexGroups:
      return "NoVertexGroups";
    case legacy::MissReason::InvalidGroupTable:
      return "InvalidGroupTable";
    case legacy::MissReason::MatrixIndexOutOfRange:
      return "MatrixIndexOutOfRange";
    case legacy::MissReason::VertexGroupOutOfRange:
      return "VertexGroupOutOfRange";
    case legacy::MissReason::FallbacksFailed:
      return "FallbacksFailed";
  }
  return "?";
}

bool IsAdapterOnlyReason(RuntimeGroupPaletteKernelMiss reason) {
  return reason == RuntimeGroupPaletteKernelMiss::NoSkinningData ||
         reason == RuntimeGroupPaletteKernelMiss::NoPosePalette ||
         reason == RuntimeGroupPaletteKernelMiss::NoVertexGroups;
}

// =============================================================================
// D. 适配层输入派生建模
// =============================================================================
// core 适配层（war3_shadow_renderer_core.cpp:6623-6647）：
//   !hasSkinningData()                                  -> NoSkinningData 早退
//   pose.matrixPalette.empty() || pose.matrixCount == 0 -> NoPosePalette 早退
//   RuntimeVertexGroupSlotCount(...) == 0               -> NoVertexGroups 早退
//   随后 groupCount = matrixGroupSizes.size()（D5：不做 min 收窄）
bool DeriveCoreAdapterInputs(const legacy::CoreResourceRecord& resource,
                             const legacy::CorePoseRecord& pose,
                             RuntimeGroupPaletteInput& out,
                             legacy::MissDetail& outPreDetail) {
  out = {};
  outPreDetail = {};
  outPreDetail.poseCount = pose.matrixCount;
  outPreDetail.groupCount = uint32_t(resource.matrixGroupSizes.size());
  outPreDetail.matrixIndexCount = uint32_t(resource.matrixIndices.size());

  if (!resource.hasSkinningData()) {
    outPreDetail.reason = legacy::MissReason::NoSkinningData;
    return false;
  }
  if (pose.matrixPalette.empty() || pose.matrixCount == 0u) {
    outPreDetail.reason = legacy::MissReason::NoPosePalette;
    return false;
  }

  // RuntimeVertexGroupSlotCount 的 min 链 + 128Ki clamp（core.cpp:6486-6493）
  std::size_t count = resource.vertexGroupIndices.size();
  const uint32_t vertexCount =
      resource.vertexCount != 0u ? resource.vertexCount
                                 : uint32_t(resource.positions.size() / 3u);
  if (vertexCount != 0u)
    count = (std::min)(count, std::size_t(vertexCount));
  constexpr std::size_t kMaxSemanticVertexGroupSlots = 128u * 1024u;
  count = (std::min)(count, kMaxSemanticVertexGroupSlots);

  // 与旧实现副本里的 RuntimeVertexGroupSlotCount 逐输入相等（同一 pre-S2 函数）；
  // 若派生建模与副本不一致则视为 mismatch（由调用方的外层差分捕获）。
  if (count != legacy::RuntimeVertexGroupSlotCount(resource)) {
    outPreDetail.reason = legacy::MissReason::None;
    return false;
  }

  if (count == 0u) {
    outPreDetail.reason = legacy::MissReason::NoVertexGroups;
    return false;
  }

  out.vertexGroupIndices = resource.vertexGroupIndices.data();
  out.vertexGroupSlotCount = count;
  out.matrixGroupSizes = resource.matrixGroupSizes.data();
  out.groupCount = uint32_t(resource.matrixGroupSizes.size());
  out.matrixIndices = resource.matrixIndices.data();
  out.matrixIndexCount = resource.matrixIndices.size();
  out.posePalette = pose.matrixPalette.data();
  out.posePaletteSize = pose.matrixPalette.size();
  out.poseMatrixCount = pose.matrixCount;
  return true;
}

// upper 适配层（war3_upper_layer_shadow.cpp:110-133）：
//   !hasSkinningData() / 空 pose                  -> 早退
//   vertexGroupCount = min(geoset.vertexGroupCount, vertexGroupIndices.size())
//   groupCount       = min(geoset.matrixGroupCount, matrixGroupSizes.size())
bool DeriveUpperAdapterInputs(const legacy::UpperResourceRecord& geoset,
                              const legacy::UpperPoseRecord& pose,
                              RuntimeGroupPaletteInput& out,
                              legacy::MissReason& outPreReason) {
  out = {};
  outPreReason = legacy::MissReason::None;

  if (!geoset.hasSkinningData()) {
    outPreReason = legacy::MissReason::NoSkinningData;
    return false;
  }
  if (pose.matrixPalette.empty() || pose.matrixCount == 0) {
    outPreReason = legacy::MissReason::NoPosePalette;
    return false;
  }
  const uint32_t vertexGroupCount = std::min<uint32_t>(
      geoset.vertexGroupCount, uint32_t(geoset.vertexGroupIndices.size()));
  if (vertexGroupCount == 0u) {
    outPreReason = legacy::MissReason::NoVertexGroups;
    return false;
  }

  out.vertexGroupIndices = geoset.vertexGroupIndices.data();
  out.vertexGroupSlotCount = std::size_t(vertexGroupCount);
  out.matrixGroupSizes = geoset.matrixGroupSizes.data();
  out.groupCount = std::min<uint32_t>(
      geoset.matrixGroupCount, uint32_t(geoset.matrixGroupSizes.size()));
  out.matrixIndices = geoset.matrixIndices.data();
  out.matrixIndexCount = geoset.matrixIndices.size();
  out.posePalette = pose.matrixPalette.data();
  out.posePaletteSize = pose.matrixPalette.size();
  out.poseMatrixCount = pose.matrixCount;
  return true;
}

// =============================================================================
// E. 当前适配层建模（S2 b02 成本/安全收口后：直接借用调用方 vector + 单趟有界扫描）
// =============================================================================
// core 当前实现：来源链前 1 趟共享 maxSlot 扫描（仅来源链使用）；内核随后固定
// 256 项数组单趟同时求 maxSlot + unique，不接受 caller-provided trusted max。
// unique 重建只存在于 logFailure 诊断路径。
bool CurrentCoreAdapterBuild(const legacy::CoreResourceRecord& resource,
                             const RuntimeGroupPaletteInput& in,
                             std::vector<Matrix4>& outPalette,
                             uint32_t& outMaxVertexGroupSlot,
                             bool& outUsesAveraging,
                             RuntimeGroupPaletteKernelDetail* outDetail) {
  outPalette.clear();
  outMaxVertexGroupSlot = 0u;
  outUsesAveraging = false;

  outMaxVertexGroupSlot = FindRuntimeGroupPaletteMaxSlot(
      resource.vertexGroupIndices.data(), in.vertexGroupSlotCount);

  RuntimeGroupPaletteKernelDetail kernelDetail = {};
  const bool kernelSucceeded = TryBuildRuntimeGroupPaletteKernel(
      in, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot,
      outPalette, outMaxVertexGroupSlot, outUsesAveraging, &kernelDetail);

  if (outDetail != nullptr) {
    outDetail->reason = kernelDetail.reason;
    outDetail->group = kernelDetail.group;
    outDetail->matrixIndex = kernelDetail.matrixIndex;
    outDetail->poseCount = kernelDetail.poseCount;
    outDetail->groupCount = kernelDetail.groupCount;
    outDetail->maxVertexGroupSlot = kernelDetail.maxVertexGroupSlot;
    outDetail->matrixIndexCount = kernelDetail.matrixIndexCount;
  }
  return kernelSucceeded;
}

// upper 当前实现：无来源链预计算 max，内核内部走固定 256 项单趟扫描；输出直接写调用方 vector。
bool CurrentUpperAdapterBuild(const RuntimeGroupPaletteInput& in,
                              std::vector<Matrix4>& outPalette,
                              uint32_t& outMaxVertexGroupSlot,
                              bool& outUsesAveraging) {
  outPalette.clear();
  outMaxVertexGroupSlot = 0u;
  outUsesAveraging = false;

  return TryBuildRuntimeGroupPaletteKernel(
      in, RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap, outPalette,
      outMaxVertexGroupSlot, outUsesAveraging, nullptr);
}

// =============================================================================
// F. 随机输入生成
// =============================================================================
void GenerateCoreInput(SplitMix64& rng, uint64_t iteration,
                       legacy::CoreResourceRecord& resource,
                       legacy::CorePoseRecord& pose, bool& clampPhaseA,
                       bool& clampPhaseB) {
  clampPhaseA = false;
  clampPhaseB = false;

  const uint64_t boundaryPhase = iteration % 50000ull;
  if (boundaryPhase == 49999ull) {
    // 128Ki clamp / min 链边界（core 独有 128Ki clamp）
    clampPhaseA = ((iteration / 50000ull) % 2ull) == 0ull;
    clampPhaseB = !clampPhaseA;
    resource.vertexGroupIndices.assign(131200u, uint8_t(3u));
    resource.vertexGroupIndices[4u] = 9u;  // 被扫到的最大值
    if (clampPhaseA) {
      // size > 128Ki 且 vertexCount 派生为 0 -> 只由 128Ki clamp 收窄
      resource.vertexCount = 0u;
      resource.positions.clear();
      for (std::size_t i = 131072u; i < resource.vertexGroupIndices.size(); ++i)
        resource.vertexGroupIndices[i] = 250u;  // clamp 之外，必须不可见
    } else {
      // vertexCount = 131150 -> min 链先收窄到 131150，clamp 不绑定
      resource.vertexCount = 131150u;
      resource.positions.clear();
      for (std::size_t i = 131150u; i < resource.vertexGroupIndices.size(); ++i)
        resource.vertexGroupIndices[i] = 250u;  // min 之外，必须不可见
    }
    resource.matrixGroupSizes.assign(1u, 3u);
    resource.matrixIndices = {0u, 1u, 2u};
    pose.matrixPalette.resize(4u);
    for (uint32_t j = 0u; j < 4u; ++j)
      pose.matrixPalette[j] = MakePoseMatrix(iteration, j);
    pose.matrixCount = 4u;
    return;
  }

  const uint32_t slotCount = rng.nextU32(24u);
  resource.vertexGroupIndices.resize(slotCount);
  for (uint32_t s = 0u; s < slotCount; ++s) {
    const uint32_t roll = rng.nextU32(100u);
    resource.vertexGroupIndices[s] =
        uint8_t(roll < 85u ? rng.nextU32(10u) : rng.nextU32(256u));
  }
  if (iteration % 1000ull == 0ull)
    resource.vertexGroupIndices.clear();  // 保证 NoSkinningData 分支必被覆盖

  const uint32_t vcRoll = rng.nextU32(100u);
  if (vcRoll < 40u) {
    resource.vertexCount = 0u;
    resource.positions.resize(std::size_t(rng.nextU32(12u)) * 3u);
  } else if (vcRoll < 70u) {
    resource.vertexCount = 0u;
    resource.positions.clear();
  } else {
    resource.positions.clear();
    resource.vertexCount = rng.nextU32(slotCount + 4u);
  }

  const uint32_t groupSizeCount = rng.nextU32(7u);
  resource.matrixGroupSizes.resize(groupSizeCount);
  for (uint32_t g = 0u; g < groupSizeCount; ++g)
    resource.matrixGroupSizes[g] =
        rng.nextU32(100u) < 85u ? rng.nextU32(3u) : rng.nextU32(8u);

  const uint32_t matrixIndexCount = rng.nextU32(7u);
  resource.matrixIndices.resize(matrixIndexCount);
  for (uint32_t m = 0u; m < matrixIndexCount; ++m)
    resource.matrixIndices[m] =
        rng.nextU32(100u) < 80u ? rng.nextU32(10u) : rng.nextU32(4096u);

  const uint32_t posePaletteSize = rng.nextU32(9u);
  pose.matrixPalette.resize(posePaletteSize);
  for (uint32_t j = 0u; j < posePaletteSize; ++j)
    pose.matrixPalette[j] = MakePoseMatrix(iteration, j);
  pose.matrixCount =
      rng.nextU32(100u) < 80u ? posePaletteSize : rng.nextU32(11u);
  if (iteration % 997ull == 0ull) {
    pose.matrixPalette.clear();  // 空 pose 前置校验
    pose.matrixCount = 0u;
  }
}

void GenerateUpperInput(SplitMix64& rng, uint64_t iteration,
                        legacy::UpperResourceRecord& geoset,
                        legacy::UpperPoseRecord& pose, bool& minChainPhase,
                        bool& noClampPhase) {
  minChainPhase = false;
  noClampPhase = false;

  const uint64_t boundaryPhase = iteration % 50000ull;
  if (boundaryPhase == 49999ull) {
    const bool upperNoClamp = ((iteration / 50000ull) % 2ull) == 0ull;
    minChainPhase = !upperNoClamp;
    noClampPhase = upperNoClamp;
    geoset.vertexGroupIndices.assign(131200u, uint8_t(3u));
    geoset.vertexGroupIndices[4u] = 9u;
    if (upperNoClamp) {
      // upper 没有 128Ki clamp：count = min(vertexGroupCount, size) = 131200，
      // 尾部 250 必须可见（与 core 的 clamp 行为形成显式对照）
      geoset.vertexGroupCount = 131200u;
      for (std::size_t i = 131150u; i < geoset.vertexGroupIndices.size(); ++i)
        geoset.vertexGroupIndices[i] = 250u;
    } else {
      geoset.vertexGroupCount = 131150u;
      for (std::size_t i = 131150u; i < geoset.vertexGroupIndices.size(); ++i)
        geoset.vertexGroupIndices[i] = 250u;  // min 之外，必须不可见
    }
    geoset.matrixGroupSizes.assign(1u, 3u);
    geoset.matrixGroupCount = 1u;
    geoset.matrixIndices = {0u, 1u, 2u};
    pose.matrixPalette.resize(4u);
    for (uint32_t j = 0u; j < 4u; ++j)
      pose.matrixPalette[j] = MakePoseMatrix(iteration, j);
    pose.matrixCount = 4u;
    return;
  }

  const uint32_t slotCount = rng.nextU32(24u);
  geoset.vertexGroupIndices.resize(slotCount);
  for (uint32_t s = 0u; s < slotCount; ++s) {
    const uint32_t roll = rng.nextU32(100u);
    geoset.vertexGroupIndices[s] =
        uint8_t(roll < 85u ? rng.nextU32(10u) : rng.nextU32(256u));
  }
  if (iteration % 1000ull == 0ull)
    geoset.vertexGroupIndices.clear();

  // vertexGroupCount = min(geoset.vertexGroupCount, size)
  const uint32_t vgcRoll = rng.nextU32(100u);
  if (vgcRoll < 45u)
    geoset.vertexGroupCount = slotCount;
  else if (vgcRoll < 80u)
    geoset.vertexGroupCount = rng.nextU32(slotCount + 1u);
  else
    geoset.vertexGroupCount = rng.nextU32(slotCount + 6u);

  const uint32_t groupSizeCount = rng.nextU32(7u);
  geoset.matrixGroupSizes.resize(groupSizeCount);
  for (uint32_t g = 0u; g < groupSizeCount; ++g)
    geoset.matrixGroupSizes[g] =
        rng.nextU32(100u) < 85u ? rng.nextU32(3u) : rng.nextU32(8u);

  // matrixGroupCount = min(geoset.matrixGroupCount, size)
  const uint32_t mgcRoll = rng.nextU32(100u);
  if (mgcRoll < 45u)
    geoset.matrixGroupCount = groupSizeCount;
  else if (mgcRoll < 80u)
    geoset.matrixGroupCount = rng.nextU32(groupSizeCount + 1u);
  else
    geoset.matrixGroupCount = rng.nextU32(groupSizeCount + 6u);

  const uint32_t matrixIndexCount = rng.nextU32(7u);
  geoset.matrixIndices.resize(matrixIndexCount);
  for (uint32_t m = 0u; m < matrixIndexCount; ++m)
    geoset.matrixIndices[m] =
        rng.nextU32(100u) < 80u ? rng.nextU32(10u) : rng.nextU32(4096u);

  const uint32_t posePaletteSize = rng.nextU32(9u);
  pose.matrixPalette.resize(posePaletteSize);
  for (uint32_t j = 0u; j < posePaletteSize; ++j)
    pose.matrixPalette[j] = MakePoseMatrix(iteration, j);
  pose.matrixCount =
      rng.nextU32(100u) < 80u ? posePaletteSize : rng.nextU32(11u);
  if (iteration % 997ull == 0ull) {
    pose.matrixPalette.clear();
    pose.matrixCount = 0u;
  }
}

// =============================================================================
// G. 两组随机差分
// =============================================================================
void RunCoreGroup(GroupStats& stats) {
  SplitMix64 rng(kSeedCoreGroup);

  legacy::CoreResourceRecord resource;
  legacy::CorePoseRecord pose;
  resource.vertexGroupIndices.reserve(131200u);
  resource.positions.reserve(4096u);
  resource.matrixGroupSizes.reserve(8u);
  resource.matrixIndices.reserve(8u);
  pose.matrixPalette.reserve(8u);

  Outcome legacyOut;
  Outcome kernelOut;
  legacy::MissDetail legacyDetail;
  std::string why;

  for (uint64_t iteration = 0u; iteration < kGroupIterations; ++iteration) {
    bool clampPhaseA = false;
    bool clampPhaseB = false;
    GenerateCoreInput(rng, iteration, resource, pose, clampPhaseA, clampPhaseB);
    ++stats.inputs;

    RuntimeGroupPaletteInput kernelInput = {};
    legacy::MissDetail preDetail;
    const bool adapterReady =
        DeriveCoreAdapterInputs(resource, pose, kernelInput, preDetail);

    legacyOut = Outcome{};
    legacyDetail = {};
    legacyOut.ok = legacy::TryBuildRuntimeGroupPaletteFiveStep(
        resource, pose, legacyOut.palette, legacyOut.maxSlot,
        legacyOut.usesAveraging, &legacyDetail);
    legacyOut.reason = legacyDetail.reason;
    legacyOut.detailGroup = legacyDetail.group;
    legacyOut.detailMatrixIndex = legacyDetail.matrixIndex;
    legacyOut.detailPoseCount = legacyDetail.poseCount;
    legacyOut.detailGroupCount = legacyDetail.groupCount;
    legacyOut.detailMaxSlot = legacyDetail.maxVertexGroupSlot;
    legacyOut.detailMatrixIndexCount = legacyDetail.matrixIndexCount;

    bool mismatch = false;

    if (!adapterReady) {
      ++stats.adapterEarlyExit;
      switch (preDetail.reason) {
        case legacy::MissReason::NoSkinningData:
          ++stats.earlyNoSkinningData;
          break;
        case legacy::MissReason::NoPosePalette:
          ++stats.earlyNoPosePalette;
          break;
        case legacy::MissReason::NoVertexGroups:
          ++stats.earlyNoVertexGroups;
          break;
        default:
          break;
      }
      // 适配层早退：内核不参与；旧实现副本必须同样早退且 reason / detail 一致
      Outcome expectedEarly;
      expectedEarly.ok = false;
      expectedEarly.reason = preDetail.reason;
      expectedEarly.detailPoseCount = preDetail.poseCount;
      expectedEarly.detailGroupCount = preDetail.groupCount;
      expectedEarly.detailMaxSlot = preDetail.maxVertexGroupSlot;
      expectedEarly.detailMatrixIndexCount = preDetail.matrixIndexCount;
      if (OutcomeDiffers(expectedEarly, legacyOut, true, why)) {
        mismatch = true;
        why = "adapter early exit (" + ReasonName(preDetail.reason) +
              ") vs legacy: " + why;
      }
    } else {
      ++stats.kernelInvoked;
      RuntimeGroupPaletteOutput kernelOutput = {};
      RuntimeGroupPaletteKernelDetail kernelDetail = {};
      const bool kernelOk = TryBuildRuntimeGroupPaletteKernel(
          kernelInput, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot,
          kernelOutput.palette, kernelOutput.maxVertexGroupSlot,
          kernelOutput.usesAveraging, &kernelDetail);
      OutcomeFromKernel(kernelOutput, kernelDetail, kernelOut);
      kernelOut.ok = kernelOk;

      // 适配层前置校验已通过时，内核不得报出三条适配层专属早退原因
      if (IsAdapterOnlyReason(kernelDetail.reason)) {
        mismatch = true;
        why = "kernel reported an adapter-only pre-check reason while the "
              "modeled adapter had already passed it";
      }

      if (!mismatch && OutcomeDiffers(legacyOut, kernelOut, true, why))
        mismatch = true;

      if (kernelOk) {
        ++stats.kernelOk;
        if (legacyOut.reason != legacy::MissReason::None)
          ++stats.kernelOkAfterMainPathFailure;
      }
      // reason 计数按 detail 的最终 note（成功或失败都计）：fallback 成功时
      // InvalidGroupTable / MatrixIndexOutOfRange / VertexGroupOutOfRange 仍保留在
      // detail 里（与 pre-S2 的 Note 覆写语义一致）。
      switch (kernelDetail.reason) {
        case RuntimeGroupPaletteKernelMiss::InvalidGroupTable:
          ++stats.kernelInvalidGroupTable;
          break;
        case RuntimeGroupPaletteKernelMiss::MatrixIndexOutOfRange:
          ++stats.kernelMatrixIndexOutOfRange;
          break;
        case RuntimeGroupPaletteKernelMiss::VertexGroupOutOfRange:
          ++stats.kernelVertexGroupOutOfRange;
          break;
        case RuntimeGroupPaletteKernelMiss::FallbacksFailed:
          ++stats.kernelFallbacksFailed;
          break;
        default:
          break;
      }
      if (kernelOutput.usesAveraging)
        ++stats.usesAveragingTrue;
      if (kernelOutput.maxVertexGroupSlot == 255u)
        ++stats.maxSlotIs255;
    }

    if (clampPhaseA || clampPhaseB) {
      ++stats.boundaryCases;
      if (legacyOut.maxSlot != 9u || kernelOut.maxSlot != 9u) {
        mismatch = true;
        why = std::string("128Ki clamp / min-chain boundary: expected "
                          "maxVertexGroupSlot 9, legacy=") +
              std::to_string(legacyOut.maxSlot) +
              " kernel=" + std::to_string(kernelOut.maxSlot);
      }
    }

    if (mismatch) {
      ++stats.mismatches;
      if (stats.printedMismatches < kMismatchCaseLimit) {
        ++stats.printedMismatches;
        std::cerr << "war3_runtime_group_palette_kernel_diff_test: CORE MISMATCH"
                  << " iteration=" << iteration << " why=" << why
                  << " adapterReady=" << (adapterReady ? 1 : 0)
                  << " slots=" << resource.vertexGroupIndices.size()
                  << " vertexCount=" << resource.vertexCount
                  << " positions=" << resource.positions.size()
                  << " groupSizes=" << resource.matrixGroupSizes.size()
                  << " matrixIndices=" << resource.matrixIndices.size()
                  << " posePalette=" << pose.matrixPalette.size()
                  << " poseCount=" << pose.matrixCount
                  << " legacy{ok=" << (legacyOut.ok ? 1 : 0)
                  << ",avg=" << (legacyOut.usesAveraging ? 1 : 0)
                  << ",max=" << legacyOut.maxSlot
                  << ",size=" << legacyOut.palette.size()
                  << ",reason=" << uint32_t(legacyOut.reason) << "}"
                  << " kernel{ok=" << (kernelOut.ok ? 1 : 0)
                  << ",avg=" << (kernelOut.usesAveraging ? 1 : 0)
                  << ",max=" << kernelOut.maxSlot
                  << ",size=" << kernelOut.palette.size()
                  << ",reason=" << uint32_t(kernelOut.reason) << "}\n";
      }
    }
  }
}

void RunUpperGroup(GroupStats& stats) {
  SplitMix64 rng(kSeedUpperGroup);

  legacy::UpperResourceRecord geoset;
  legacy::UpperPoseRecord pose;
  geoset.vertexGroupIndices.reserve(131200u);
  geoset.matrixGroupSizes.reserve(8u);
  geoset.matrixIndices.reserve(8u);
  pose.matrixPalette.reserve(8u);

  Outcome legacyOut;
  Outcome kernelOut;
  std::string why;

  for (uint64_t iteration = 0u; iteration < kGroupIterations; ++iteration) {
    bool minChainPhase = false;
    bool noClampPhase = false;
    GenerateUpperInput(rng, iteration, geoset, pose, minChainPhase,
                       noClampPhase);
    ++stats.inputs;

    RuntimeGroupPaletteInput kernelInput = {};
    legacy::MissReason preReason = legacy::MissReason::None;
    const bool adapterReady =
        DeriveUpperAdapterInputs(geoset, pose, kernelInput, preReason);

    legacyOut = Outcome{};
    legacy::MissReason legacyExit = legacy::MissReason::None;
    legacyOut.ok = legacy::TryBuildRuntimeGroupPaletteFourStep(
        geoset, pose, legacyOut.palette, legacyOut.maxSlot,
        legacyOut.usesAveraging, &legacyExit);
    legacyOut.reason = legacyExit;

    bool mismatch = false;

    if (!adapterReady) {
      ++stats.adapterEarlyExit;
      switch (preReason) {
        case legacy::MissReason::NoSkinningData:
          ++stats.earlyNoSkinningData;
          break;
        case legacy::MissReason::NoPosePalette:
          ++stats.earlyNoPosePalette;
          break;
        case legacy::MissReason::NoVertexGroups:
          ++stats.earlyNoVertexGroups;
          break;
        default:
          break;
      }
      if (legacyOut.ok || legacyOut.reason != preReason) {
        mismatch = true;
        why = "adapter early exit (" + ReasonName(preReason) +
              ") vs legacy exit (" + ReasonName(legacyOut.reason) + ")";
      }
    } else {
      ++stats.kernelInvoked;
      RuntimeGroupPaletteOutput kernelOutput = {};
      RuntimeGroupPaletteKernelDetail kernelDetail = {};
      const bool kernelOk = TryBuildRuntimeGroupPaletteKernel(
          kernelInput, RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap,
          kernelOutput.palette, kernelOutput.maxVertexGroupSlot,
          kernelOutput.usesAveraging, &kernelDetail);
      OutcomeFromKernel(kernelOutput, kernelDetail, kernelOut);
      kernelOut.ok = kernelOk;

      if (IsAdapterOnlyReason(kernelDetail.reason)) {
        mismatch = true;
        why = "kernel reported an adapter-only pre-check reason while the "
              "modeled adapter had already passed it";
      }
      if (!mismatch && OutcomeDiffers(legacyOut, kernelOut, false, why))
        mismatch = true;

      if (kernelOk) {
        ++stats.kernelOk;
        if (legacyOut.reason != legacy::MissReason::None)
          ++stats.kernelOkAfterMainPathFailure;
      }
      // reason 计数按 detail 的最终 note（成功或失败都计）：fallback 成功时
      // InvalidGroupTable / MatrixIndexOutOfRange / VertexGroupOutOfRange 仍保留在
      // detail 里（与 pre-S2 的 Note 覆写语义一致）。
      switch (kernelDetail.reason) {
        case RuntimeGroupPaletteKernelMiss::InvalidGroupTable:
          ++stats.kernelInvalidGroupTable;
          break;
        case RuntimeGroupPaletteKernelMiss::MatrixIndexOutOfRange:
          ++stats.kernelMatrixIndexOutOfRange;
          break;
        case RuntimeGroupPaletteKernelMiss::VertexGroupOutOfRange:
          ++stats.kernelVertexGroupOutOfRange;
          break;
        case RuntimeGroupPaletteKernelMiss::FallbacksFailed:
          ++stats.kernelFallbacksFailed;
          break;
        default:
          break;
      }
      if (kernelOutput.usesAveraging)
        ++stats.usesAveragingTrue;
      if (kernelOutput.maxVertexGroupSlot == 255u)
        ++stats.maxSlotIs255;
    }

    if (minChainPhase || noClampPhase) {
      ++stats.boundaryCases;
      const uint32_t expectedMax = noClampPhase ? 250u : 9u;
      if (legacyOut.maxSlot != expectedMax || kernelOut.maxSlot != expectedMax) {
        mismatch = true;
        why = std::string("upper min-chain boundary (") +
              (noClampPhase ? "no 128Ki clamp, tail visible"
                            : "min(vertexGroupCount,size) binds") +
              "): expected " + std::to_string(expectedMax) +
              " legacy=" + std::to_string(legacyOut.maxSlot) +
              " kernel=" + std::to_string(kernelOut.maxSlot);
      }
    }

    if (mismatch) {
      ++stats.mismatches;
      if (stats.printedMismatches < kMismatchCaseLimit) {
        ++stats.printedMismatches;
        std::cerr
            << "war3_runtime_group_palette_kernel_diff_test: UPPER MISMATCH"
            << " iteration=" << iteration << " why=" << why
            << " adapterReady=" << (adapterReady ? 1 : 0)
            << " slots=" << geoset.vertexGroupIndices.size()
            << " vertexGroupCount=" << geoset.vertexGroupCount
            << " groupSizes=" << geoset.matrixGroupSizes.size()
            << " matrixGroupCount=" << geoset.matrixGroupCount
            << " matrixIndices=" << geoset.matrixIndices.size()
            << " posePalette=" << pose.matrixPalette.size()
            << " poseCount=" << pose.matrixCount
            << " legacy{ok=" << (legacyOut.ok ? 1 : 0)
            << ",avg=" << (legacyOut.usesAveraging ? 1 : 0)
            << ",max=" << legacyOut.maxSlot
            << ",size=" << legacyOut.palette.size()
            << ",exit=" << uint32_t(legacyOut.reason) << "}"
            << " kernel{ok=" << (kernelOut.ok ? 1 : 0)
            << ",avg=" << (kernelOut.usesAveraging ? 1 : 0)
            << ",max=" << kernelOut.maxSlot
            << ",size=" << kernelOut.palette.size()
            << ",reason=" << uint32_t(kernelOut.reason) << "}\n";
      }
    }
  }
}

// =============================================================================
// H. 适配层派生边界显式夹具（不依赖随机命中）
// =============================================================================
struct CoreBoundaryFixture {
  const char* name;
  uint32_t slots;
  uint32_t highSlotIndex;
  uint8_t highSlotValue;
  uint32_t vertexCount;
  uint32_t positionsFloats;
  uint32_t groupSizes;
  uint32_t expectedSlotCount;
  uint32_t expectedMaxSlot;
};

void RunCoreBoundaryFixtures(GroupStats& stats) {
  // expectedSlotCount / expectedMaxSlot 是适配层派生 + 旧实现共同给出的答案。
  // 注意 RuntimeVertexGroupSlotCount 的 min 链顺序：先 min(size, vertexCount)，
  // 再 min(count, 128Ki)；因此 vertexCount > 128Ki 时 clamp 仍然绑定（夹具 2）。
  const CoreBoundaryFixture fixtures[] = {
      {"slots>128Ki, vertexCount=0, positions empty -> 128Ki clamp binds",
       131200u, 4u, 9u, 0u, 0u, 1u, 131072u, 9u},
      {"slots>128Ki, vertexCount=131150 -> clamp applies after the min chain",
       131200u, 4u, 9u, 131150u, 0u, 1u, 131072u, 9u},
      {"slots>128Ki, vertexCount=100000 -> min chain binds below the clamp",
       131200u, 99999u, 200u, 100000u, 0u, 1u, 100000u, 200u},
      {"vertexCount < slots -> min chain", 40u, 3u, 200u, 8u, 0u, 1u, 8u, 200u},
      {"positions/3 < slots -> min chain from positions", 40u, 20u, 250u, 0u, 30u,
       1u, 10u, 3u},
  };

  for (const CoreBoundaryFixture& fixture : fixtures) {
    legacy::CoreResourceRecord resource;
    legacy::CorePoseRecord pose;
    resource.vertexGroupIndices.assign(fixture.slots, uint8_t(3u));
    if (fixture.highSlotIndex < fixture.slots)
      resource.vertexGroupIndices[fixture.highSlotIndex] =
          fixture.highSlotValue;
    resource.vertexCount = fixture.vertexCount;
    resource.positions.assign(fixture.positionsFloats, 0.0f);
    resource.matrixGroupSizes.assign(fixture.groupSizes, 1u);
    resource.matrixIndices = {0u};
    pose.matrixPalette.resize(8u);
    for (uint32_t j = 0u; j < 8u; ++j)
      pose.matrixPalette[j] = MakePoseMatrix(0u, j);
    pose.matrixCount = 8u;

    RuntimeGroupPaletteInput kernelInput = {};
    legacy::MissDetail preDetail;
    const bool adapterReady =
        DeriveCoreAdapterInputs(resource, pose, kernelInput, preDetail);
    ++stats.inputs;

    Outcome legacyOut;
    legacy::MissDetail legacyDetail;
    legacyOut.ok = legacy::TryBuildRuntimeGroupPaletteFiveStep(
        resource, pose, legacyOut.palette, legacyOut.maxSlot,
        legacyOut.usesAveraging, &legacyDetail);
    legacyOut.reason = legacyDetail.reason;
    legacyOut.detailGroup = legacyDetail.group;
    legacyOut.detailMatrixIndex = legacyDetail.matrixIndex;
    legacyOut.detailPoseCount = legacyDetail.poseCount;
    legacyOut.detailGroupCount = legacyDetail.groupCount;
    legacyOut.detailMaxSlot = legacyDetail.maxVertexGroupSlot;
    legacyOut.detailMatrixIndexCount = legacyDetail.matrixIndexCount;

    ++stats.kernelInvoked;
    require(adapterReady, std::string("boundary fixture: ") + fixture.name +
                              " -> adapter must pass pre-checks");
    require(kernelInput.vertexGroupSlotCount ==
                std::size_t(fixture.expectedSlotCount),
            std::string("boundary fixture: ") + fixture.name +
                " -> derived vertexGroupSlotCount " +
                std::to_string(kernelInput.vertexGroupSlotCount) + " != " +
                std::to_string(fixture.expectedSlotCount));

    RuntimeGroupPaletteOutput kernelOutput = {};
    RuntimeGroupPaletteKernelDetail kernelDetail = {};
    const bool kernelOk = TryBuildRuntimeGroupPaletteKernel(
        kernelInput, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot,
        kernelOutput, &kernelDetail);
    Outcome kernelOut;
    OutcomeFromKernel(kernelOutput, kernelDetail, kernelOut);
    kernelOut.ok = kernelOk;

    std::string why;
    require(!OutcomeDiffers(legacyOut, kernelOut, true, why),
            std::string("boundary fixture: ") + fixture.name + " -> " + why);
    require(kernelOut.maxSlot == fixture.expectedMaxSlot,
            std::string("boundary fixture: ") + fixture.name +
                " -> kernel maxVertexGroupSlot " +
                std::to_string(kernelOut.maxSlot) + " != " +
                std::to_string(fixture.expectedMaxSlot));
    require(legacyOut.maxSlot == fixture.expectedMaxSlot,
            std::string("boundary fixture: ") + fixture.name +
                " -> legacy maxVertexGroupSlot " +
                std::to_string(legacyOut.maxSlot) + " != " +
                std::to_string(fixture.expectedMaxSlot));
    ++stats.boundaryCases;
  }

  // 三条前置校验之一：空 vertexGroupIndices（hasSkinningData == false）
  {
    legacy::CoreResourceRecord resource;
    legacy::CorePoseRecord pose;
    resource.matrixGroupSizes = {1u};
    resource.matrixIndices = {0u};
    pose.matrixPalette.resize(2u);
    pose.matrixCount = 2u;
    RuntimeGroupPaletteInput kernelInput = {};
    legacy::MissDetail preDetail;
    ++stats.inputs;
    ++stats.adapterEarlyExit;
    ++stats.earlyNoSkinningData;
    ++stats.boundaryCases;
    const bool ready =
        DeriveCoreAdapterInputs(resource, pose, kernelInput, preDetail);
    require(!ready && preDetail.reason == legacy::MissReason::NoSkinningData,
            "boundary fixture: empty vertexGroupIndices -> adapter "
            "NoSkinningData (kernel not involved)");
  }
  // 三条前置校验之二：空 pose
  {
    legacy::CoreResourceRecord resource;
    legacy::CorePoseRecord pose;
    resource.vertexGroupIndices = {0u, 1u};
    resource.matrixGroupSizes = {1u};
    resource.matrixIndices = {0u};
    pose.matrixCount = 0u;  // 空 pose
    RuntimeGroupPaletteInput kernelInput = {};
    legacy::MissDetail preDetail;
    ++stats.inputs;
    ++stats.adapterEarlyExit;
    ++stats.earlyNoPosePalette;
    ++stats.boundaryCases;
    const bool ready =
        DeriveCoreAdapterInputs(resource, pose, kernelInput, preDetail);
    require(!ready && preDetail.reason == legacy::MissReason::NoPosePalette,
            "boundary fixture: empty pose -> adapter NoPosePalette (kernel not "
            "involved)");
  }
  // 三条前置校验之三（空 vertexGroups ⇒ NoVertexGroups）在 core 侧**不可达**，
  // 这是差分过程中得到的结构性发现，而不是被跳过的覆盖：
  //   hasSkinningData() 要求 vertexGroupIndices 非空（runtime_contract.h:105），
  //   而 RuntimeVertexGroupSlotCount 只在 vertexGroupIndices 为空时才可能为 0
  //   （min 链只在 vertexCount != 0 时收窄），所以 pre-S2 与当前 core 的
  //   "vertexGroupSlotCount == 0u -> NoVertexGroups" 是死分支（内核同形分支亦然）。
  // 这里显式验证"非空槽位数组的派生 count 恒 >= 1"，随机组另有计数器断言该值为 0。
  {
    const uint8_t values[] = {0u, 5u, 255u};
    for (uint8_t value : values) {
      legacy::CoreResourceRecord resource;
      legacy::CorePoseRecord pose;
      resource.vertexGroupIndices = {value};
      resource.matrixGroupSizes = {1u};
      resource.matrixIndices = {0u};
      pose.matrixPalette.resize(2u);
      pose.matrixCount = 2u;
      RuntimeGroupPaletteInput kernelInput = {};
      legacy::MissDetail preDetail;
      ++stats.inputs;
      ++stats.boundaryCases;
      const bool ready =
          DeriveCoreAdapterInputs(resource, pose, kernelInput, preDetail);
      require(ready && kernelInput.vertexGroupSlotCount >= 1u,
              "boundary fixture: non-empty vertexGroupIndices never yields "
              "core NoVertexGroups (dead branch in pre-S2 and current core)");
    }
  }
}

struct UpperBoundaryFixture {
  const char* name;
  uint32_t slots;
  uint32_t vertexGroupCount;
  uint32_t groupSizes;
  uint32_t matrixGroupCount;
  uint32_t expectedSlotCount;
  uint32_t expectedGroupCount;
};

void RunUpperBoundaryFixtures(GroupStats& stats) {
  const UpperBoundaryFixture fixtures[] = {
      {"vertexGroupCount < slots -> slot count narrowed", 40u, 5u, 2u, 2u, 5u, 2u},
      {"vertexGroupCount > slots -> slot count = size", 10u, 99u, 2u, 2u, 10u, 2u},
      {"matrixGroupCount < size -> groupCount narrowed", 10u, 10u, 6u, 2u, 10u,
       2u},
      {"matrixGroupCount > size -> groupCount = size", 10u, 10u, 2u, 9u, 10u, 2u},
  };

  for (const UpperBoundaryFixture& fixture : fixtures) {
    legacy::UpperResourceRecord geoset;
    legacy::UpperPoseRecord pose;
    geoset.vertexGroupIndices.assign(fixture.slots, uint8_t(1u));
    geoset.vertexGroupCount = fixture.vertexGroupCount;
    geoset.matrixGroupSizes.assign(fixture.groupSizes, 1u);
    geoset.matrixGroupCount = fixture.matrixGroupCount;
    geoset.matrixIndices = {0u, 1u};
    pose.matrixPalette.resize(4u);
    for (uint32_t j = 0u; j < 4u; ++j)
      pose.matrixPalette[j] = MakePoseMatrix(0u, j);
    pose.matrixCount = 4u;

    RuntimeGroupPaletteInput kernelInput = {};
    legacy::MissReason preReason = legacy::MissReason::None;
    const bool adapterReady =
        DeriveUpperAdapterInputs(geoset, pose, kernelInput, preReason);
    ++stats.inputs;
    ++stats.kernelInvoked;
    ++stats.boundaryCases;

    require(adapterReady, std::string("upper boundary: ") + fixture.name +
                              " -> adapter must pass pre-checks");
    require(kernelInput.vertexGroupSlotCount ==
                std::size_t(fixture.expectedSlotCount),
            std::string("upper boundary: ") + fixture.name +
                " -> derived vertexGroupSlotCount " +
                std::to_string(kernelInput.vertexGroupSlotCount) + " != " +
                std::to_string(fixture.expectedSlotCount));
    require(kernelInput.groupCount == fixture.expectedGroupCount,
            std::string("upper boundary: ") + fixture.name +
                " -> derived groupCount " +
                std::to_string(kernelInput.groupCount) + " != " +
                std::to_string(fixture.expectedGroupCount));

    Outcome legacyOut;
    legacy::MissReason legacyExit = legacy::MissReason::None;
    legacyOut.ok = legacy::TryBuildRuntimeGroupPaletteFourStep(
        geoset, pose, legacyOut.palette, legacyOut.maxSlot,
        legacyOut.usesAveraging, &legacyExit);
    legacyOut.reason = legacyExit;

    RuntimeGroupPaletteOutput kernelOutput = {};
    RuntimeGroupPaletteKernelDetail kernelDetail = {};
    const bool kernelOk = TryBuildRuntimeGroupPaletteKernel(
        kernelInput, RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap,
        kernelOutput, &kernelDetail);
    Outcome kernelOut;
    OutcomeFromKernel(kernelOutput, kernelDetail, kernelOut);
    kernelOut.ok = kernelOk;

    std::string why;
    require(!OutcomeDiffers(legacyOut, kernelOut, false, why),
            std::string("upper boundary: ") + fixture.name + " -> " + why);
  }

  // upper 的 no-128Ki-clamp 对照：count = min(vertexGroupCount, size)，尾部可见
  {
    legacy::UpperResourceRecord geoset;
    legacy::UpperPoseRecord pose;
    geoset.vertexGroupIndices.assign(131200u, uint8_t(3u));
    geoset.vertexGroupIndices[131150u] = 250u;
    geoset.vertexGroupCount = 131200u;
    geoset.matrixGroupSizes.assign(1u, 1u);
    geoset.matrixGroupCount = 1u;
    geoset.matrixIndices = {0u};
    pose.matrixPalette.resize(4u);
    for (uint32_t j = 0u; j < 4u; ++j)
      pose.matrixPalette[j] = MakePoseMatrix(0u, j);
    pose.matrixCount = 4u;

    RuntimeGroupPaletteInput kernelInput = {};
    legacy::MissReason preReason = legacy::MissReason::None;
    const bool adapterReady =
        DeriveUpperAdapterInputs(geoset, pose, kernelInput, preReason);
    ++stats.inputs;
    ++stats.kernelInvoked;
    ++stats.boundaryCases;
    require(adapterReady && kernelInput.vertexGroupSlotCount == 131200u,
            "upper boundary: no 128Ki clamp -> slot count stays 131200");
    RuntimeGroupPaletteOutput kernelOutput = {};
    RuntimeGroupPaletteKernelDetail kernelDetail = {};
    TryBuildRuntimeGroupPaletteKernel(
        kernelInput, RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap,
        kernelOutput, &kernelDetail);
    require(kernelOutput.maxVertexGroupSlot == 250u,
            "upper boundary: without the core-only 128Ki clamp the tail slot "
            "(250 at index 131150) stays visible");
  }
}

// =============================================================================
// I. 成本口径（分配次数 / 峰值 / 容量复用 / 扫描趟数）
// =============================================================================
struct MeasureWindow {
  uint64_t allocs = 0u;
  uint64_t frees = 0u;
  uint64_t bytes = 0u;
  std::size_t peakLive = 0u;
  std::size_t maxSingle = 0u;
};

// 被测 lambda 内**不得**做 require / 打印 / 字符串构造，否则会污染分配计数。
template <typename Fn>
MeasureWindow MeasureCost(Fn&& fn) {
  const uint64_t a0 = s2alloc::g_totalAllocs;
  const uint64_t f0 = s2alloc::g_totalFrees;
  const uint64_t b0 = s2alloc::g_totalBytes;
  s2alloc::g_windowPeakLive = s2alloc::g_liveBytes;
  s2alloc::g_windowMaxSingle = 0u;
  s2alloc::g_tracking = true;
  fn();
  s2alloc::g_tracking = false;

  MeasureWindow window;
  window.allocs = s2alloc::g_totalAllocs - a0;
  window.frees = s2alloc::g_totalFrees - f0;
  window.bytes = s2alloc::g_totalBytes - b0;
  window.peakLive = s2alloc::g_windowPeakLive;
  window.maxSingle = s2alloc::g_windowMaxSingle;
  return window;
}

void PrintMeasure(const char* label, const MeasureWindow& window,
                  const void* dataPtr, std::size_t capacity) {
  std::cout << "  cost " << label << ": allocs=" << window.allocs
            << " frees=" << window.frees << " bytes=" << window.bytes
            << " peakLiveBytes=" << window.peakLive
            << " maxSingleAllocBytes=" << window.maxSingle;
  if (dataPtr != nullptr)
    std::cout << " outDataPtr=" << dataPtr << " outCapacity=" << capacity;
  std::cout << '\n';
}

// 典型成本夹具：48 槽位（唯一槽位 = 0..7，max = 7）、8 个单元素 group、
// 8 个矩阵索引、8 个 pose 矩阵 -> 主路径成功，palette.size() == 8。
void BuildCoreCostFixture(legacy::CoreResourceRecord& resource,
                          legacy::CorePoseRecord& pose) {
  resource.vertexGroupIndices.resize(48u);
  for (uint32_t i = 0u; i < 48u; ++i)
    resource.vertexGroupIndices[i] = uint8_t(i % 8u);
  resource.vertexCount = 0u;
  resource.positions.assign(48u * 3u, 0.0f);
  resource.matrixGroupSizes.assign(8u, 1u);
  resource.matrixIndices.resize(8u);
  for (uint32_t i = 0u; i < 8u; ++i)
    resource.matrixIndices[i] = i;
  pose.matrixPalette.resize(8u);
  for (uint32_t j = 0u; j < 8u; ++j)
    pose.matrixPalette[j] = MakePoseMatrix(0u, j);
  pose.matrixCount = 8u;
}

void BuildUpperCostFixture(legacy::UpperResourceRecord& geoset,
                           legacy::UpperPoseRecord& pose) {
  geoset.vertexGroupIndices.resize(48u);
  for (uint32_t i = 0u; i < 48u; ++i)
    geoset.vertexGroupIndices[i] = uint8_t(i % 8u);
  geoset.vertexGroupCount = 48u;
  geoset.matrixGroupSizes.assign(8u, 1u);
  geoset.matrixGroupCount = 8u;
  geoset.matrixIndices.resize(8u);
  for (uint32_t i = 0u; i < 8u; ++i)
    geoset.matrixIndices[i] = i;
  pose.matrixPalette.resize(8u);
  for (uint32_t j = 0u; j < 8u; ++j)
    pose.matrixPalette[j] = MakePoseMatrix(0u, j);
  pose.matrixCount = 8u;
}

// 非门禁佐证：用中位时间比例估算 ScanRuntimeGroupPaletteSlots 的遍历趟数。
// 噪声未建模，只与"源码结构 = 2 趟"互相佐证，不作为成本等价结论。
template <typename Fn>
double MedianNs(uint32_t samples, uint32_t reps, Fn&& fn) {
  std::vector<double> timings;
  timings.reserve(samples);
  for (uint32_t s = 0u; s < samples; ++s) {
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t r = 0u; r < reps; ++r)
      fn();
    const auto end = std::chrono::steady_clock::now();
    timings.push_back(
        double(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
                .count()) /
        double(reps));
  }
  std::sort(timings.begin(), timings.end());
  return timings[timings.size() / 2u];
}

void RunCostMeasurement() {
  std::cout << "\n[cost] 分配计数口径：仅本 TU 重载的全局 operator new/delete；"
               "不含 CRT / Vulkan / 驱动侧分配。\n";
  std::cout << "[cost] 分配次数与容量/指针变化是实测值；槽位扫描趟数由源码结构"
               "（静态门禁同步断言）与时间比例佐证共同给出。\n";

  legacy::CoreResourceRecord coreResource;
  legacy::CorePoseRecord corePose;
  BuildCoreCostFixture(coreResource, corePose);
  RuntimeGroupPaletteInput coreInput = {};
  legacy::MissDetail corePre;
  const bool coreReady =
      DeriveCoreAdapterInputs(coreResource, corePose, coreInput, corePre);
  require(coreReady, "cost fixture: core adapter must pass pre-checks");

  legacy::UpperResourceRecord upperResource;
  legacy::UpperPoseRecord upperPose;
  BuildUpperCostFixture(upperResource, upperPose);
  RuntimeGroupPaletteInput upperInput = {};
  legacy::MissReason upperPre = legacy::MissReason::None;
  const bool upperReady =
      DeriveUpperAdapterInputs(upperResource, upperPose, upperInput, upperPre);
  require(upperReady, "cost fixture: upper adapter must pass pre-checks");

  // (1) 旧实现 5 步：全新输出对象
  {
    std::vector<Matrix4> out;
    uint32_t maxSlot = 0u;
    bool averaging = false;
    bool ok = false;
    const MeasureWindow window = MeasureCost([&]() {
      ok = legacy::TryBuildRuntimeGroupPaletteFiveStep(
          coreResource, corePose, out, maxSlot, averaging, nullptr);
    });
    require(ok, "cost fixture: legacy 5-step fresh must succeed");
    PrintMeasure("legacy5 fresh-out", window, nullptr, 0u);
  }

  // (2) 旧实现 5 步：复用同一输出对象（pre-S2 的容量复用保证）
  {
    std::vector<Matrix4> out;
    uint32_t maxSlot = 0u;
    bool averaging = false;
    MeasureCost([&]() {
      legacy::TryBuildRuntimeGroupPaletteFiveStep(
          coreResource, corePose, out, maxSlot, averaging, nullptr);
    });
    const void* ptr1 = out.data();
    const std::size_t cap1 = out.capacity();
    const MeasureWindow second = MeasureCost([&]() {
      legacy::TryBuildRuntimeGroupPaletteFiveStep(
          coreResource, corePose, out, maxSlot, averaging, nullptr);
    });
    const bool sameBuffer = (ptr1 == out.data()) && (cap1 == out.capacity());
    PrintMeasure("legacy5 reused-out (2nd call)", second, out.data(),
                 out.capacity());
    require(sameBuffer,
            "cost fixture: legacy 5-step must reuse the caller vector buffer "
            "across calls (capacity preserved, 0 reallocation)");
  }

  // (3) 当前 core 适配层 + 内核：复用同一调用方输出对象
  {
    std::vector<Matrix4> out;
    uint32_t maxSlot = 0u;
    bool averaging = false;
    RuntimeGroupPaletteKernelDetail detail = {};
    MeasureCost([&]() {
      CurrentCoreAdapterBuild(coreResource, coreInput, out, maxSlot, averaging,
                              &detail);
    });
    const void* ptr1 = out.data();
    const std::size_t cap1 = out.capacity();
    const MeasureWindow second = MeasureCost([&]() {
      CurrentCoreAdapterBuild(coreResource, coreInput, out, maxSlot, averaging,
                              &detail);
    });
    const bool sameBuffer = (ptr1 == out.data()) && (cap1 == out.capacity());
    PrintMeasure("current-core-adapter reused-out (2nd call)", second, out.data(),
                 out.capacity());
    std::cout << "  cost current-core-adapter capacity: call1 ptr=" << ptr1
              << " cap=" << cap1 << " -> call2 ptr=" << out.data()
              << " cap=" << out.capacity()
              << " pointerChanged=" << (sameBuffer ? 0 : 1) << '\n';
    require(sameBuffer,
            "cost fixture: current core adapter must preserve the caller "
            "vector buffer (direct kernel output into caller storage)");
  }

  // (4) 内核对同一 RuntimeGroupPaletteOutput 连续两次：内核自身会复用
  {
    RuntimeGroupPaletteOutput out = {};
    RuntimeGroupPaletteKernelDetail detail = {};
    MeasureCost([&]() {
      TryBuildRuntimeGroupPaletteKernel(
          coreInput, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot,
          out, &detail);
    });
    const void* ptr1 = out.palette.data();
    const std::size_t cap1 = out.palette.capacity();
    const MeasureWindow second = MeasureCost([&]() {
      TryBuildRuntimeGroupPaletteKernel(
          coreInput, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot,
          out, &detail);
    });
    const bool sameBuffer =
        (ptr1 == out.palette.data()) && (cap1 == out.palette.capacity());
    PrintMeasure("kernel reused-output (2nd call)", second, out.palette.data(),
                 out.palette.capacity());
    std::cout << "  cost kernel-inner capacity: call1 ptr=" << ptr1
              << " cap=" << cap1 << " -> call2 ptr=" << out.palette.data()
              << " cap=" << out.palette.capacity()
              << " pointerChanged=" << (sameBuffer ? 0 : 1) << '\n';
    require(sameBuffer,
            "cost fixture: the RuntimeGroupPaletteOutput wrapper must reuse "
            "its own palette buffer across calls");
  }

  // (5) 直接内核调用：全新输出对象（作为对照；生产适配层不再这样用）
  {
    const MeasureWindow window = MeasureCost([&]() {
      RuntimeGroupPaletteOutput out = {};
      RuntimeGroupPaletteKernelDetail detail = {};
      TryBuildRuntimeGroupPaletteKernel(
          coreInput, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot,
          out, &detail);
      g_sink += out.palette.size();
    });
    PrintMeasure("kernel fresh-output-per-call", window, nullptr, 0u);
  }

  // (6) upper 侧：旧实现 4 步与当前适配层（均直接复用调用方输出）
  {
    std::vector<Matrix4> out;
    uint32_t maxSlot = 0u;
    bool averaging = false;
    legacy::MissReason exit = legacy::MissReason::None;
    MeasureCost([&]() {
      legacy::TryBuildRuntimeGroupPaletteFourStep(
          upperResource, upperPose, out, maxSlot, averaging, &exit);
    });
    const void* ptr1 = out.data();
    const std::size_t cap1 = out.capacity();
    const MeasureWindow second = MeasureCost([&]() {
      legacy::TryBuildRuntimeGroupPaletteFourStep(
          upperResource, upperPose, out, maxSlot, averaging, &exit);
    });
    const bool sameBuffer = (ptr1 == out.data()) && (cap1 == out.capacity());
    PrintMeasure("legacy4 reused-out (2nd call)", second, out.data(),
                 out.capacity());
    require(sameBuffer,
            "cost fixture: legacy 4-step must reuse the caller vector buffer");
  }
  {
    std::vector<Matrix4> out;
    uint32_t maxSlot = 0u;
    bool averaging = false;
    MeasureCost([&]() {
      CurrentUpperAdapterBuild(upperInput, out, maxSlot, averaging);
    });
    const void* ptr1 = out.data();
    const std::size_t cap1 = out.capacity();
    const MeasureWindow second = MeasureCost([&]() {
      CurrentUpperAdapterBuild(upperInput, out, maxSlot, averaging);
    });
    const bool sameBuffer = (ptr1 == out.data()) && (cap1 == out.capacity());
    PrintMeasure("current-upper-adapter reused-out (2nd call)", second, out.data(),
                 out.capacity());
    std::cout << "  cost current-upper-adapter capacity: call1 ptr=" << ptr1
              << " cap=" << cap1 << " -> call2 ptr=" << out.data()
              << " cap=" << out.capacity()
              << " pointerChanged=" << (sameBuffer ? 0 : 1) << '\n';
    require(sameBuffer,
            "cost fixture: current upper adapter must preserve the caller "
            "vector buffer (direct kernel output into caller storage)");
  }

  // (7) 槽位扫描趟数：时间比例佐证（源码结构见下方口径表）
  const std::size_t scanCount = 131072u;
  std::vector<uint8_t> scanSlots(scanCount);
  for (std::size_t i = 0u; i < scanCount; ++i)
    scanSlots[i] = uint8_t(i % 251u);

  const double singlePassNs = MedianNs(9u, 20u, [&]() {
    uint32_t maxSlot = 0u;
    for (std::size_t i = 0u; i < scanCount; ++i)
      maxSlot = (std::max)(maxSlot, uint32_t(scanSlots[i]));
    g_sink += maxSlot;
  });

  const double kernelScanNs = MedianNs(9u, 20u, [&]() {
    const RuntimeGroupPaletteSlotScan scan =
        ScanRuntimeGroupPaletteSlots(scanSlots.data(), scanCount);
    g_sink += scan.maxVertexGroupSlot + uint64_t(scan.uniqueGroupSlots.size());
  });

  const double twoPassReferenceNs = MedianNs(9u, 20u, [&]() {
    uint32_t maxSlot = 0u;
    for (std::size_t i = 0u; i < scanCount; ++i)
      maxSlot = (std::max)(maxSlot, uint32_t(scanSlots[i]));
    std::vector<uint32_t> unique;
    unique.reserve(std::size_t(maxSlot) + 1u);
    std::array<bool, 256> seen = {};
    for (std::size_t i = 0u; i < scanCount; ++i) {
      const uint8_t slot = scanSlots[i];
      if (!seen[slot]) {
        seen[slot] = true;
        unique.push_back(uint32_t(slot));
      }
    }
    g_sink += unique.size();
  });

  const double kernelRatio =
      singlePassNs > 0.0 ? kernelScanNs / singlePassNs : 0.0;
  const double referenceRatio =
      singlePassNs > 0.0 ? twoPassReferenceNs / singlePassNs : 0.0;
  std::cout << "  cost slot-scan timing (median of 9 samples x 20 reps, "
            << scanCount << " slots): singlePassNs=" << singlePassNs
            << " kernelScanNs=" << kernelScanNs
            << " twoPassReferenceNs=" << twoPassReferenceNs
            << " ratio(kernel/single)=" << kernelRatio
            << " ratio(2passRef/single)=" << referenceRatio << '\n';
  // 时间比例只作佐证：它同时受缓存、循环体形态（dedup 走 push_back 分支）与
  // maxSlot 扫描向量化影响，**不是趟数本身**，也不是成本等价结论。
  require(kernelRatio > 1.15,
          "cost fixture: kernel slot-scan timing ratio should exceed a single "
          "pass (fixed scan + public vector conversion; informational only)");

  std::cout
      << "\n[cost] 槽位扫描趟数口径（slot-scan passes，源码结构 + 门禁断言）：\n"
         "  core  pre-S2 : 2 趟 = 内联 maxSlot(backup 6612-6616) + "
         "去重(backup 6737-6743)\n"
         "  core  b02    : 2 趟 = 共享 FindRuntimeGroupPaletteMaxSlot"
         "（来源链活输入，1 趟）+ 内核固定 256 项单趟扫描（maxSlot+unique，1 趟）；"
         "不接受 trusted max\n"
         "  upper pre-S2 : 2 趟 = 内联 maxSlot(backup 116-118) + "
         "去重(backup 123-129)\n"
         "  upper b02    : 1 趟 = 内核固定 256 项单趟扫描（maxSlot+unique）\n"
         "  => core 保持 pre-S2 的 2 趟；upper 从 2 趟降为 1 趟；"
         "内核不再有 unique vector 堆分配。\n"
         "  输出容量：core/upper 适配层把调用方 vector 直接交给内核写，"
         "连续调用应保持 data()/capacity() 不变。\n"
         "  注：ratio(kernel/single) 只是时间比例佐证（受循环体形态与缓存影响），"
         "不等于趟数；趟数以源码结构为准。\n"
         "  本测试不宣称热路径成本等价，只报告实测分配/容量/指针与源码结构。\n";
}

// =============================================================================
// J. 覆盖度门禁 + 主流程
// =============================================================================
void CheckCoverage(const char* side, const GroupStats& stats, bool coreSide) {
  const std::string tag = std::string("coverage ") + side + ": ";
  require(stats.inputs >= kGroupIterations, tag + "inputs < 300000");
  require(stats.earlyNoSkinningData > 0u, tag + "NoSkinningData not hit");
  require(stats.earlyNoPosePalette > 0u, tag + "NoPosePalette not hit");
  if (coreSide) {
    // core 侧的第三条前置校验是死分支（见 RunCoreBoundaryFixtures 的说明）：
    // hasSkinningData() 已要求 vertexGroupIndices 非空。
    require(stats.earlyNoVertexGroups == 0u,
            tag + "core NoVertexGroups must stay unreachable (dead branch)");
  } else {
    require(stats.earlyNoVertexGroups > 0u, tag + "NoVertexGroups not hit");
  }
  require(stats.adapterEarlyExit ==
              stats.earlyNoSkinningData + stats.earlyNoPosePalette +
                  stats.earlyNoVertexGroups,
          tag + "adapter early exits must all be one of the three pre-checks");
  require(stats.kernelOk > 0u, tag + "no successful build");
  require(stats.kernelOkAfterMainPathFailure > 0u,
          tag + "fallback-saved path not hit");
  require(stats.kernelInvalidGroupTable > 0u, tag + "InvalidGroupTable not hit");
  require(stats.kernelMatrixIndexOutOfRange > 0u,
          tag + "MatrixIndexOutOfRange not hit");
  require(stats.kernelVertexGroupOutOfRange > 0u,
          tag + "VertexGroupOutOfRange not hit");
  if (coreSide) {
    // 5 步集合的 FallbacksFailed 也不可达：适配层三条前置校验已保证 pose 非空，
    // 而 buildUniformPosePalette 在 pose 非空时恒成功（paletteCount = max(maxSlot+1,
    // groupCount) >= 1），因此四项 fallback 不可能全部失败。
    // 推论：pre-S2/当前 core 的 tryFallbacks 内 logFailure() 采样同样不可达。
    require(stats.kernelFallbacksFailed == 0u,
            tag + "core FallbacksFailed must stay unreachable: the 5-step "
                  "uniform-root broadcast always succeeds after the pose "
                  "pre-check");
  } else {
    require(stats.kernelFallbacksFailed > 0u, tag + "FallbacksFailed not hit");
  }
  require(stats.usesAveragingTrue > 0u, tag + "usesAveraging not hit");
  require(stats.maxSlotIs255 > 0u, tag + "maxSlot 255 not hit");
  require(stats.boundaryCases > 0u, tag + "derived boundary phases not hit");
  require(stats.mismatches == 0u, tag + "mismatches present");
}

void PrintGroupStats(const char* side, const GroupStats& stats) {
  std::cout << "[" << side << "] inputs=" << stats.inputs
            << " mismatches=" << stats.mismatches
            << " adapterEarlyExit=" << stats.adapterEarlyExit
            << " (NoSkinningData=" << stats.earlyNoSkinningData
            << ", NoPosePalette=" << stats.earlyNoPosePalette
            << ", NoVertexGroups=" << stats.earlyNoVertexGroups << ")"
            << " kernelInvocations=" << stats.kernelInvoked
            << " kernelOk=" << stats.kernelOk
            << " kernelOkAfterMainPathFailure="
            << stats.kernelOkAfterMainPathFailure << '\n';
  std::cout << "[" << side << "] kernelFailures: InvalidGroupTable="
            << stats.kernelInvalidGroupTable
            << " MatrixIndexOutOfRange=" << stats.kernelMatrixIndexOutOfRange
            << " VertexGroupOutOfRange=" << stats.kernelVertexGroupOutOfRange
            << " FallbacksFailed=" << stats.kernelFallbacksFailed
            << " usesAveragingTrue=" << stats.usesAveragingTrue
            << " maxSlot255=" << stats.maxSlotIs255
            << " boundaryCases=" << stats.boundaryCases << '\n';
}

int runAll() {
  std::cout
      << "war3_runtime_group_palette_kernel_diff_test\n"
      << "  scope: legacy pre-S2 palette builders (5-step core / 4-step upper) "
         "vs S2 shared kernel\n"
      << "  legacy identity: " << kLegacyCorePath << "\n"
      << "                   SHA-256 " << kLegacyCoreSha256 << "\n"
      << "                   " << kLegacyUpperPath << "\n"
      << "                   SHA-256 " << kLegacyUpperSha256 << "\n"
      << "  prng: SplitMix64 (state += 0x9E3779B97F4A7C15; z ^= z >> 30; "
         "z *= 0xBF58476D1CE4E5B9; z ^= z >> 27; z *= 0x94D049BB133111EB; "
         "z ^= z >> 31)\n"
      << "  seedCore=" << Hex64(kSeedCoreGroup)
      << " seedUpper=" << Hex64(kSeedUpperGroup)
      << " iterationsPerGroup=" << kGroupIterations << "\n"
      << "  adapter coverage: 用派生建模 + 内核差分覆盖两侧适配层的输入派生"
         "（core 的 128Ki clamp/min 链、upper 的 min 链）与三条前置校验；\n"
      << "    两个生产 TryBuildRuntimeGroupPalette 依赖 d3d9 记录类型与 TU "
         "内部状态，无法在宿主机链接，故不链接它们；\n"
      << "    适配层文本一致性由静态门禁 "
         "AutoTest/test_runtime_group_palette_kernel_single_source_static.py "
         "负责。\n"
      << "  not covered: 旧实现的来源链（producer 快照 / +0x08 槽位 / 记忆槽位 / "
         "Game.dll arena）与日志面。\n";

  GroupStats coreStats;
  GroupStats upperStats;
  RunCoreGroup(coreStats);
  RunUpperGroup(upperStats);
  RunCoreBoundaryFixtures(coreStats);
  RunUpperBoundaryFixtures(upperStats);

  PrintGroupStats("core", coreStats);
  PrintGroupStats("upper", upperStats);
  CheckCoverage("core", coreStats, true);
  CheckCoverage("upper", upperStats, false);

  RunCostMeasurement();

  std::cout << "\nwar3_runtime_group_palette_kernel_diff_test: SUMMARY"
            << " seedCore=" << Hex64(kSeedCoreGroup)
            << " seedUpper=" << Hex64(kSeedUpperGroup)
            << " iterationsPerGroup=" << kGroupIterations
            << " coreInputs=" << coreStats.inputs
            << " coreMismatches=" << coreStats.mismatches
            << " upperInputs=" << upperStats.inputs
            << " upperMismatches=" << upperStats.mismatches
            << " legacyCoreSha256=" << kLegacyCoreSha256
            << " legacyUpperSha256=" << kLegacyUpperSha256 << '\n';
  std::cout << "war3_runtime_group_palette_kernel_diff_test: 数值等价只作为进展；"
               "本测试不据此宣称热路径成本等价（成本差异见上方实测口径与"
               "不确定性说明）。\n";

  if (g_failures != 0u) {
    std::cerr << "war3_runtime_group_palette_kernel_diff_test: " << g_failures
              << " failure(s)\n";
    return 1;
  }
  if (coreStats.mismatches != 0u || upperStats.mismatches != 0u) {
    std::cerr << "war3_runtime_group_palette_kernel_diff_test: mismatches "
              << coreStats.mismatches << " (core) / " << upperStats.mismatches
              << " (upper)\n";
    return 1;
  }
  std::cout << "war3_runtime_group_palette_kernel_diff_test: 0 mismatch in "
            << coreStats.inputs << " core inputs + " << upperStats.inputs
            << " upper inputs\n";
  return 0;
}

}  // namespace

int main() { return runAll(); }
