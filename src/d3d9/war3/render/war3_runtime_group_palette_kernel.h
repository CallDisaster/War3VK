#pragma once

// 2026-09-17 S2（收窄版）：两个 TryBuildRuntimeGroupPalette 共享的纯计算内核。
//
// 只共享计算：最大槽位扫描 / 唯一槽位去重 / 矩阵重映射 / group 表校验 /
// 分组等权平均。设计依据 docs/plan/2026-09-17-s2-shared-compute-kernel-design.md
// §2.1（52 行逐语句相同片段）与 §3（签名草案）。
//
// 明确**不**共享（见设计 §2.2 D1-D8）：
//   * 来源选择链（shadow-core 的 producer 快照 / +0x08 槽位 / 记忆槽位 /
//     Game.dll arena 读取）仍全部留在 war3_shadow_renderer_core.cpp 的适配层；
//   * 失败诊断面（RuntimeGroupPaletteMissReason / logFailure / stats）留在各自适配层；
//   * 槽位计数与 groupCount 派生（D4/D5）由适配层算好后经 Input 传入，
//     内核内部**不做任何 min() 收窄**，也不替调用方决定上界。
//
// 硬规则：
//   1. 行结构与两侧实现逐语句一致（含 Matrix4 accum(0.0f) 起点、
//      groupSize == 1u ? accum : accum / float(groupSize)、
//      usesAveraging 只在 groupSize > 1u 置位、fallback 短路顺序不变）；
//   2. 任何"来源"概念（renderablePart / slot / arena / producer / Selection）
//      都不得进入本文件；
//   3. 本文件 0 处游戏进程裸地址读取；只读调用者提供的有效视图；
//      除 util_matrix.h 外不 include 任何项目头。

#include "../../../util/util_matrix.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace dxvk::war3::render {

// 只读视图：由各自的薄适配层从自己的记录类型填充，内核不认识任何一种记录。
// 所有权 / 有效期仍属调用方；内核不保存、不跨帧、不做任何内存读取。
struct RuntimeGroupPaletteInput {
  const uint8_t*  vertexGroupIndices   = nullptr;  // 槽位数组
  size_t          vertexGroupSlotCount = 0u;       // 适配层决定（core: 128Ki/vertexCount clamp；upper: min(vertexGroupCount,size)）
  const uint32_t* matrixGroupSizes     = nullptr;
  uint32_t        groupCount           = 0u;       // 适配层决定（core: size()；upper: min(matrixGroupCount,size)）
  const uint32_t* matrixIndices        = nullptr;
  size_t          matrixIndexCount     = 0u;
  const Matrix4*  posePalette          = nullptr;  // pose.matrixPalette.data()
  size_t          posePaletteSize      = 0u;       // pose.matrixPalette.size()
  uint32_t        poseMatrixCount      = 0u;       // pose.matrixCount
};

// 与 shadow-core 的 RuntimeGroupPaletteMissReason 取值一一对应，但独立定义，
// 避免内核依赖 shadow/ 的 TU 内部类型。
enum class RuntimeGroupPaletteKernelMiss : uint32_t {
  None = 0,
  NoSkinningData,
  NoPosePalette,
  NoVertexGroups,
  InvalidGroupTable,
  MatrixIndexOutOfRange,
  VertexGroupOutOfRange,
  FallbacksFailed,
};

enum class RuntimeGroupPaletteAliasRelation : uint8_t {
  NonOverlapping = 0,
  Overlapping = 1,
  Invalid = 2,
};

// 纯整数半开区间 [begin, begin+byteLength) 分类，不读取任何地址。
// 零长度不重叠；起点/长度造成地址溢出时返回 Invalid，由调用方 fail-closed。
inline RuntimeGroupPaletteAliasRelation
ClassifyRuntimeGroupPaletteByteRangeOverlap(
    uintptr_t lhsBegin, uint64_t lhsByteLength,
    uintptr_t rhsBegin, uint64_t rhsByteLength) {
  if (lhsByteLength == 0u || rhsByteLength == 0u)
    return RuntimeGroupPaletteAliasRelation::NonOverlapping;

  const uint64_t maxAddress = uint64_t(UINTPTR_MAX);
  const uint64_t lhsBegin64 = uint64_t(lhsBegin);
  const uint64_t rhsBegin64 = uint64_t(rhsBegin);
  if (lhsByteLength > maxAddress - lhsBegin64 ||
      rhsByteLength > maxAddress - rhsBegin64)
    return RuntimeGroupPaletteAliasRelation::Invalid;

  const uint64_t lhsEnd = lhsBegin64 + lhsByteLength;
  const uint64_t rhsEnd = rhsBegin64 + rhsByteLength;
  if (lhsEnd <= rhsBegin64 || rhsEnd <= lhsBegin64)
    return RuntimeGroupPaletteAliasRelation::NonOverlapping;
  return RuntimeGroupPaletteAliasRelation::Overlapping;
}

// 元素数 * 元素大小 -> byte 长度；乘法溢出返回 false。
inline bool RuntimeGroupPaletteTryByteLength(
    size_t elementCount, size_t elementSize, uint64_t& outByteLength) {
  outByteLength = 0u;
  if (elementSize == 0u)
    return true;
  if (elementCount > (std::numeric_limits<size_t>::max)() / elementSize)
    return false;
  outByteLength = uint64_t(elementCount) * uint64_t(elementSize);
  return true;
}

struct RuntimeGroupPaletteKernelDetail {
  RuntimeGroupPaletteKernelMiss reason = RuntimeGroupPaletteKernelMiss::None;
  uint32_t group = UINT32_MAX;
  uint32_t matrixIndex = UINT32_MAX;
  uint32_t poseCount = 0u;
  uint32_t groupCount = 0u;
  uint32_t maxVertexGroupSlot = 0u;
  uint32_t matrixIndexCount = 0u;
  bool     aliasedInputOutput = false;
  RuntimeGroupPaletteAliasRelation aliasRelation =
      RuntimeGroupPaletteAliasRelation::NonOverlapping;
};

// 行为差异必须用参数表达，不得用"取 min"抹平：
enum class RuntimeGroupPaletteFallbackSet : uint8_t {
  MatrixAndPoseRemap       = 0,  // 4 步：direct / sparse / directPose / sparsePose（upper-layer 现状）
  MatrixPoseAndUniformRoot = 1,  // 5 步：+ 广播根矩阵                       （shadow-core 现状）
};

struct RuntimeGroupPaletteOutput {
  std::vector<Matrix4> palette;
  uint32_t maxVertexGroupSlot = 0u;
  bool     usesAveraging = false;
};

// 片段 1（可分离）：等价于两侧相同的"最大槽位扫描 + 唯一槽位去重"。
// 单独暴露是为了让 core 保持"去重发生在来源链之后"的惰性时序。
//
// 2026-09-20 b02 成本/安全收口（仅注释，不改行为）：内核使用单趟有界扫描同时
// 求 maxSlot 与 unique，固定 256 项数组覆盖 uint8 槽位域，不再接受 caller-provided
// trusted max，也不再为 unique 分配 vector。core 来源链仍先用 Find... 求 max（1 趟），
// 随后内核单趟，总计 2 趟；upper 直接内核单趟。生产适配层直接复用调用方 palette
// vector。实测分配/容量/趟数见 war3_runtime_group_palette_kernel_diff_test.cpp。
struct RuntimeGroupPaletteSlotScan {
  uint32_t maxVertexGroupSlot = 0u;
  std::vector<uint32_t> uniqueGroupSlots;   // 出现顺序、已去重
};

inline uint32_t FindRuntimeGroupPaletteMaxSlot(
    const uint8_t* vertexGroupIndices, size_t vertexGroupSlotCount) {
  uint32_t maxVertexGroupSlot = 0u;

  // 只读视图自洽性守卫：nullptr 视图不扫描。适配层不会带 count 传 null
  // （来源是 std::vector::data()），这里只是拒绝 UB，不收窄任何计数上界。
  if (vertexGroupIndices == nullptr)
    return maxVertexGroupSlot;

  for (size_t i = 0u; i < vertexGroupSlotCount; ++i) {
    const uint8_t groupSlot = vertexGroupIndices[i];
    maxVertexGroupSlot =
        (std::max)(maxVertexGroupSlot, uint32_t(groupSlot));
  }
  return maxVertexGroupSlot;
}

struct RuntimeGroupPaletteSlotScanFixed {
  uint32_t maxVertexGroupSlot = 0u;
  std::array<uint8_t, 256> uniqueGroupSlots = {};
  uint32_t uniqueGroupSlotCount = 0u;
};

// 单趟有界扫描：同时求 maxSlot 与按首次出现顺序去重。slot 是 uint8 域，
// 固定 256 项 seen/输出数组即可覆盖全值域，内核不再为 unique 结果做堆分配。
inline RuntimeGroupPaletteSlotScanFixed ScanRuntimeGroupPaletteSlotsFixed(
    const uint8_t* vertexGroupIndices, size_t vertexGroupSlotCount) {
  RuntimeGroupPaletteSlotScanFixed scan = {};

  if (vertexGroupIndices == nullptr)
    return scan;

  std::array<bool, 256> seenGroupSlots = {};
  for (size_t i = 0u; i < vertexGroupSlotCount; ++i) {
    const uint8_t groupSlot = vertexGroupIndices[i];
    scan.maxVertexGroupSlot =
        (std::max)(scan.maxVertexGroupSlot, uint32_t(groupSlot));
    if (!seenGroupSlots[groupSlot]) {
      seenGroupSlots[groupSlot] = true;
      scan.uniqueGroupSlots[scan.uniqueGroupSlotCount] = groupSlot;
      ++scan.uniqueGroupSlotCount;
    }
  }
  return scan;
}

// 公开测试兼容接口：仅把上面的单趟固定结果搬到 vector；不是第二套算法。
inline RuntimeGroupPaletteSlotScan ScanRuntimeGroupPaletteSlots(
    const uint8_t* vertexGroupIndices, size_t vertexGroupSlotCount) {
  RuntimeGroupPaletteSlotScan scan = {};
  const RuntimeGroupPaletteSlotScanFixed fixed =
      ScanRuntimeGroupPaletteSlotsFixed(vertexGroupIndices, vertexGroupSlotCount);
  scan.maxVertexGroupSlot = fixed.maxVertexGroupSlot;
  scan.uniqueGroupSlots.reserve(size_t(fixed.uniqueGroupSlotCount));
  for (uint32_t i = 0u; i < fixed.uniqueGroupSlotCount; ++i)
    scan.uniqueGroupSlots.push_back(uint32_t(fixed.uniqueGroupSlots[i]));
  return scan;
}

// 共享接口的别名防御：不读取任何矩阵，也不构造 data()+capacity 端点指针。
// 只看 output storage capacity 与 posePalette 有效视图的 byte 半开区间。
inline RuntimeGroupPaletteAliasRelation
ClassifyRuntimeGroupPaletteOutputPoseAlias(
    const std::vector<Matrix4>& outPalette,
    const RuntimeGroupPaletteInput& in) {
  if (in.posePalette == nullptr || in.posePaletteSize == 0u ||
      outPalette.capacity() == 0u)
    return RuntimeGroupPaletteAliasRelation::NonOverlapping;

  uint64_t outByteLength = 0u;
  uint64_t poseByteLength = 0u;
  if (!RuntimeGroupPaletteTryByteLength(outPalette.capacity(), sizeof(Matrix4),
                                        outByteLength) ||
      !RuntimeGroupPaletteTryByteLength(in.posePaletteSize, sizeof(Matrix4),
                                        poseByteLength))
    return RuntimeGroupPaletteAliasRelation::Invalid;

  return ClassifyRuntimeGroupPaletteByteRangeOverlap(
      reinterpret_cast<uintptr_t>(outPalette.data()), outByteLength,
      reinterpret_cast<uintptr_t>(in.posePalette), poseByteLength);
}

// 片段 2：前置校验 + （4/5 步）fallback + 分组等权平均。
// 前置校验与 miss 语义严格按设计 §2.1 的行结构实现，不引入任何新的 clamp。
//
// 生产接口直接借用调用方的 palette vector，避免局部输出 + move 造成调用方容量
// 失效；内核单趟有界扫描同时求 maxSlot 与 unique，不接受 caller-provided trusted max。
inline bool TryBuildRuntimeGroupPaletteKernel(
    const RuntimeGroupPaletteInput& in,
    RuntimeGroupPaletteFallbackSet fallbackSet,
    std::vector<Matrix4>& outPalette,
    uint32_t& outMaxVertexGroupSlot,
    bool& outUsesAveraging,
    RuntimeGroupPaletteKernelDetail* outDetail = nullptr) {
  const RuntimeGroupPaletteAliasRelation aliasRelation =
      ClassifyRuntimeGroupPaletteOutputPoseAlias(outPalette, in);

  outPalette.clear();
  outMaxVertexGroupSlot = 0u;
  outUsesAveraging = false;
  if (outDetail != nullptr)
    *outDetail = {};

  auto noteMiss = [&](RuntimeGroupPaletteKernelMiss reason,
                      uint32_t group = UINT32_MAX,
                      uint32_t matrixIndex = UINT32_MAX) {
    if (outDetail == nullptr)
      return;
    outDetail->reason = reason;
    outDetail->group = group;
    outDetail->matrixIndex = matrixIndex;
    outDetail->poseCount = in.poseMatrixCount;
    // core 的 NoteRuntimeGroupPaletteMiss 用 matrixGroupSizes.size()；
    // 适配层把该值原样放进 in.groupCount（upper 用 min 派生，且不消费 detail）。
    outDetail->groupCount = in.groupCount;
    outDetail->maxVertexGroupSlot = outMaxVertexGroupSlot;
    outDetail->matrixIndexCount = uint32_t(in.matrixIndexCount);
  };

  if (aliasRelation != RuntimeGroupPaletteAliasRelation::NonOverlapping) {
    if (outDetail != nullptr) {
      noteMiss(RuntimeGroupPaletteKernelMiss::FallbacksFailed);
      outDetail->aliasedInputOutput =
          aliasRelation == RuntimeGroupPaletteAliasRelation::Overlapping;
      outDetail->aliasRelation = aliasRelation;
    }
    return false;
  }

  // 两侧适配层都已先做过同样的前置校验（core 6592-6610 / upper 107-114）；
  // 内核再校验一次，保证任何视图组合都不会越界读，并把同样的 reason 报回。
  if (in.posePalette == nullptr || in.posePaletteSize == 0u ||
      in.poseMatrixCount == 0u) {
    noteMiss(RuntimeGroupPaletteKernelMiss::NoPosePalette);
    return false;
  }
  if (in.vertexGroupIndices == nullptr || in.vertexGroupSlotCount == 0u) {
    noteMiss(RuntimeGroupPaletteKernelMiss::NoVertexGroups);
    return false;
  }
  // 2026-09-17 对抗性复核修正：groupCount > 0 却给出空 group 表视图属调用方违约。
  // 这里显式拒绝而不是在下面解引用空指针（两个生产调用方都以 vector::data() +
  // size() 成对传入，因此该组合不会出现；此守卫是契约硬化，不改变既有行为）。
  if (in.groupCount != 0u && in.matrixGroupSizes == nullptr) {
    noteMiss(RuntimeGroupPaletteKernelMiss::InvalidGroupTable);
    return false;
  }
  // 同形守卫：matrixIndexCount > 0 却给出空 matrixIndices 视图时，主路径与
  // direct/sparse 重映射都会在 maxSlot < matrixIndexCount 时解引用它。
  // （2026-09-17 对抗性复核 + 子代理探针实测：无此守卫会访问违例 0xC0000005。）
  if (in.matrixIndexCount != 0u && in.matrixIndices == nullptr) {
    noteMiss(RuntimeGroupPaletteKernelMiss::InvalidGroupTable);
    return false;
  }

  const RuntimeGroupPaletteSlotScanFixed scan =
      ScanRuntimeGroupPaletteSlotsFixed(in.vertexGroupIndices,
                                        in.vertexGroupSlotCount);
  outMaxVertexGroupSlot = scan.maxVertexGroupSlot;
  const uint8_t* uniqueGroupSlots = scan.uniqueGroupSlots.data();
  const uint32_t uniqueGroupSlotCount = scan.uniqueGroupSlotCount;

  // shadow-core 在来源链之后无条件写下一条 reason=None 的记录（upper-layer
  // 不传 detail，无副作用）。这里复刻同一记录，使适配层映射后的
  // RuntimeGroupPaletteMissDetail 与抽取前逐字段一致。
  noteMiss(RuntimeGroupPaletteKernelMiss::None);

  auto buildDirectMatrixRemap = [&]() -> bool {
    if (in.matrixIndexCount == 0u ||
        size_t(outMaxVertexGroupSlot) >= in.matrixIndexCount) {
      return false;
    }

    outPalette.resize(size_t(outMaxVertexGroupSlot) + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group) {
      const uint32_t matrixIndex = in.matrixIndices[group];
      if (matrixIndex >= in.poseMatrixCount ||
          matrixIndex >= in.posePaletteSize) {
        return false;
      }
      outPalette[group] = in.posePalette[matrixIndex];
    }
    return true;
  };

  auto buildSparseMatrixRemap = [&]() -> bool {
    if (in.matrixIndexCount == 0u || uniqueGroupSlotCount == 0u ||
        size_t(uniqueGroupSlotCount) > in.matrixIndexCount) {
      return false;
    }

    outPalette.assign(size_t(outMaxVertexGroupSlot) + 1u, Matrix4(0.0f));
    for (size_t i = 0; i < size_t(uniqueGroupSlotCount); ++i) {
      const uint32_t matrixIndex = in.matrixIndices[i];
      if (matrixIndex >= in.poseMatrixCount ||
          matrixIndex >= in.posePaletteSize) {
        return false;
      }
      outPalette[uint32_t(uniqueGroupSlots[i])] = in.posePalette[matrixIndex];
    }
    return true;
  };

  auto buildDirectPosePalette = [&]() -> bool {
    if (outMaxVertexGroupSlot >= in.poseMatrixCount ||
        size_t(outMaxVertexGroupSlot) >= in.posePaletteSize) {
      return false;
    }

    outPalette.resize(size_t(outMaxVertexGroupSlot) + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group)
      outPalette[group] = in.posePalette[group];
    return true;
  };

  auto buildSparsePosePalette = [&]() -> bool {
    if (uniqueGroupSlotCount == 0u ||
        size_t(uniqueGroupSlotCount) > in.poseMatrixCount ||
        size_t(uniqueGroupSlotCount) > in.posePaletteSize) {
      return false;
    }

    outPalette.assign(size_t(outMaxVertexGroupSlot) + 1u, Matrix4(0.0f));
    for (size_t i = 0; i < size_t(uniqueGroupSlotCount); ++i)
      outPalette[uint32_t(uniqueGroupSlots[i])] = in.posePalette[i];
    return true;
  };

  // shadow-core 独有第 5 步：把根矩阵广播到 max(maxSlot+1, groupCount)。
  // 它只在 MatrixPoseAndUniformRoot 集合里参与短路链，且必须保持在链尾。
  auto buildUniformPosePalette = [&]() -> bool {
    if (in.poseMatrixCount == 0u || in.posePaletteSize == 0u)
      return false;

    const uint32_t paletteCount =
        (std::max)(outMaxVertexGroupSlot + 1u, in.groupCount);
    if (paletteCount == 0u)
      return false;

    outPalette.assign(size_t(paletteCount), in.posePalette[0]);
    return true;
  };

  auto tryFallbacks = [&]() -> bool {
    const bool ok = buildDirectMatrixRemap() || buildSparseMatrixRemap() ||
                    buildDirectPosePalette() || buildSparsePosePalette() ||
                    (fallbackSet ==
                         RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot &&
                     buildUniformPosePalette());
    if (!ok)
      noteMiss(RuntimeGroupPaletteKernelMiss::FallbacksFailed);
    return ok;
  };

  const uint32_t groupCount = in.groupCount;
  if (groupCount == 0u) {
    return tryFallbacks();
  }

  std::vector<uint32_t> prefix(groupCount, 0u);
  uint32_t running = 0u;
  for (uint32_t i = 0u; i < groupCount; ++i) {
    prefix[i] = running;
    running += in.matrixGroupSizes[i];
  }
  if (running > in.matrixIndexCount) {
    noteMiss(RuntimeGroupPaletteKernelMiss::InvalidGroupTable);
    return tryFallbacks();
  }

  outPalette.resize(size_t(groupCount));
  for (uint32_t group = 0u; group < groupCount; ++group) {
    const uint32_t groupSize = in.matrixGroupSizes[group];
    const uint32_t groupBase = prefix[group];
    if (groupSize == 0u ||
        (groupBase + groupSize) > in.matrixIndexCount) {
      noteMiss(RuntimeGroupPaletteKernelMiss::InvalidGroupTable, group);
      return tryFallbacks();
    }

    Matrix4 accum(0.0f);
    for (uint32_t i = 0u; i < groupSize; ++i) {
      const uint32_t matrixIndex = in.matrixIndices[groupBase + i];
      if (matrixIndex >= in.poseMatrixCount ||
          matrixIndex >= in.posePaletteSize) {
        noteMiss(RuntimeGroupPaletteKernelMiss::MatrixIndexOutOfRange, group,
                 matrixIndex);
        return tryFallbacks();
      }
      accum += in.posePalette[matrixIndex];
    }

    if (groupSize > 1u)
      outUsesAveraging = true;
    outPalette[group] =
        groupSize == 1u ? accum : (accum / float(groupSize));
  }

  for (size_t i = 0u; i < in.vertexGroupSlotCount; ++i) {
    const uint8_t groupSlot = in.vertexGroupIndices[i];
    if (uint32_t(groupSlot) >= groupCount) {
      noteMiss(RuntimeGroupPaletteKernelMiss::VertexGroupOutOfRange,
               uint32_t(groupSlot));
      return tryFallbacks();
    }
  }

  return true;
}

// 测试/旧调用点兼容：仅把已有 RuntimeGroupPaletteOutput 转成直接借用形式；
// 计算仍由上面的唯一实现完成，不是第二实现。
inline bool TryBuildRuntimeGroupPaletteKernel(
    const RuntimeGroupPaletteInput& in,
    RuntimeGroupPaletteFallbackSet fallbackSet,
    RuntimeGroupPaletteOutput& out,
    RuntimeGroupPaletteKernelDetail* outDetail = nullptr) {
  return TryBuildRuntimeGroupPaletteKernel(
      in, fallbackSet, out.palette, out.maxVertexGroupSlot, out.usesAveraging,
      outDetail);
}

} // namespace dxvk::war3::render
