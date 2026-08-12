#include "../../../d3d9_war3_fog_volume.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++g_failures;
  }
}

bool Near(float a, float b, float epsilon = 1.0e-4f) {
  return std::abs(a - b) <= epsilon;
}

dxvk::Vector4 TransformPoint(
    const dxvk::War3FogVolumeFrameEntry& volume,
    const dxvk::Vector4& point) {
  const auto dot = [&point](const dxvk::Vector4& row) {
    return row.x * point.x + row.y * point.y +
           row.z * point.z + row.w;
  };
  return dxvk::Vector4(
      dot(volume.worldToLocal0), dot(volume.worldToLocal1),
      dot(volume.worldToLocal2), 1.0f);
}

void TestShapesAndTransforms() {
  auto& manager = dxvk::War3FogVolumeManager::Instance();
  manager.Clear();

  const int32_t sphere = manager.AddSphere(
      100.0f, 200.0f, 300.0f, 50.0f, 0.5f, 0.25f);
  Check(sphere > 0, "valid sphere must be created");
  auto snapshot = manager.GetFrameSnapshot(17u);
  Check(snapshot.hasAny && snapshot.count == 1u,
        "active sphere must publish one immutable entry");
  Check(snapshot.frameSerial == 17u,
        "snapshot must carry the requesting frame serial");
  const auto sphereCenter = TransformPoint(
      snapshot.volumes[0], dxvk::Vector4(100.0f, 200.0f, 300.0f, 1.0f));
  const auto sphereEdge = TransformPoint(
      snapshot.volumes[0], dxvk::Vector4(150.0f, 200.0f, 300.0f, 1.0f));
  Check(Near(sphereCenter.x, 0.0f) && Near(sphereCenter.y, 0.0f) &&
            Near(sphereCenter.z, 0.0f),
        "sphere center must map to normalized local origin");
  Check(Near(sphereEdge.x, 1.0f),
        "sphere radius must map to normalized local unit edge");

  const int32_t box = manager.AddBox(
      10.0f, 20.0f, 30.0f, 4.0f, 6.0f, 8.0f, 0.4f, 0.1f);
  Check(box > 0 && manager.SetRotationDegrees(box, 0.0f, 0.0f, 90.0f),
        "valid rotated box must be accepted");
  snapshot = manager.GetFrameSnapshot(18u);
  const auto boxEntry = snapshot.volumes[1];
  const auto localXEdge = TransformPoint(
      boxEntry, dxvk::Vector4(10.0f, 22.0f, 30.0f, 1.0f));
  Check(Near(localXEdge.x, 1.0f) && Near(localXEdge.y, 0.0f),
        "Rz box transform must preserve authored full-size semantics");

  const int32_t cylinder = manager.AddCylinder(
      0.0f, 0.0f, 0.0f, 3.0f, 8.0f, 0.6f, 0.3f);
  Check(cylinder > 0, "valid cylinder must be created");
  snapshot = manager.GetFrameSnapshot(19u);
  Check(Near(snapshot.volumes[2].boundingSphere.w, 5.0f),
        "cylinder conservative bound must include radius and half-height");
  Check(!manager.SetSphereRadius(box, 10.0f) &&
            !manager.SetBoxSize(cylinder, 2.0f, 2.0f, 2.0f),
        "shape-specific setters must reject a mismatched handle");
}

void TestInputCapacityAndLifecycle() {
  auto& manager = dxvk::War3FogVolumeManager::Instance();
  manager.Clear();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  Check(manager.AddSphere(0.0f, 0.0f, 0.0f, nan, 0.5f, 0.2f) == 0,
        "non-finite radius must fail closed");
  Check(manager.AddBox(
            0.0f, 0.0f, 0.0f, 0.5f, 2.0f, 2.0f, 0.5f, 0.2f) == 0,
        "box full size below one world unit must be rejected");

  int32_t ids[dxvk::War3FogVolumeFrameSnapshot::kMaxVolumes] = {};
  for (uint32_t i = 0u;
       i < dxvk::War3FogVolumeFrameSnapshot::kMaxVolumes; ++i) {
    ids[i] = manager.AddSphere(
        float(i) * 10.0f, 0.0f, 0.0f, 2.0f, 0.25f, 0.1f);
    Check(ids[i] > 0, "each in-budget fog volume must be created");
  }
  Check(manager.GetVolumeCount() ==
            dxvk::War3FogVolumeFrameSnapshot::kMaxVolumes,
        "live count must equal the fixed capacity");
  Check(manager.AddSphere(0.0f, 0.0f, 0.0f, 2.0f, 0.25f, 0.1f) == 0,
        "ninth fog volume must be rejected");

  Check(manager.SetActive(ids[0], false),
        "active volume must support temporary disable");
  auto snapshot = manager.GetFrameSnapshot(20u);
  Check(snapshot.count ==
            dxvk::War3FogVolumeFrameSnapshot::kMaxVolumes - 1u,
        "disabled volume must stay live but leave the render snapshot");
  Check(manager.IsAlive(ids[0]),
        "disable must not destroy the managed handle");
  Check(manager.Remove(ids[0]) && !manager.IsAlive(ids[0]),
        "destroyed handle must become stale immediately");

  manager.Clear();
  Check(manager.GetVolumeCount() == 0u && !manager.HasActiveVolumes(),
        "map-session clear must remove all CPU fog state");
  Check(!manager.IsAlive(ids[1]),
        "pre-reset internal ids must not alias the new map session");
}

} // namespace

int main() {
  TestShapesAndTransforms();
  TestInputCapacityAndLifecycle();
  if (g_failures != 0) {
    std::cerr << g_failures << " fog-volume manager test(s) failed\n";
    return 1;
  }
  std::cout << "War3 fog-volume manager tests passed\n";
  return 0;
}
