// M1 语义职责迁移：新模块的宿主机等价测试（differential + 边界）。
//
// 三部分组成：
//   (1) 模块头文件内联判定的边界用例（保留原测试的逐条边界覆盖）；
//   (2) 逐条差分等价：把**迁移前** d3d9_device.cpp 的实现（legacy 参考，见
//       war3_device_semantic_predicates_legacy_reference.inc，自动取自 git HEAD
//       的逐字节原文）与迁出后的模块实现放在同一输入域上对比，要求
//       - 返回值 / 输出参数逐位相同，
//       - 求值副作用（path blocker 计数器、widget 身份写穿缓存）增量与终态相同。
//       输入域 = 枚举小域 + 固定种子随机域；
//   (3) 运行时配置 getter 的 --probe 模式：由
//       AutoTest/test_device_semantic_predicates_equivalence_static.py 在不同 env 下驱动，
//       同时对比模块实现与 legacy 参考实现（同一进程，同一 env）。
//
// 链接说明：本可执行文件链接真实的 war3_device_semantic_predicates.cpp，并为
// 设备层/注册表/钩子原语提供**宿主机替身**（下面的 "host stubs"）。替身把外部世界
// （可读内存范围、widget 身份缓存、三个注册表、语义场景开关）变成测试可控的表；
// legacy 参考与模块实现**共用同一份替身**，因此差分隔离出的正是 M1 搬动的那部分逻辑。
//
// 边界（未覆盖，如实声明）：
//   - 替身不是生产实现：本测试证明"同一输入下两个实现的判定与控制流一致"，
//     不证明注册表/内存读取原语本身正确；
//   - 函数内 static 的 env getter 在一进程内只读一次 env，因此单个进程只能覆盖
//     一组 env 分支；驱动脚本用多组 env 多次运行覆盖回退分支；
//   - 未覆盖 device.cpp 侧的调用点编排（属 M4 范围）。

#include "../war3_device_semantic_predicates.h"

// 模块 .cpp 所依赖的原语头（宿主机替身需要完整声明；模块头本身只带其中一部分）。
#include "../../debug/war3_shadow_build_context_trace.h"
#include "../../core/war3_internal_test_config.h"
#include "../../core/war3_memory.h"
#include "../../core/war3_game_structs.h"
#include "../../core/war3_semantic_shadow_gate.h"
#include "../../hooks/war3_hook_widget_identity.h"
#include "../../render/war3_shadow_object_registry.h"
#include "../../../../util/util_env.h"

// 迁移前实现（verbatim，自动生成；见 .inc 头部 provenance）。
// 它自带全局 using-directive 与 namespace m1_legacy_reference。
#include "war3_device_semantic_predicates_legacy_reference.inc"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sem = dxvk::war3::semantic;
namespace legacy = m1_legacy_reference;

namespace {

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;

void Check(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::cerr << "FAIL line " << line << ": " << expr << "\n";
  }
}

void DiffU64(const char* what, uint64_t a, uint64_t b, int line) {
  ++g_checks;
  if (a != b) {
    ++g_failures;
    std::cerr << "DIFF line " << line << " " << what << ": module=" << a
              << " legacy=" << b << "\n";
  }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)
#define DIFF_U64(what, a, b) DiffU64(what, uint64_t(a), uint64_t(b), __LINE__)

} // namespace

// ===========================================================================
// host stubs：被测模块在 device 内依赖的原语（宿主机替身）
// ===========================================================================
namespace {

struct ReadableBlock {
  const void* base = nullptr;
  size_t size = 0u;
};

std::vector<ReadableBlock> g_readable;
std::unordered_map<void*, uint32_t> g_widgetRawcodeByPtr;
std::unordered_map<uint32_t, uint32_t> g_widgetRawcodeByHandle;
std::vector<dxvk::war3::render::RenderObjectInfo> g_renderObjects;
std::vector<dxvk::war3::render::ShadowObjectRecord> g_shadowObjects;
std::vector<dxvk::war3::render::VisibleRenderableRecord> g_visible;
bool g_semanticSceneRuntimeEnabled = true;
std::vector<std::vector<uint8_t>> g_unitBuffers;
std::vector<std::vector<uint8_t>> g_widgetBuffers;

struct HookState {
  std::unordered_map<void*, uint32_t> byPtr;
  std::unordered_map<uint32_t, uint32_t> byHandle;
};

HookState SnapshotHooks() {
  return HookState{g_widgetRawcodeByPtr, g_widgetRawcodeByHandle};
}

void RestoreHooks(const HookState& s) {
  g_widgetRawcodeByPtr = s.byPtr;
  g_widgetRawcodeByHandle = s.byHandle;
}

bool HookStatesEqual(const HookState& a, const HookState& b) {
  return a.byPtr == b.byPtr && a.byHandle == b.byHandle;
}

void ClearWorld() {
  g_readable.clear();
  g_widgetRawcodeByPtr.clear();
  g_widgetRawcodeByHandle.clear();
  g_renderObjects.clear();
  g_shadowObjects.clear();
  g_visible.clear();
  g_unitBuffers.clear();
  g_widgetBuffers.clear();
  g_semanticSceneRuntimeEnabled = true;
}

void RegisterReadable(const void* base, size_t size) {
  g_readable.push_back(ReadableBlock{base, size});
}

void* MakeUnitBuffer(uint32_t rawcode, uint32_t flags5C, bool spriteNonNull) {
  g_unitBuffers.emplace_back(0x80u, uint8_t(0u));
  uint8_t* bytes = g_unitBuffers.back().data();
  std::memcpy(bytes + dxvk::war3::CUnitOffsets::Rawcode, &rawcode,
              sizeof(rawcode));
  std::memcpy(bytes + dxvk::war3::CUnitOffsets::Flags5C, &flags5C,
              sizeof(flags5C));
  void* sprite = spriteNonNull
                     ? reinterpret_cast<void*>(uintptr_t(0xABC0u))
                     : nullptr;
  std::memcpy(bytes + dxvk::war3::CUnitOffsets::Sprite, &sprite,
              sizeof(sprite));
  RegisterReadable(bytes, 0x80u);
  return bytes;
}

void* MakeWidgetBuffer(bool magicOk, uint32_t rawcode, bool hasRawcode) {
  g_widgetBuffers.emplace_back(0x40u, uint8_t(0u));
  uint8_t* bytes = g_widgetBuffers.back().data();
  const uint32_t magic = magicOk ? 0x2B5DB42Cu : 0x12345678u;
  std::memcpy(bytes + 0x0Cu, &magic, sizeof(magic));
  const uint32_t rc = hasRawcode ? rawcode : 0u;
  std::memcpy(bytes + 0x30u, &rc, sizeof(rc));
  RegisterReadable(bytes, 0x40u);
  return bytes;
}

} // namespace

namespace dxvk::env {

std::string getEnvVar(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

} // namespace dxvk::env

namespace dxvk::war3 {

bool IsReadableRangeFast(const void* p, size_t size) {
  if (p == nullptr || size == 0u)
    return false;
  const uintptr_t address = reinterpret_cast<uintptr_t>(p);
  for (const auto& block : g_readable) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(block.base);
    if (address < base || size > block.size)
      continue;
    if (address - base <= block.size - size)
      return true;
  }
  return false;
}

bool SafeCopy(void* destination, const void* source, size_t size) noexcept {
  if (destination == nullptr || source == nullptr || size == 0u)
    return false;
  if (!IsReadableRangeFast(source, size))
    return false;
  std::memcpy(destination, source, size);
  return true;
}

} // namespace dxvk::war3

namespace dxvk::war3::hooks {

uint32_t QueryWidgetRawcodeByPtr(void* widgetPtr) {
  const auto it = g_widgetRawcodeByPtr.find(widgetPtr);
  return it != g_widgetRawcodeByPtr.end() ? it->second : 0u;
}

uint32_t QueryWidgetRawcodeByHandle(uint32_t jHandle) {
  const auto it = g_widgetRawcodeByHandle.find(jHandle);
  return it != g_widgetRawcodeByHandle.end() ? it->second : 0u;
}

void NoteWidgetIdentityFromDrawcall(void* widgetPtr, uint32_t rawcode,
                                    uint32_t jHandle) {
  if (widgetPtr != nullptr)
    g_widgetRawcodeByPtr[widgetPtr] = rawcode;
  if (jHandle != 0u)
    g_widgetRawcodeByHandle[jHandle] = rawcode;
}

} // namespace dxvk::war3::hooks

namespace dxvk::war3::internal {

bool IsSemanticSceneSubmissionRuntimeEnabled() {
  return g_semanticSceneRuntimeEnabled;
}

} // namespace dxvk::war3::internal

namespace dxvk::war3::render {

RenderObjectIdentitySnapshot::RenderObjectIdentitySnapshot()
    : kind(ObjectKind::Unknown) {
}

bool RenderObjectIdentitySnapshot::HasContext() const {
  return worldObjectEntry != nullptr || sceneNode != nullptr ||
         unitPtr != nullptr || jHandle != 0u;
}

bool RenderObjectIdentitySnapshot::HasStableIdentity() const {
  return HasContext() || rawcode != 0u || kind != ObjectKind::Unknown;
}

RenderObjectRegistry& RenderObjectRegistry::instance() {
  static RenderObjectRegistry s_instance;
  return s_instance;
}

const RenderObjectInfo* RenderObjectRegistry::findBySceneNode(
    void* sceneNode) const {
  for (const auto& object : g_renderObjects) {
    if (sceneNode != nullptr && object.sceneNode == sceneNode)
      return &object;
  }
  return nullptr;
}

const RenderObjectInfo* RenderObjectRegistry::findByEntry(
    void* worldObjectEntry) const {
  for (const auto& object : g_renderObjects) {
    if (worldObjectEntry != nullptr && object.worldObjectEntry == worldObjectEntry)
      return &object;
  }
  return nullptr;
}

const RenderObjectInfo* RenderObjectRegistry::findByHandle(
    uint32_t jHandle) const {
  for (const auto& object : g_renderObjects) {
    if (jHandle != 0u && object.jHandle == jHandle)
      return &object;
  }
  return nullptr;
}

VisibleRenderableRegistry& VisibleRenderableRegistry::instance() {
  static VisibleRenderableRegistry s_instance;
  return s_instance;
}

bool VisibleRenderableRegistry::queryByRenderablePartAndLayer(
    void* renderablePart, uint32_t layerIndex,
    VisibleRenderableRecord& out) const {
  for (const auto& record : g_visible) {
    if (record.renderablePart == renderablePart &&
        record.layerIndex == layerIndex) {
      out = record;
      return true;
    }
  }
  return false;
}

const VisibleRenderableRecord* VisibleRenderablePartLayerQueryCache::queryPtr(
    const VisibleRenderableRegistry& registry, void* renderablePart,
    uint32_t layerIndex) noexcept {
  const uintptr_t key =
      (reinterpret_cast<uintptr_t>(renderablePart) >> 4u) ^
      (uintptr_t(layerIndex) * uintptr_t(0x9E3779B1u));
  Entry& entry = m_entries[static_cast<size_t>(key) & (kEntryCount - 1u)];
  if (entry.renderablePart == renderablePart &&
      entry.layerIndex == layerIndex && entry.generation == m_generation) {
    return entry.found ? &entry.record : nullptr;
  }
  entry.renderablePart = renderablePart;
  entry.layerIndex = layerIndex;
  entry.generation = m_generation;
  entry.found =
      registry.queryByRenderablePartAndLayer(renderablePart, layerIndex,
                                             entry.record);
  return entry.found ? &entry.record : nullptr;
}

ShadowObjectRegistry& ShadowObjectRegistry::instance() {
  static ShadowObjectRegistry s_instance;
  return s_instance;
}

bool ShadowObjectRegistry::findBySceneNode(void* sceneNode,
                                           ShadowObjectRecord& out) const {
  for (const auto& object : g_shadowObjects) {
    if (sceneNode != nullptr && object.sceneNode == sceneNode) {
      out = object;
      return true;
    }
  }
  return false;
}

bool ShadowObjectRegistry::findByWorldObjectEntry(
    void* worldObjectEntry, ShadowObjectRecord& out) const {
  for (const auto& object : g_shadowObjects) {
    if (worldObjectEntry != nullptr &&
        object.worldObjectEntry == worldObjectEntry) {
      out = object;
      return true;
    }
  }
  return false;
}

bool ShadowObjectRegistry::findByHandle(uint32_t jHandle,
                                        ShadowObjectRecord& out) const {
  for (const auto& object : g_shadowObjects) {
    if (jHandle != 0u && object.jHandle == jHandle) {
      out = object;
      return true;
    }
  }
  return false;
}

bool ShadowObjectRegistry::findByRuntimeModel(void* runtimeModelPtr,
                                              ShadowObjectRecord& out) const {
  for (const auto& object : g_shadowObjects) {
    if (runtimeModelPtr != nullptr &&
        object.runtimeModelPtr == runtimeModelPtr) {
      out = object;
      return true;
    }
  }
  return false;
}

} // namespace dxvk::war3::render

// ===========================================================================
// 输入域
// ===========================================================================
namespace {

using Packet = dxvk::war3::shadow::ShadowDrawPacket;
using Renderable = dxvk::war3::shadow::ShadowRenderableRecord;
using Sample = dxvk::war3::render::CurrentDrawAuthoritativeSample;
using Record = dxvk::war3::render::CurrentDrawContractRecord;
using Visible = dxvk::war3::render::VisibleRenderableRecord;
using ObjectKind = dxvk::war3::render::ObjectKind;
using QueueKind = dxvk::war3::render::VisibleRenderableQueueKind;
using DrawPath = dxvk::war3::shadow::ShadowDrawPath;
using AlphaMode = dxvk::war3::shadow::ShadowAlphaMode;

struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed) {}
  uint64_t next() {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31u);
  }
  uint32_t pick(uint32_t n) { return static_cast<uint32_t>(next() % n); }
  bool coin() { return (next() & 1u) != 0u; }
};

const uint32_t kRawcodes[] = {
    0u,          0x4C546272u /*'LTbr'*/, 0x7254624Cu /*swapped*/,
    0x59546272u /*'YTbr'*/, 0x72546259u /*swapped*/, 0x68706F6Fu /*'hfoo'*/,
    0xFFFFFFFFu, 0x2B5DB42Cu,
};
constexpr uint32_t kRawcodeCount =
    sizeof(kRawcodes) / sizeof(kRawcodes[0]);

const ObjectKind kObjectKinds[] = {
    ObjectKind::Unknown, ObjectKind::Unit,  ObjectKind::Building,
    ObjectKind::Destructible, ObjectKind::Item, ObjectKind::Effect,
};
constexpr uint32_t kObjectKindCount =
    sizeof(kObjectKinds) / sizeof(kObjectKinds[0]);

void* PtrFrom(uint64_t value) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
}

void* const kPtrPool[] = {
    nullptr, PtrFrom(0x1000u), PtrFrom(0x2000u), PtrFrom(0x3000u),
    PtrFrom(0x4000u), PtrFrom(0x5000u), PtrFrom(0x6000u),
};
constexpr uint32_t kPtrCount = sizeof(kPtrPool) / sizeof(kPtrPool[0]);

Packet MakePacket(Rng& rng, uint64_t frameSerial) {
  Packet packet = {};
  auto& r = packet.renderable;
  r.rawcode = kRawcodes[rng.pick(kRawcodeCount)];
  r.jHandle = (rng.pick(4u) == 0u)
                  ? 0u
                  : static_cast<uint32_t>(0x100000u + rng.pick(8u));
  r.unitPtr = kPtrPool[rng.pick(kPtrCount)];
  r.runtimeModelPtr = kPtrPool[rng.pick(kPtrCount)];
  r.modelResourcePtr = kPtrPool[rng.pick(kPtrCount)];
  r.modelKey = (rng.pick(3u) == 0u) ? rng.pick(4u) : 0u;
  r.worldObjectEntry = kPtrPool[rng.pick(kPtrCount)];
  r.sceneNode = kPtrPool[rng.pick(kPtrCount)];
  r.renderablePart = kPtrPool[rng.pick(kPtrCount)];
  r.meshData = kPtrPool[rng.pick(kPtrCount)];
  r.layerIndex = rng.pick(3u);
  r.objectKind = kObjectKinds[rng.pick(kObjectKindCount)];
  r.queueKind = (rng.pick(4u) == 0u) ? QueueKind::Transparent
                                     : QueueKind::MainQueue;
  r.groupIdx = static_cast<int8_t>(static_cast<int>(rng.pick(5u)) - 1);
  r.unitFlags5C = (rng.pick(3u) == 0u)
                      ? dxvk::war3::UnitFlags5C::Building
                      : ((rng.pick(3u) == 0u) ? 0xFFFFFFFFu : 0u);
  r.pathBlocker = (rng.pick(5u) == 0u);
  r.geosetIndex = (rng.pick(3u) == 0u)
                      ? dxvk::war3::shadow::kInvalidShadowContractGeosetIndex
                      : rng.pick(4u);
  r.runtimeGeosetPtr = (rng.pick(3u) == 0u) ? kPtrPool[rng.pick(kPtrCount)]
                                            : nullptr;
  r.runtimeGeosetDataPtr = (rng.pick(4u) == 0u) ? kPtrPool[rng.pick(kPtrCount)]
                                                : nullptr;
  r.frameSerial = frameSerial;
  r.transparentType = rng.pick(4u);
  r.stage = static_cast<int16_t>(rng.pick(20u));

  packet.resource.geosetIndex =
      (rng.pick(3u) == 0u)
          ? dxvk::war3::shadow::kInvalidShadowContractGeosetIndex
          : rng.pick(4u);
  packet.resource.vertexCount = (rng.pick(3u) == 0u) ? rng.pick(64u) : 0u;
  packet.resource.modelResourcePtr =
      (rng.pick(3u) == 0u) ? kPtrPool[rng.pick(kPtrCount)] : nullptr;
  packet.resource.modelKey = (rng.pick(3u) == 0u) ? rng.pick(4u) : 0u;
  packet.path = rng.coin() ? DrawPath::Skinned : DrawPath::Rigid;
  packet.usesDynamicMeshPositions = (rng.pick(3u) == 0u);
  packet.resource.dynamicPositionStream =
      (rng.pick(3u) == 0u) ? kPtrPool[rng.pick(kPtrCount)] : nullptr;
  packet.resource.dynamicPositionStride =
      (rng.pick(4u) == 0u) ? 12u : rng.pick(16u);
  packet.pose.hasWorldTransform = (rng.pick(3u) == 0u);

  auto& m = packet.material;
  m.signatureHash = (rng.pick(3u) == 0u) ? rng.pick(4u) : 0u;
  m.alphaMode = static_cast<AlphaMode>(rng.pick(3u));
  m.blendOrDrawMode = (rng.pick(3u) == 0u) ? rng.pick(4u) : 0u;
  m.queueKind = (rng.pick(3u) == 0u)
                    ? 0u
                    : static_cast<uint32_t>(QueueKind::Transparent);
  m.layerContractResolved = (rng.pick(3u) == 0u);
  m.layerIndex = rng.pick(3u);
  return packet;
}

void PopulateWorldFromPacket(const Packet& packet, Rng& rng) {
  const auto& r = packet.renderable;
  if (r.unitPtr != nullptr) {
    const uint32_t unitRawcode =
        kRawcodes[rng.pick(kRawcodeCount)];
    const uint32_t unitFlags =
        (rng.pick(3u) == 0u) ? dxvk::war3::UnitFlags5C::Building
                             : ((rng.pick(3u) == 0u) ? 0xFFFFFFFFu : 0u);
    const bool sprite = (rng.pick(3u) != 0u);
    const void* buffer = MakeUnitBuffer(unitRawcode, unitFlags, sprite);
    (void)buffer;
  }
  if (r.worldObjectEntry != nullptr) {
    const bool magicOk = (rng.pick(2u) == 0u);
    const bool hasRawcode = (rng.pick(3u) != 0u);
    MakeWidgetBuffer(magicOk, kRawcodes[rng.pick(kRawcodeCount)], hasRawcode);
  }
  if (rng.pick(2u) == 0u) {
    dxvk::war3::render::RenderObjectInfo info = {};
    info.worldObjectEntry = r.worldObjectEntry;
    info.sceneNode = r.sceneNode;
    info.unitPtr = r.unitPtr;
    info.jHandle = r.jHandle;
    info.rawcode = kRawcodes[rng.pick(kRawcodeCount)];
    info.kind = kObjectKinds[rng.pick(kObjectKindCount)];
    info.groupIdx = static_cast<int>(rng.pick(3u)) - 1;
    g_renderObjects.push_back(info);
  }
  if (rng.pick(2u) == 0u) {
    dxvk::war3::render::ShadowObjectRecord record = {};
    record.worldObjectEntry = r.worldObjectEntry;
    record.sceneNode = r.sceneNode;
    record.unitPtr = r.unitPtr;
    record.runtimeModelPtr = r.runtimeModelPtr;
    record.jHandle = r.jHandle;
    record.kind = kObjectKinds[rng.pick(kObjectKindCount)];
    g_shadowObjects.push_back(record);
  }
  if (r.renderablePart != nullptr && rng.pick(2u) == 0u) {
    Visible visible = {};
    visible.renderablePart = r.renderablePart;
    visible.layerIndex = r.layerIndex;
    visible.queueKind = rng.coin() ? QueueKind::MainQueue
                                   : QueueKind::Transparent;
    visible.sceneNode = (rng.pick(2u) == 0u) ? r.sceneNode : kPtrPool[rng.pick(kPtrCount)];
    visible.meshData = (rng.pick(2u) == 0u) ? r.meshData : kPtrPool[rng.pick(kPtrCount)];
    visible.identity.jHandle = (rng.pick(3u) == 0u) ? r.jHandle : 0u;
    visible.identity.handleId = (rng.pick(3u) == 0u) ? r.jHandle : 0u;
    visible.identity.unitPtr = (rng.pick(2u) == 0u) ? r.unitPtr : kPtrPool[rng.pick(kPtrCount)];
    visible.identity.worldObjectEntry =
        (rng.pick(2u) == 0u) ? r.worldObjectEntry : kPtrPool[rng.pick(kPtrCount)];
    visible.identity.groupIdx = static_cast<int8_t>(static_cast<int>(rng.pick(3u)) - 1);
    g_visible.push_back(visible);
  }
  if (r.unitPtr != nullptr || r.worldObjectEntry != nullptr) {
    const bool cacheHit = (rng.pick(2u) == 0u);
    if (cacheHit) {
      const uint32_t cachedRawcode = kRawcodes[rng.pick(kRawcodeCount)];
      if (r.unitPtr != nullptr)
        g_widgetRawcodeByPtr[r.unitPtr] = cachedRawcode;
      if (r.worldObjectEntry != nullptr)
        g_widgetRawcodeByPtr[r.worldObjectEntry] = cachedRawcode;
      if (r.jHandle != 0u)
        g_widgetRawcodeByHandle[r.jHandle] = cachedRawcode;
    }
  }
}

// ---------------------------------------------------------------------------
// 差分：选择键 / rawcode 谓词（头文件内联）
// ---------------------------------------------------------------------------
void DiffTypeShimValues() {
  // legacy 参考里的三个类型 shim（见 .inc 头部说明）必须与模块类型逐枚举值同构。
  DIFF_U64("Shim.Source.None",
           uint32_t(sem::War3SemanticDirectSelectionKeySource::None),
           uint32_t(legacy::War3SemanticDirectSelectionKeySource::None));
  DIFF_U64("Shim.Source.UnitPtr",
           uint32_t(sem::War3SemanticDirectSelectionKeySource::UnitPtr),
           uint32_t(legacy::War3SemanticDirectSelectionKeySource::UnitPtr));
  DIFF_U64("Shim.Source.JHandle",
           uint32_t(sem::War3SemanticDirectSelectionKeySource::JHandle),
           uint32_t(legacy::War3SemanticDirectSelectionKeySource::JHandle));
  DIFF_U64("Shim.Source.RuntimeModel",
           uint32_t(sem::War3SemanticDirectSelectionKeySource::RuntimeModel),
           uint32_t(legacy::War3SemanticDirectSelectionKeySource::RuntimeModel));
  DIFF_U64("Shim.Source.WorldObject",
           uint32_t(sem::War3SemanticDirectSelectionKeySource::WorldObject),
           uint32_t(legacy::War3SemanticDirectSelectionKeySource::WorldObject));
  DIFF_U64("Shim.Source.SceneNode",
           uint32_t(sem::War3SemanticDirectSelectionKeySource::SceneNode),
           uint32_t(legacy::War3SemanticDirectSelectionKeySource::SceneNode));
  DIFF_U64("Shim.Source.ModelMesh",
           uint32_t(sem::War3SemanticDirectSelectionKeySource::ModelMesh),
           uint32_t(legacy::War3SemanticDirectSelectionKeySource::ModelMesh));
  DIFF_U64("Shim.Source.RenderablePart",
           uint32_t(sem::War3SemanticDirectSelectionKeySource::RenderablePart),
           uint32_t(legacy::War3SemanticDirectSelectionKeySource::RenderablePart));
  DIFF_U64("Shim.Backing.NotChecked",
           uint32_t(sem::War3SemanticDirectMainWorldBackingStatus::NotChecked),
           uint32_t(legacy::War3SemanticDirectMainWorldBackingStatus::NotChecked));
  DIFF_U64("Shim.Backing.Pass",
           uint32_t(sem::War3SemanticDirectMainWorldBackingStatus::Pass),
           uint32_t(legacy::War3SemanticDirectMainWorldBackingStatus::Pass));
  DIFF_U64("Shim.Backing.NoRenderablePart",
           uint32_t(sem::War3SemanticDirectMainWorldBackingStatus::NoRenderablePart),
           uint32_t(legacy::War3SemanticDirectMainWorldBackingStatus::NoRenderablePart));
  DIFF_U64("Shim.Backing.LookupMiss",
           uint32_t(sem::War3SemanticDirectMainWorldBackingStatus::LookupMiss),
           uint32_t(legacy::War3SemanticDirectMainWorldBackingStatus::LookupMiss));
  DIFF_U64("Shim.Backing.NonMainQueue",
           uint32_t(sem::War3SemanticDirectMainWorldBackingStatus::NonMainQueue),
           uint32_t(legacy::War3SemanticDirectMainWorldBackingStatus::NonMainQueue));
  DIFF_U64("Shim.Backing.NonWorldGroup",
           uint32_t(sem::War3SemanticDirectMainWorldBackingStatus::NonWorldGroup),
           uint32_t(legacy::War3SemanticDirectMainWorldBackingStatus::NonWorldGroup));
  DIFF_U64("Shim.Backing.IdentityMismatch",
           uint32_t(sem::War3SemanticDirectMainWorldBackingStatus::IdentityMismatch),
           uint32_t(legacy::War3SemanticDirectMainWorldBackingStatus::IdentityMismatch));
  DIFF_U64("Shim.Backing.SceneNodeMismatch",
           uint32_t(sem::War3SemanticDirectMainWorldBackingStatus::SceneNodeMismatch),
           uint32_t(legacy::War3SemanticDirectMainWorldBackingStatus::SceneNodeMismatch));
  DIFF_U64("Shim.Backing.MeshDataMismatch",
           uint32_t(sem::War3SemanticDirectMainWorldBackingStatus::MeshDataMismatch),
           uint32_t(legacy::War3SemanticDirectMainWorldBackingStatus::MeshDataMismatch));
}

void DiffKeyPredicates() {
  Rng rng(0x4D312D4B4559ull); // 'M1-KEY'

  // 穷举：byte swap 的 256 个低字节槽 + 边界
  for (uint32_t i = 0u; i < 256u; ++i) {
    const uint32_t value = (i << 24u) | (i << 8u);
    DIFF_U64("ByteSwap", sem::War3SemanticByteSwapU32(value),
             legacy::War3SemanticByteSwapU32(value));
  }
  const uint32_t kBoundaryValues[] = {0u, 0xFFFFFFFFu, 0x80000000u, 1u,
                                      0x0000FFFFu, 0xFFFF0000u, 0x00FF00FFu};
  for (uint32_t v : kBoundaryValues) {
    DIFF_U64("ByteSwap.boundary", sem::War3SemanticByteSwapU32(v),
             legacy::War3SemanticByteSwapU32(v));
  }

  const char prefixes[][2] = {{'L', 'T'}, {'Y', 'T'}, {'h', 'f'}, {'\0', 'x'}};
  for (uint32_t i = 0u; i < kRawcodeCount; ++i) {
    for (const auto& p : prefixes) {
      DIFF_U64("FourCcHasPrefix",
               sem::War3SemanticFourCcHasPrefix(kRawcodes[i], p[0], p[1]),
               legacy::War3SemanticFourCcHasPrefix(kRawcodes[i], p[0], p[1]));
    }
  }
  for (uint32_t i = 0u; i < 65536u; ++i) {
    const uint32_t value = (i << 16u) | (i ^ 0x5A5Au);
    DIFF_U64("FourCcHasPrefix.rand",
             sem::War3SemanticFourCcHasPrefix(value, 'L', 'T'),
             legacy::War3SemanticFourCcHasPrefix(value, 'L', 'T'));
  }

  // 穷举等值与字节序：小域 + 零值
  const uint32_t kFourCcDomain[] = {0u,          0x4C546272u, 0x7254624Cu,
                                    0x272u,      0xFFFFFFFFu, 0x4C546200u};
  for (uint32_t a : kFourCcDomain) {
    for (uint32_t b : kFourCcDomain) {
      DIFF_U64("FourCcEqualEitherOrder",
               sem::War3SemanticFourCcEqualEitherOrder(a, b),
               legacy::War3SemanticFourCcEqualEitherOrder(a, b));
    }
  }

  for (uint32_t i = 0u; i < kRawcodeCount; ++i) {
    DIFF_U64("RawcodeLooksStaticWorldCaster",
             sem::War3SemanticRawcodeLooksStaticWorldCaster(kRawcodes[i]),
             legacy::War3SemanticRawcodeLooksStaticWorldCaster(kRawcodes[i]));
  }
  for (uint32_t i = 0u; i < 200000u; ++i) {
    const uint32_t value = static_cast<uint32_t>(rng.next());
    DIFF_U64("RawcodeLooksStaticWorldCaster.rand",
             sem::War3SemanticRawcodeLooksStaticWorldCaster(value),
             legacy::War3SemanticRawcodeLooksStaticWorldCaster(value));
  }

  for (uint32_t k = 0u; k < kObjectKindCount; ++k) {
    DIFF_U64("IsSemanticUnitObject",
             sem::War3IsSemanticUnitObject(kObjectKinds[k]),
             legacy::War3IsSemanticUnitObject(kObjectKinds[k]));
  }

  // 稳定单元资源：5 个输入位的穷举（2^5 = 32）
  for (uint32_t mask = 0u; mask < 32u; ++mask) {
    Packet a = {};
    Packet b = {};
    const bool runtimeModel = (mask & 1u) != 0u;
    const bool modelResourcePtr = (mask & 2u) != 0u;
    const bool modelKey = (mask & 4u) != 0u;
    const bool resourceModelPtr = (mask & 8u) != 0u;
    const bool resourceModelKey = (mask & 16u) != 0u;
    for (Packet* p : {&a, &b}) {
      p->renderable.runtimeModelPtr = runtimeModel ? PtrFrom(0x2000u) : nullptr;
      p->renderable.modelResourcePtr =
          modelResourcePtr ? PtrFrom(0x3000u) : nullptr;
      p->renderable.modelKey = modelKey ? 0x1234u : 0u;
      p->resource.modelResourcePtr =
          resourceModelPtr ? PtrFrom(0x4000u) : nullptr;
      p->resource.modelKey = resourceModelKey ? 0x5678u : 0u;
    }
    DIFF_U64("PacketHasStableUnitResource",
             sem::War3SemanticPacketHasStableUnitResource(a),
             legacy::War3SemanticPacketHasStableUnitResource(b));
  }

  // 选择键：10 个身份槽的穷举（2^10）+ 上界指针
  const uint64_t kIdentityValues[2] = {0u, 1u};
  for (uint32_t mask = 0u; mask < 1024u; ++mask) {
    Packet packet = {};
    Sample sample = {};
    auto bit = [&](uint32_t index) { return (mask >> index) & 1u; };
    packet.renderable.jHandle = bit(0) ? 0x1234u : 0u;
    packet.renderable.unitPtr = bit(1) ? PtrFrom(0x1000u) : nullptr;
    packet.renderable.runtimeModelPtr = bit(2) ? PtrFrom(0x2000u) : nullptr;
    packet.renderable.worldObjectEntry = bit(3) ? PtrFrom(0x3000u) : nullptr;
    packet.renderable.sceneNode = bit(4) ? PtrFrom(0x4000u) : nullptr;
    packet.renderable.modelResourcePtr = bit(5) ? PtrFrom(0x5000u) : nullptr;
    packet.renderable.meshData = bit(6) ? PtrFrom(0x6000u) : nullptr;
    packet.renderable.renderablePart = bit(7) ? PtrFrom(0x7000u) : nullptr;
    sample.contract.jHandle = bit(8) ? 0x99u : 0u;
    sample.contract.unitPtr = bit(9) ? PtrFrom(0x8000u) : nullptr;
    (void)kIdentityValues;

    sem::War3SemanticDirectSelectionKeySource moduleSource =
        sem::War3SemanticDirectSelectionKeySource::None;
    legacy::War3SemanticDirectSelectionKeySource legacySource =
        legacy::War3SemanticDirectSelectionKeySource::None;
    const uint64_t moduleKey =
        sem::War3SemanticDirectSelectionKey(packet, sample, &moduleSource);
    const uint64_t legacyKey =
        legacy::War3SemanticDirectSelectionKey(packet, sample, &legacySource);
    DIFF_U64("DirectSelectionKey", moduleKey, legacyKey);
    DIFF_U64("DirectSelectionKey.source", uint32_t(moduleSource),
             uint32_t(legacySource));
    // nullptr outSource 分支必须给出同一 key。
    DIFF_U64("DirectSelectionKey.nullout", moduleKey,
             sem::War3SemanticDirectSelectionKey(packet, sample, nullptr));
  }

  // contract 侧身份（renderable 全空）
  for (uint32_t mask = 0u; mask < 8u; ++mask) {
    Packet packet = {};
    Sample sample = {};
    sample.contract.worldObjectEntry = (mask & 1u) ? PtrFrom(0x9000u) : nullptr;
    sample.contract.sceneNode = (mask & 2u) ? PtrFrom(0xA000u) : nullptr;
    sample.contract.renderablePart = (mask & 4u) ? PtrFrom(0xB000u) : nullptr;
    sem::War3SemanticDirectSelectionKeySource moduleSource =
        sem::War3SemanticDirectSelectionKeySource::None;
    legacy::War3SemanticDirectSelectionKeySource legacySource =
        legacy::War3SemanticDirectSelectionKeySource::None;
    DIFF_U64("DirectSelectionKey.contract",
             sem::War3SemanticDirectSelectionKey(packet, sample, &moduleSource),
             legacy::War3SemanticDirectSelectionKey(packet, sample,
                                                     &legacySource));
    DIFF_U64("DirectSelectionKey.contract.source", uint32_t(moduleSource),
             uint32_t(legacySource));
  }

  // 上界：全 1 位身份
  Packet maxPacket = {};
  Sample maxSample = {};
  maxPacket.renderable.jHandle = 0xFFFFFFFFu;
  maxPacket.renderable.unitPtr = PtrFrom(~uintptr_t(0));
  maxPacket.renderable.worldObjectEntry = PtrFrom(~uintptr_t(0));
  maxPacket.renderable.sceneNode = PtrFrom(~uintptr_t(0));
  DIFF_U64("DirectSelectionKey.max",
           sem::War3SemanticDirectSelectionKey(maxPacket, maxSample, nullptr),
           legacy::War3SemanticDirectSelectionKey(maxPacket, maxSample,
                                                  nullptr));

  // 随机 packet 上的选择键
  for (uint32_t i = 0u; i < 20000u; ++i) {
    const Packet packet = MakePacket(rng, i + 1u);
    Sample sample = {};
    DIFF_U64("DirectSelectionKey.rand",
             sem::War3SemanticDirectSelectionKey(packet, sample, nullptr),
             legacy::War3SemanticDirectSelectionKey(packet, sample, nullptr));
  }
}

// ---------------------------------------------------------------------------
// 差分：单元 core 读取 / 一致性
// ---------------------------------------------------------------------------
void DiffUnitCore() {
  Rng rng(0x4D312D554E4954ull); // 'M1-UNIT'
  for (uint32_t i = 0u; i < 4000u; ++i) {
    ClearWorld();
    const uint32_t rawcode = kRawcodes[rng.pick(kRawcodeCount)];
    const uint32_t flags =
        (rng.pick(3u) == 0u) ? dxvk::war3::UnitFlags5C::Building
                             : ((rng.pick(3u) == 0u) ? 0xFFFFFFFFu : 0u);
    const bool spriteNonNull = (rng.pick(3u) != 0u);
    const bool readable = (rng.pick(4u) != 0u);
    Renderable renderable = {};
    renderable.rawcode = kRawcodes[rng.pick(kRawcodeCount)];
    renderable.unitFlags5C = flags;
    renderable.frameSerial = i;
    renderable.runtimeModelPtr = kPtrPool[rng.pick(kPtrCount)];
    if (readable) {
      renderable.unitPtr = MakeUnitBuffer(rawcode, flags, spriteNonNull);
    } else {
      // 未登记为可读：两个实现都必须 fail-closed。
      renderable.unitPtr = PtrFrom(0xDEAD0000u + i);
    }

    uint32_t moduleRawcode = 0u, moduleFlags = 0u;
    void* moduleSprite = nullptr;
    uint32_t legacyRawcode = 0u, legacyFlags = 0u;
    void* legacySprite = nullptr;
    const bool moduleOk = sem::War3SemanticReadUnitCore(
        renderable, moduleRawcode, moduleFlags, moduleSprite);
    const bool legacyOk = legacy::War3SemanticReadUnitCore(
        renderable, legacyRawcode, legacyFlags, legacySprite);
    DIFF_U64("ReadUnitCore.ok", moduleOk, legacyOk);
    DIFF_U64("ReadUnitCore.rawcode", moduleRawcode, legacyRawcode);
    DIFF_U64("ReadUnitCore.flags", moduleFlags, legacyFlags);
    DIFF_U64("ReadUnitCore.spriteNonNull", moduleSprite != nullptr,
             legacySprite != nullptr);

    Packet packet = {};
    packet.renderable = renderable;
    const bool moduleConsistent =
        sem::War3SemanticPacketHasConsistentUnitCore(packet);
    const bool legacyConsistent =
        legacy::War3SemanticPacketHasConsistentUnitCore(packet);
    DIFF_U64("PacketHasConsistentUnitCore", moduleConsistent,
             legacyConsistent);
  }
}

// ---------------------------------------------------------------------------
// 差分：main-world visible backing
// ---------------------------------------------------------------------------
void DiffMainWorldBacking() {
  Rng rng(0x4D312D4241434Bull); // 'M1-BACK'
  const int kGroupDomain[] = {-1, 0, 1, 2};
  for (uint32_t qi = 0u; qi < 2u; ++qi) {
    for (int group : kGroupDomain) {
      for (uint32_t mask = 0u; mask < 16u; ++mask) {
        ClearWorld();
        Packet packet = {};
        const bool hasPart = (mask & 1u) == 0u;
        packet.renderable.renderablePart = hasPart ? PtrFrom(0x7000u) : nullptr;
        packet.renderable.unitPtr = (mask & 2u) ? PtrFrom(0x1000u) : nullptr;
        packet.renderable.worldObjectEntry =
            (mask & 4u) ? PtrFrom(0x3000u) : nullptr;
        packet.renderable.jHandle = (mask & 8u) ? 0x100001u : 0u;
        packet.renderable.sceneNode = (mask & 2u) ? PtrFrom(0x4000u) : nullptr;
        packet.renderable.meshData = (mask & 4u) ? PtrFrom(0x5000u) : nullptr;

        Visible visible = {};
        visible.renderablePart = packet.renderable.renderablePart;
        visible.layerIndex = packet.renderable.layerIndex;
        visible.queueKind = (qi == 0u) ? QueueKind::MainQueue
                                       : QueueKind::Transparent;
        visible.identity.groupIdx = static_cast<int8_t>(group);
        visible.identity.unitPtr = (mask & 2u) ? PtrFrom(0x1000u) : nullptr;
        visible.identity.worldObjectEntry =
            (mask & 4u) ? PtrFrom(0x3000u) : nullptr;
        visible.identity.jHandle = (mask & 8u) ? 0x100001u : 0u;
        visible.identity.handleId = (mask & 8u) ? 0x1u : 0u;
        visible.sceneNode = packet.renderable.sceneNode;
        visible.meshData = packet.renderable.meshData;

        sem::War3SemanticDirectMainWorldBackingStatus moduleStatus =
            sem::War3SemanticDirectMainWorldBackingStatus::NotChecked;
        legacy::War3SemanticDirectMainWorldBackingStatus legacyStatus =
            legacy::War3SemanticDirectMainWorldBackingStatus::NotChecked;
        const bool moduleMatch =
            sem::War3SemanticDirectPacketMatchesMainWorldVisibleRecord(
                packet, visible, &moduleStatus);
        const bool legacyMatch =
            legacy::War3SemanticDirectPacketMatchesMainWorldVisibleRecord(
                packet, visible, &legacyStatus);
        DIFF_U64("MatchesMainWorldVisibleRecord", moduleMatch, legacyMatch);
        DIFF_U64("MatchesMainWorldVisibleRecord.status", uint32_t(moduleStatus),
                 uint32_t(legacyStatus));

        // 同一记录经注册表查询的 backing 版本。
        g_visible.push_back(visible);
        moduleStatus = sem::War3SemanticDirectMainWorldBackingStatus::NotChecked;
        legacyStatus = legacy::War3SemanticDirectMainWorldBackingStatus::NotChecked;
        DIFF_U64("HasMainWorldVisibleBacking",
                 sem::War3SemanticDirectPacketHasMainWorldVisibleBacking(
                     packet, &moduleStatus),
                 legacy::War3SemanticDirectPacketHasMainWorldVisibleBacking(
                     packet, &legacyStatus));
        DIFF_U64("HasMainWorldVisibleBacking.status", uint32_t(moduleStatus),
                 uint32_t(legacyStatus));
        // 注册表 miss 分支
        ClearWorld();
        moduleStatus = sem::War3SemanticDirectMainWorldBackingStatus::NotChecked;
        legacyStatus = legacy::War3SemanticDirectMainWorldBackingStatus::NotChecked;
        DIFF_U64("HasMainWorldVisibleBacking.miss",
                 sem::War3SemanticDirectPacketHasMainWorldVisibleBacking(
                     packet, &moduleStatus),
                 legacy::War3SemanticDirectPacketHasMainWorldVisibleBacking(
                     packet, &legacyStatus));
        DIFF_U64("HasMainWorldVisibleBacking.miss.status",
                 uint32_t(moduleStatus), uint32_t(legacyStatus));
      }
    }
  }

  // 随机 backing 组合
  for (uint32_t i = 0u; i < 20000u; ++i) {
    ClearWorld();
    const Packet packet = MakePacket(rng, 100000u + i);
    PopulateWorldFromPacket(packet, rng);
    sem::War3SemanticDirectMainWorldBackingStatus moduleStatus =
        sem::War3SemanticDirectMainWorldBackingStatus::NotChecked;
    legacy::War3SemanticDirectMainWorldBackingStatus legacyStatus =
        legacy::War3SemanticDirectMainWorldBackingStatus::NotChecked;
    DIFF_U64("HasMainWorldVisibleBacking.rand",
             sem::War3SemanticDirectPacketHasMainWorldVisibleBacking(
                 packet, &moduleStatus),
             legacy::War3SemanticDirectPacketHasMainWorldVisibleBacking(
                 packet, &legacyStatus));
    DIFF_U64("HasMainWorldVisibleBacking.rand.status",
             uint32_t(moduleStatus), uint32_t(legacyStatus));
  }
}

// ---------------------------------------------------------------------------
// 差分：path blocker（rawcode / jHandle / widget 直读）
// ---------------------------------------------------------------------------
void DiffPathBlocker() {
  Rng rng(0x4D312D424C4F43ull); // 'M1-BLOC'

  for (uint32_t i = 0u; i < kRawcodeCount; ++i) {
    DIFF_U64("IsLosBlockerFourCc", sem::IsLosBlockerFourCc(kRawcodes[i]),
             legacy::IsLosBlockerFourCc(kRawcodes[i]));
  }
  for (uint32_t i = 0u; i < 100000u; ++i) {
    const uint32_t value = static_cast<uint32_t>(rng.next());
    DIFF_U64("IsLosBlockerFourCc.rand", sem::IsLosBlockerFourCc(value),
             legacy::IsLosBlockerFourCc(value));
  }

  for (uint32_t i = 0u; i < 6000u; ++i) {
    ClearWorld();
    if (rng.pick(2u) == 0u) {
      dxvk::war3::render::RenderObjectInfo info = {};
      info.jHandle = 0x100000u + rng.pick(8u);
      info.rawcode = kRawcodes[rng.pick(kRawcodeCount)];
      g_renderObjects.push_back(info);
    }
    if (rng.pick(2u) == 0u) {
      g_widgetRawcodeByHandle[0x100000u + rng.pick(8u)] =
          kRawcodes[rng.pick(kRawcodeCount)];
    }
    const uint32_t jHandle = (rng.pick(4u) == 0u)
                                 ? 0u
                                 : static_cast<uint32_t>(0x100000u + rng.pick(8u));
    DIFF_U64("IsLosBlockerByJHandleFallback",
             sem::War3ShadowIsLosBlockerByJHandleFallback(jHandle),
             legacy::War3ShadowIsLosBlockerByJHandleFallback(jHandle));

    // widget 指针直读：分开的可读/不可读、magic 对/错、rawcode 有/无
    void* widget = nullptr;
    switch (rng.pick(5u)) {
      case 0u:
        widget = nullptr;
        break;
      case 1u:
        widget = PtrFrom(0xDEAD0000u + i); // 不可读
        break;
      case 2u:
        widget = MakeWidgetBuffer(true, kRawcodes[rng.pick(kRawcodeCount)], true);
        break;
      case 3u:
        widget = MakeWidgetBuffer(false, 0u, false);
        break;
      default:
        widget = MakeWidgetBuffer(true, 0u, false);
        break;
    }
    const bool moduleBlocker =
        sem::War3ShadowIsLosBlockerByWidgetPtr(widget, jHandle);
    const bool legacyBlocker =
        legacy::War3ShadowIsLosBlockerByWidgetPtr(widget, jHandle);
    DIFF_U64("IsLosBlockerByWidgetPtr", moduleBlocker, legacyBlocker);
  }

  // 负缓存 TTL 边界：同一指针跨帧序列
  {
    ClearWorld();
    void* widget = MakeWidgetBuffer(false, 0u, false);
    for (uint64_t frame = 1u; frame <= 40u; ++frame) {
      DIFF_U64("WidgetNegativeCacheHit.seq",
               sem::War3ShadowIsLosBlockerByWidgetPtr(widget, 0u, false, frame),
               legacy::War3ShadowIsLosBlockerByWidgetPtr(widget, 0u, false,
                                                         frame));
    }
    DIFF_U64("WidgetNegativeCacheHit.frame0",
             sem::War3ShadowIsLosBlockerByWidgetPtr(widget, 0u, false, 0u),
             legacy::War3ShadowIsLosBlockerByWidgetPtr(widget, 0u, false, 0u));
    DIFF_U64("WidgetNegativeCacheHit.null",
             sem::War3ShadowIsLosBlockerByWidgetPtr(nullptr, 0u, false, 5u),
             legacy::War3ShadowIsLosBlockerByWidgetPtr(nullptr, 0u, false, 5u));
  }

  // 统一 packet 判定（含副作用计数与 widget 写穿）
  for (uint32_t i = 0u; i < 20000u; ++i) {
    ClearWorld();
    Packet packet = MakePacket(rng, 200000u + i);
    PopulateWorldFromPacket(packet, rng);
    const HookState before = SnapshotHooks();

    RestoreHooks(before);
    const uint32_t moduleBefore =
        sem::g_pathBlockerEligibilityGateRejectCount.load();
    const bool moduleResult = sem::War3PacketIsPathBlocker(packet);
    const uint32_t moduleAfter =
        sem::g_pathBlockerEligibilityGateRejectCount.load();
    const HookState afterModule = SnapshotHooks();

    RestoreHooks(before);
    const uint32_t legacyBefore =
        legacy::g_pathBlockerEligibilityGateRejectCount.load();
    const bool legacyResult = legacy::War3PacketIsPathBlocker(packet);
    const uint32_t legacyAfter =
        legacy::g_pathBlockerEligibilityGateRejectCount.load();
    const HookState afterLegacy = SnapshotHooks();

    DIFF_U64("PacketIsPathBlocker", moduleResult, legacyResult);
    DIFF_U64("PacketIsPathBlocker.counterDelta", moduleAfter - moduleBefore,
             legacyAfter - legacyBefore);
    DIFF_U64("PacketIsPathBlocker.hookState", HookStatesEqual(afterModule, afterLegacy), 1u);

    RestoreHooks(before);

    // 提交闸（同一 packet，走完整 eligibility + 计数器）
    for (uint32_t kindIndex = 0u; kindIndex < kObjectKindCount; ++kindIndex) {
      for (uint32_t unitsOnly = 0u; unitsOnly < 2u; ++unitsOnly) {
        RestoreHooks(before);
        const uint32_t mBefore =
            sem::g_pathBlockerEligibilityGateRejectCount.load();
        const bool mResult = sem::War3ShouldSubmitSemanticPacket(
            packet, kObjectKinds[kindIndex], unitsOnly != 0u);
        const uint32_t mAfter =
            sem::g_pathBlockerEligibilityGateRejectCount.load();

        RestoreHooks(before);
        const uint32_t lBefore =
            legacy::g_pathBlockerEligibilityGateRejectCount.load();
        const bool lResult = legacy::War3ShouldSubmitSemanticPacket(
            packet, kObjectKinds[kindIndex], unitsOnly != 0u);
        const uint32_t lAfter =
            legacy::g_pathBlockerEligibilityGateRejectCount.load();

        DIFF_U64("ShouldSubmitSemanticPacket", mResult, lResult);
        DIFF_U64("ShouldSubmitSemanticPacket.counter",
                 mAfter - mBefore, lAfter - lBefore);
      }
    }
    RestoreHooks(before);
  }
}

// ---------------------------------------------------------------------------
// 差分：对象类型裁决 / 动态单元证据 / 材质与静态 caster / fast 变体
// ---------------------------------------------------------------------------
void DiffEligibilityPredicates() {
  Rng rng(0x4D312D454C4947ull); // 'M1-ELIG'
  for (uint32_t i = 0u; i < 30000u; ++i) {
    ClearWorld();
    const Packet packet = MakePacket(rng, 300000u + i);
    PopulateWorldFromPacket(packet, rng);
    const HookState before = SnapshotHooks();

    for (uint32_t unitsOnly = 0u; unitsOnly < 2u; ++unitsOnly) {
      RestoreHooks(before);
      DIFF_U64("HasSemanticDynamicUnitEvidence",
               sem::War3HasSemanticDynamicUnitEvidence(packet),
               legacy::War3HasSemanticDynamicUnitEvidence(packet));

      for (uint32_t k = 0u; k < kObjectKindCount; ++k) {
        DIFF_U64("IsEligibleSemanticDynamicUnit",
                 sem::War3IsEligibleSemanticDynamicUnit(packet, kObjectKinds[k]),
                 legacy::War3IsEligibleSemanticDynamicUnit(packet,
                                                           kObjectKinds[k]));
        DIFF_U64("ShouldSubmitSemanticPacketFast",
                 sem::War3ShouldSubmitSemanticPacketFast(packet,
                                                         unitsOnly != 0u),
                 legacy::War3ShouldSubmitSemanticPacketFast(packet,
                                                            unitsOnly != 0u));
        DIFF_U64("LooksSubmitEligibleForScoringFast",
                 sem::War3LooksSubmitEligibleForScoringFast(packet,
                                                            unitsOnly != 0u),
                 legacy::War3LooksSubmitEligibleForScoringFast(
                     packet, unitsOnly != 0u));
      }
    }

    RestoreHooks(before);
    DIFF_U64("SemanticMaterialIsSafeOpaqueWorldCaster",
             sem::War3SemanticMaterialIsSafeOpaqueWorldCaster(packet),
             legacy::War3SemanticMaterialIsSafeOpaqueWorldCaster(packet));

    for (uint32_t geoset = 0u; geoset < 2u; ++geoset) {
      for (uint32_t geometry = 0u; geometry < 2u; ++geometry) {
        for (uint32_t k = 0u; k < kObjectKindCount; ++k) {
          DIFF_U64(
              "IsEligibleSemanticStaticWorldCaster",
              sem::War3IsEligibleSemanticStaticWorldCaster(
                  packet, kObjectKinds[k], geoset != 0u, geometry != 0u),
              legacy::War3IsEligibleSemanticStaticWorldCaster(
                  packet, kObjectKinds[k], geoset != 0u, geometry != 0u));
        }
      }
    }

    DIFF_U64("ResolveSemanticPacketObjectKind",
             sem::War3ResolveSemanticPacketObjectKind(packet.renderable),
             legacy::War3ResolveSemanticPacketObjectKind(packet.renderable));
    DIFF_U64("ResolveSemanticPacketObjectKindFast",
             sem::War3ResolveSemanticPacketObjectKindFast(packet),
             legacy::War3ResolveSemanticPacketObjectKindFast(packet));

    RestoreHooks(before);
  }
}

// ---------------------------------------------------------------------------
// 差分：帧评分 / 帧优先策略
// ---------------------------------------------------------------------------
void DiffFramePreference() {
  Rng rng(0x4D312D4652414Dull); // 'M1-FRAM'
  for (uint32_t i = 0u; i < 4000u; ++i) {
    ClearWorld();
    auto candidate = std::make_shared<dxvk::war3::shadow::ShadowSubmissionFrame>();
    auto current = std::make_shared<dxvk::war3::shadow::ShadowSubmissionFrame>();
    candidate->frameSerial = rng.pick(4u) == 0u ? 0u : rng.next() % 1000u;
    current->frameSerial = rng.pick(4u) == 0u ? 0u : rng.next() % 1000u;
    candidate->sourcePublishRevision = rng.pick(3u);
    current->sourcePublishRevision = rng.pick(3u);
    const uint32_t candidateDraws = rng.pick(4u);
    const uint32_t currentDraws = rng.pick(4u);
    for (uint32_t d = 0u; d < candidateDraws; ++d)
      candidate->draws.push_back(MakePacket(rng, 400000u + i * 4u + d));
    for (uint32_t d = 0u; d < currentDraws; ++d)
      current->draws.push_back(MakePacket(rng, 500000u + i * 4u + d));

    for (uint32_t unitsOnly = 0u; unitsOnly < 2u; ++unitsOnly) {
      const auto moduleScore = sem::War3ScoreSemanticSceneFrame(
          candidate.get(), unitsOnly != 0u);
      const auto legacyScore = legacy::War3ScoreSemanticSceneFrame(
          candidate.get(), unitsOnly != 0u);
      DIFF_U64("Score.input", moduleScore.inputDrawCount,
               legacyScore.inputDrawCount);
      DIFF_U64("Score.eligible", moduleScore.eligibleDrawCount,
               legacyScore.eligibleDrawCount);
      DIFF_U64("Score.skinned", moduleScore.skinnedDrawCount,
               legacyScore.skinnedDrawCount);

      DIFF_U64("ShouldPreferSemanticSceneFrame",
               sem::War3ShouldPreferSemanticSceneFrame(candidate, current,
                                                       unitsOnly != 0u),
               legacy::War3ShouldPreferSemanticSceneFrame(
                   candidate, current, unitsOnly != 0u));
      // 同指针 / 空帧 / 帧号 0 的边界
      DIFF_U64("ShouldPrefer.self",
               sem::War3ShouldPreferSemanticSceneFrame(candidate, candidate,
                                                       unitsOnly != 0u),
               legacy::War3ShouldPreferSemanticSceneFrame(
                   candidate, candidate, unitsOnly != 0u));
      std::shared_ptr<const dxvk::war3::shadow::ShadowSubmissionFrame> nullFrame;
      DIFF_U64("ShouldPrefer.null",
               sem::War3ShouldPreferSemanticSceneFrame(
                   nullFrame, current, unitsOnly != 0u),
               legacy::War3ShouldPreferSemanticSceneFrame(
                   nullFrame, current, unitsOnly != 0u));
      DIFF_U64("ShouldPrefer.zeroframe",
               sem::War3ShouldPreferSemanticSceneFrame(
                   candidate, nullFrame, unitsOnly != 0u),
               legacy::War3ShouldPreferSemanticSceneFrame(
                   candidate, nullFrame, unitsOnly != 0u));
    }
  }
}

// ---------------------------------------------------------------------------
// 差分：记录级选择键（含 visible 提示指针与查询缓存）
// ---------------------------------------------------------------------------
void DiffRecordSelectionKey() {
  Rng rng(0x4D312D52454344ull); // 'M1-RECD'
  for (uint32_t i = 0u; i < 20000u; ++i) {
    ClearWorld();
    Record record = {};
    record.renderablePart = PtrFrom(0x7000u);
    record.layerIndex = rng.pick(3u);
    record.jHandle = (rng.pick(3u) == 0u) ? 0x100001u : 0u;
    record.unitPtr = (rng.pick(3u) == 0u) ? PtrFrom(0x1000u) : nullptr;
    record.worldObjectEntry = (rng.pick(3u) == 0u) ? PtrFrom(0x3000u) : nullptr;
    record.sceneNode = (rng.pick(3u) == 0u) ? PtrFrom(0x4000u) : nullptr;
    record.meshPayloadPtr = (rng.pick(3u) == 0u) ? PtrFrom(0x5000u) : nullptr;
    record.payloadWord108 = rng.pick(4u);
    record.payloadWord11C = rng.pick(4u);

    if (rng.pick(2u) == 0u) {
      Visible visible = {};
      visible.renderablePart = record.renderablePart;
      visible.layerIndex = record.layerIndex;
      visible.identity.jHandle = (rng.pick(3u) == 0u) ? 0x100001u : 0u;
      visible.identity.handleId = (rng.pick(3u) == 0u) ? 0x2u : 0u;
      visible.identity.unitPtr = (rng.pick(3u) == 0u) ? PtrFrom(0x1000u) : nullptr;
      visible.identity.worldObjectEntry =
          (rng.pick(3u) == 0u) ? PtrFrom(0x3000u) : nullptr;
      visible.sceneNode = (rng.pick(3u) == 0u) ? PtrFrom(0x4000u) : nullptr;
      g_visible.push_back(visible);
    }

    dxvk::war3::render::VisibleRenderablePartLayerQueryCache moduleCache;
    dxvk::war3::render::VisibleRenderablePartLayerQueryCache legacyCache;
    const dxvk::war3::render::VisibleRenderableRecord* moduleHint = nullptr;
    const dxvk::war3::render::VisibleRenderableRecord* legacyHint = nullptr;
    const uint64_t moduleKey = sem::War3SemanticDirectRecordSelectionKey(
        record, &moduleHint, &moduleCache);
    const uint64_t legacyKey = legacy::War3SemanticDirectRecordSelectionKey(
        record, &legacyHint, &legacyCache);
    DIFF_U64("DirectRecordSelectionKey", moduleKey, legacyKey);
    DIFF_U64("DirectRecordSelectionKey.hintNull", moduleHint == nullptr,
             legacyHint == nullptr);
    if (moduleHint != nullptr && legacyHint != nullptr) {
      DIFF_U64("DirectRecordSelectionKey.hintJHandle",
               moduleHint->identity.jHandle, legacyHint->identity.jHandle);
      DIFF_U64("DirectRecordSelectionKey.hintHandleId",
               moduleHint->identity.handleId, legacyHint->identity.handleId);
      DIFF_U64("DirectRecordSelectionKey.hintUnitPtr",
               moduleHint->identity.unitPtr != nullptr,
               legacyHint->identity.unitPtr != nullptr);
    }
    // 无缓存分支（直接走 registry）
    DIFF_U64("DirectRecordSelectionKey.noCache",
             sem::War3SemanticDirectRecordSelectionKey(record, nullptr,
                                                       nullptr),
             legacy::War3SemanticDirectRecordSelectionKey(record, nullptr,
                                                          nullptr));
  }
}

// ---------------------------------------------------------------------------
// 原有边界用例（模块头文件内联判定）
// ---------------------------------------------------------------------------
void RunHeaderBoundaryChecks() {
  using Source = sem::War3SemanticDirectSelectionKeySource;

  CHECK(sem::War3IsSemanticUnitObject(ObjectKind::Unit));
  CHECK(!sem::War3IsSemanticUnitObject(ObjectKind::Unknown));
  CHECK(!sem::War3IsSemanticUnitObject(ObjectKind::Building));
  CHECK(!sem::War3IsSemanticUnitObject(ObjectKind::Destructible));
  CHECK(!sem::War3IsSemanticUnitObject(ObjectKind::Item));
  CHECK(!sem::War3IsSemanticUnitObject(ObjectKind::Effect));

  CHECK(sem::War3SemanticByteSwapU32(0x11223344u) == 0x44332211u);
  CHECK(sem::War3SemanticByteSwapU32(0u) == 0u);
  CHECK(sem::War3SemanticByteSwapU32(0xFFFFFFFFu) == 0xFFFFFFFFu);

  const uint32_t kLtbr = 0x4C546272u;
  const uint32_t kYtbr = 0x59546272u;
  const uint32_t kHfoo = 0x68706F6Fu;
  const uint32_t kLtbrSwapped = sem::War3SemanticByteSwapU32(kLtbr);
  const uint32_t kYtbrSwapped = sem::War3SemanticByteSwapU32(kYtbr);

  CHECK(sem::War3SemanticFourCcHasPrefix(kLtbr, 'L', 'T'));
  CHECK(sem::War3SemanticFourCcHasPrefix(kLtbrSwapped, 'L', 'T'));
  CHECK(sem::War3SemanticFourCcHasPrefix(kYtbr, 'Y', 'T'));
  CHECK(sem::War3SemanticFourCcHasPrefix(kYtbrSwapped, 'Y', 'T'));
  CHECK(!sem::War3SemanticFourCcHasPrefix(kYtbr, 'L', 'T'));
  CHECK(!sem::War3SemanticFourCcHasPrefix(kHfoo, 'L', 'T'));
  CHECK(!sem::War3SemanticFourCcHasPrefix(0u, 'L', 'T'));
  CHECK(!sem::War3SemanticFourCcHasPrefix(0xFFFFFFFFu, 'L', 'T'));

  CHECK(sem::War3SemanticFourCcEqualEitherOrder(kLtbr, kLtbr));
  CHECK(sem::War3SemanticFourCcEqualEitherOrder(kLtbr, kLtbrSwapped));
  CHECK(!sem::War3SemanticFourCcEqualEitherOrder(0u, kLtbrSwapped));
  CHECK(!sem::War3SemanticFourCcEqualEitherOrder(kLtbr, 0u));
  CHECK(!sem::War3SemanticFourCcEqualEitherOrder(kLtbr, kHfoo));
  CHECK(sem::War3SemanticFourCcEqualEitherOrder(0u, 0u));

  CHECK(!sem::War3SemanticRawcodeLooksStaticWorldCaster(0u));
  CHECK(sem::War3SemanticRawcodeLooksStaticWorldCaster(kLtbr));
  CHECK(sem::War3SemanticRawcodeLooksStaticWorldCaster(kLtbrSwapped));
  CHECK(sem::War3SemanticRawcodeLooksStaticWorldCaster(kYtbr));
  CHECK(!sem::War3SemanticRawcodeLooksStaticWorldCaster(kHfoo));
  CHECK(!sem::War3SemanticRawcodeLooksStaticWorldCaster(0xFFFFFFFFu));

  Packet packet = {};
  CHECK(!sem::War3SemanticPacketHasStableUnitResource(packet));
  packet.renderable.runtimeModelPtr = PtrFrom(0x2000u);
  CHECK(!sem::War3SemanticPacketHasStableUnitResource(packet));
  packet.renderable.modelResourcePtr = PtrFrom(0x3000u);
  CHECK(sem::War3SemanticPacketHasStableUnitResource(packet));

  packet = {};
  packet.renderable.runtimeModelPtr = PtrFrom(0x2000u);
  packet.renderable.modelKey = 0x1234u;
  CHECK(sem::War3SemanticPacketHasStableUnitResource(packet));

  packet = {};
  packet.renderable.runtimeModelPtr = PtrFrom(0x2000u);
  packet.resource.modelResourcePtr = PtrFrom(0x4000u);
  CHECK(sem::War3SemanticPacketHasStableUnitResource(packet));

  packet = {};
  packet.renderable.runtimeModelPtr = PtrFrom(0x2000u);
  packet.resource.modelKey = 0x5678u;
  CHECK(sem::War3SemanticPacketHasStableUnitResource(packet));

  packet = {};
  packet.renderable.modelResourcePtr = PtrFrom(0x3000u);
  packet.renderable.modelKey = 0x1234u;
  packet.resource.modelResourcePtr = PtrFrom(0x4000u);
  packet.resource.modelKey = 0x5678u;
  CHECK(!sem::War3SemanticPacketHasStableUnitResource(packet));

  void* const ptrUnitA = PtrFrom(0x1000u);
  void* const ptrUnitB = PtrFrom(0x1100u);
  void* const ptrModel = PtrFrom(0x5000u);
  void* const ptrWorld = PtrFrom(0x6000u);
  void* const ptrScene = PtrFrom(0x7000u);
  void* const ptrPart = PtrFrom(0x8000u);
  void* const ptrMesh = PtrFrom(0x9000u);

  Packet kp = {};
  Sample ks = {};
  Source source = Source::None;

  const uint64_t keyNone = sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(keyNone == 0u);
  CHECK(source == Source::None);
  CHECK(sem::War3SemanticDirectSelectionKey(kp, ks, nullptr) == keyNone);

  kp.renderable.jHandle = 0x1234u;
  const uint64_t keyHandle =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::JHandle);
  CHECK(keyHandle != 0u);

  kp.renderable.unitPtr = ptrUnitA;
  CHECK(sem::War3SemanticDirectSelectionKey(kp, ks, &source) == keyHandle);
  CHECK(source == Source::JHandle);

  kp = {};
  kp.renderable.unitPtr = ptrUnitA;
  const uint64_t keyUnit =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::UnitPtr);
  CHECK(keyUnit != 0u);
  CHECK(keyUnit != keyHandle);

  ks.contract.unitPtr = ptrUnitB;
  CHECK(sem::War3SemanticDirectSelectionKey(kp, ks, &source) == keyUnit);
  CHECK(source == Source::UnitPtr);

  kp = {};
  ks = {};
  ks.contract.jHandle = 0x77u;
  const uint64_t keyContractHandle =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::JHandle);
  CHECK(keyContractHandle != 0u);

  ks = {};
  ks.contract.unitPtr = ptrUnitB;
  const uint64_t keyContractUnit =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::UnitPtr);
  CHECK(keyContractUnit != 0u);
  CHECK(keyContractUnit != keyUnit);

  ks = {};
  kp = {};
  kp.renderable.runtimeModelPtr = ptrModel;
  const uint64_t keyRuntimeModel =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::RuntimeModel);
  CHECK(keyRuntimeModel != 0u);

  kp = {};
  kp.renderable.worldObjectEntry = ptrWorld;
  const uint64_t keyWorld =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::WorldObject);
  CHECK(keyWorld != 0u);

  kp = {};
  ks = {};
  ks.contract.worldObjectEntry = ptrWorld;
  const uint64_t keyContractWorld =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::WorldObject);
  CHECK(keyContractWorld == keyWorld);

  kp = {};
  ks = {};
  kp.renderable.sceneNode = ptrScene;
  const uint64_t keyScene =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::SceneNode);
  CHECK(keyScene != 0u);

  kp = {};
  ks = {};
  ks.contract.sceneNode = ptrScene;
  const uint64_t keyContractScene =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::SceneNode);
  CHECK(keyContractScene == keyScene);

  kp = {};
  ks = {};
  kp.renderable.modelResourcePtr = ptrModel;
  kp.renderable.meshData = ptrMesh;
  const uint64_t keyModelMesh =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::ModelMesh);
  CHECK(keyModelMesh != 0u);
  kp.renderable.meshData = nullptr;
  CHECK(sem::War3SemanticDirectSelectionKey(kp, ks, &source) == 0u);
  CHECK(source == Source::None);
  kp.renderable.renderablePart = ptrPart;
  CHECK(source == Source::None ||
        sem::War3SemanticDirectSelectionKey(kp, ks, nullptr) != 0u);

  kp = {};
  ks = {};
  kp.renderable.renderablePart = ptrPart;
  const uint64_t keyPart =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::RenderablePart);
  CHECK(keyPart != 0u);

  kp = {};
  ks = {};
  ks.contract.renderablePart = ptrPart;
  const uint64_t keyContractPart =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::RenderablePart);
  CHECK(keyContractPart == keyPart);

  const uint64_t distinct[] = {keyHandle,     keyUnit,      keyRuntimeModel,
                               keyWorld,      keyScene,     keyModelMesh,
                               keyPart};
  for (size_t i = 0u; i < 7u; ++i) {
    for (size_t j = i + 1u; j < 7u; ++j)
      CHECK(distinct[i] != distinct[j]);
  }

  kp = {};
  ks = {};
  kp.renderable.jHandle = 0xFFFFFFFFu;
  kp.renderable.unitPtr = PtrFrom(~uintptr_t(0));
  kp.renderable.worldObjectEntry = PtrFrom(~uintptr_t(0));
  const uint64_t keyMax =
      sem::War3SemanticDirectSelectionKey(kp, ks, &source);
  CHECK(source == Source::JHandle);
  CHECK(keyMax != 0u);
  CHECK(keyMax == sem::War3SemanticDirectSelectionKey(kp, ks, nullptr));
}

// ---------------------------------------------------------------------------
// --probe：env getter 的模块实现 vs legacy 实现（同一进程、同一 env）
// ---------------------------------------------------------------------------
int RunProbe() {
  struct BoolProbe {
    const char* name;
    bool (*module)();
    bool (*legacy)();
  };
  const BoolProbe boolProbes[] = {
      {"War3SemanticValidateUnitCoreRuntime",
       &sem::War3SemanticValidateUnitCoreRuntime,
       &legacy::War3SemanticValidateUnitCoreRuntime},
      {"War3SemanticObjectGroupedSelectionRuntime",
       &sem::War3SemanticObjectGroupedSelectionRuntime,
       &legacy::War3SemanticObjectGroupedSelectionRuntime},
      {"War3SemanticStickySelectionLeaseRuntime",
       &sem::War3SemanticStickySelectionLeaseRuntime,
       &legacy::War3SemanticStickySelectionLeaseRuntime},
      {"War3SemanticStickySelectionBroadLeasePreferenceRuntime",
       &sem::War3SemanticStickySelectionBroadLeasePreferenceRuntime,
       &legacy::War3SemanticStickySelectionBroadLeasePreferenceRuntime},
      {"War3SemanticStickySelectionFillRuntime",
       &sem::War3SemanticStickySelectionFillRuntime,
       &legacy::War3SemanticStickySelectionFillRuntime},
      {"War3SemanticStickyPartSelectionRuntime",
       &sem::War3SemanticStickyPartSelectionRuntime,
       &legacy::War3SemanticStickyPartSelectionRuntime},
      {"War3SemanticRejectUnsafeAlphaCasterRuntime",
       &sem::War3SemanticRejectUnsafeAlphaCasterRuntime,
       &legacy::War3SemanticRejectUnsafeAlphaCasterRuntime},
      {"War3SemanticRejectAlphaBlendCasterRuntime",
       &sem::War3SemanticRejectAlphaBlendCasterRuntime,
       &legacy::War3SemanticRejectAlphaBlendCasterRuntime},
      {"War3WidgetProbeSafeCopyRuntime", &sem::War3WidgetProbeSafeCopyRuntime,
       &legacy::War3WidgetProbeSafeCopyRuntime},
      {"War3WidgetNegativeFrameCacheRuntime",
       &sem::War3WidgetNegativeFrameCacheRuntime,
       &legacy::War3WidgetNegativeFrameCacheRuntime},
  };
  for (const auto& probe : boolProbes) {
    std::cout << "probe " << probe.name << " module=" << (probe.module() ? 1 : 0)
              << " legacy=" << (probe.legacy() ? 1 : 0) << "\n";
  }

  struct U32Probe {
    const char* name;
    uint64_t (*module)();
    uint64_t (*legacy)();
  };
  const U32Probe u32Probes[] = {
      {"War3SemanticStickySelectionLeaseFramesRuntime",
       []() -> uint64_t { return sem::War3SemanticStickySelectionLeaseFramesRuntime(); },
       []() -> uint64_t { return legacy::War3SemanticStickySelectionLeaseFramesRuntime(); }},
      {"War3SemanticStickySelectionFillMarginRuntime",
       []() -> uint64_t { return sem::War3SemanticStickySelectionFillMarginRuntime(); },
       []() -> uint64_t { return legacy::War3SemanticStickySelectionFillMarginRuntime(); }},
      {"War3SemanticStickyPartSelectionMinRecordsRuntime",
       []() -> uint64_t { return sem::War3SemanticStickyPartSelectionMinRecordsRuntime(); },
       []() -> uint64_t { return legacy::War3SemanticStickyPartSelectionMinRecordsRuntime(); }},
      {"War3WidgetNegativeFrameCacheTtlFrames",
       []() -> uint64_t { return sem::War3WidgetNegativeFrameCacheTtlFrames(); },
       []() -> uint64_t { return legacy::War3WidgetNegativeFrameCacheTtlFrames(); }},
      {"War3GetEnvU32.dedicated",
       []() -> uint64_t { return sem::War3GetEnvU32("DXVK_WAR3_M1_PROBE_VALUE", 7u); },
       []() -> uint64_t { return legacy::War3GetEnvU32("DXVK_WAR3_M1_PROBE_VALUE", 7u); }},
  };
  for (const auto& probe : u32Probes) {
    std::cout << "probe " << probe.name << " module=" << probe.module()
              << " legacy=" << probe.legacy() << "\n";
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--probe")
    return RunProbe();

  RunHeaderBoundaryChecks();
  DiffTypeShimValues();
  DiffKeyPredicates();
  DiffUnitCore();
  DiffMainWorldBacking();
  DiffPathBlocker();
  DiffEligibilityPredicates();
  DiffFramePreference();
  DiffRecordSelectionKey();

  std::cout << "device semantic predicates equivalence: "
            << (g_checks - g_failures) << "/" << g_checks
            << " checks passed\n";
  return g_failures == 0u ? 0 : 1;
}
