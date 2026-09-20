#include "../../model/war3_model_resource_cache.h"
#include <cassert>
#include <cstdio>
#include <type_traits>

using namespace dxvk::war3::model;

int main() {
  static_assert(std::is_trivially_copyable_v<ShadowGeosetIdentityView>);
  static_assert(sizeof(ShadowGeosetIdentityView) <= 40);
  ShadowGeosetResourceRecord record;
  const auto empty = ProjectGeosetIdentity(record);
  assert(!empty.geosetPtr && !empty.geosetDataPtr && !empty.modelResourcePtr);
  assert(empty.modelKey == 0 && empty.geosetIndex == kInvalidShadowGeosetIndex);
  record.geosetPtr = reinterpret_cast<void*>(1);
  record.geosetDataPtr = reinterpret_cast<void*>(2);
  record.modelResourcePtr = reinterpret_cast<void*>(3);
  record.modelKey = UINT64_MAX;
  record.geosetIndex = 7;
  record.positions.resize(30000, 1);
  record.indices.resize(10000, 2);
  const auto view = ProjectGeosetIdentity(record);
  auto materialized = record;
  materialized.firstSeenFrame = 9;
  materialized.lastSeenFrame = 99;
  materialized.lastRuntimeRefreshFrame = 100;
  const auto previous = ProjectGeosetIdentity(materialized);
  assert(view.geosetPtr == previous.geosetPtr);
  assert(view.geosetDataPtr == previous.geosetDataPtr);
  assert(view.modelResourcePtr == previous.modelResourcePtr);
  assert(view.modelKey == previous.modelKey && view.geosetIndex == previous.geosetIndex);
  record.positions.clear();
  record.indices.clear();
  record.modelKey = 0;
  assert(view.modelKey == UINT64_MAX && view.geosetIndex == 7);
  std::puts("PASS: owned identity projection preserves five fields without payload ownership");
}
