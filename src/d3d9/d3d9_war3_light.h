#pragma once

#include "d3d9_include.h"
#include "../dxvk/dxvk_include.h"
#include "../util/util_vector.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dxvk {

/**
 * @brief 点光源运行时描述。
 * @note position.w = range；color.w = intensity；params.x = shadow intensity。
 */
struct War3PointLight {
  Vector4 position = Vector4(0.0f); // xyz = world pos, w = range
  Vector4 color = Vector4(0.0f);    // rgb = color, w = intensity
  Vector4 params = Vector4(0.0f);   // x = shadow_intensity (0..1)

  int32_t id = 0;
  bool active = true;
};

/**
 * @brief 每帧共享的点光源快照。
 * @note 由 War3LightManager 在一次 mutex 临界区构建并按值返回；consumer
 *       持有不可变副本，避免 worker 与主线程重建缓存时发生数据竞争。
 */
struct War3PointLightFrameSnapshot {
  static constexpr uint32_t kMaxLights = 16u;
  static constexpr uint32_t kMaxShadowLights = 4u;

  std::array<War3PointLight, kMaxLights> lights = {};
  uint32_t count = 0;
  uint32_t shadowCount = 0; // lights[0..shadowCount) 优先为投射阴影的光源
  uint64_t generation = 0;
  uint64_t frameSerial = 0;
  bool hasAny = false;
};

/**
 * @brief 点光源管理器：写侧加锁，读侧使用锁内缓存并按值取得帧快照。
 * @thread_safety 写接口与快照缓存互斥；HasActiveLights 为 atomic 快路径。
 * @performance 禁用点光时主渲染只查 atomic 计数，不进锁、不拷贝 vector。
 */
class War3LightManager {
public:
  static War3LightManager& Instance() {
    static War3LightManager s_instance;
    return s_instance;
  }

  /**
   * @brief 添加点光源。
   * @return 新光源 id；失败返回 0。
   */
  int32_t AddPointLight(float x, float y, float z, float range,
                        float r, float g, float b, float intensity,
                        float shadowIntensity = 0.0f) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lights.size() >= War3PointLightFrameSnapshot::kMaxLights)
      return 0;

    const int32_t id = ++m_nextId;
    War3PointLight light = {};
    light.position = Vector4(x, y, z, std::max(1.0f, range));
    light.color = Vector4(ClampNonNegative(r), ClampNonNegative(g),
                          ClampNonNegative(b), ClampNonNegative(intensity));
    light.params = Vector4(ClampShadowIntensity(shadowIntensity), 0.0f, 0.0f, 0.0f);
    light.id = id;
    light.active = true;
    m_lights.push_back(light);
    m_liveCount.store(static_cast<uint32_t>(m_lights.size()),
                      std::memory_order_release);
    RecountActiveLocked();
    ++m_generation;
    InvalidateSnapshotLocked();
    return id;
  }

  bool UpdatePointLight(int32_t id, float x, float y, float z, float range,
                        float r, float g, float b, float intensity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& light : m_lights) {
      if (light.id != id || !light.active)
        continue;
      light.position = Vector4(x, y, z, std::max(1.0f, range));
      light.color = Vector4(ClampNonNegative(r), ClampNonNegative(g),
                            ClampNonNegative(b), ClampNonNegative(intensity));
      ++m_generation;
      InvalidateSnapshotLocked();
      return true;
    }
    return false;
  }

  bool UpdatePointLightEx(int32_t id, float x, float y, float z, float range,
                          float r, float g, float b, float intensity,
                          float shadowIntensity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& light : m_lights) {
      if (light.id != id || !light.active)
        continue;
      light.position = Vector4(x, y, z, std::max(1.0f, range));
      light.color = Vector4(ClampNonNegative(r), ClampNonNegative(g),
                            ClampNonNegative(b), ClampNonNegative(intensity));
      light.params.x = ClampShadowIntensity(shadowIntensity);
      ++m_generation;
      InvalidateSnapshotLocked();
      return true;
    }
    return false;
  }

  bool SetPointLightShadowIntensity(int32_t id, float shadowIntensity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& light : m_lights) {
      if (light.id != id)
        continue;
      light.params.x = ClampShadowIntensity(shadowIntensity);
      ++m_generation;
      InvalidateSnapshotLocked();
      return true;
    }
    return false;
  }

  bool SetPointLightActive(int32_t id, bool active) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& light : m_lights) {
      if (light.id != id)
        continue;
      if (light.active == active)
        return true;
      light.active = active;
      RecountActiveLocked();
      ++m_generation;
      InvalidateSnapshotLocked();
      return true;
    }
    return false;
  }

  bool SetPointLightPosition(int32_t id, float x, float y, float z) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& light : m_lights) {
      if (light.id != id)
        continue;
      light.position.x = x;
      light.position.y = y;
      light.position.z = z;
      ++m_generation;
      InvalidateSnapshotLocked();
      return true;
    }
    return false;
  }

  bool SetPointLightColorIntensity(int32_t id, float r, float g, float b,
                                   float intensity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& light : m_lights) {
      if (light.id != id)
        continue;
      light.color = Vector4(ClampNonNegative(r), ClampNonNegative(g),
                            ClampNonNegative(b), ClampNonNegative(intensity));
      ++m_generation;
      InvalidateSnapshotLocked();
      return true;
    }
    return false;
  }

  bool SetPointLightRadius(int32_t id, float range) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& light : m_lights) {
      if (light.id != id)
        continue;
      light.position.w = std::max(1.0f, range);
      ++m_generation;
      InvalidateSnapshotLocked();
      return true;
    }
    return false;
  }

  bool IsPointLightAlive(int32_t id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_lights.begin(), m_lights.end(),
                       [id](const War3PointLight& light) {
                         return light.id == id;
                       });
  }

  bool RemovePointLight(int32_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_lights.begin(); it != m_lights.end(); ++it) {
      if (it->id != id)
        continue;
      m_lights.erase(it);
      m_liveCount.store(static_cast<uint32_t>(m_lights.size()),
                        std::memory_order_release);
      RecountActiveLocked();
      ++m_generation;
      InvalidateSnapshotLocked();
      return true;
    }
    return false;
  }

  void ClearLights() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lights.clear();
    m_hasTestLight = false;
    m_liveCount.store(0u, std::memory_order_release);
    m_activeCount.store(0u, std::memory_order_release);
    ++m_generation;
    InvalidateSnapshotLocked();
  }

  /**
   * @brief 创建默认测试点光源（仅调试）。
   */
  void InitTestLight() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_hasTestLight || !m_lights.empty())
      return;
    m_hasTestLight = true;

    War3PointLight light = {};
    light.position = Vector4(0.0f, 0.0f, 400.0f, 2000.0f);
    light.color = Vector4(1.0f, 0.95f, 0.85f, 3.0f);
    light.params = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
    light.id = ++m_nextId;
    light.active = true;
    m_lights.push_back(light);
    m_liveCount.store(static_cast<uint32_t>(m_lights.size()),
                      std::memory_order_release);
    RecountActiveLocked();
    ++m_generation;
    InvalidateSnapshotLocked();
  }

  /**
   * @brief 兼容旧接口：返回活动光源副本。
   * @warning 热路径请改用 GetFrameSnapshot，避免每帧 vector 堆分配。
   */
  std::vector<War3PointLight> GetActiveLights() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lights;
  }

  /**
   * @brief 无锁判断是否存在活动点光源。
   */
  bool HasActiveLights() const {
    return m_activeCount.load(std::memory_order_acquire) > 0u;
  }

  uint32_t GetLightCount() const {
    return m_liveCount.load(std::memory_order_acquire);
  }

  uint64_t GetGeneration() const {
    return m_generation.load(std::memory_order_acquire);
  }

  /**
   * @brief 构建/复用本帧点光源快照，并按相机距离做重要度排序。
   * @param frameSerial 当前帧号；同帧多次调用直接返回缓存。
   * @param cameraPos 相机世界坐标（用于距离排序）；若未知可传 (0,0,0)。
   * @param maxLights 兼容参数；快照始终保留 canonical 16 灯，consumer 本地截断。
   * @param maxShadowLights 兼容参数；canonical shadow prefix 始终最多 4 灯。
   * @return 帧快照副本。按值返回保证调用方在锁释放后仍持有稳定数据。
   * @performance 同帧 O(1) 构建 + 固定大小副本；跨帧 O(N log N) 小 N（<=16）。
   */
  War3PointLightFrameSnapshot GetFrameSnapshot(
      uint64_t frameSerial,
      const Vector4& cameraPos,
      uint32_t maxLights = War3PointLightFrameSnapshot::kMaxLights,
      uint32_t maxShadowLights = War3PointLightFrameSnapshot::kMaxShadowLights) {
    // Cache identity is frame+generation only. Therefore the cached payload
    // must not depend on whichever consumer happens to call first. Point-shadow
    // consumers clamp shadowCount locally; direct-light consumers receive the
    // full canonical list, including shadow-capable lights outside that prefix.
    (void)maxLights;
    (void)maxShadowLights;
    const auto sameCamera = [&](const Vector4& a, const Vector4& b) {
      return a.x == b.x && a.y == b.y && a.z == b.z;
    };
    // Cache fields are intentionally always inspected under the mutex. The old
    // unlocked fast path raced writers, and returning an internal reference let
    // a second consumer overwrite data while the first one was still reading it.
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_snapshotValid &&
        m_snapshot.frameSerial == frameSerial &&
        m_snapshot.generation == m_generation.load(std::memory_order_relaxed) &&
        sameCamera(m_snapshotCameraPos, cameraPos)) {
      return m_snapshot;
    }

    BuildSnapshotLocked(frameSerial, cameraPos);
    return m_snapshot;
  }

private:
  War3LightManager() = default;

  static float ClampNonNegative(float value) {
    return std::max(0.0f, value);
  }

  static float ClampShadowIntensity(float value) {
    return std::clamp(value, 0.0f, 1.0f);
  }

  static double LightImportance(const War3PointLight& light,
                                const Vector4& cameraPos) {
    if (!std::isfinite(cameraPos.x) || !std::isfinite(cameraPos.y) ||
        !std::isfinite(cameraPos.z))
      return 0.0;

    const double dx = double(light.position.x) - double(cameraPos.x);
    const double dy = double(light.position.y) - double(cameraPos.y);
    const double dz = double(light.position.z) - double(cameraPos.z);
    const double distSq = dx * dx + dy * dy + dz * dz;
    const double range = std::max(double(light.position.w), 1.0);
    const double intensity = std::max(double(light.color.w), 0.0);
    const double colorPeak = std::max({double(light.color.x),
                                       double(light.color.y),
                                       double(light.color.z), 0.0});
    // Match the actual direct-light energy contract. Ignoring RGB allowed a
    // black high-intensity light to consume one of the four cube-shadow slots.
    const double score = intensity * colorPeak * range * range /
                         (1.0 + distSq);
    return std::isfinite(score) && score >= 0.0 ? score : 0.0;
  }

  void InvalidateSnapshotLocked() {
    m_snapshotValid = false;
  }

  void RecountActiveLocked() {
    const uint32_t count = static_cast<uint32_t>(std::count_if(
        m_lights.begin(), m_lights.end(),
        [](const War3PointLight& light) { return light.active; }));
    m_activeCount.store(count, std::memory_order_release);
  }

  void BuildSnapshotLocked(uint64_t frameSerial,
                           const Vector4& cameraPos) {
    m_snapshot = {};
    m_snapshot.frameSerial = frameSerial;
    m_snapshot.generation = m_generation.load(std::memory_order_relaxed);
    m_snapshotCameraPos = cameraPos;

    if (m_lights.empty()) {
      m_snapshotValid = true;
      return;
    }

    constexpr uint32_t maxLights = War3PointLightFrameSnapshot::kMaxLights;
    constexpr uint32_t maxShadowLights =
        War3PointLightFrameSnapshot::kMaxShadowLights;

    struct Ranked {
      War3PointLight light;
      double score = 0.0;
      bool castsShadow = false;
    };

    std::array<Ranked, War3PointLightFrameSnapshot::kMaxLights> ranked = {};
    uint32_t rankedCount = 0;
    for (const auto& light : m_lights) {
      if (!light.active)
        continue;
      const bool finiteLight =
          std::isfinite(light.position.x) &&
          std::isfinite(light.position.y) &&
          std::isfinite(light.position.z) &&
          std::isfinite(light.position.w) &&
          std::isfinite(light.color.x) && std::isfinite(light.color.y) &&
          std::isfinite(light.color.z) && std::isfinite(light.color.w);
      const float colorPeak = std::max(
          {light.color.x, light.color.y, light.color.z, 0.0f});
      if (!finiteLight || light.color.w <= 1e-5f ||
          colorPeak <= 0.0f || light.position.w <= 1e-3f)
        continue;
      if (rankedCount >= War3PointLightFrameSnapshot::kMaxLights)
        break;
      Ranked item = {};
      item.light = light;
      item.score = LightImportance(light, cameraPos);
      item.castsShadow = light.params.x > 1e-4f;
      ranked[rankedCount++] = item;
    }

    // 阴影投射光源优先保留，再按重要度排序。
    std::stable_sort(ranked.begin(), ranked.begin() + rankedCount,
                     [](const Ranked& a, const Ranked& b) {
                       if (a.castsShadow != b.castsShadow)
                         return a.castsShadow && !b.castsShadow;
                       return a.score > b.score;
                     });

    uint32_t shadowPacked = 0;
    uint32_t totalPacked = 0;
    std::array<bool, War3PointLightFrameSnapshot::kMaxLights> packed = {};
    // 第一遍：只装阴影投射光源。
    for (uint32_t i = 0; i < rankedCount && shadowPacked < maxShadowLights; ++i) {
      if (!ranked[i].castsShadow)
        continue;
      m_snapshot.lights[totalPacked++] = ranked[i].light;
      packed[i] = true;
      ++shadowPacked;
    }
    m_snapshot.shadowCount = shadowPacked;

    // 第二遍：补齐所有未入 shadow prefix 的灯。这里必须包括
    // 超出 4 灯 prefix 的 castsShadow 灯，它们仍然是有效 direct lights。
    for (uint32_t i = 0; i < rankedCount && totalPacked < maxLights; ++i) {
      if (packed[i])
        continue;
      m_snapshot.lights[totalPacked++] = ranked[i].light;
    }

    // shadowIntensity=0 的灯不进 prefix，但和超出 prefix 的 shadow 灯
    // 一样会留在 canonical direct-light payload 中。
    m_snapshot.count = totalPacked;
    m_snapshot.hasAny = totalPacked > 0u;
    m_snapshotValid = true;
  }

  mutable std::mutex m_mutex;
  std::vector<War3PointLight> m_lights;
  std::atomic<uint32_t> m_liveCount{0};
  std::atomic<uint32_t> m_activeCount{0};
  std::atomic<uint64_t> m_generation{1};
  int32_t m_nextId = 0;
  bool m_hasTestLight = false;

  War3PointLightFrameSnapshot m_snapshot = {};
  Vector4 m_snapshotCameraPos = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  bool m_snapshotValid = false;
};

} // namespace dxvk
