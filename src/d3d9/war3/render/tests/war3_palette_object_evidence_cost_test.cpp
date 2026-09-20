// 2026-09-17 D6 前置（上级要求）：对象级 palette 证据记录器的**离线 CPU 成本测量**。
//
// 纯宿主机、纯 CPU：不加载游戏、不创建 Vulkan/D3D9 设备、不做任何像素或 GPU 时间测量。
// 本文件**不修改**记录器 / 环 / 发射器的语义（发现缺陷只打印，不改）。全部测量都走
// **生产实例** PaletteObjectRecorder()（war3_palette_object_evidence_sink.cpp 的
// g_paletteObjectEvidence），不复制一份记录器。
//
// 覆盖（对应上级要求的五项）：
//   1) 容量/饱和：填满 1024 槽观察表，测插入与查找的**最坏探测长度**（打印实测最大探测
//      次数与分布），以及表满后每次 NoteReject 的代价；
//   2) 淘汰/拒绝触发点：droppedPerFrame(64/帧) / droppedPerSession(3584) /
//      droppedTerminalReserve(512) / droppedTableFull(1024) / droppedPayloadConflict /
//      droppedDuplicatePerFrame 的**精确触发条件 + 实测计数值**；
//   3) 分配：填满 + 关闭 + 32 次 Reset 循环**仍零堆分配**（统计本 TU 的全局 operator
//      new/delete；_DEBUG 下另用 _CrtMemDifference 交叉验证）；
//   4) 关闭开销：CloseWindow 在满表（1024 条）下的耗时（QPC，us），分 0 / 64 / 512 个
//      终态实际发出的三种情形；
//   5) 开关开销：子门关闭时采集点判定 PaletteObjectEvidenceEnabled() 的单次成本（ns），
//      以及开启时每事件成本（R/S/E/D 各一组，ns/事件）。
//
// 措辞上限：这里的纳秒数是**CPU 侧**的实测值，只说明本机 CPU 上这些**代码路径**的成本；
// 它不是 GPU 开销，也不是像素证据，更不构成任何"实机性能已达标/已稳定"的结论。
// 未覆盖项在 stdout 末尾的 UNCOVERED 行与 docs/plan/2026-09-17-p0-object-evidence-cost-measurement.md
// 中如实列出。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../tools/war3_frame_evidence.h"
#include "../../tools/war3_palette_object_evidence_sink.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

// ---------------------------------------------------------------------------
// 零堆分配证据：统计本 TU 的全局 operator new / delete（与记录器宿主机测试同一机制）。
// 这是**进程级**替换，因此记录器、发射器、环与 CRT 之外的一切 C++ 堆分配都会被计入。
// 说明：meson release 构建下 _CrtMemDifference 不可用（非 _DEBUG 时是空实现），
// 所以主判据是 operator new / delete 计数；_DEBUG 下额外打印 CRT 差分作为交叉验证。
// ---------------------------------------------------------------------------
namespace {
std::atomic<uint64_t> g_newCalls{0u};
std::atomic<uint64_t> g_newArrayCalls{0u};
std::atomic<uint64_t> g_deleteCalls{0u};
std::atomic<uint64_t> g_deleteArrayCalls{0u};
} // namespace

void* operator new(std::size_t size) {
  g_newCalls.fetch_add(1u, std::memory_order_relaxed);
  void* memory = std::malloc(size ? size : 1u);
  if (memory == nullptr)
    std::abort();
  return memory;
}
void* operator new[](std::size_t size) {
  g_newArrayCalls.fetch_add(1u, std::memory_order_relaxed);
  void* memory = std::malloc(size ? size : 1u);
  if (memory == nullptr)
    std::abort();
  return memory;
}
void operator delete(void* memory) noexcept {
  if (memory == nullptr)
    return;
  g_deleteCalls.fetch_add(1u, std::memory_order_relaxed);
  std::free(memory);
}
void operator delete[](void* memory) noexcept {
  if (memory == nullptr)
    return;
  g_deleteArrayCalls.fetch_add(1u, std::memory_order_relaxed);
  std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete[](memory); }

namespace {

using dxvk::war3::tools::evidence::Event;
using dxvk::war3::tools::evidence::PaletteObjectEventRecord;
using dxvk::war3::tools::evidence::PaletteObjectEvidence;
using dxvk::war3::tools::evidence::PaletteObjectFrames;
using dxvk::war3::tools::evidence::PaletteObjectKey;
using dxvk::war3::tools::evidence::PaletteObjectRecorder;
using dxvk::war3::tools::evidence::PaletteObjectRejectReason;
using dxvk::war3::tools::evidence::PaletteObjectSource;

using Counters = PaletteObjectEvidence::Counters;

constexpr uint32_t kCapacity = PaletteObjectEvidence::kWatchCapacity;   // 1024
constexpr uint64_t kPerFrameBudget = PaletteObjectEvidence::kPerFrameBudget; // 64
constexpr uint64_t kNormalBudget = PaletteObjectEvidence::kNormalBudget;     // 3584
constexpr uint64_t kTerminalReserve = PaletteObjectEvidence::kTerminalReserve; // 512
constexpr uint64_t kTotalBudget = PaletteObjectEvidence::kTotalBudget;       // 4096

uint32_t g_failures = 0u;
uint32_t g_checks = 0u;
uint64_t g_qpcFrequency = 1u;
volatile uint64_t g_sink = 0u;
uint64_t g_emitterCalls = 0u;

bool Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("[FAIL] %s\n", what);
  }
  return condition;
}

uint64_t Now() {
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  return static_cast<uint64_t>(value.QuadPart);
}

uint64_t TicksToNs(uint64_t ticks) {
  return ticks * 1000000000ull / (g_qpcFrequency ? g_qpcFrequency : 1u);
}

PaletteObjectEvidence& Rec(); // 前置声明：MakeKey 需要记录器的当前会话

PaletteObjectKey MakeKey(uint64_t part) {
  PaletteObjectKey key;
  key.renderablePart = part;
  key.runtimeModelPtr = part + 0x1000u;
  key.jHandle = part + 7u;
  key.rawcode = 0x68303031u + (part & 0xFFu);
  // 2026-09-18 P0-1/P0-39：键必须携带**记录器当前会话**（与生产同源：4 个构键点均用
  // ActiveSession() 构造键）。此前硬编码的会话号与各处 Reset(...) 的实参不一致，
  // 且 0 代际键在**有会话**的记录器上会被接纳边界拒绝（会与读方的会话镜像校验冲突）。
  key.sessionGeneration = Rec().sessionGeneration();
  key.deviceEpoch = 0xC057D000u;
  key.lifecycleIdentity = part * 3u + 1u;
  key.identityWeak = false;
  key.epochUnknown = false;
  return key;
}

PaletteObjectFrames Frames(uint64_t frame) {
  PaletteObjectFrames frames;
  frames.renderFrame = frame;
  return frames;
}

PaletteObjectEvidence& Rec() { return PaletteObjectRecorder(); }

// 空发射器：测量记录器状态机本身（不含 wire 编码 / 环 append）。
void NullEmitter(void* /*context*/, const PaletteObjectEventRecord& /*record*/) {
  ++g_emitterCalls;
}

// 生产形状的发射器（**不含环 append**）：转换点 + 空消费者。生产发射器还会做
// ActiveSession() / PaletteObjectEvidenceEnabled() / Record(session,event)（环 append），
// 这三项由生产往返测试与 §"未覆盖"说明覆盖，不在这里冒充。
void WireEncodeEmitter(void* /*context*/, const PaletteObjectEventRecord& record) {
  Event event{};
  if (dxvk::war3::tools::evidence::EncodePaletteObjectEvent(record, 1u, event)) {
    ++g_emitterCalls;
    g_sink = g_sink + event.bits[5];
  }
}

uint64_t MedianOf(uint64_t* samples, uint32_t count) {
  for (uint32_t i = 1u; i < count; ++i) {
    const uint64_t value = samples[i];
    uint32_t j = i;
    while (j > 0u && samples[j - 1u] > value) {
      samples[j] = samples[j - 1u];
      --j;
    }
    samples[j] = value;
  }
  return samples[count / 2u];
}

// ---------------------------------------------------------------------------
// 1. 容量/饱和：最坏探测长度
// ---------------------------------------------------------------------------
// 机制说明（必须如实写清）：记录器**不暴露**逐键探测次数，因此探测长度由本测试用
// 生产 PaletteObjectEvidence::HashKey 与**与 Find/Insert 完全一致的探测顺序**在影子表上
// 复算：起始槽 = HashKey % 1024；Insert 取从起始槽起的第一个非 used 槽（墓碑可复用）；
// Find 从起始槽线性探测到命中或**真正的空槽**为止。本场景没有删除，故无墓碑。
// 交叉验证：生产记录器填满后 droppedProbeLimit 必须为 0（从未走满探测范围），
// 且表满后一条**从未出现过的键**必然让 Find 走满 1024 槽 ⇒ droppedProbeLimit +1。
struct ProbeStats {
  const char* label = "";
  uint64_t maxProbe = 0u;
  uint64_t totalProbe = 0u;
  uint64_t distinctStartSlots = 0u;
  uint32_t worstIndex = 0u;
  uint32_t bestIndex = 0u;
  uint64_t histogram[11] = {};
  uint64_t insertNsMax = 0u;
  uint64_t insertNsTotal = 0u;
  uint64_t lookupWorstNs = 0u;
  uint64_t lookupBestNs = 0u;
  uint64_t lookupSamples = 0u;
  uint64_t droppedProbeLimit = 0u;
};

uint32_t BucketOf(uint64_t probe) {
  if (probe <= 2u)
    return static_cast<uint32_t>(probe - 1u);           // 1, 2
  if (probe <= 4u)
    return 2u;                                          // 3-4
  if (probe <= 8u)
    return 3u;
  if (probe <= 16u)
    return 4u;
  if (probe <= 32u)
    return 5u;
  if (probe <= 64u)
    return 6u;
  if (probe <= 128u)
    return 7u;
  if (probe <= 256u)
    return 8u;
  if (probe <= 512u)
    return 9u;
  return 10u;
}

const uint64_t kFillFramesBase = 1000u;
const uint64_t kLookupFrame = 500000u;
const uint64_t kTableFullFrameBase = 700000u;
constexpr uint64_t kLookups = 200000u;

PaletteObjectKey FillKey(uint32_t index) { return MakeKey(0x900000u + index * 0x40u); }

// 一条**键族**的完整探测/查找测量：影子表复算探测长度（生产 HashKey + 与 Find/Insert 相同的
// 探测顺序），并用**同帧同载荷的第二次 NoteServed** 在生产记录器上隔离 Find 的运行时成本。
// 两个键族都测，因为最坏探测长度取决于键分布，单一键族会把"这一族的病态"冒充成"普遍最坏"。
ProbeStats MeasureKeyFamily(const char* label, uint64_t base, uint64_t stride) {
  PaletteObjectEvidence& rec = Rec();
  rec.Configure(nullptr, nullptr);
  rec.Reset(0xC0570001u, 0xC0570002u);

  bool occupied[kCapacity];
  bool seenStart[kCapacity];
  uint64_t probeShadow[kCapacity];
  for (uint32_t i = 0u; i < kCapacity; ++i) {
    occupied[i] = false;
    seenStart[i] = false;
    probeShadow[i] = 0u;
  }

  ProbeStats stats;
  stats.label = label;
  for (uint32_t i = 0u; i < kCapacity; ++i) {
    const PaletteObjectKey key = MakeKey(base + static_cast<uint64_t>(i) * stride);
    const uint32_t start = static_cast<uint32_t>(
        PaletteObjectEvidence::HashKey(key) % kCapacity);
    if (!seenStart[start]) {
      seenStart[start] = true;
      ++stats.distinctStartSlots;
    }
    uint32_t probe = 0u;
    while (occupied[(start + probe) % kCapacity])
      ++probe;
    occupied[(start + probe) % kCapacity] = true;
    probeShadow[i] = static_cast<uint64_t>(probe) + 1u; // 访问过的槽数（起始槽算 1）

    // 逐次插入的**运行时**成本包含每对 QPC 调用的固定开销（见 main 的 QPC_OVERHEAD 行）。
    const uint64_t t0 = Now();
    rec.NoteReject(key, PaletteObjectRejectReason::R1,
                   Frames(kFillFramesBase + i / kPerFrameBudget));
    const uint64_t t1 = Now();
    const uint64_t ns = TicksToNs(t1 - t0);
    if (ns > stats.insertNsMax)
      stats.insertNsMax = ns;
    stats.insertNsTotal += ns;

    stats.totalProbe += probeShadow[i];
    stats.histogram[BucketOf(probeShadow[i])]++;
    if (probeShadow[i] > stats.maxProbe) {
      stats.maxProbe = probeShadow[i];
      stats.worstIndex = i;
    }
  }
  stats.bestIndex = 0u;
  for (uint32_t i = 0u; i < kCapacity; ++i) {
    if (probeShadow[i] == 1u) {
      stats.bestIndex = i;
      break;
    }
  }
  const Counters counters = rec.counters();
  stats.droppedProbeLimit = counters.droppedProbeLimit;
  std::printf("PROBE_SHADOW family=%s inserted=%u watchCount=%u distinctStartSlots=%llu "
              "maxProbe=%llu meanProbeX1000=%llu insertNsMax=%llu insertNsMeanX1000=%llu "
              "droppedProbeLimit=%llu droppedTableFull=%llu\n",
              label, static_cast<unsigned>(kCapacity), static_cast<unsigned>(rec.watchCount()),
              static_cast<unsigned long long>(stats.distinctStartSlots),
              static_cast<unsigned long long>(stats.maxProbe),
              static_cast<unsigned long long>(stats.totalProbe * 1000ull / kCapacity),
              static_cast<unsigned long long>(stats.insertNsMax),
              static_cast<unsigned long long>(stats.insertNsTotal * 1000ull / kCapacity),
              static_cast<unsigned long long>(counters.droppedProbeLimit),
              static_cast<unsigned long long>(counters.droppedTableFull));
  std::printf("PROBE_HISTOGRAM family=%s buckets(1,2,3-4,5-8,9-16,17-32,33-64,65-128,129-256,"
              "257-512,513-1024)=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
              label,
              static_cast<unsigned long long>(stats.histogram[0]),
              static_cast<unsigned long long>(stats.histogram[1]),
              static_cast<unsigned long long>(stats.histogram[2]),
              static_cast<unsigned long long>(stats.histogram[3]),
              static_cast<unsigned long long>(stats.histogram[4]),
              static_cast<unsigned long long>(stats.histogram[5]),
              static_cast<unsigned long long>(stats.histogram[6]),
              static_cast<unsigned long long>(stats.histogram[7]),
              static_cast<unsigned long long>(stats.histogram[8]),
              static_cast<unsigned long long>(stats.histogram[9]),
              static_cast<unsigned long long>(stats.histogram[10]));
  std::printf("PROBE_WORST_KEY family=%s index=%u probe=%llu ; PROBE_BEST_KEY family=%s index=%u "
              "probe=1 (shadow replay of the production Find/Insert probe order + production "
              "HashKey)\n",
              label, static_cast<unsigned>(stats.worstIndex),
              static_cast<unsigned long long>(stats.maxProbe), label,
              static_cast<unsigned>(stats.bestIndex));

  Check(rec.watchCount() == kCapacity, "the watch table must fill to kWatchCapacity");
  Check(counters.droppedProbeLimit == 0u,
        "filling the table must never exhaust the probe range");
  Check(counters.droppedTableFull == 0u, "filling the table must not report TableFull");
  Check(stats.maxProbe >= 2u, "a 1024-slot table with 1024 keys must show probe collisions");

  // 查找成本：用**同帧同载荷的第二次 NoteServed** 恰好隔离 Find（Find → 载荷比较 → 去重计数）。
  const PaletteObjectFrames lookupFrames = Frames(kLookupFrame);
  const PaletteObjectKey worstKey = MakeKey(base + static_cast<uint64_t>(stats.worstIndex) * stride);
  const PaletteObjectKey bestKey = MakeKey(base + static_cast<uint64_t>(stats.bestIndex) * stride);

  rec.NoteServed(worstKey, PaletteObjectSource::ArenaSlot, 0x1u,
                 lookupFrames); // 本帧第一条：发射（预算内）
  const uint64_t worstBefore = rec.counters().droppedDuplicatePerFrame;
  uint64_t t0 = Now();
  for (uint64_t i = 0u; i < kLookups; ++i)
    rec.NoteServed(worstKey, PaletteObjectSource::ArenaSlot, 0x1u, lookupFrames);
  uint64_t t1 = Now();
  stats.lookupWorstNs = TicksToNs(t1 - t0) / kLookups;
  const uint64_t worstDedup = rec.counters().droppedDuplicatePerFrame - worstBefore;

  const uint64_t bestBefore = rec.counters().droppedDuplicatePerFrame;
  rec.NoteServed(bestKey, PaletteObjectSource::ArenaSlot, 0x1u, lookupFrames);
  t0 = Now();
  for (uint64_t i = 0u; i < kLookups; ++i)
    rec.NoteServed(bestKey, PaletteObjectSource::ArenaSlot, 0x1u, lookupFrames);
  t1 = Now();
  stats.lookupBestNs = TicksToNs(t1 - t0) / kLookups;
  const uint64_t bestDedup = rec.counters().droppedDuplicatePerFrame - bestBefore;
  stats.lookupSamples = kLookups;

  std::printf("LOOKUP_RUNTIME family=%s samples=%llu frame=%llu worstProbe=%llu worstNs=%llu "
              "bestProbe=1 bestNs=%llu ratioX1000=%llu dedupWorst=%llu dedupBest=%llu "
              "(isolated Find + payload compare + dedup counter; measured on the production "
              "recorder instance)\n",
              label, static_cast<unsigned long long>(kLookups),
              static_cast<unsigned long long>(kLookupFrame),
              static_cast<unsigned long long>(stats.maxProbe),
              static_cast<unsigned long long>(stats.lookupWorstNs),
              static_cast<unsigned long long>(stats.lookupBestNs),
              static_cast<unsigned long long>(stats.lookupBestNs
                                                  ? stats.lookupWorstNs * 1000ull /
                                                        stats.lookupBestNs
                                                  : 0ull),
              static_cast<unsigned long long>(worstDedup),
              static_cast<unsigned long long>(bestDedup));
  Check(worstDedup == kLookups && bestDedup == kLookups,
        "the isolated lookup loop must take the same-frame dedup path every time");
  Check(stats.lookupBestNs > 0u && stats.lookupWorstNs >= stats.lookupBestNs,
        "the worst-placed key must not look up faster than the best-placed key");
  return stats;
}

void MeasureProbeLengths() {
  // 两个键族：A = 既有记录器宿主机用例使用的族（part 步进 0x40），B = 指针样族（8 字节对齐）。
  const ProbeStats familyA = MeasureKeyFamily("A_existing_test_keys(part=0x900000+i*0x40)",
                                              0x900000ull, 0x40ull);
  const ProbeStats familyB = MeasureKeyFamily("B_pointer_like_keys(part=0x1000000+i*8)",
                                              0x1000000ull, 8ull);
  std::printf("PROBE_SUMMARY familyA_maxProbe=%llu familyB_maxProbe=%llu "
              "familyA_distinctStarts=%llu familyB_distinctStarts=%llu "
              "familyA_worstNs=%llu familyB_worstNs=%llu\n",
              static_cast<unsigned long long>(familyA.maxProbe),
              static_cast<unsigned long long>(familyB.maxProbe),
              static_cast<unsigned long long>(familyA.distinctStartSlots),
              static_cast<unsigned long long>(familyB.distinctStartSlots),
              static_cast<unsigned long long>(familyA.lookupWorstNs),
              static_cast<unsigned long long>(familyB.lookupWorstNs));

  // 表满后每次 NoteReject 的代价：**布隆前置否定**让「未知键」不再走满 1024 槽（2026-09-17：
  // 加布隆前实测 ~850 ns/次、droppedProbeLimit 每条 +1；加布隆后为常数级负查找）→
  // Insert 一次立即被拒（m_watchCount >= kWatchCapacity）→ 计数。第一条还会公告一次 TableFull 终态。
  // 此刻表内是键族 B 的 1024 条。
  PaletteObjectEvidence& rec = Rec();
  const uint64_t probeBefore = rec.counters().droppedProbeLimit;
  const uint64_t tableFullBefore = rec.counters().droppedTableFull;
  const uint64_t closedBefore = rec.counters().closedTableFull;
  uint64_t t0 = Now();
  rec.NoteReject(MakeKey(0xAA0000u), PaletteObjectRejectReason::R3, Frames(kTableFullFrameBase));
  uint64_t t1 = Now();
  const uint64_t firstFullNs = TicksToNs(t1 - t0);
  constexpr uint64_t kFullCalls = 1000u;
  t0 = Now();
  for (uint64_t i = 0u; i < kFullCalls; ++i)
    rec.NoteReject(MakeKey(0xAB0000u + i * 0x40u), PaletteObjectRejectReason::R3,
                   Frames(kTableFullFrameBase + 1u + i));
  t1 = Now();
  const uint64_t fullNs = TicksToNs(t1 - t0) / kFullCalls;
  const Counters afterFull = rec.counters();
  std::printf("TABLEFULL firstUnknownNs=%llu subsequentCalls=%llu subsequentNs=%llu "
              "droppedProbeLimitDelta=%llu droppedTableFullDelta=%llu closedTableFullDelta=%llu "
              "watchCount=%u (each unknown key is a bloom-miss O(1) negative lookup plus "
              "one immediate Insert refusal; the pre-bloom code walked all 1024 slots per key)\n",
              static_cast<unsigned long long>(firstFullNs),
              static_cast<unsigned long long>(kFullCalls),
              static_cast<unsigned long long>(fullNs),
              static_cast<unsigned long long>(afterFull.droppedProbeLimit - probeBefore),
              static_cast<unsigned long long>(afterFull.droppedTableFull - tableFullBefore),
              static_cast<unsigned long long>(afterFull.closedTableFull - closedBefore),
              static_cast<unsigned>(rec.watchCount()));
  // 布隆前置否定：表满 + 未知键**不得**走满探测范围（这正是加布隆要消除的退化）。
  // 布隆有**设计内**的假阳性（约 1.5%）：要求未知键的全表走查降到 10% 以下（加布隆前是 100%）。
  const uint64_t walkDelta = afterFull.droppedProbeLimit - probeBefore;
  Check(walkDelta * 10u <= kFullCalls,
        "the bloom pre-filter must cut full-table walks for unknown keys to well under 10% "
        "(100% before the bloom was added)");
  Check(afterFull.droppedTableFull - tableFullBefore == kFullCalls + 1u,
        "every refused insertion must be counted in droppedTableFull");
  Check(afterFull.closedTableFull - closedBefore == 1u,
        "TableFull may be announced (and settled) only once per window");
}

// ---------------------------------------------------------------------------
// 2. 淘汰/拒绝触发点
// ---------------------------------------------------------------------------
void PrintTrigger(const char* name, const char* condition, const Counters& c,
                  uint64_t emittedDelta) {
  std::printf("TRIGGER %s condition=\"%s\" emittedDelta=%llu droppedPerFrame=%llu "
              "droppedPerSession=%llu droppedTableFull=%llu droppedProbeLimit=%llu "
              "droppedDuplicatePerFrame=%llu droppedPayloadConflict=%llu "
              "droppedTerminalReserve=%llu terminalEmitted=%llu closedTableFull=%llu\n",
              name, condition, static_cast<unsigned long long>(emittedDelta),
              static_cast<unsigned long long>(c.droppedPerFrame),
              static_cast<unsigned long long>(c.droppedPerSession),
              static_cast<unsigned long long>(c.droppedTableFull),
              static_cast<unsigned long long>(c.droppedProbeLimit),
              static_cast<unsigned long long>(c.droppedDuplicatePerFrame),
              static_cast<unsigned long long>(c.droppedPayloadConflict),
              static_cast<unsigned long long>(c.droppedTerminalReserve),
              static_cast<unsigned long long>(c.terminalEmitted),
              static_cast<unsigned long long>(c.closedTableFull));
}

void MeasureTriggerPoints() {
  PaletteObjectEvidence& rec = Rec();

  { // droppedPerFrame：同一帧内只允许 64 条事件；第 65 条起记 droppedPerFrame。
    rec.Configure(nullptr, nullptr);
    rec.Reset(0xC0571001u, 0xC0571002u);
    for (uint32_t i = 0u; i < 100u; ++i)
      rec.NoteReject(FillKey(i), PaletteObjectRejectReason::R1, Frames(1u + i));
    const uint64_t emittedBefore = rec.counters().emitted;
    constexpr uint64_t kFrame = 900000u;
    for (uint32_t i = 0u; i < 100u; ++i)
      rec.NoteReject(FillKey(i), PaletteObjectRejectReason::R1, Frames(kFrame));
    const Counters c = rec.counters();
    PrintTrigger("droppedPerFrame",
                 "more than kPerFrameBudget(64) accepted events in ONE render frame",
                 c, c.emitted - emittedBefore);
    Check(c.droppedPerFrame == 36u && c.emitted - emittedBefore == 64u,
          "droppedPerFrame must trigger at exactly the 65th accepted event of one frame");
    Check(c.droppedPerSession == 0u,
          "a per-frame refusal must not be charged to the session budget");
  }

  { // droppedPerSession：普通事件上限 kNormalBudget(3584)；再来的普通事件记 droppedPerSession。
    rec.Configure(nullptr, nullptr);
    rec.Reset(0xC0572001u, 0xC0572002u);
    const PaletteObjectKey key = FillKey(7u);
    rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(1u));
    uint64_t frame = 1u;
    while (rec.counters().emitted < kNormalBudget && frame < 100000u) {
      ++frame;
      rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(frame));
    }
    const uint64_t emittedAtLimit = rec.counters().emitted;
    constexpr uint64_t kExtra = 3u;
    for (uint64_t i = 0u; i < kExtra; ++i) {
      ++frame;
      rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(frame));
    }
    const Counters c = rec.counters();
    PrintTrigger("droppedPerSession",
                 "normal events reach kNormalBudget(3584) = kTotalBudget(4096) - "
                 "kTerminalReserve(512)",
                 c, c.emitted - emittedAtLimit);
    Check(emittedAtLimit == kNormalBudget && c.emitted == kNormalBudget,
          "normal events must stop exactly at kNormalBudget");
    Check(c.droppedPerSession == kExtra && c.droppedPerFrame == 0u,
          "normal events refused by the terminal reserve must be counted in droppedPerSession");
  }

  { // droppedTerminalReserve：满表结算时预留只放行 512 条终态，其余记 droppedTerminalReserve。
    rec.Configure(nullptr, nullptr);
    rec.Reset(0xC0573001u, 0xC0573002u);
    for (uint32_t i = 0u; i < kCapacity; ++i)
      rec.NoteReject(FillKey(i), PaletteObjectRejectReason::R1,
                     Frames(kFillFramesBase + i / kPerFrameBudget));
    const uint64_t emittedBefore = rec.counters().emitted;
    rec.CloseWindow(kFillFramesBase + kCapacity / kPerFrameBudget + 1u);
    const Counters c = rec.counters();
    PrintTrigger("droppedTerminalReserve",
                 "CloseWindow settles kWatchCapacity(1024) entries while "
                 "kTerminalReserve(512) terminals are available",
                 c, c.emitted - emittedBefore);
    Check(c.terminalEmitted == kTerminalReserve,
          "the terminal reserve (512) must be fully consumed by the 1024-entry settlement");
    Check(c.droppedTerminalReserve == kCapacity - kTerminalReserve,
          "every terminal past the 512 reserve must be counted in droppedTerminalReserve");
  }

  { // droppedTableFull：表满后第一条新键公告一次 TableFull 终态，其余只累加。
    rec.Configure(nullptr, nullptr);
    rec.Reset(0xC0574001u, 0xC0574002u);
    for (uint32_t i = 0u; i < kCapacity; ++i)
      rec.NoteReject(FillKey(i), PaletteObjectRejectReason::R1,
                     Frames(kFillFramesBase + i / kPerFrameBudget));
    const uint64_t emittedBefore = rec.counters().emitted;
    rec.NoteReject(MakeKey(0xAC0000u), PaletteObjectRejectReason::R3, Frames(800000u));
    rec.NoteReject(MakeKey(0xAC0040u), PaletteObjectRejectReason::R3, Frames(800001u));
    const Counters c = rec.counters();
    PrintTrigger("droppedTableFull",
                 "m_watchCount reaches kWatchCapacity(1024) and Insert returns nullptr",
                 c, c.emitted - emittedBefore);
    Check(c.droppedTableFull == 2u && c.closedTableFull == 1u,
          "droppedTableFull counts every refusal while TableFull is settled only once");
  }

  { // droppedDuplicatePerFrame / droppedPayloadConflict：四个阶段各一对（等价 / 冲突）。
    rec.Configure(nullptr, nullptr);
    rec.Reset(0xC0575001u, 0xC0575002u);
    const PaletteObjectKey key = MakeKey(0xD6D600u);
    rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(10u)); // 建条目（也发射）
    const uint64_t emittedBefore = rec.counters().emitted;
    constexpr uint64_t kFrame = 20u;
    rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(kFrame));            // 本帧第一条
    rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(kFrame));            // 等价
    rec.NoteReject(key, PaletteObjectRejectReason::R2, Frames(kFrame));            // 冲突
    rec.NoteServed(key, PaletteObjectSource::ArenaSlot, 0x11u, Frames(kFrame));
    rec.NoteServed(key, PaletteObjectSource::ArenaSlot, 0x11u, Frames(kFrame));    // 等价
    rec.NoteServed(key, PaletteObjectSource::PoseKernel, 0x11u, Frames(kFrame));   // 冲突
    rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0x22u, Frames(kFrame), false);
    rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0x22u, Frames(kFrame), false); // 等价
    rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0x22u, Frames(kFrame), true);  // 冲突
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x33u, Frames(kFrame), false);
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x33u, Frames(kFrame),
                  false); // 等价
    rec.NoteDrawn(key, PaletteObjectSource::Unknown, 0x44u, Frames(kFrame), true);  // 冲突
    const Counters c = rec.counters();
    PrintTrigger("droppedDuplicatePerFrame+droppedPayloadConflict",
                 "same key / same render frame / same stage at most once, decided ONLY by "
                 "payload equivalence (4 stages, one equivalent + one conflict each)",
                 c, c.emitted - emittedBefore);
    Check(c.droppedDuplicatePerFrame == 4u,
          "four payload-equivalent same-frame repeats must count as droppedDuplicatePerFrame");
    Check(c.droppedPayloadConflict == 4u,
          "four payload-conflicting same-frame repeats must count as droppedPayloadConflict");
    Check(c.emitted - emittedBefore == 4u,
          "one frame must keep exactly one event per stage (no conflict may become an event)");
    Check(c.droppedPerFrame == 0u && c.droppedPerSession == 0u,
          "same-frame aggregation must not be charged to the budget buckets");
  }
}

// ---------------------------------------------------------------------------
// 3. 分配：填满 + 关闭 + 32 次 Reset 循环仍零堆分配
// ---------------------------------------------------------------------------
void FillTable(uint64_t frameBase) {
  PaletteObjectEvidence& rec = Rec();
  for (uint32_t i = 0u; i < kCapacity; ++i)
    rec.NoteReject(FillKey(i), PaletteObjectRejectReason::R1,
                   Frames(frameBase + i / kPerFrameBudget));
}

void MeasureAllocation() {
  PaletteObjectEvidence& rec = Rec();
  rec.Configure(nullptr, nullptr);
  rec.Reset(0xC0576001u, 0xC0576002u);
  FillTable(kFillFramesBase);
  rec.CloseWindow(kFillFramesBase + 100u);
  // 预热 printf 与 CRT（避免把 CRT 首次缓冲分配算进被测区间）。
  std::printf("ALLOC warmup sizeofRecorder=%llu sizeofRecord=%llu sizeofEvent=%llu\n",
              static_cast<unsigned long long>(sizeof(PaletteObjectEvidence)),
              static_cast<unsigned long long>(sizeof(PaletteObjectEventRecord)),
              static_cast<unsigned long long>(sizeof(Event)));
  const uint64_t newBefore = g_newCalls.load(std::memory_order_relaxed);
  const uint64_t newArrayBefore = g_newArrayCalls.load(std::memory_order_relaxed);
  const uint64_t deleteBefore = g_deleteCalls.load(std::memory_order_relaxed);
  const uint64_t deleteArrayBefore = g_deleteArrayCalls.load(std::memory_order_relaxed);
#ifdef _DEBUG
  _CrtMemState crtBefore, crtAfter, crtDiff;
  _CrtMemCheckpoint(&crtBefore);
#endif
  constexpr uint32_t kCycles = 32u;
  for (uint32_t cycle = 0u; cycle < kCycles; ++cycle) {
    rec.Reset(0xC0576001u + cycle, 0xC0576002u + cycle);
    FillTable(kFillFramesBase + 100000u * (cycle + 1u));
    rec.CloseWindow(kFillFramesBase + 100000u * (cycle + 1u) + 100u);
  }
#ifdef _DEBUG
  _CrtMemCheckpoint(&crtAfter);
  const int crtDifference = _CrtMemDifference(&crtDiff, &crtBefore, &crtAfter);
#endif
  const uint64_t newAfter = g_newCalls.load(std::memory_order_relaxed);
  const uint64_t newArrayAfter = g_newArrayCalls.load(std::memory_order_relaxed);
  const uint64_t deleteAfter = g_deleteCalls.load(std::memory_order_relaxed);
  const uint64_t deleteArrayAfter = g_deleteArrayCalls.load(std::memory_order_relaxed);
  std::printf("ALLOC cycles=%u fillPerCycle=%u newDelta=%llu newArrayDelta=%llu "
              "deleteDelta=%llu deleteArrayDelta=%llu\n",
              static_cast<unsigned>(kCycles + 1u), static_cast<unsigned>(kCapacity),
              static_cast<unsigned long long>(newAfter - newBefore),
              static_cast<unsigned long long>(newArrayAfter - newArrayBefore),
              static_cast<unsigned long long>(deleteAfter - deleteBefore),
              static_cast<unsigned long long>(deleteArrayAfter - deleteArrayBefore));
#ifdef _DEBUG
  std::printf("ALLOC _CrtMemDifference=%d (totalCount=%lld) [debug CRT cross-check]\n",
              crtDifference, static_cast<long long>(crtDiff.lTotalCount));
#else
  std::printf("ALLOC _CrtMemDifference=unavailable (release CRT: _DEBUG not defined); the "
              "process-wide operator new/delete counters above are the mechanism used\n");
#endif
  Check(newAfter == newBefore && newArrayAfter == newArrayBefore,
        "the recorder must not call operator new / new[] across 33 fill+close cycles");
  Check(deleteAfter == deleteBefore && deleteArrayAfter == deleteArrayBefore,
        "the recorder must not call operator delete / delete[] across 33 fill+close cycles");
}

// ---------------------------------------------------------------------------
// 4. 关闭开销：CloseWindow 在满表（1024 条）下的耗时
// ---------------------------------------------------------------------------
struct CloseCaseResult {
  uint64_t emittedTerminals = 0u;
  uint64_t droppedReserve = 0u;
  uint64_t minNs = 0u;
  uint64_t medNs = 0u;
  uint64_t maxNs = 0u;
  uint64_t emittedBeforeClose = 0u;
};

CloseCaseResult TimeCloseWindow(uint32_t reserveConsumed, uint32_t repeats) {
  PaletteObjectEvidence& rec = Rec();
  rec.Configure(nullptr, nullptr);
  CloseCaseResult out;
  uint64_t samples[64];
  Check(repeats <= 64u, "the close-timing sample buffer must hold every repeat");
  for (uint32_t r = 0u; r < repeats; ++r) {
    rec.Reset(0xC105E0000ull + r, 0xC105E0001ull + r);
    // 先消耗 reserveConsumed 条终态预留（每条：建条目 = 1 普通事件 + NoteObjectGone = 1 终态）。
    for (uint32_t i = 0u; i < reserveConsumed; ++i) {
      const PaletteObjectKey key = MakeKey(0x810000u + i);
      // 2026-09-18 阶段 C：**每个键一个独立帧**。
      // 原实现按 `i / kPerFrameBudget` 分帧（每帧 128 个键，而每帧预算为 64）⇒
      // **一半的 NoteReject 被每帧预算拒发**，本循环实际只消耗了 reserveConsumed/2 条预留，
      // 与它的注释（"先消耗 reserveConsumed 条终态预留"）不符。
      // 该缺陷此前被另一个缺陷掩盖：修复前"被预算拒发也照样建条目"，于是 NoteObjectGone
      // 仍能对每个键发出终态，表面上刚好消耗 N 条。
      // 现在两类入口都在 Insert 前过预算（本阶段 C 的修复），掩蔽消失 ⇒ 修夹具使其意图成立。
      // **这不是放宽断言，而是让夹具真正做到它所声称的事。**
      rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(10u + i));
      rec.NoteObjectGone(key);
    }
    // 再填满 1024 槽观察表（每帧 64 条普通事件）。
    FillTable(100000u);
    const Counters before = rec.counters();
    const uint64_t t0 = Now();
    rec.CloseWindow(200000u);
    const uint64_t t1 = Now();
    samples[r] = TicksToNs(t1 - t0);
    const Counters after = rec.counters();
    out.emittedBeforeClose = before.emitted;
    out.emittedTerminals = after.terminalEmitted - before.terminalEmitted;
    out.droppedReserve = after.droppedTerminalReserve - before.droppedTerminalReserve;
  }
  uint64_t minNs = samples[0];
  uint64_t maxNs = samples[0];
  for (uint32_t r = 1u; r < repeats; ++r) {
    if (samples[r] < minNs)
      minNs = samples[r];
    if (samples[r] > maxNs)
      maxNs = samples[r];
  }
  out.minNs = minNs;
  out.maxNs = maxNs;
  out.medNs = MedianOf(samples, repeats);
  return out;
}

void MeasureCloseWindow() {
  constexpr uint32_t kRepeats = 21u;
  const uint32_t cases[3] = {0u, static_cast<uint32_t>(kTerminalReserve) - 64u,
                             static_cast<uint32_t>(kTerminalReserve)};
  for (uint32_t index = 0u; index < 3u; ++index) {
    const uint32_t reserveConsumed = cases[index];
    const CloseCaseResult result = TimeCloseWindow(reserveConsumed, kRepeats);
    std::printf("CLOSE reserveConsumed=%u entries=%u repeats=%u emittedTerminals=%llu "
                "droppedTerminalReserve=%llu minUs=%llu medianUs=%llu maxUs=%llu "
                "emittedBeforeClose=%llu\n",
                static_cast<unsigned>(reserveConsumed), static_cast<unsigned>(kCapacity),
                static_cast<unsigned>(kRepeats),
                static_cast<unsigned long long>(result.emittedTerminals),
                static_cast<unsigned long long>(result.droppedReserve),
                static_cast<unsigned long long>(result.minNs / 1000u),
                static_cast<unsigned long long>(result.medNs / 1000u),
                static_cast<unsigned long long>(result.maxNs / 1000u),
                static_cast<unsigned long long>(result.emittedBeforeClose));
    Check(result.emittedTerminals == kTerminalReserve - reserveConsumed,
          "CloseWindow must emit exactly the remaining terminals allowed by the 512 reserve");
    Check(result.droppedReserve == kCapacity - result.emittedTerminals,
          "every settled entry past the reserve must be counted in droppedTerminalReserve");
  }
}

// ---------------------------------------------------------------------------
// 5. 开关开销
// ---------------------------------------------------------------------------
double TimeStage(const char* label, uint32_t which, PaletteObjectEvidence::EmitFn emitter,
                 uint64_t events) {
  PaletteObjectEvidence& rec = Rec();
  rec.Configure(emitter, nullptr);
  rec.Reset(0xE0570000u + which * 16u, 0xE0570001u + which * 16u);
  const PaletteObjectKey key = MakeKey(0x990000u + which * 0x100u);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, Frames(1u)); // 建条目（预算内）
  const uint64_t t0 = Now();
  for (uint64_t i = 0u; i < events; ++i) {
    const PaletteObjectFrames frames = Frames(2u + i);
    switch (which) {
    case 0u: rec.NoteReject(key, PaletteObjectRejectReason::R1, frames); break;
    case 1u: rec.NoteServed(key, PaletteObjectSource::ArenaSlot, 0x11u, frames); break;
    case 2u: rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0x22u, frames, false); break;
    default: rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x33u, frames, false); break;
    }
  }
  const uint64_t t1 = Now();
  const uint64_t totalNs = TicksToNs(t1 - t0);
  const double perEvent = events ? static_cast<double>(totalNs) / static_cast<double>(events) : 0.0;
  std::printf("EVENT stage=%s emitter=%s events=%llu totalNs=%llu nsPerEventX1000=%llu\n", label,
              emitter == &WireEncodeEmitter ? "wire-encode-no-ring" : "none",
              static_cast<unsigned long long>(events),
              static_cast<unsigned long long>(totalNs),
              static_cast<unsigned long long>(perEvent * 1000.0));
  return perEvent;
}

void MeasureGateAndEvents() {
  // 5a) 子门关闭时采集点判定：生产 PaletteObjectEvidenceEnabled()（另一 TU 的非 inline 调用
  //     + 函数局部 static 的线程安全初始化卫兵）。默认环境（未设 env）下子门关闭。
  constexpr uint64_t kPredicateCalls = 2000000u;
  uint64_t t0 = Now();
  for (uint64_t i = 0u; i < kPredicateCalls; ++i) {
    if (dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled())
      g_sink = g_sink + 1u;
  }
  uint64_t t1 = Now();
  const uint64_t predicateNs = TicksToNs(t1 - t0) / kPredicateCalls;
  std::printf("GATE predicate=PaletteObjectEvidenceEnabled calls=%llu nsPerCallX1000=%llu "
              "(sub-gate default OFF in this process: %s)\n",
              static_cast<unsigned long long>(kPredicateCalls),
              static_cast<unsigned long long>(predicateNs * 1000ull),
              dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled() ? "on" : "off");
  Check(predicateNs < 100u, "the sub-gate predicate must stay in the nanosecond range");

  // 5b) 开启时每事件成本（R/S/E/D 各一组，两种发射器）。措辞：**CPU 侧**测量。
  constexpr uint64_t kEvents = 2000u;
  const char* labels[4] = {"R(NoteReject)", "S(NoteServed)", "E(NoteEnqueued)", "D(NoteDrawn)"};
  for (uint32_t which = 0u; which < 4u; ++which)
    TimeStage(labels[which], which, &NullEmitter, kEvents);
  for (uint32_t which = 0u; which < 4u; ++which)
    TimeStage(labels[which], which, &WireEncodeEmitter, kEvents);
  std::printf("EVENT_NOTE all numbers are CPU-side code-path costs; they are NOT GPU work, NOT "
              "pixel evidence, and the production emitter's ring append (Record) plus "
              "ActiveSession()/sub-gate re-check are excluded here.\n");
}

} // namespace

int main() {
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  g_qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
  std::printf("war3_palette_object_evidence_cost_test: offline CPU-side cost measurement "
              "(host-only, no game, no GPU, no pixels)\n");
  std::printf("COST_ENV qpcFrequency=%llu sizeofRecorder=%llu sizeofRecord=%llu sizeofEvent=%llu "
              "kWatchCapacity=%u kPerFrameBudget=%llu kNormalBudget=%llu kTerminalReserve=%llu "
              "kTotalBudget=%llu\n",
              static_cast<unsigned long long>(g_qpcFrequency),
              static_cast<unsigned long long>(sizeof(PaletteObjectEvidence)),
              static_cast<unsigned long long>(sizeof(PaletteObjectEventRecord)),
              static_cast<unsigned long long>(sizeof(Event)),
              static_cast<unsigned>(kCapacity),
              static_cast<unsigned long long>(kPerFrameBudget),
              static_cast<unsigned long long>(kNormalBudget),
              static_cast<unsigned long long>(kTerminalReserve),
              static_cast<unsigned long long>(kTotalBudget));

  // QPC 固定开销（每次计时 = 一对 QueryPerformanceCounter）——用于解读**逐次**计时的数字
  // （键族插入计时是逐次 QPC，包含该固定开销；下面的循环计时每次只用一对 QPC，可忽略）。
  constexpr uint64_t kQpcPairs = 200000u;
  uint64_t qpcSink = 0u;
  const uint64_t qpc0 = Now();
  for (uint64_t i = 0u; i < kQpcPairs; ++i)
    qpcSink += Now();
  const uint64_t qpc1 = Now();
  const uint64_t qpcNsPerPair = TicksToNs(qpc1 - qpc0) / kQpcPairs;
  std::printf("QPC_OVERHEAD pairs=%llu nsPerPairX1000=%llu qpcSinkLow=%llu\n",
              static_cast<unsigned long long>(kQpcPairs),
              static_cast<unsigned long long>(qpcNsPerPair * 1000ull),
              static_cast<unsigned long long>(qpcSink & 0xFFu));

  MeasureProbeLengths();
  MeasureTriggerPoints();
  MeasureAllocation();
  MeasureCloseWindow();
  MeasureGateAndEvents();

  std::printf("UNCOVERED ring append (Record(session,event)) cost, wire export/JSON serialization, "
              "GPU submission, GPU execution, pixels, other CPUs/compilers, and any production "
              "frame budget under a real game workload are NOT measured here.\n");
  std::printf("COST_VERDICT=%s checks=%u failures=%u\n", g_failures == 0u ? "PASS" : "FAIL",
              static_cast<unsigned>(g_checks), static_cast<unsigned>(g_failures));
  std::printf("SUMMARY: %s\n", g_failures == 0u ? "all checks passed" : "FAILURES PRESENT");
  return g_failures == 0u ? 0 : 1;
}
