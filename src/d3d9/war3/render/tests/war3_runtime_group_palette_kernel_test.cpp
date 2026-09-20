#include "../war3_runtime_group_palette_kernel.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

// 2026-09-17 S2（收窄版）生产函数级测试：共享纯计算内核的行为合同（设计 §4 T1-T13
// + 对抗性复核补测 T14-T17：空视图守卫、退化指针视图、fallback 后 usesAveraging 保全、
// 五个失败入口的残留逐项对齐）。
// 全部断言直接调用 ScanRuntimeGroupPaletteSlots / TryBuildRuntimeGroupPaletteKernel，
// 不做任何字符串静态检查；两个 fallback 集合的差异（D2）与两侧计数派生差异（D4/D5）
// 都由参数显式构造并逐位断言。

namespace {

using dxvk::Matrix4;
using dxvk::war3::render::RuntimeGroupPaletteFallbackSet;
using dxvk::war3::render::ClassifyRuntimeGroupPaletteByteRangeOverlap;
using dxvk::war3::render::ClassifyRuntimeGroupPaletteOutputPoseAlias;
using dxvk::war3::render::RuntimeGroupPaletteAliasRelation;
using dxvk::war3::render::RuntimeGroupPaletteInput;
using dxvk::war3::render::RuntimeGroupPaletteKernelDetail;
using dxvk::war3::render::RuntimeGroupPaletteKernelMiss;
using dxvk::war3::render::RuntimeGroupPaletteOutput;
using dxvk::war3::render::RuntimeGroupPaletteSlotScan;
using dxvk::war3::render::ScanRuntimeGroupPaletteSlots;
using dxvk::war3::render::TryBuildRuntimeGroupPaletteKernel;

constexpr uint32_t kNoIndex = UINT32_MAX;

uint32_t g_failures = 0u;

bool require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "war3_runtime_group_palette_kernel_test: FAIL " << message
              << '\n';
    ++g_failures;
  }
  return condition;
}

// 每个元素都可辨识的矩阵；不含 -0.0f / NaN，保证逐位比较有意义。
Matrix4 makeMatrix(float seed) {
  Matrix4 m(0.0f);
  for (uint32_t row = 0u; row < 4u; ++row) {
    for (uint32_t column = 0u; column < 4u; ++column) {
      m[row][column] =
          seed + float(row) * 0.25f + float(column) * 0.0625f + 1.0f;
    }
  }
  return m;
}

bool matrixEqual(const Matrix4& a, const Matrix4& b) {
  for (uint32_t row = 0u; row < 4u; ++row) {
    for (uint32_t column = 0u; column < 4u; ++column) {
      if (a[row][column] != b[row][column])
        return false;
    }
  }
  return true;
}

std::vector<Matrix4> makePose(uint32_t count) {
  std::vector<Matrix4> pose;
  pose.reserve(count);
  for (uint32_t i = 0u; i < count; ++i)
    pose.push_back(makeMatrix(float(i) + 1.0f));
  return pose;
}

struct Fixture {
  std::vector<uint8_t> slots;
  std::vector<uint32_t> groupSizes;
  std::vector<uint32_t> matrixIndices;
  std::vector<Matrix4> pose;
  // 显式计数覆盖：size_t(-1) 表示"取容器大小"，用于构造 D4/D5 派生差异。
  size_t slotCountOverride = size_t(-1);
  uint32_t groupCountOverride = kNoIndex;
  uint32_t poseCountOverride = kNoIndex;

  RuntimeGroupPaletteInput makeInput() const {
    RuntimeGroupPaletteInput in = {};
    in.vertexGroupIndices = slots.data();
    in.vertexGroupSlotCount =
        slotCountOverride != size_t(-1) ? slotCountOverride : slots.size();
    in.matrixGroupSizes = groupSizes.data();
    in.groupCount = groupCountOverride != kNoIndex
                        ? groupCountOverride
                        : uint32_t(groupSizes.size());
    in.matrixIndices = matrixIndices.data();
    in.matrixIndexCount = matrixIndices.size();
    in.posePalette = pose.data();
    in.posePaletteSize = pose.size();
    in.poseMatrixCount =
        poseCountOverride != kNoIndex ? poseCountOverride : uint32_t(pose.size());
    return in;
  }
};

struct BuildResult {
  bool ok = false;
  RuntimeGroupPaletteOutput out;
  RuntimeGroupPaletteKernelDetail detail;
};

BuildResult run(const RuntimeGroupPaletteInput& in,
                RuntimeGroupPaletteFallbackSet fallbackSet) {
  BuildResult result;
  result.ok = TryBuildRuntimeGroupPaletteKernel(in, fallbackSet, result.out,
                                                &result.detail);
  return result;
}

// 断言 palette 的每个槽位：appearance 中给出的槽位必须等于 pose[poseIndex]，
// 其余槽位必须是零矩阵。
bool requirePaletteSlots(
    const std::vector<Matrix4>& palette,
    const std::vector<std::pair<size_t, Matrix4>>& expected) {
  for (size_t i = 0u; i < palette.size(); ++i) {
    Matrix4 want(0.0f);
    bool wantSet = false;
    for (const auto& entry : expected) {
      if (entry.first == i) {
        want = entry.second;
        wantSet = true;
        break;
      }
    }
    if (!wantSet)
      want = Matrix4(0.0f);
    if (!matrixEqual(palette[i], want)) {
      std::cerr << "war3_runtime_group_palette_kernel_test: slot " << i
                << " mismatch\n";
      ++g_failures;
      return false;
    }
  }
  return true;
}

// ---- T1 分组等权平均（逐位） ----
bool testT1GroupAveraging() {
  Fixture f;
  f.slots = {0u, 0u, 0u};
  f.groupSizes = {3u, 1u};
  f.matrixIndices = {0u, 1u, 2u, 0u};
  f.pose = makePose(3u);
  const BuildResult r =
      run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);

  require(r.ok, "T1 returns true");
  require(r.out.palette.size() == 2u, "T1 palette.size() == groupCount");
  Matrix4 accum(0.0f);
  accum += f.pose[0];
  accum += f.pose[1];
  accum += f.pose[2];
  require(matrixEqual(r.out.palette[0], accum / 3.0f),
          "T1 palette[0] == (p0+p1+p2)/3.0f bitwise");
  require(matrixEqual(r.out.palette[1], f.pose[0]),
          "T1 groupSize==1 palette[1] == p0 bitwise");
  require(r.out.usesAveraging, "T1 usesAveraging == true");
  require(r.out.maxVertexGroupSlot == 0u, "T1 maxVertexGroupSlot == 0");
  require(r.detail.reason == RuntimeGroupPaletteKernelMiss::None,
          "T1 detail reason == None");
  require(r.detail.poseCount == 3u && r.detail.groupCount == 2u,
          "T1 detail counts");
  return true;
}

// ---- T2 单元素组不做除法 ----
bool testT2SingleElementGroup() {
  Fixture f;
  f.slots = {0u, 1u};
  f.groupSizes = {1u, 1u};
  f.matrixIndices = {0u, 1u};
  f.pose = makePose(2u);
  const BuildResult r =
      run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);

  require(r.ok, "T2 returns true");
  require(r.out.palette.size() == 2u, "T2 palette.size() == 2");
  require(matrixEqual(r.out.palette[0], f.pose[0]),
          "T2 palette[0] == p0 bitwise (no division)");
  require(matrixEqual(r.out.palette[1], f.pose[1]),
          "T2 palette[1] == p1 bitwise (no division)");
  require(!r.out.usesAveraging, "T2 usesAveraging == false");
  return true;
}

// ---- T3 usesAveraging 只由 groupSize > 1 置位 ----
bool testT3UsesAveragingGate() {
  {
    Fixture f;
    f.slots = {0u, 1u, 2u};
    f.groupSizes = {1u, 1u, 1u};
    f.matrixIndices = {0u, 1u, 2u};
    f.pose = makePose(3u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok, "T3a returns true");
    require(!r.out.usesAveraging, "T3a all groupSize==1 -> usesAveraging false");
  }
  {
    Fixture f;
    f.slots = {0u, 0u, 1u};
    f.groupSizes = {2u, 1u};
    f.matrixIndices = {0u, 1u, 0u};
    f.pose = makePose(2u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok, "T3b returns true");
    require(r.out.usesAveraging, "T3b any groupSize>1 -> usesAveraging true");
  }
  {
    // 该输入的主路径在置位 usesAveraging 之前就已失败（第 0 组 groupSize==0），
    // 因此 fallback 成功后该标志仍为 false。
    // 注意（2026-09-17 对抗性复核更正）：**fallback 本身不重置该标志**，
    // "fallback 路径不会置位 usesAveraging" 的说法不成立；主路径已置位后被
    // fallback 救回的情形见 T16（与抽取前行为一致，不是回归）。
    Fixture f;
    f.slots = {0u, 0u};
    f.groupSizes = {0u};
    f.matrixIndices = {1u, 0u};
    f.pose = makePose(2u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok, "T3c fallback returns true");
    require(!r.out.usesAveraging,
            "T3c flag stays false because the main path failed before setting it");
  }
  return true;
}

// ---- T4 输出尺寸 / 稀疏路径零填充 ----
bool testT4OutputShape() {
  {
    // 平均路径：size == groupCount。
    Fixture f;
    f.slots = {0u, 1u};
    f.groupSizes = {2u, 1u};
    f.matrixIndices = {0u, 0u, 1u};
    f.pose = makePose(2u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok && r.out.palette.size() == 2u,
            "T4 averaging path size == groupCount");
  }
  {
    // buildSparseMatrixRemap：size == maxSlot+1，未写槽位为 Matrix4(0.0f)。
    Fixture f;
    f.slots = {3u, 1u};
    f.groupSizes = {0u};
    f.matrixIndices = {5u, 6u, 99u, 99u};
    f.pose = makePose(8u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok, "T4 sparse matrix remap returns true");
    require(r.out.palette.size() == 4u,
            "T4 sparse matrix remap size == maxSlot+1");
    require(matrixEqual(r.out.palette[0], Matrix4(0.0f)) &&
                matrixEqual(r.out.palette[2], Matrix4(0.0f)),
            "T4 sparse matrix remap leaves unwritten slots zero");
    require(matrixEqual(r.out.palette[3], f.pose[5]) &&
                matrixEqual(r.out.palette[1], f.pose[6]),
            "T4 sparse matrix remap slot->matrixIndices[i] mapping");
  }
  {
    // buildSparsePosePalette：size == maxSlot+1，未写槽位为 Matrix4(0.0f)。
    Fixture f;
    f.slots = {5u, 1u};
    f.groupSizes = {0u};
    f.matrixIndices = {99u, 99u};
    f.pose = makePose(3u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok, "T4 sparse pose palette returns true");
    require(r.out.palette.size() == 6u,
            "T4 sparse pose palette size == maxSlot+1");
    require(matrixEqual(r.out.palette[5], f.pose[0]) &&
                matrixEqual(r.out.palette[1], f.pose[1]),
            "T4 sparse pose palette uniqueSlots[i] -> pose[i]");
    require(matrixEqual(r.out.palette[0], Matrix4(0.0f)) &&
                matrixEqual(r.out.palette[2], Matrix4(0.0f)) &&
                matrixEqual(r.out.palette[3], Matrix4(0.0f)) &&
                matrixEqual(r.out.palette[4], Matrix4(0.0f)),
            "T4 sparse pose palette zero tail");
  }
  return true;
}

// ---- T5 最大槽位扫描 / 去重 ----
bool testT5SlotScan() {
  {
    const std::vector<uint8_t> slots = {0u, 5u, 5u, 2u};
    const RuntimeGroupPaletteSlotScan scan =
        ScanRuntimeGroupPaletteSlots(slots.data(), slots.size());
    require(scan.maxVertexGroupSlot == 5u, "T5 max slot scan == 5");
    require(scan.uniqueGroupSlots.size() == 3u &&
                scan.uniqueGroupSlots[0] == 0u &&
                scan.uniqueGroupSlots[1] == 5u &&
                scan.uniqueGroupSlots[2] == 2u,
            "T5 unique slots keep appearance order without duplicates");
  }
  {
    const std::vector<uint8_t> slots = {255u};
    const RuntimeGroupPaletteSlotScan scan =
        ScanRuntimeGroupPaletteSlots(slots.data(), slots.size());
    require(scan.maxVertexGroupSlot == 255u, "T5 max slot scan == 255");
  }
  {
    // 只读前缀：slotCount 是唯一上界来源，容器更长的尾部不得进入扫描。
    const std::vector<uint8_t> slots = {0u, 5u, 255u, 2u};
    require(ScanRuntimeGroupPaletteSlots(slots.data(), 1u).maxVertexGroupSlot ==
                0u,
            "T5 slotCount==1 scans only slot 0");
    require(ScanRuntimeGroupPaletteSlots(slots.data(), 2u).maxVertexGroupSlot ==
                5u,
            "T5 slotCount==2 scans prefix {0,5}");
    require(ScanRuntimeGroupPaletteSlots(slots.data(), 4u).maxVertexGroupSlot ==
                255u,
            "T5 slotCount==4 sees the container tail");
    require(ScanRuntimeGroupPaletteSlots(slots.data(), 0u).uniqueGroupSlots
                .empty(),
            "T5 slotCount==0 yields no unique slots");
  }
  return true;
}

// ---- T6 失败路径逐条 reason / group / matrixIndex ----
bool testT6FailureReasons() {
  {
    // groupCount == 0 -> 走 fallback 集合。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok, "T6 groupCount==0 falls back and succeeds");
    require(r.out.palette.size() == 1u &&
                matrixEqual(r.out.palette[0], f.pose[0]),
            "T6 groupCount==0 direct remap result");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::None,
            "T6 groupCount==0 detail reason None");
  }
  {
    // running > matrixIndexCount -> InvalidGroupTable（无 group 序号）。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {3u};
    f.matrixIndices = {0u, 1u};
    f.pose = makePose(2u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::InvalidGroupTable,
            "T6 running > matrixIndexCount -> InvalidGroupTable");
    require(r.detail.group == kNoIndex,
            "T6 running check carries no group index");
    require(r.ok, "T6 running check still rescues via fallback");
  }
  {
    // 某组 groupSize == 0 -> InvalidGroupTable 且附 group 序号。
    Fixture f;
    f.slots = {0u, 1u, 2u};
    f.groupSizes = {1u, 0u, 1u};
    f.matrixIndices = {0u, 0u, 1u};
    f.pose = makePose(3u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::InvalidGroupTable,
            "T6 groupSize==0 -> InvalidGroupTable");
    require(r.detail.group == 1u, "T6 groupSize==0 reports group 1");
    require(r.detail.matrixIndex == kNoIndex,
            "T6 groupSize==0 has no matrix index");
  }
  {
    // groupBase + groupSize 越过 matrixIndexCount（uint32 求和溢出下可达）。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {0xFFFFFFFFu, 2u};
    f.matrixIndices = {0u, 0u, 0u, 0u, 0u};
    f.pose = makePose(2u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::InvalidGroupTable,
            "T6 groupBase+groupSize overrun -> InvalidGroupTable");
    require(r.detail.group == 0u, "T6 groupBase+groupSize reports group 0");
  }
  {
    // matrixIndex >= poseMatrixCount（|| 左侧）。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u};
    f.matrixIndices = {5u};
    f.pose = makePose(8u);
    f.poseCountOverride = 5u;
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.detail.reason ==
                RuntimeGroupPaletteKernelMiss::MatrixIndexOutOfRange,
            "T6 matrixIndex>=poseMatrixCount -> MatrixIndexOutOfRange");
    require(r.detail.group == 0u && r.detail.matrixIndex == 5u,
            "T6 matrixIndex>=poseMatrixCount carries group/matrixIndex");
    require(r.ok, "T6 matrix index miss still rescues via direct pose palette");
  }
  {
    // matrixIndex < poseMatrixCount 但 >= posePaletteSize（|| 右侧）。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u};
    f.matrixIndices = {5u};
    f.pose = makePose(3u);
    f.poseCountOverride = 8u;
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.detail.reason ==
                RuntimeGroupPaletteKernelMiss::MatrixIndexOutOfRange,
            "T6 matrixIndex>=posePaletteSize -> MatrixIndexOutOfRange");
    require(r.detail.group == 0u && r.detail.matrixIndex == 5u,
            "T6 palette-size arm carries group/matrixIndex");
  }
  {
    // 某顶点槽位 >= groupCount -> VertexGroupOutOfRange（附 groupSlot）。
    Fixture f;
    f.slots = {0u, 1u, 5u};
    f.groupSizes = {1u, 1u};
    f.matrixIndices = {0u, 1u};
    f.pose = makePose(2u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok, "T6 vertex slot range miss rescues via uniform broadcast");
    require(r.detail.reason ==
                RuntimeGroupPaletteKernelMiss::VertexGroupOutOfRange,
            "T6 slot>=groupCount -> VertexGroupOutOfRange");
    require(r.detail.group == 5u,
            "T6 VertexGroupOutOfRange carries the offending slot");
    require(r.out.palette.size() == 6u,
            "T6 uniform broadcast size == max(maxSlot+1, groupCount)");
    for (size_t i = 0u; i < r.out.palette.size(); ++i)
      require(matrixEqual(r.out.palette[i], f.pose[0]),
              "T6 uniform broadcast equals pose front");
  }
  {
    // 4 步全部失败 -> FallbacksFailed。
    Fixture f;
    f.slots = {0u, 1u, 2u};
    f.groupSizes = {3u};
    f.matrixIndices = {0u, 0u, 99u};
    f.pose = makePose(2u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T6 matrix-set fallbacks all failed -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
            "T6 all fallbacks failed -> FallbacksFailed");
  }
  return true;
}

// ---- T7 fallback 集合差异（D2 行为守卫） ----
bool testT7FallbackSetDifference() {
  Fixture f;
  f.slots = {0u, 1u, 2u};
  f.groupSizes = {3u};
  f.matrixIndices = {0u, 0u, 99u};
  f.pose = makePose(2u);

  const BuildResult remapOnly =
      run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
  require(!remapOnly.ok,
          "T7 MatrixAndPoseRemap (upper-layer) returns false");
  require(remapOnly.detail.reason ==
              RuntimeGroupPaletteKernelMiss::FallbacksFailed,
          "T7 MatrixAndPoseRemap detail == FallbacksFailed");

  const BuildResult withUniform = run(
      f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
  require(withUniform.ok,
          "T7 MatrixPoseAndUniformRoot (shadow-core) returns true");
  require(withUniform.out.palette.size() == 3u,
          "T7 uniform size == max(maxSlot+1, matrixGroupSizes.size())");
  for (size_t i = 0u; i < withUniform.out.palette.size(); ++i)
    require(matrixEqual(withUniform.out.palette[i], f.pose.front()),
            "T7 uniform broadcast equals matrixPalette.front()");
  require(withUniform.detail.reason ==
              RuntimeGroupPaletteKernelMiss::MatrixIndexOutOfRange,
          "T7 uniform success keeps the last error note in detail");
  return true;
}

// ---- T8 计数派生差异（D4/D5 行为守卫） ----
bool testT8CountDerivationDifference() {
  {
    // D4：同一份槽位数组，slotCount 不同 -> maxSlot / 去重结果随之不同。
    Fixture f;
    f.slots = {0u, 5u, 255u, 7u};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);

    RuntimeGroupPaletteInput truncated = f.makeInput();
    truncated.vertexGroupSlotCount = 1u;  // 例如 upper-layer 的 vertexGroupCount
    RuntimeGroupPaletteInput full = f.makeInput();  // core: 128Ki/vertexCount clamp

    const BuildResult truncatedResult = run(
        truncated, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    const BuildResult fullResult =
        run(full, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);

    require(truncatedResult.out.maxVertexGroupSlot == 0u,
            "T8 slotCount=1 -> maxSlot 0");
    require(fullResult.out.maxVertexGroupSlot == 255u,
            "T8 slotCount=size -> maxSlot 255");
    require(truncatedResult.out.maxVertexGroupSlot !=
                fullResult.out.maxVertexGroupSlot,
            "T8 D4: slot count derivation changes the scanned upper bound");
    require(ScanRuntimeGroupPaletteSlots(f.slots.data(), 1u)
                .uniqueGroupSlots.size() == 1u &&
                ScanRuntimeGroupPaletteSlots(f.slots.data(), 4u)
                    .uniqueGroupSlots.size() == 4u,
            "T8 D4: dedup result follows the caller-supplied slot count");
  }
  {
    // D5：同一份 matrixGroupSizes，groupCount 不同 -> 分组数量随之不同。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u, 1u, 1u};
    f.matrixIndices = {0u, 1u, 2u};
    f.pose = makePose(3u);

    RuntimeGroupPaletteInput coreStyle = f.makeInput();  // groupCount = size()
    RuntimeGroupPaletteInput upperStyle = f.makeInput();
    upperStyle.groupCount = 1u;  // min(matrixGroupCount, size())

    const BuildResult coreResult = run(
        coreStyle, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    const BuildResult upperResult = run(
        upperStyle, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);

    require(coreResult.ok && coreResult.out.palette.size() == 3u,
            "T8 groupCount=size -> palette.size()==3");
    require(upperResult.ok && upperResult.out.palette.size() == 1u,
            "T8 groupCount=min(...) -> palette.size()==1 (no internal clamp)");
    require(coreResult.out.palette.size() != upperResult.out.palette.size(),
            "T8 D5: group count derivation changes the averaging extent");
  }
  return true;
}

// ---- T9 fallback 短路顺序可观测（4 步集合） ----
bool testT9FallbackOrder() {
  {
    // 只有 direct 能成立：数组顺序敏感（p5,p6,p7,p0）。
    Fixture f;
    f.slots = {3u, 1u};
    f.groupSizes = {0u};
    f.matrixIndices = {5u, 6u, 7u, 0u};
    f.pose = makePose(8u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(r.ok, "T9 direct branch returns true");
    const std::vector<std::pair<size_t, Matrix4>> expected = {
        {0u, f.pose[5]}, {1u, f.pose[6]}, {2u, f.pose[7]}, {3u, f.pose[0]}};
    require(r.out.palette.size() == 4u, "T9 direct branch size == maxSlot+1");
    require(requirePaletteSlots(r.out.palette, expected),
            "T9 direct branch maps group -> matrixIndices[group] in table order");
  }
  {
    // direct 失败（尾部越界），只有 sparse 成立：未列槽位必须为零。
    Fixture f;
    f.slots = {3u, 1u};
    f.groupSizes = {0u};
    f.matrixIndices = {5u, 6u, 99u, 99u};
    f.pose = makePose(8u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(r.ok, "T9 sparse matrix branch returns true");
    const std::vector<std::pair<size_t, Matrix4>> expected = {
        {3u, f.pose[5]}, {1u, f.pose[6]}};
    require(r.out.palette.size() == 4u, "T9 sparse matrix size == maxSlot+1");
    require(requirePaletteSlots(r.out.palette, expected),
            "T9 sparse matrix uses unique slot order, zero fills the rest");
    require(r.detail.reason ==
                RuntimeGroupPaletteKernelMiss::InvalidGroupTable,
            "T9 sparse branch keeps the main-path failure note");
    require(r.detail.group == 0u,
            "T9 sparse branch keeps the main-path failure group");
  }
  {
    // direct / sparse 均失败，只有 directPose 成立：直取 pose[0..maxSlot]。
    Fixture f;
    f.slots = {3u, 1u};
    f.groupSizes = {0u};
    f.matrixIndices = {99u, 98u, 97u, 96u};
    f.pose = makePose(8u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(r.ok, "T9 direct pose branch returns true");
    const std::vector<std::pair<size_t, Matrix4>> expected = {
        {0u, f.pose[0]}, {1u, f.pose[1]}, {2u, f.pose[2]}, {3u, f.pose[3]}};
    require(r.out.palette.size() == 4u, "T9 direct pose size == maxSlot+1");
    require(requirePaletteSlots(r.out.palette, expected),
            "T9 direct pose copies pose[group] with no zero fill");
  }
  {
    // direct / sparse / directPose 均失败，只有 sparsePose 成立。
    Fixture f;
    f.slots = {5u, 1u};
    f.groupSizes = {0u};
    f.matrixIndices = {99u, 99u};
    f.pose = makePose(3u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(r.ok, "T9 sparse pose branch returns true");
    const std::vector<std::pair<size_t, Matrix4>> expected = {
        {5u, f.pose[0]}, {1u, f.pose[1]}};
    require(r.out.palette.size() == 6u, "T9 sparse pose size == maxSlot+1");
    require(requirePaletteSlots(r.out.palette, expected),
            "T9 sparse pose maps uniqueSlots[i] -> pose[i]");
  }
  return true;
}

// ---- T10 退化与异常输入 ----
bool testT10DegenerateInputs() {
  {
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = {};
    RuntimeGroupPaletteInput in = f.makeInput();
    in.posePalette = nullptr;
    in.posePaletteSize = 0u;
    in.poseMatrixCount = 0u;
    const BuildResult r = run(
        in, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(!r.ok && r.out.palette.empty(), "T10 empty pose palette -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::NoPosePalette,
            "T10 empty pose palette -> NoPosePalette");
  }
  {
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    RuntimeGroupPaletteInput in = f.makeInput();
    in.poseMatrixCount = 0u;
    const BuildResult r = run(
        in, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(!r.ok, "T10 poseMatrixCount==0 -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::NoPosePalette,
            "T10 poseMatrixCount==0 -> NoPosePalette");
  }
  {
    Fixture f;
    f.slots = {};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(!r.ok, "T10 slotCount==0 -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::NoVertexGroups,
            "T10 slotCount==0 -> NoVertexGroups");
  }
  {
    RuntimeGroupPaletteInput in = {};
    const BuildResult r = run(
        in, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(!r.ok && r.out.palette.empty() && r.out.maxVertexGroupSlot == 0u &&
                !r.out.usesAveraging,
            "T10 all-null view -> false with untouched output");
  }
  {
    // matrixIndices 空但 matrixGroupSizes 非空：4 步集合里 4 条全部失败。
    Fixture f;
    f.slots = {0u, 1u, 2u};
    f.groupSizes = {3u};
    f.matrixIndices = {};
    f.pose = makePose(2u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T10 empty matrixIndices with groups -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
            "T10 empty matrixIndices -> FallbacksFailed");
    require(r.out.palette.size() <= 3u,
            "T10 empty matrixIndices writes no out-of-range slot");
  }
  {
    // uniqueGroupSlots.size() > matrixIndexCount：sparse 两条都被拒绝。
    Fixture f;
    f.slots = {0u, 1u};
    f.groupSizes = {2u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T10 unique slots > matrixIndexCount -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
            "T10 unique slots > matrixIndexCount -> FallbacksFailed");
  }
  {
    // maxSlot 极大而 matrixIndexCount 极小：必须失败而不是截断。
    Fixture f;
    f.slots = {255u, 254u};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T10 huge maxSlot with tiny table -> false");
    require(r.out.palette.size() <= 256u,
            "T10 huge maxSlot never allocates beyond maxSlot+1");
  }
  return true;
}

// ---- T11 确定性 / 失败残留 ----
bool testT11DeterminismAndResidue() {
  Fixture f;
  f.slots = {0u, 0u, 0u};
  f.groupSizes = {3u, 1u};
  f.matrixIndices = {0u, 1u, 2u, 0u};
  f.pose = makePose(3u);
  const BuildResult first = run(
      f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
  const BuildResult second = run(
      f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
  require(first.ok && second.ok, "T11 both calls succeed");
  require(first.out.palette.size() == second.out.palette.size(),
          "T11 repeated calls keep the same size");
  for (size_t i = 0u; i < first.out.palette.size(); ++i)
    require(matrixEqual(first.out.palette[i], second.out.palette[i]),
            "T11 repeated calls are bitwise identical");
  require(first.out.usesAveraging == second.out.usesAveraging &&
              first.out.maxVertexGroupSlot == second.out.maxVertexGroupSlot &&
              first.detail.reason == second.detail.reason,
          "T11 repeated calls keep the same flags/detail");

  {
    // 失败残留：主路径已 resize(groupCount) 并写了 group 0，随后 4 步全失败。
    Fixture g;
    g.slots = {255u, 254u};
    g.groupSizes = {1u};
    g.matrixIndices = {0u};
    g.pose = makePose(1u);
    const BuildResult r = run(
        g.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T11 residue case returns false");
    require(r.out.palette.size() == 1u,
            "T11 failure keeps the main path's resize(groupCount) residue");
    require(matrixEqual(r.out.palette[0], g.pose[0]),
            "T11 failure keeps the main path's written group 0");
  }
  return true;
}

// ---- T12 规模边界 ----
bool testT12ScaleBoundaries() {
  {
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok && r.out.palette.size() == 1u &&
                matrixEqual(r.out.palette[0], f.pose[0]),
            "T12 groupCount==1 && slotCount==1");
  }
  {
    // matrixIndex == poseMatrixCount - 1 恰好合法。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u};
    f.matrixIndices = {2u};
    f.pose = makePose(3u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok && matrixEqual(r.out.palette[0], f.pose[2]),
            "T12 matrixIndex == poseMatrixCount-1 is legal");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::None,
            "T12 last legal matrix index keeps reason None");
  }
  {
    // 256 个互不相同的槽位：seenGroupSlots 全满，去重结果与槽位一一对应。
    Fixture f;
    f.slots.resize(256u);
    f.groupSizes.assign(256u, 1u);
    f.matrixIndices.resize(256u);
    for (uint32_t i = 0u; i < 256u; ++i) {
      f.slots[i] = uint8_t(i);
      f.matrixIndices[i] = i;
    }
    f.pose = makePose(256u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok, "T12 256 slots / 256 groups returns true");
    require(r.out.palette.size() == 256u && r.out.maxVertexGroupSlot == 255u,
            "T12 256 slots -> full 256-entry palette");
    require(!r.out.usesAveraging, "T12 256 singleton groups -> no averaging");
    require(matrixEqual(r.out.palette[0], f.pose[0]) &&
                matrixEqual(r.out.palette[255], f.pose[255]),
            "T12 256 slots map group -> pose[group]");
    const RuntimeGroupPaletteSlotScan scan =
        ScanRuntimeGroupPaletteSlots(f.slots.data(), f.slots.size());
    require(scan.uniqueGroupSlots.size() == 256u,
            "T12 seenGroupSlots saturates at 256 unique slots");
  }
  return true;
}

// ---- T13 表驱动等价回归（4 段 lambda 语义） ----
bool testT13TableDrivenEquivalence() {
  struct DirectCase {
    std::vector<uint8_t> slots;
    std::vector<uint32_t> matrixIndices;
    std::vector<uint32_t> expectedPoseIndices;  // 按 group 序号
  };
  const std::vector<DirectCase> directCases = {
      {{0u}, {2u}, {2u}},
      {{0u, 1u, 2u}, {2u, 0u, 1u}, {2u, 0u, 1u}},
      {{3u, 1u}, {5u, 6u, 7u, 0u}, {5u, 6u, 7u, 0u}},
  };
  for (size_t caseIndex = 0u; caseIndex < directCases.size(); ++caseIndex) {
    const DirectCase& testCase = directCases[caseIndex];
    Fixture f;
    f.slots = testCase.slots;
    f.groupSizes = {0u};
    f.matrixIndices = testCase.matrixIndices;
    f.pose = makePose(8u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(r.ok, "T13 direct table case succeeds");
    require(r.out.palette.size() == testCase.expectedPoseIndices.size(),
            "T13 direct table case size");
    for (size_t i = 0u; i < testCase.expectedPoseIndices.size(); ++i)
      require(matrixEqual(r.out.palette[i],
                          f.pose[testCase.expectedPoseIndices[i]]),
              "T13 direct table order is index-sensitive");
  }

  struct SparseCase {
    std::vector<uint8_t> slots;
    std::vector<uint32_t> matrixIndices;
    std::vector<std::pair<size_t, uint32_t>> expected;  // slot -> pose 序号
  };
  const std::vector<SparseCase> sparseCases = {
      {{0u, 1u}, {4u, 3u}, {{0u, 4u}, {1u, 3u}}},
      {{3u, 1u}, {5u, 6u, 99u, 99u}, {{3u, 5u}, {1u, 6u}}},
  };
  for (size_t caseIndex = 0u; caseIndex < sparseCases.size(); ++caseIndex) {
    const SparseCase& testCase = sparseCases[caseIndex];
    Fixture f;
    f.slots = testCase.slots;
    f.groupSizes = {0u};
    f.matrixIndices = testCase.matrixIndices;
    f.pose = makePose(8u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(r.ok, "T13 sparse table case succeeds");
    std::vector<std::pair<size_t, Matrix4>> expected;
    for (const auto& entry : testCase.expected)
      expected.push_back({entry.first, f.pose[entry.second]});
    require(requirePaletteSlots(r.out.palette, expected),
            "T13 sparse table fills unique slots in appearance order");
  }

  {
    // sparsePose 表：uniqueSlots[i] -> pose[i]，与 matrixIndices 无关。
    Fixture f;
    f.slots = {4u, 2u, 0u};
    f.groupSizes = {0u};
    f.matrixIndices = {99u, 99u, 99u};
    f.pose = makePose(3u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(r.ok, "T13 sparse pose table case succeeds");
    const std::vector<std::pair<size_t, Matrix4>> expected = {
        {4u, f.pose[0]}, {2u, f.pose[1]}, {0u, f.pose[2]}};
    require(requirePaletteSlots(r.out.palette, expected),
            "T13 sparse pose ignores matrixIndices entirely");
  }
  return true;
}

// ---- T14 groupCount > 0 却给出空 group 表视图：契约守卫（复核 B-1） ----
bool testT14NullGroupSizeTableView() {
  Fixture f;
  f.slots = {7u};        // 若扫描发生，maxVertexGroupSlot 会是 7
  f.groupSizes = {1u};   // groupCount 保持 1，但视图被显式置空
  f.matrixIndices = {0u};
  f.pose = makePose(1u);
  RuntimeGroupPaletteInput in = f.makeInput();
  in.matrixGroupSizes = nullptr;
  const BuildResult r =
      run(in, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
  require(!r.ok, "T14 groupCount>0 with null matrixGroupSizes -> false");
  require(r.detail.reason == RuntimeGroupPaletteKernelMiss::InvalidGroupTable,
          "T14 null group table -> InvalidGroupTable");
  require(r.out.palette.empty(), "T14 null group table writes nothing");
  require(r.out.maxVertexGroupSlot == 0u &&
              r.detail.maxVertexGroupSlot == 0u,
          "T14 guard fires before the slot scan (maxSlot stays 0)");
  require(r.detail.groupCount == 1u && r.detail.matrixIndexCount == 1u &&
              r.detail.poseCount == 1u,
          "T14 detail still reports the rejected view counts");
  {
    // groupCount == 0 时不触发该守卫：空表视图是合法的"无分组"输入。
    RuntimeGroupPaletteInput emptyGroups = f.makeInput();
    emptyGroups.groupCount = 0u;
    emptyGroups.matrixGroupSizes = nullptr;
    const BuildResult r0 = run(
        emptyGroups, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r0.ok && r0.out.palette.size() == 8u,
            "T14 groupCount==0 with null group table stays legal");
  }
  return true;
}

// ---- T15 退化指针视图：空 matrixIndices / 空 posePalette ----
bool testT15DegeneratePointerViews() {
  {
    // matrixIndices == nullptr && matrixIndexCount > 0。
    // 2026-09-17：主线程按对抗性复核与探针实测（修复前该严格组合为 0xC0000005
    // 访问违例）补上同形守卫 matrixIndexCount>0 && matrixIndices==nullptr ->
    // InvalidGroupTable，因此这里改用**严格组合**：slots={0}（maxSlot=0 < count=1）、
    // groupSizes={1}（groupCount=1，会真正走到主路径/A/B 的索引读取）。
    Fixture f;
    f.slots = {0u};          // maxSlot=0 < matrixIndexCount
    f.groupSizes = {1u};     // groupCount=1：守卫必须先于槽位扫描与索引读取触发
    f.matrixIndices = {0u};  // count=1，随后把指针置空
    f.pose = makePose(1u);
    RuntimeGroupPaletteInput in = f.makeInput();
    in.matrixIndices = nullptr;
    const BuildResult r =
        run(in, RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T15a null matrixIndices view -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::InvalidGroupTable,
            "T15a null matrixIndices -> InvalidGroupTable without dereference");
    require(r.out.palette.empty(), "T15a null matrixIndices writes nothing");
    // 守卫在槽位扫描之前触发：maxVertexGroupSlot 必须仍为 0
    require(r.out.maxVertexGroupSlot == 0u,
            "T15a guard fires before the slot scan");
  }
  {
    // posePalette == nullptr && posePaletteSize > 0：空指针本身即早退。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    RuntimeGroupPaletteInput in = f.makeInput();
    in.posePalette = nullptr;   // posePaletteSize 仍为 1
    const BuildResult r =
        run(in, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(!r.ok, "T15b null posePalette with size>0 -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::NoPosePalette,
            "T15b null posePalette -> NoPosePalette");
    require(r.out.palette.empty() && r.out.maxVertexGroupSlot == 0u &&
                !r.out.usesAveraging,
            "T15b null posePalette writes nothing");
    require(r.detail.poseCount == 1u && r.detail.maxVertexGroupSlot == 0u &&
                r.detail.matrixIndexCount == 1u,
            "T15b detail reports the view counts before any write");
  }
  {
    // 反向：posePalette 有效但 size 计数为 0 —— 同样在 NoPosePalette 早退。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    RuntimeGroupPaletteInput in = f.makeInput();
    in.posePaletteSize = 0u;
    const BuildResult r =
        run(in, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(!r.ok &&
                r.detail.reason == RuntimeGroupPaletteKernelMiss::NoPosePalette,
            "T15c posePaletteSize==0 -> NoPosePalette");
  }
  {
    // matrixIndices 有效但 matrixIndexCount == 0：不是空指针场景，走正常失败路径
    // （A/B 由计数守卫早退，C 救回）。
    Fixture f;
    f.slots = {0u};
    f.groupSizes = {1u};
    f.matrixIndices = {};
    f.pose = makePose(1u);
    const BuildResult r =
        run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok && r.out.palette.size() == 1u &&
                matrixEqual(r.out.palette[0], f.pose[0]),
            "T15d matrixIndexCount==0 is not a null-pointer case (C rescues)");
  }
  return true;
}

// ---- T16 等价值保全：主路径已置位的 usesAveraging 不因走 fallback 而丢失 ----
bool testT16AveragingFlagSurvivesFallback() {
  {
    // 主路径第 0 组（size 2）完成 -> usesAveraging=true；第 1 组 matrixIndex 越界失败
    // -> 4 步集合由 buildSparseMatrixRemap 救回，且 B 用 matrixIndices[i] 覆盖了
    // 主路径的组平均结果（p1/p0 顺序可与主路径的 (p0+p1)/2 区分）。
    Fixture f;
    f.slots = {0u, 0u, 5u};          // maxSlot=5, uniqueSlots={0,5}
    f.groupSizes = {2u, 1u};
    f.matrixIndices = {1u, 0u, 99u};
    f.pose = makePose(2u);
    const BuildResult r =
        run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(r.ok, "T16a rescued by buildSparseMatrixRemap");
    require(r.out.usesAveraging,
            "T16a usesAveraging set by the main path survives the fallback");
    require(r.out.palette.size() == 6u, "T16a sparse size == maxSlot+1");
    require(matrixEqual(r.out.palette[0], f.pose[1]) &&
                matrixEqual(r.out.palette[5], f.pose[0]),
            "T16a sparse (mi order 1,0) overwrote the main-path average");
    require(matrixEqual(r.out.palette[1], Matrix4(0.0f)) &&
                matrixEqual(r.out.palette[4], Matrix4(0.0f)),
            "T16a sparse zero-fills the unlisted slots");
    require(r.detail.reason ==
                RuntimeGroupPaletteKernelMiss::MatrixIndexOutOfRange &&
                r.detail.group == 1u && r.detail.matrixIndex == 99u,
            "T16a detail keeps the last main-path failure");
  }
  {
    // 主路径在槽位范围校验处失败，且 A/B/C/D 全部在写入前早退
    // -> 5 步集合由 buildUniformPosePalette 救回，usesAveraging 仍保持 true。
    Fixture f;
    f.slots = {0u, 0u, 5u, 4u};      // maxSlot=5, uniqueSlots={0,5,4}（3）
    f.groupSizes = {2u};
    f.matrixIndices = {0u, 0u};      // count 2 < uniqueSlots.size() -> B 早退
    f.pose = makePose(1u);
    const BuildResult r = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r.ok, "T16b rescued by buildUniformPosePalette");
    require(r.out.usesAveraging,
            "T16b usesAveraging set by the main path survives the uniform rescue");
    require(r.out.palette.size() == 6u,
            "T16b uniform size == max(maxSlot+1, groupCount)");
    for (size_t i = 0u; i < r.out.palette.size(); ++i)
      require(matrixEqual(r.out.palette[i], f.pose[0]),
              "T16b uniform broadcast result");
    require(r.detail.reason ==
                RuntimeGroupPaletteKernelMiss::VertexGroupOutOfRange &&
                r.detail.group == 5u,
            "T16b detail keeps the slot-range failure");
  }
  return true;
}

// ---- T17 失败残留逐入口（4 步集合全败时"最后一次写入"的逐位对齐） ----
bool testT17ResiduePerFailureEntry() {
  // 说明：5 步集合的 FallbacksFailed 在构造上不可达（前置校验通过后
  // buildUniformPosePalette 必定成功），因此"全败残留"只能用 4 步集合构造；
  // 每个入口都同时断言同一输入的 5 步结果成功，以证明该差异只属 upper-layer 语义（D2）。

  // 入口 1：groupCount == 0（主路径尚未 resize，且 A/B/C/D 全部在写入前早退）。
  {
    Fixture f;
    f.slots = {5u, 4u};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    RuntimeGroupPaletteInput in = f.makeInput();
    in.groupCount = 0u;
    const BuildResult r =
        run(in, RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T17-1 groupCount==0 -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
            "T17-1 groupCount==0 -> FallbacksFailed");
    require(r.out.palette.empty(),
            "T17-1 residue: main path never resized, no builder wrote");
    const BuildResult r5 = run(
        in, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r5.ok && r5.out.palette.size() == 6u,
            "T17-1 5-step set cannot fail (uniform rescue)");
  }
  // 入口 2：running > matrixIndexCount（主路径尚未 resize）。
  {
    Fixture f;
    f.slots = {5u, 4u};
    f.groupSizes = {2u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    const BuildResult r =
        run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T17-2 running>matrixIndexCount -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
            "T17-2 running>matrixIndexCount -> FallbacksFailed (note overwritten)");
    require(r.out.palette.empty(), "T17-2 residue: resize not reached");
    const BuildResult r5 = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r5.ok, "T17-2 5-step set cannot fail");
  }
  // 入口 3：groupSize == 0（主路径已 resize(groupCount) 并写完整第 0 组）。
  {
    Fixture f;
    f.slots = {5u, 4u};
    f.groupSizes = {1u, 0u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    const BuildResult r =
        run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T17-3 groupSize==0 -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
            "T17-3 groupSize==0 -> FallbacksFailed");
    require(r.out.palette.size() == 2u,
            "T17-3 residue keeps resize(groupCount) with 2 entries");
    require(matrixEqual(r.out.palette[0], f.pose[0]),
            "T17-3 residue keeps the completed group 0");
    require(matrixEqual(r.out.palette[1], Matrix4()),
            "T17-3 untouched entry keeps value-initialized identity (not zero)");
    const BuildResult r5 = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r5.ok && r5.out.palette.size() == 6u,
            "T17-3 5-step set cannot fail (uniform size == maxSlot+1)");
  }
  // 入口 4：主路径 matrixIndex 越界（组的写入在校验之后，因此第 0 组也未写）。
  {
    Fixture f;
    f.slots = {5u, 4u};
    f.groupSizes = {1u};
    f.matrixIndices = {99u};
    f.pose = makePose(1u);
    const BuildResult r =
        run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T17-4 main-path matrixIndex OOR -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
            "T17-4 -> FallbacksFailed");
    require(r.out.palette.size() == 1u,
            "T17-4 residue keeps resize(groupCount) with 1 entry");
    require(matrixEqual(r.out.palette[0], Matrix4()),
            "T17-4 residue is the untouched identity entry, not pose[0]");
    require(!matrixEqual(r.out.palette[0], f.pose[0]),
            "T17-4 proves the failing group was never written");
    const BuildResult r5 = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r5.ok, "T17-4 5-step set cannot fail");
  }
  // 入口 5：槽位越界（主路径已写完整个平均结果）。
  {
    Fixture f;
    f.slots = {5u, 4u};
    f.groupSizes = {1u};
    f.matrixIndices = {0u};
    f.pose = makePose(1u);
    const BuildResult r =
        run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T17-5 slot OOR -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
            "T17-5 -> FallbacksFailed (VertexGroupOutOfRange note overwritten)");
    require(r.out.palette.size() == 1u,
            "T17-5 residue keeps resize(groupCount) with 1 entry");
    require(matrixEqual(r.out.palette[0], f.pose[0]),
            "T17-5 residue keeps the completed main-path average");
    const BuildResult r5 = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r5.ok && r5.out.palette.size() == 6u,
            "T17-5 5-step set cannot fail (uniform broadcast)");
  }
  // 入口 5 变体：唯一能出现"最后尝试的 builder 留下部分写入"的形态 ——
  // A 因 maxSlot >= matrixIndexCount 早退、B 进入后写满 assign(maxSlot+1, 0) 并在第 2 项失败。
  {
    Fixture f;
    f.slots = {5u, 4u};                // maxSlot=5, uniqueSlots={5,4}（2）
    f.groupSizes = {1u};
    f.matrixIndices = {0u, 99u, 99u};   // count 3
    f.pose = makePose(1u);
    const BuildResult r =
        run(f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap);
    require(!r.ok, "T17-6 builder partial write -> false");
    require(r.detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
            "T17-6 -> FallbacksFailed");
    require(r.out.palette.size() == 6u,
            "T17-6 residue is buildSparseMatrixRemap assign(maxSlot+1, zero)");
    require(matrixEqual(r.out.palette[5], f.pose[0]),
            "T17-6 residue keeps B first successful sparse write");
    for (size_t i = 0u; i < 5u; ++i)
      require(matrixEqual(r.out.palette[i], Matrix4(0.0f)),
              "T17-6 B wrote zero (not identity) for the untouched slots");
    const BuildResult r5 = run(
        f.makeInput(), RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot);
    require(r5.ok && r5.out.palette.size() == 6u,
            "T17-6 5-step set cannot fail");
  }
  return true;
}

// ---- T18 input posePalette / output storage 别名防御 ----
bool testT18AliasedInputOutput() {
  Fixture f;
  f.slots = {0u};
  f.groupSizes = {1u};
  f.matrixIndices = {0u};
  f.pose = makePose(1u);

  std::vector<Matrix4> aliasedOutput = f.pose;
  RuntimeGroupPaletteInput in = f.makeInput();
  in.posePalette = aliasedOutput.data();
  in.posePaletteSize = aliasedOutput.size();
  in.poseMatrixCount = uint32_t(aliasedOutput.size());

  RuntimeGroupPaletteKernelDetail detail = {};
  uint32_t maxSlot = 0u;
  bool usesAveraging = false;
  const bool ok = TryBuildRuntimeGroupPaletteKernel(
      in, RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot,
      aliasedOutput, maxSlot, usesAveraging, &detail);

  require(!ok, "T18 aliased pose/output -> false");
  require(detail.aliasedInputOutput, "T18 detail marks aliasedInputOutput");
  require(detail.aliasRelation == RuntimeGroupPaletteAliasRelation::Overlapping,
          "T18 detail aliasRelation == Overlapping");
  require(detail.reason == RuntimeGroupPaletteKernelMiss::FallbacksFailed,
          "T18 alias fails closed with FallbacksFailed");

  // capacity > size、clear 后仍有 capacity 的 output storage 也必须被识别。
  std::vector<Matrix4> capacityOnly;
  capacityOnly.reserve(2u);
  capacityOnly.clear();
  RuntimeGroupPaletteInput capacityInput = f.makeInput();
  capacityInput.posePalette = capacityOnly.data();
  capacityInput.posePaletteSize = capacityOnly.capacity();
  capacityInput.poseMatrixCount = uint32_t(capacityOnly.capacity());
  const RuntimeGroupPaletteAliasRelation capacityRelation =
      ClassifyRuntimeGroupPaletteOutputPoseAlias(capacityOnly, capacityInput);
  require(capacityRelation == RuntimeGroupPaletteAliasRelation::Overlapping,
          "T18b capacity>size storage counts as overlapping");
  return true;
}

// ---- T19 半开区间分类器纯整数边界（不读取任何地址） ----
bool testT19ByteRangeOverlapClassifier() {
  using Relation = RuntimeGroupPaletteAliasRelation;
  const auto classify = [](uintptr_t lhsBegin, uint64_t lhsLength,
                           uintptr_t rhsBegin, uint64_t rhsLength) {
    return ClassifyRuntimeGroupPaletteByteRangeOverlap(
        lhsBegin, lhsLength, rhsBegin, rhsLength);
  };

  require(classify(100u, 10u, 110u, 10u) == Relation::NonOverlapping,
          "T19 adjacent right is non-overlapping");
  require(classify(110u, 10u, 100u, 10u) == Relation::NonOverlapping,
          "T19 adjacent left is non-overlapping");
  require(classify(100u, 10u, 100u, 10u) == Relation::Overlapping,
          "T19 same begin/end is overlapping");
  require(classify(100u, 100u, 120u, 10u) == Relation::Overlapping,
          "T19 internal subrange is overlapping");
  require(classify(120u, 10u, 100u, 100u) == Relation::Overlapping,
          "T19 reverse containment is overlapping");
  require(classify(100u, 0u, 100u, 10u) == Relation::NonOverlapping,
          "T19 zero lhs is non-overlapping");
  require(classify(100u, 10u, 100u, 0u) == Relation::NonOverlapping,
          "T19 zero rhs is non-overlapping");
  require(classify(100u, 40u, 140u, 20u) == Relation::NonOverlapping,
          "T19 touching half-open intervals are non-overlapping");
  require(classify(100u, 50u, 140u, 20u) == Relation::Overlapping,
          "T19 positive overlap is overlapping");

  const uintptr_t maxAddress =
      (std::numeric_limits<uintptr_t>::max)();
  require(classify(maxAddress - 10u, 10u, 0u, 1u) ==
              Relation::NonOverlapping,
          "T19 max representable end is non-overlapping");
  require(classify(maxAddress - 10u, 11u, 0u, 1u) == Relation::Invalid,
          "T19 address overflow is invalid");

  uint64_t bytes = 0u;
  require(!dxvk::war3::render::RuntimeGroupPaletteTryByteLength(
              (std::numeric_limits<size_t>::max)(), 2u, bytes),
          "T19 element byte-length multiplication overflow is rejected");
  require(dxvk::war3::render::RuntimeGroupPaletteTryByteLength(4u, sizeof(Matrix4), bytes) &&
              bytes == uint64_t(4u * sizeof(Matrix4)),
          "T19 normal byte length is accepted");
  return true;
}

int runAll() {
  testT1GroupAveraging();
  testT2SingleElementGroup();
  testT3UsesAveragingGate();
  testT4OutputShape();
  testT5SlotScan();
  testT6FailureReasons();
  testT7FallbackSetDifference();
  testT8CountDerivationDifference();
  testT9FallbackOrder();
  testT10DegenerateInputs();
  testT11DeterminismAndResidue();
  testT12ScaleBoundaries();
  testT13TableDrivenEquivalence();
  testT14NullGroupSizeTableView();
  testT15DegeneratePointerViews();
  testT16AveragingFlagSurvivesFallback();
  testT17ResiduePerFailureEntry();
  testT18AliasedInputOutput();
  testT19ByteRangeOverlapClassifier();
  if (g_failures != 0u) {
    std::cerr << "war3_runtime_group_palette_kernel_test: " << g_failures
              << " failure(s)\n";
    return 1;
  }
  std::cout << "war3_runtime_group_palette_kernel_test: all T1-T19 passed\n";
  return 0;
}

}  // namespace

int main() { return runAll(); }
