#pragma once

// Data layer (x64 migration phase 3): the map author's fog volumes are CPU
// state written by JASS natives. The volumetric pass reads a snapshot each
// frame; in proxy mode the snapshot is what goes to the host, and every
// query is answered here, in the game process.

#include "../../../util/util_vector.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dxvk {

enum class War3FogVolumeShape : uint32_t {
  Sphere = 0u,
  Box = 1u,
  Cylinder = 2u,
};

/**
 * @brief 渲染线程持有的局部体积雾不可变条目。
 *
 * worldToLocal 把世界坐标变换到标准体积：球/圆柱半径为 1，Box 范围为
 * [-1, 1]。params.x=shape，y=density，z=edge feather，w 保留。
 */
struct War3FogVolumeFrameEntry {
  Vector4 worldToLocal0 = Vector4(0.0f);
  Vector4 worldToLocal1 = Vector4(0.0f);
  Vector4 worldToLocal2 = Vector4(0.0f);
  Vector4 params = Vector4(0.0f);
  Vector4 boundingSphere = Vector4(0.0f); // xyz=center, w=radius
  int32_t id = 0;
};

struct War3FogVolumeFrameSnapshot {
  static constexpr uint32_t kMaxVolumes = 8u;

  std::array<War3FogVolumeFrameEntry, kMaxVolumes> volumes = {};
  uint32_t count = 0u;
  uint64_t generation = 0u;
  uint64_t frameSerial = 0u;
  bool hasAny = false;
};

/**
 * @brief One fog volume as its author gave it, for a consumer that rebuilds
 *        the volumes elsewhere (the FogVolumes scene record).
 *
 * halfSize is the manager's own form: a sphere's radius three times, a box's
 * half extents, a cylinder's radius twice and half its height.
 */
struct War3FogVolumeAuthored {
  War3FogVolumeShape shape = War3FogVolumeShape::Sphere;
  Vector4 position = Vector4(0.0f);
  Vector4 halfSize = Vector4(1.0f);
  Vector4 rotationDegrees = Vector4(0.0f);
  float density = 0.0f;
  float edgeFeather = 0.0f;
};

/**
 * @brief 地图作者局部体积雾管理器。
 *
 * 写侧由 JASS/API 线程通过互斥锁修改；渲染侧按值取得固定上限快照，绝不
 * 在 fragment 路径读取 JASS 状态。全部输入在此再次验证，直接 C API 调用
 * 不能绕过 JAPI 的有限性、尺寸和容量边界。
 */
class War3FogVolumeManager {
public:
  static War3FogVolumeManager& Instance();

  int32_t AddSphere(float x, float y, float z, float radius,
                    float density, float edgeFeather);
  int32_t AddBox(float x, float y, float z,
                 float sizeX, float sizeY, float sizeZ,
                 float density, float edgeFeather);
  int32_t AddCylinder(float x, float y, float z,
                      float radius, float height,
                      float density, float edgeFeather);

  bool SetActive(int32_t id, bool active);
  bool SetPosition(int32_t id, float x, float y, float z);
  // 欧拉角单位为度，固定按 Rz * Ry * Rx 组合；Z 为常用 War3 水平朝向。
  bool SetRotationDegrees(int32_t id, float x, float y, float z);
  bool SetDensity(int32_t id, float density);
  bool SetEdgeFeather(int32_t id, float edgeFeather);
  bool SetSphereRadius(int32_t id, float radius);
  bool SetBoxSize(int32_t id, float sizeX, float sizeY, float sizeZ);
  bool SetCylinderSize(int32_t id, float radius, float height);

  bool IsAlive(int32_t id) const;
  bool Remove(int32_t id);
  void Clear();

  bool HasActiveVolumes() const;
  uint32_t GetVolumeCount() const;
  uint64_t GetGeneration() const;
  War3FogVolumeFrameSnapshot GetFrameSnapshot(uint64_t frameSerial);
  // The volumes GetFrameSnapshot would return, in the same order and with the
  // same limit, as authored. Returns how many were written.
  uint32_t ListFrameVolumesAuthored(
      std::array<War3FogVolumeAuthored, War3FogVolumeFrameSnapshot::kMaxVolumes>& out) const;

private:
  struct Record {
    War3FogVolumeFrameEntry frame = {};
    War3FogVolumeShape shape = War3FogVolumeShape::Sphere;
    Vector4 position = Vector4(0.0f);
    Vector4 halfSize = Vector4(1.0f);
    Vector4 rotationDegrees = Vector4(0.0f);
    float density = 0.0f;
    float edgeFeather = 0.0f;
    bool active = true;
  };

  War3FogVolumeManager() = default;

  int32_t AddLocked(War3FogVolumeShape shape,
                    const Vector4& position, const Vector4& halfSize,
                    float density, float edgeFeather);
  Record* FindLocked(int32_t id);
  const Record* FindLocked(int32_t id) const;
  static bool RebuildDerived(Record& record);
  void NoteMutationLocked();
  void RecountActiveLocked();

  mutable std::mutex m_mutex;
  std::vector<Record> m_volumes;
  std::atomic<uint32_t> m_liveCount{0u};
  std::atomic<uint32_t> m_activeCount{0u};
  std::atomic<uint64_t> m_generation{1u};
  int32_t m_nextId = 0;
  War3FogVolumeFrameSnapshot m_snapshot = {};
  bool m_snapshotValid = false;
};

} // namespace dxvk
