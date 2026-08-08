// war3_render_objects.h - 渲染对象追踪模块
// 提供当前帧渲染对象的收集和查询

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dxvk {
namespace war3 {
namespace render {

// ============================================================================
// 渲染对象类型
// ============================================================================

enum class ObjectKind : uint8_t {
  Unknown = 0,
  Unit,         // 普通单位
  Building,     // 建筑
  Destructible, // 可破坏物
  Item,         // 物品
  Effect,       // 特效
};

const char *ObjectKindToString(ObjectKind kind);

// ============================================================================
// 渲染对象信息
// ============================================================================

struct RenderObjectInfo {
  // 渲染层指针
  void *worldObjectEntry = nullptr; // WorldObjectEntry*
  void *sceneNode = nullptr;        // SceneNode*

  // 游戏层指针
  void *unitPtr = nullptr;  // CUnit*
  void *agentPtr = nullptr; // CAgentBaseAbs*

  // Handle 信息
  uint32_t handleId = 0; // HandleId (不含 0x100000)
  uint32_t jHandle = 0;  // 完整 jHandle (handleId | 0x100000)

  // 对象属性
  uint32_t rawcode = 0;   // 四字码 (如 'hfoo')
  uint32_t agentType = 0; // Agent 类型 FourCC
  uint32_t flags5C = 0;   // CUnit flags at 0x5C

  ObjectKind kind = ObjectKind::Unknown;
  int groupIdx = -1; // 来自哪个渲染组 (0=单位, 1=建筑/选择圈, 2=装饰/特效)

  // 便捷判断
  bool isBuilding() const { return kind == ObjectKind::Building; }
  bool isUnit() const { return kind == ObjectKind::Unit; }
  bool isDestructible() const { return kind == ObjectKind::Destructible; }
  bool hasValidHandle() const { return jHandle != 0; }
};

struct RenderObjectBatchItem {
  void *worldObjectEntry = nullptr;
  void *unitPtr = nullptr;
  void *sceneNode = nullptr;
  uint32_t jHandle = 0;
  int groupIdx = -1;
};

enum class RenderObjectBatchResolveMode : uint8_t {
  IdentityOnly = 0,
  FastMetadata,
  FullResolve,
};

// ============================================================================
// 渲染对象注册表（单例）
// ============================================================================

class RenderObjectRegistry {
public:
  static RenderObjectRegistry &instance();

  // ========== 生命周期 ==========

  // 帧开始时清空所有数据
  void beginFrame();

  // 帧结束后可调用（可选，用于统计）
  void endFrame();

  // ========== 数据收集（由 Hook 调用） ==========

  // 阶段1：WorldObjects_RenderGroup 调用
  // 注册对象，建立 WorldObjectEntry -> UnitPtr 映射
  void registerWorldObject(void *worldObjectEntry, void *unitPtr, int groupIdx);
  void
  registerWorldObjectsBatch(const std::vector<RenderObjectBatchItem> &items,
                            RenderObjectBatchResolveMode mode);

  // 阶段2：WorldObjectEntry_Render 调用
  // 建立 SceneNode -> WorldObjectEntry 映射
  void mapSceneNode(void *worldObjectEntry, void *sceneNode);

  // ========== 数据查询（由 ExecBatch 等调用） ==========

  // 根据 SceneNode 查询完整对象信息
  // 这是主要查询入口，ExecBatch 使用 element+0x14 作为 sceneNode
  bool queryBySceneNode(void *sceneNode, RenderObjectInfo &out) const;
  const RenderObjectInfo *findBySceneNode(void *sceneNode) const;

  // 根据 WorldObjectEntry 查询
  bool queryByEntry(void *worldObjectEntry, RenderObjectInfo &out) const;

  // 根据 jHandle 查询
  const RenderObjectInfo *findByHandle(uint32_t jHandle) const;
  const RenderObjectInfo *findByEntry(void *worldObjectEntry) const;

  // ========== 批量查询 ==========

  // 获取所有已注册对象
  std::vector<RenderObjectInfo> getAllObjects() const;

  // 获取指定类型的对象
  std::vector<RenderObjectInfo> getObjectsByKind(ObjectKind kind) const;

  // 获取所有单位（不含建筑）
  std::vector<RenderObjectInfo> getUnits() const;

  // 获取所有建筑
  std::vector<RenderObjectInfo> getBuildings() const;

  // 按索引获取对象（无固定顺序，仅用于调试/枚举）
  bool getObjectByIndex(size_t index, RenderObjectInfo &out) const;

  // ========== 统计信息 ==========

  size_t getObjectCount() const;
  size_t getSceneNodeMappingCount() const;
  uint64_t getFrameNumber() const { return m_frameNumber.load(); }

private:
  RenderObjectRegistry() = default;

  // 填充对象的 Handle 和属性信息
  void resolveObjectInfo(RenderObjectInfo &info) const;

  struct Snapshot {
    std::unordered_map<void *, RenderObjectInfo> byEntry;
    // 说明：为了降低 ExecBatch 热路径的查询开销，直接存储指向 byEntry value 的指针。
    // 前提：本 Snapshot 在发布后不会再修改，因此指针在该 Snapshot 生命周期内稳定。
    std::unordered_map<void *, RenderObjectInfo *> sceneToInfo;
    std::unordered_map<uint32_t, RenderObjectInfo *> handleToInfo;
    size_t lastEntryCount = 0;
    size_t lastSceneCount = 0;
    size_t lastHandleCount = 0;
  };

  static constexpr uint32_t kSnapshotCount = 2;
  std::array<Snapshot, kSnapshotCount> m_snapshots = {};
  std::atomic<uint32_t> m_publishedIndex{0};
  uint32_t m_writeIndex = 0;
  std::thread::id m_renderThreadId = {};

  Snapshot &writeSnapshot();
  const Snapshot &readSnapshot() const;
  const Snapshot &snapshotForThread() const;

  // 帧计数
  std::atomic<uint64_t> m_frameNumber{0};
};

// ============================================================================
// TLS 当前批次信息（ExecBatch 期间有效）
// ============================================================================

// 获取当前批次的 jHandle（0 表示未知）
uint32_t GetCurrentBatchHandle();

// 设置当前批次的 jHandle（由 Hook 调用）
void SetCurrentBatchHandle(uint32_t jHandle);

// 获取当前批次的对象信息
const RenderObjectInfo *GetCurrentBatchObject();

// 设置当前批次的对象信息
void SetCurrentBatchObject(const RenderObjectInfo *info);

// Present-owned map transition hook. This invalidates only process-global
// lookup aliases; published frame snapshots keep their existing two-slot
// lifetime and are replaced by War3Renderer::ResetMapSession().
void ResetRenderObjectMapSessionCaches();

} // namespace render
} // namespace war3
} // namespace dxvk
