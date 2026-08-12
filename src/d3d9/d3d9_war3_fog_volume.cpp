#include "d3d9_war3_fog_volume.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dxvk {
namespace {

constexpr float kMaximumCoordinate = 1'000'000.0f;
constexpr float kMinimumHalfExtent = 0.5f;
constexpr float kMaximumHalfExtent = 100'000.0f;
constexpr float kMaximumDensity = 2.0f;
constexpr float kMaximumRotationMagnitude = 360'000.0f;
constexpr float kPi = 3.14159265358979323846f;

bool FiniteInRange(float value, float lo, float hi) {
  return std::isfinite(value) && value >= lo && value <= hi;
}

bool ValidPosition(const Vector4& value) {
  return FiniteInRange(value.x, -kMaximumCoordinate, kMaximumCoordinate) &&
         FiniteInRange(value.y, -kMaximumCoordinate, kMaximumCoordinate) &&
         FiniteInRange(value.z, -kMaximumCoordinate, kMaximumCoordinate);
}

bool ValidHalfSize(const Vector4& value) {
  return FiniteInRange(value.x, kMinimumHalfExtent, kMaximumHalfExtent) &&
         FiniteInRange(value.y, kMinimumHalfExtent, kMaximumHalfExtent) &&
         FiniteInRange(value.z, kMinimumHalfExtent, kMaximumHalfExtent);
}

bool ValidDensity(float value) {
  return FiniteInRange(value, 0.0f, kMaximumDensity);
}

bool ValidFeather(float value) {
  return FiniteInRange(value, 0.0f, 1.0f);
}

bool ValidRotation(float value) {
  return FiniteInRange(value, -kMaximumRotationMagnitude,
                       kMaximumRotationMagnitude);
}

float NormalizeDegrees(float value) {
  const float normalized = std::remainder(value, 360.0f);
  return normalized == -0.0f ? 0.0f : normalized;
}

Vector4 MakeWorldToLocalRow(const Vector4& axis, float extent,
                            const Vector4& position) {
  const float invExtent = 1.0f / extent;
  const Vector4 row(axis.x * invExtent, axis.y * invExtent,
                    axis.z * invExtent, 0.0f);
  return Vector4(row.x, row.y, row.z,
                 -(row.x * position.x + row.y * position.y +
                   row.z * position.z));
}

} // namespace

War3FogVolumeManager& War3FogVolumeManager::Instance() {
  static War3FogVolumeManager s_instance;
  return s_instance;
}

int32_t War3FogVolumeManager::AddSphere(
    float x, float y, float z, float radius,
    float density, float edgeFeather) {
  const Vector4 position(x, y, z, 1.0f);
  const Vector4 halfSize(radius, radius, radius, 0.0f);
  std::lock_guard<std::mutex> lock(m_mutex);
  return AddLocked(War3FogVolumeShape::Sphere, position, halfSize,
                   density, edgeFeather);
}

int32_t War3FogVolumeManager::AddBox(
    float x, float y, float z, float sizeX, float sizeY, float sizeZ,
    float density, float edgeFeather) {
  const Vector4 position(x, y, z, 1.0f);
  const Vector4 halfSize(sizeX * 0.5f, sizeY * 0.5f, sizeZ * 0.5f, 0.0f);
  std::lock_guard<std::mutex> lock(m_mutex);
  return AddLocked(War3FogVolumeShape::Box, position, halfSize,
                   density, edgeFeather);
}

int32_t War3FogVolumeManager::AddCylinder(
    float x, float y, float z, float radius, float height,
    float density, float edgeFeather) {
  const Vector4 position(x, y, z, 1.0f);
  const Vector4 halfSize(radius, radius, height * 0.5f, 0.0f);
  std::lock_guard<std::mutex> lock(m_mutex);
  return AddLocked(War3FogVolumeShape::Cylinder, position, halfSize,
                   density, edgeFeather);
}

int32_t War3FogVolumeManager::AddLocked(
    War3FogVolumeShape shape, const Vector4& position,
    const Vector4& halfSize, float density, float edgeFeather) {
  if (m_volumes.size() >= War3FogVolumeFrameSnapshot::kMaxVolumes ||
      !ValidPosition(position) || !ValidHalfSize(halfSize) ||
      !ValidDensity(density) || !ValidFeather(edgeFeather))
    return 0;

  if (m_nextId == std::numeric_limits<int32_t>::max())
    return 0;

  Record record = {};
  record.frame.id = ++m_nextId;
  record.shape = shape;
  record.position = position;
  record.halfSize = halfSize;
  record.density = density;
  record.edgeFeather = edgeFeather;
  record.active = true;
  if (!RebuildDerived(record))
    return 0;

  const int32_t id = record.frame.id;
  m_volumes.push_back(record);
  m_liveCount.store(static_cast<uint32_t>(m_volumes.size()),
                    std::memory_order_release);
  NoteMutationLocked();
  return id;
}

War3FogVolumeManager::Record* War3FogVolumeManager::FindLocked(int32_t id) {
  const auto entry = std::find_if(
      m_volumes.begin(), m_volumes.end(),
      [id](const Record& record) { return record.frame.id == id; });
  return entry != m_volumes.end() ? &*entry : nullptr;
}

const War3FogVolumeManager::Record*
War3FogVolumeManager::FindLocked(int32_t id) const {
  const auto entry = std::find_if(
      m_volumes.begin(), m_volumes.end(),
      [id](const Record& record) { return record.frame.id == id; });
  return entry != m_volumes.end() ? &*entry : nullptr;
}

bool War3FogVolumeManager::SetActive(int32_t id, bool active) {
  std::lock_guard<std::mutex> lock(m_mutex);
  Record* record = FindLocked(id);
  if (!record)
    return false;
  if (record->active == active)
    return true;
  record->active = active;
  if (!RebuildDerived(*record))
    return false;
  NoteMutationLocked();
  return true;
}

bool War3FogVolumeManager::SetPosition(
    int32_t id, float x, float y, float z) {
  const Vector4 position(x, y, z, 1.0f);
  if (!ValidPosition(position))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  Record* record = FindLocked(id);
  if (!record)
    return false;
  record->position = position;
  if (!RebuildDerived(*record))
    return false;
  NoteMutationLocked();
  return true;
}

bool War3FogVolumeManager::SetRotationDegrees(
    int32_t id, float x, float y, float z) {
  if (!ValidRotation(x) || !ValidRotation(y) || !ValidRotation(z))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  Record* record = FindLocked(id);
  if (!record)
    return false;
  record->rotationDegrees = Vector4(
      NormalizeDegrees(x), NormalizeDegrees(y), NormalizeDegrees(z), 0.0f);
  if (!RebuildDerived(*record))
    return false;
  NoteMutationLocked();
  return true;
}

bool War3FogVolumeManager::SetDensity(int32_t id, float density) {
  if (!ValidDensity(density))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  Record* record = FindLocked(id);
  if (!record)
    return false;
  record->density = density;
  if (!RebuildDerived(*record))
    return false;
  NoteMutationLocked();
  return true;
}

bool War3FogVolumeManager::SetEdgeFeather(
    int32_t id, float edgeFeather) {
  if (!ValidFeather(edgeFeather))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  Record* record = FindLocked(id);
  if (!record)
    return false;
  record->edgeFeather = edgeFeather;
  if (!RebuildDerived(*record))
    return false;
  NoteMutationLocked();
  return true;
}

bool War3FogVolumeManager::SetSphereRadius(int32_t id, float radius) {
  const Vector4 halfSize(radius, radius, radius, 0.0f);
  if (!ValidHalfSize(halfSize))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  Record* record = FindLocked(id);
  if (!record || record->shape != War3FogVolumeShape::Sphere)
    return false;
  record->halfSize = halfSize;
  if (!RebuildDerived(*record))
    return false;
  NoteMutationLocked();
  return true;
}

bool War3FogVolumeManager::SetBoxSize(
    int32_t id, float sizeX, float sizeY, float sizeZ) {
  const Vector4 halfSize(sizeX * 0.5f, sizeY * 0.5f,
                         sizeZ * 0.5f, 0.0f);
  if (!ValidHalfSize(halfSize))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  Record* record = FindLocked(id);
  if (!record || record->shape != War3FogVolumeShape::Box)
    return false;
  record->halfSize = halfSize;
  if (!RebuildDerived(*record))
    return false;
  NoteMutationLocked();
  return true;
}

bool War3FogVolumeManager::SetCylinderSize(
    int32_t id, float radius, float height) {
  const Vector4 halfSize(radius, radius, height * 0.5f, 0.0f);
  if (!ValidHalfSize(halfSize))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  Record* record = FindLocked(id);
  if (!record || record->shape != War3FogVolumeShape::Cylinder)
    return false;
  record->halfSize = halfSize;
  if (!RebuildDerived(*record))
    return false;
  NoteMutationLocked();
  return true;
}

bool War3FogVolumeManager::IsAlive(int32_t id) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return FindLocked(id) != nullptr;
}

bool War3FogVolumeManager::Remove(int32_t id) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto entry = std::find_if(
      m_volumes.begin(), m_volumes.end(),
      [id](const Record& record) { return record.frame.id == id; });
  if (entry == m_volumes.end())
    return false;
  m_volumes.erase(entry);
  m_liveCount.store(static_cast<uint32_t>(m_volumes.size()),
                    std::memory_order_release);
  NoteMutationLocked();
  return true;
}

void War3FogVolumeManager::Clear() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_volumes.clear();
  m_liveCount.store(0u, std::memory_order_release);
  NoteMutationLocked();
}

bool War3FogVolumeManager::HasActiveVolumes() const {
  return m_activeCount.load(std::memory_order_acquire) > 0u;
}

uint32_t War3FogVolumeManager::GetVolumeCount() const {
  return m_liveCount.load(std::memory_order_acquire);
}

uint64_t War3FogVolumeManager::GetGeneration() const {
  return m_generation.load(std::memory_order_acquire);
}

War3FogVolumeFrameSnapshot War3FogVolumeManager::GetFrameSnapshot(
    uint64_t frameSerial) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const uint64_t generation = m_generation.load(std::memory_order_relaxed);
  if (m_snapshotValid && m_snapshot.generation == generation) {
    m_snapshot.frameSerial = frameSerial;
    return m_snapshot;
  }

  m_snapshot = {};
  m_snapshot.generation = generation;
  m_snapshot.frameSerial = frameSerial;
  for (const Record& record : m_volumes) {
    if (!record.active || record.density <= 1.0e-6f)
      continue;
    if (m_snapshot.count >= War3FogVolumeFrameSnapshot::kMaxVolumes)
      break;
    m_snapshot.volumes[m_snapshot.count++] = record.frame;
  }
  m_snapshot.hasAny = m_snapshot.count > 0u;
  m_snapshotValid = true;
  return m_snapshot;
}

bool War3FogVolumeManager::RebuildDerived(Record& record) {
  if (!ValidPosition(record.position) || !ValidHalfSize(record.halfSize) ||
      !ValidDensity(record.density) || !ValidFeather(record.edgeFeather) ||
      !ValidRotation(record.rotationDegrees.x) ||
      !ValidRotation(record.rotationDegrees.y) ||
      !ValidRotation(record.rotationDegrees.z))
    return false;

  const float rx = record.rotationDegrees.x * (kPi / 180.0f);
  const float ry = record.rotationDegrees.y * (kPi / 180.0f);
  const float rz = record.rotationDegrees.z * (kPi / 180.0f);
  const float sx = std::sin(rx);
  const float cx = std::cos(rx);
  const float sy = std::sin(ry);
  const float cy = std::cos(ry);
  const float sz = std::sin(rz);
  const float cz = std::cos(rz);

  // R = Rz * Ry * Rx；以下三列是局部 X/Y/Z 轴在世界空间中的方向。
  const Vector4 axisX(cz * cy, sz * cy, -sy, 0.0f);
  const Vector4 axisY(cz * sy * sx - sz * cx,
                      sz * sy * sx + cz * cx, cy * sx, 0.0f);
  const Vector4 axisZ(cz * sy * cx + sz * sx,
                      sz * sy * cx - cz * sx, cy * cx, 0.0f);

  record.frame.worldToLocal0 = MakeWorldToLocalRow(
      axisX, record.halfSize.x, record.position);
  record.frame.worldToLocal1 = MakeWorldToLocalRow(
      axisY, record.halfSize.y, record.position);
  record.frame.worldToLocal2 = MakeWorldToLocalRow(
      axisZ, record.halfSize.z, record.position);
  record.frame.params = Vector4(
      static_cast<float>(record.shape), record.density,
      record.edgeFeather, record.active ? 1.0f : 0.0f);

  float radius = record.halfSize.x;
  if (record.shape == War3FogVolumeShape::Box) {
    radius = std::sqrt(record.halfSize.x * record.halfSize.x +
                       record.halfSize.y * record.halfSize.y +
                       record.halfSize.z * record.halfSize.z);
  } else if (record.shape == War3FogVolumeShape::Cylinder) {
    radius = std::sqrt(record.halfSize.x * record.halfSize.x +
                       record.halfSize.z * record.halfSize.z);
  }
  if (!std::isfinite(radius) || radius < kMinimumHalfExtent)
    return false;
  record.frame.boundingSphere = Vector4(
      record.position.x, record.position.y, record.position.z, radius);
  return true;
}

void War3FogVolumeManager::NoteMutationLocked() {
  ++m_generation;
  m_snapshotValid = false;
  RecountActiveLocked();
}

void War3FogVolumeManager::RecountActiveLocked() {
  const uint32_t count = static_cast<uint32_t>(std::count_if(
      m_volumes.begin(), m_volumes.end(),
      [](const Record& record) {
        return record.active && record.density > 1.0e-6f;
      }));
  m_activeCount.store(count, std::memory_order_release);
}

} // namespace dxvk
