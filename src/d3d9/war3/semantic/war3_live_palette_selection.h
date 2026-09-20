#pragma once

// M2-1（device.cpp 语义职责迁移第二步第一片）：live palette 选择链的纯计算
// helper 与运行时配置 getter。
// 迁出范围：FNV-1a palette 哈希、48 字节 3x4 pose 解码、CModel +0x5C/+0x60
//           FinalPose 数组安全读取、runtimeModel ±0xA0 alias 解析，以及
//           SafeCopy/Refresh/AllowCModelFallback/PaletteDiagnostics 四个
//           env getter。
// M2-2（第二片）：选择链本体 War3TryBuildLiveRuntimeGroupPalette、
//           War3SemanticPaletteSource 来源枚举、War3LivePaletteBuild* 分相
//           计时类型（QPC 校正逻辑逐字保留）、resolvePaletteSlotIndex lambda
//           的 4096 项 thread_local slot 缓存 + 8192 项 lookup 加速器，以及
//           Gap A 复核计数器 g_devicePaletteSlotCache*（M1 D 类先例：模块内
//           唯一定义 + 头文件 extern；war3_diag Query 访问器留在 device.cpp
//           原位经 using-directive 读取）。
// 方案见 docs/plan/2026-09-16-device-semantic-responsibility-migration.md §3 M2
// 与 docs/plan/2026-09-18-relay-m2-t2t3-workorder.md §2。
//
// 行为约束：本模块只搬运 d3d9_device.cpp 中原有的定义，判定结果、判定顺序与
// 读取口径逐点不变；d3d9_device.cpp 只保留调用点（与 M1 相同的 using 先例）。
// 等价证据见 docs/plan/2026-09-18-m2-1-migration-equivalence-record.md 与
// docs/plan/2026-09-18-m2-2-migration-equivalence-record.md。

#include "../../../util/util_matrix.h"
#include "../../../util/util_bit.h"
#include "../../../util/util_time.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk::war3::shadow {
struct ShadowPacketResource;
}
namespace dxvk::war3::render::skin {
struct Selection;
}
namespace dxvk {
struct War3ShadowCaptureStats;
}

namespace dxvk::war3::semantic {

// ---------------------------------------------------------------------------
// live palette 纯计算 helper（FNV-1a 哈希 / 48 字节 pose 解码 / pose 数组读取 /
// runtimeModel alias 解析）
// ---------------------------------------------------------------------------
uint64_t War3SemanticHashMatrixPalette(const Matrix4* matrices,
                                       uint32_t matrixCount);
Matrix4 War3DecodeRuntimePoseMatrix48(const uint8_t* poseBytes);
bool War3TryReadRuntimePoseArray(void* runtimeModelPtr,
                                 uint32_t& outPoseCount,
                                 void*& outPoseArrayPtr);
void* War3ResolveLivePoseRuntimeAlias(void* runtimeModelPtr,
                                      uint32_t& outPoseCount,
                                      void*& outPoseArrayPtr);

// ---------------------------------------------------------------------------
// live palette 选择链的运行时配置 getter
// ---------------------------------------------------------------------------
bool War3SemanticLivePaletteSafeCopyRuntime();
bool War3SemanticLivePaletteRefreshRuntime();
bool War3SemanticLivePaletteAllowCModelFallbackRuntime();
bool War3SemanticPaletteDiagnosticsRuntime();


// ---------------------------------------------------------------------------
// M2-2：调色板来源选择链本体（来源枚举 / 分相计时类型 / 选择链声明 /
//       Gap A slot 缓存复核计数器）。
// 以下类型与声明从 d3d9_device.cpp 逐字节迁入（唯一例外：选择链声明的
// 默认实参按迁移铁律集中在头文件声明上，定义不再重复）。device.cpp 的
// 调用点、war3_diag Query 访问器与计时报告消费经同一条 using-directive
// 机械解析（M1/M2-1 同型）。
// ---------------------------------------------------------------------------
// Phase 7.28：skinned palette content stability probe。
// 当 skinned packet 的 palette 在 submit 阶段完成选择后，我们需要知道这份
// palette 具体是从哪条来源读回的。不同来源在稳定性、读取时机和时效性上差距
// 极大，也就是用户观察到的"储物桶(rigid)不闪、skinned 单位闪"的真正分水岭。
// 该枚举刻意保持 "submit 端视角"：DrawTimeCaptured 才是和 current-draw 同
// 帧同事件捕获到的真源；其它分支都涉及一次性重读或跨帧缓存。
enum class War3SemanticPaletteSource : uint32_t {
  None = 0,                           // 没有 live palette，fallback 到 packet 原始 palette
  DrawTimeCaptured = 1,               // current-draw 同步捕获的 palette（最稳定）
  SubmitTimeGlobalSlot = 2,           // Game.dll + 0xBC6BD0 全局 palette 通路，slotIndex 通过 renderablePart+0x08 读取
  SubmitTimeBlendedPaletteCache = 3,  // QueryBlendedPaletteBySlotIndex（Hook_RuntimeMatrixWrite 同帧捕获）
  SubmitTimePublishedPoseRegistry = 4, // 发布过的 PoseRegistry record
  SubmitTimeCModelFallback = 5,       // CModel +0x5C/+0x60 的 FinalPoseMatrixArray（当前 1.27a 偏移不可信）
  OwnedPartSnapshot = 6,             // CPU-owned producer publication, not a native allocation lease
};

enum class War3LivePaletteBuildPhase : uint8_t {
  GroupScan = 0u,
  SlotResolve,
  FrameTagQuery,
  PartSnapshot,
  GlobalModuleLookup,
  GlobalPointerRead,
  GlobalRangeCheck,
  GlobalSafeCopy,
  GlobalDecode,
  GlobalHash,
  BlendedSlot,
  PoseFallback,
  Count,
};

constexpr size_t kWar3LivePaletteBuildPhaseCount =
    static_cast<size_t>(War3LivePaletteBuildPhase::Count);

struct War3LivePaletteBuildTiming {
  std::array<uint64_t, kWar3LivePaletteBuildPhaseCount> ticks = {};
  std::array<uint32_t, kWar3LivePaletteBuildPhaseCount> calls = {};
  uint64_t qpcOverheadTicks = 0u;
};

class War3LivePaletteBuildRawTiming final {
public:
  explicit War3LivePaletteBuildRawTiming(
      War3LivePaletteBuildTiming* timing)
      : m_timing(timing) {
  }

  ~War3LivePaletteBuildRawTiming() {
    if (m_timing != nullptr)
      closeCurrent(dxvk::high_resolution_clock::get_counter());
  }

  War3LivePaletteBuildRawTiming(
      const War3LivePaletteBuildRawTiming&) = delete;
  War3LivePaletteBuildRawTiming& operator=(
      const War3LivePaletteBuildRawTiming&) = delete;

  inline void enter(War3LivePaletteBuildPhase phase) {
    if (m_timing == nullptr)
      return;
    const int64_t now = dxvk::high_resolution_clock::get_counter();
    closeCurrent(now);
    m_phase = phase;
    m_begin = now;
    ++m_timing->calls[static_cast<size_t>(phase)];
  }

private:
  inline void closeCurrent(int64_t now) {
    if (m_begin == 0 || m_phase == War3LivePaletteBuildPhase::Count)
      return;
    if (now > m_begin) {
      const size_t index = static_cast<size_t>(m_phase);
      const uint64_t elapsed = uint64_t(now - m_begin);
      const uint64_t corrected = elapsed > m_timing->qpcOverheadTicks
          ? elapsed - m_timing->qpcOverheadTicks : 0u;
      m_timing->ticks[index] += corrected;
    }
  }

  War3LivePaletteBuildTiming* m_timing = nullptr;
  War3LivePaletteBuildPhase m_phase = War3LivePaletteBuildPhase::Count;
  int64_t m_begin = 0;
};

bool War3TryBuildLiveRuntimeGroupPalette(
    const dxvk::war3::shadow::ShadowPacketResource& resource,
    void* runtimeModelPtr,
    void* renderablePart,
    uint64_t frameSerial,
    std::vector<Matrix4>& outPalette,
    uint32_t& outMaxVertexGroupSlot,
    uint64_t& outHash,
    uint64_t* outRawPoseHash = nullptr,
    void** outPoseRuntimeModelPtr = nullptr,
    bool allowCModelFallbackForCall = false,
    War3SemanticPaletteSource* outPaletteSource = nullptr,
    uint32_t* outPaletteSlotIndex = nullptr,
    uint32_t* outPaletteMinFrameTag = nullptr,
    uint32_t* outPaletteMaxFrameTag = nullptr,
    War3LivePaletteBuildTiming* outTiming = nullptr,
    uint32_t provenMaxVertexGroupSlot = 0xFFFFFFFFu,
    dxvk::war3::render::skin::Selection* outSelection = nullptr);

// 2026-09-16 P0 Gap A：device 侧 palette 记忆槽位复核计数（M1 D 类先例：
// 模块内唯一定义，头文件 extern；war3_diag::QueryDevicePaletteSlotCache*
// 访问器留在 device.cpp 原位，经 using-directive 读取本声明）。
extern std::atomic<uint64_t> g_devicePaletteSlotCacheServedAfterConfirmCount;
extern std::atomic<uint64_t> g_devicePaletteSlotCacheRejectedStaleCount;

// ---------------------------------------------------------------------------
// M2-3：motion / churn 诊断三函数的声明。定义在模块 .cpp（逐字节迁自
// d3d9_device.cpp :7344-7520）；两个 Entry 结构是模块 .cpp 的实现细节，
// 不进本头文件。War3ShadowCaptureStats 只前向声明（重量头
// d3d9_war3_scene.h 仅出现在模块 .cpp）。
// ---------------------------------------------------------------------------
void War3NoteLivePaletteMotion(dxvk::War3ShadowCaptureStats& stats,
                               void* runtimeModelPtr,
                               uint64_t frameSerial,
                               uint64_t rawHash,
                               uint64_t groupHash);
void War3NoteDrawTimePoseMotion(dxvk::War3ShadowCaptureStats& stats,
                                void* runtimeModelPtr,
                                uint64_t frameSerial,
                                uint64_t hash);
void War3NoteSubmittedPaletteMotion(dxvk::War3ShadowCaptureStats& stats,
                                    void* runtimeModelPtr,
                                    uint64_t frameSerial,
                                    uint64_t hash);

// ---------------------------------------------------------------------------
// M2-4 (2026-09-18): palette compose-policy predicates and env getters.
// Definitions live in the module .cpp (bodies moved byte-for-byte from
// d3d9_device.cpp; extraction and bidirectional byte-identity are enforced by
// AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-4).
// Migrated symbols:
//   War3SemanticPaletteInPlaceAppendRuntime / War3SemanticDrawTimePoseRuntime
//       - pure config getters, same shape as the four M2-1 env getters
//         (call sites stay in device.cpp).
//   War3SemanticTranslationDistanceSq / War3SemanticTranslationFinite /
//   War3SemanticPaletteStorageReadable
//       - supporting pure predicates used only by LooksModelLocal.
//   War3SemanticPaletteLooksModelLocal (two overloads)
//       - the palette compose-policy (model-local heuristic) itself.
//   War3SemanticBoundsRadiusForObjectKind
//       - the pure switch(objectKind)->radius that A4 depends on (zero device
//         dependency). Whole repo: 1 definition + 7 call sites; only 1 belongs
//         to the palette family (A4), the other 6 are bounds. Those 6 call
//         sites are UNCHANGED text and still resolve through the existing
//         using namespace dxvk::war3::semantic in d3d9_device.cpp. Leaving the
//         definition in device.cpp anonymous namespace would make it
//         unreachable from this module (not injectable), and turning the
//         radius into an explicit parameter would change the A4 body and break
//         byte-identity.
//   War3SemanticHashMatrix4
//       - single-matrix FNV-1a; sister of the already-migrated
//         War3SemanticHashMatrixPalette.
// All inputs are explicit parameters; the module reads no device member, no
// registry and no hook global.
// A10 War3SemanticVectorStorageReadable (an uninstantiated function template)
// was DELETED by the 2026-09-18 dead-code ruling (separate, earlier change).
// A9 War3SemanticBuildWorldPaletteIfNeeded is dead by callers, but its body is
// the model-local -> world-space compose contract served by A4; the same
// ruling therefore MIGRATED it into this module (declaration below; body
// byte-identical in the module .cpp). See
// docs/plan/2026-09-18-a9-a10-dead-code-ruling-record.md.
// ---------------------------------------------------------------------------
bool War3SemanticPaletteInPlaceAppendRuntime();
bool War3SemanticDrawTimePoseRuntime();
float War3SemanticTranslationDistanceSq(const Matrix4& a, const Matrix4& b);
bool War3SemanticTranslationFinite(const Matrix4& m);
bool War3SemanticPaletteStorageReadable(const std::vector<Matrix4>& palette);
float War3SemanticBoundsRadiusForObjectKind(uint8_t objectKind);
bool War3SemanticPaletteLooksModelLocal(
    const Matrix4* palette,
    uint32_t paletteCount,
    const Matrix4& worldTransform,
    uint8_t objectKind,
    bool checkReadable = true);
bool War3SemanticPaletteLooksModelLocal(
    const std::vector<Matrix4>& palette,
    const Matrix4& worldTransform,
    uint8_t objectKind);
uint64_t War3SemanticHashMatrix4(const Matrix4& matrix);

// A9 (2026-09-18 dead-code ruling): model-local palette -> world-space compose
// reference (pre-A9 d3d9_device.cpp :7161-7175, where it was [[maybe_unused]]
// dead code with zero callers). Its call chain is the A4 compose-policy. The
// extraction and bidirectional byte-identity are enforced by
// AutoTest/gen_war3_live_palette_selection_legacy_reference.py --a9 and
// AutoTest/test_war3_palette_a9_migration_equivalence_static.py.
void War3SemanticBuildWorldPaletteIfNeeded(
    const std::vector<Matrix4>& sourcePalette,
    const Matrix4& worldTransform,
    uint8_t objectKind,
    std::vector<Matrix4>& outPalette);

} // namespace dxvk::war3::semantic
