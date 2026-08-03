#include "../war3_persistent_gpu_package_store.h"

#include <type_traits>

namespace gpu_skin = dxvk::war3::gpu_skin;

static_assert(!std::is_copy_constructible_v<
    gpu_skin::War3PersistentGpuPackageStore>);
static_assert(!std::is_copy_assignable_v<
    gpu_skin::War3PersistentGpuPackageStore>);
static_assert(std::is_constructible_v<
    gpu_skin::War3GpuSkinResources,
    dxvk::Rc<dxvk::DxvkDevice>,
    const gpu_skin::GpuSkinResourceBudgets&>);

int main() {
  const gpu_skin::GpuSkinRuntimeConfig defaults = {};
  if (defaults.mode != gpu_skin::GpuSkinMode::Disabled)
    return 1;
  if (!gpu_skin::War3PersistentGpuPackageStore::kD3D9ObserveOwnerEnabled)
    return 2;
  if (gpu_skin::War3PersistentGpuPackageStore::kD3D9SharedOwnerEnabled)
    return 6;
  if (gpu_skin::War3PersistentGpuPackageStore::kRequiresNativeBridge)
    return 3;
  if (!gpu_skin::War3PersistentGpuPackageStore::
          kProducerRetirementSurvivesEpochClear)
    return 4;
  if (gpu_skin::War3PersistentGpuPackageStore::kCrossEpochRetirementSafe)
    return 5;
  return 0;
}
