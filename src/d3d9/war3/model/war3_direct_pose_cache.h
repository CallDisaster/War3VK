// war3_direct_pose_cache.h — 极简 O(1) 直读 Pose 缓存
//
// 核心思想：跳过 5 个 Registry + 20 次 mutex + 30 次 map 操作的 semantic 数据链。
// 在 SpriteFrameUpdate Hook 返回后，直接从 CModel 结构体读取最终骨骼矩阵。
//
// CModel + 0x5C = FinalPoseMatrixCount（uint32_t）
// CModel + 0x60 = FinalPoseMatrixArray（float* 指向 3×4 矩阵数组）
// CModel + 0x64 = WorldMatrix3x4（float[12]，世界变换）
//
// 每帧一次 clear + N 次 O(1) write + M 次 O(1) read。
// 没有 mutex（单线程 game thread 写，render thread 读 snapshot）。
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <mutex>
#include <atomic>

#include "../core/war3_game_structs.h"

namespace dxvk::war3::model {

// 每个 Sprite 的直读 Pose 记录
struct DirectPoseEntry {
  void*    runtimeModelPtr = nullptr;   // CModel* — Hook 时已知
  void*    spritePtr       = nullptr;   // CSprite* — Hook 时已知
  void*    sceneNode       = nullptr;   // 如果可用（从 identity bridge 前推）
  void*    unitPtr         = nullptr;   // CUnit* — 如果可用
  void*    worldObjectEntry = nullptr;  // 如果可用

  // 直接从 CModel 读取的数据（O(1)，无 Registry）
  uint32_t matrixCount     = 0u;
  const float* matrixArrayPtr = nullptr; // 指向引擎内存的 3×4 矩阵数组
  float    worldMatrix[12] = {};         // 拷贝的世界变换矩阵
  bool     hasWorldTransform = false;

  // 额外元数据（直接从 CSprite / CUnit 读取）
  float    uniformScale    = 1.0f;
  float    flyHeight       = 0.0f;
  uint32_t rawcode         = 0u;
  uint32_t jHandle         = 0u;
};

// 直读 Pose 快照（双缓冲：game thread 写 staging，render thread 读 published）
class DirectPoseCache {
public:
  static DirectPoseCache& instance() {
    static DirectPoseCache s_instance;
    return s_instance;
  }

  // ========== Game Thread API（SpriteFrameUpdate Hook 回调时调用）==========

  // 每帧开始时清空 staging
  void beginFrame(uint64_t frameSerial) {
    m_stagingEntries.clear();
    m_stagingEntries.reserve(m_lastPublishedCount + 64u);
    m_stagingFrameSerial = frameSerial;
  }

  // 在 SpriteFrameUpdate Hook 返回后直接从 CModel 读取 pose，O(1) 写入
  void noteSpritePose(void* spritePtr, void* runtimeModelPtr,
                      void* sceneNode = nullptr,
                      void* unitPtr = nullptr,
                      void* worldObjectEntry = nullptr,
                      uint32_t rawcode = 0u,
                      uint32_t jHandle = 0u,
                      float flyHeight = 0.0f) {
    if (runtimeModelPtr == nullptr)
      return;

    DirectPoseEntry entry;
    entry.runtimeModelPtr    = runtimeModelPtr;
    entry.spritePtr          = spritePtr;
    entry.sceneNode          = sceneNode;
    entry.unitPtr            = unitPtr;
    entry.worldObjectEntry   = worldObjectEntry;
    entry.rawcode            = rawcode;
    entry.jHandle            = jHandle;
    entry.flyHeight          = flyHeight;

    // O(1) 直读 CModel 结构体
    ReadCModelPose(runtimeModelPtr, entry);

    m_stagingEntries.push_back(entry);
  }

  // 每帧结束后发布 snapshot
  void endFrame() {
    std::lock_guard<std::mutex> lock(m_publishMutex);
    m_publishedEntries.swap(m_stagingEntries);
    m_publishedFrameSerial = m_stagingFrameSerial;
    m_lastPublishedCount = static_cast<uint32_t>(m_publishedEntries.size());
  }

  // ========== Render Thread API（shadow contract capture 时调用）==========

  // 获取当前已发布的快照
  uint64_t publishedFrameSerial() const {
    return m_publishedFrameSerial;
  }

  const std::vector<DirectPoseEntry>& publishedEntries() const {
    return m_publishedEntries;
  }

  // 按 runtimeModelPtr 查找已发布的 pose entry
  // O(N) 线性扫描，但 N 通常 100~500，且无 mutex
  const DirectPoseEntry* findByRuntimeModel(void* runtimeModelPtr) const {
    std::lock_guard<std::mutex> lock(m_publishMutex);
    for (const auto& entry : m_publishedEntries) {
      if (entry.runtimeModelPtr == runtimeModelPtr)
        return &entry;
    }
    return nullptr;
  }

  // 直接读取指定 CModel 的当前 pose（不需要缓存，O(1) 直读内存）
  static bool ReadLiveCModelPose(void* runtimeModelPtr,
                                  uint32_t& outMatrixCount,
                                  const float*& outMatrixArrayPtr,
                                  float outWorldMatrix[12]) {
    if (runtimeModelPtr == nullptr)
      return false;

    auto* modelBase = static_cast<uint8_t*>(runtimeModelPtr);

    // CModel + 0x5C = FinalPoseMatrixCount
    uint32_t count = 0u;
    std::memcpy(&count, modelBase + CModelOffsets::FinalPoseMatrixCount,
                sizeof(uint32_t));

    // CModel + 0x60 = FinalPoseMatrixArray（指针）
    uintptr_t arrayPtr = 0u;
    std::memcpy(&arrayPtr, modelBase + CModelOffsets::FinalPoseMatrixArray,
                sizeof(uintptr_t));

    if (count == 0u || arrayPtr == 0u)
      return false;

    outMatrixCount = count;
    outMatrixArrayPtr = reinterpret_cast<const float*>(arrayPtr);

    // CModel + 0x64 = WorldMatrix3x4（float[12] 直接拷贝）
    std::memcpy(outWorldMatrix, modelBase + CModelOffsets::WorldMatrix3x4,
                sizeof(float) * 12);

    return true;
  }

  uint32_t lastPublishedCount() const { return m_lastPublishedCount; }

private:
  DirectPoseCache() = default;

  static void ReadCModelPose(void* runtimeModelPtr, DirectPoseEntry& entry) {
    auto* modelBase = static_cast<uint8_t*>(runtimeModelPtr);

    // CModel + 0x5C = FinalPoseMatrixCount
    uint32_t count = 0u;
    std::memcpy(&count, modelBase + CModelOffsets::FinalPoseMatrixCount,
                sizeof(uint32_t));

    // CModel + 0x60 = FinalPoseMatrixArray（指针）
    uintptr_t arrayPtr = 0u;
    std::memcpy(&arrayPtr, modelBase + CModelOffsets::FinalPoseMatrixArray,
                sizeof(uintptr_t));

    if (count != 0u && arrayPtr != 0u) {
      entry.matrixCount = count;
      entry.matrixArrayPtr = reinterpret_cast<const float*>(arrayPtr);
    }

    // CModel + 0x64 = WorldMatrix3x4（float[12]）
    std::memcpy(entry.worldMatrix, modelBase + CModelOffsets::WorldMatrix3x4,
                sizeof(float) * 12);

    // 检查世界变换是否有效（非零）
    bool allZero = true;
    for (int i = 0; i < 12; ++i) {
      if (entry.worldMatrix[i] != 0.0f) {
        allZero = false;
        break;
      }
    }
    entry.hasWorldTransform = !allZero;
  }

  // Game thread staging buffer
  std::vector<DirectPoseEntry> m_stagingEntries;
  uint64_t m_stagingFrameSerial = 0u;

  // Published snapshot（render thread 读取）
  mutable std::mutex m_publishMutex;
  std::vector<DirectPoseEntry> m_publishedEntries;
  uint64_t m_publishedFrameSerial = 0u;
  uint32_t m_lastPublishedCount = 0u;
};

} // namespace dxvk::war3::model
