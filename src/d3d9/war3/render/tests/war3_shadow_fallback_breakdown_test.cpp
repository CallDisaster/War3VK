#include "../war3_shadow_fallback_breakdown.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

struct Stats {
  uint32_t fallbackDrawCount = 0u;
  uint32_t fallbackDrawCountTerrain = 0u;
  uint32_t fallbackDrawCountWorldObject = 0u;
  uint32_t fallbackDrawCountUnitObject = 0u;
};

struct Record {
  dxvk::War3RenderState::StageCategory category =
      dxvk::War3RenderState::StageCategory::Unknown;
  uint8_t objectKind = 0u;
};

Stats scan(const std::vector<Record>& records) {
  Stats stats;
  dxvk::war3::render::War3ResetShadowFallbackBreakdown(
      stats, records.size());
  for (const auto& record : records) {
    dxvk::war3::render::War3AccumulateShadowFallbackClassification(
        stats, record.category, record.objectKind);
  }
  return stats;
}

void assertEqual(const Stats& a, const Stats& b) {
  assert(a.fallbackDrawCount == b.fallbackDrawCount);
  assert(a.fallbackDrawCountTerrain == b.fallbackDrawCountTerrain);
  assert(a.fallbackDrawCountWorldObject == b.fallbackDrawCountWorldObject);
  assert(a.fallbackDrawCountUnitObject == b.fallbackDrawCountUnitObject);
}

} // namespace

int main() {
  using Category = dxvk::War3RenderState::StageCategory;
  using Kind = dxvk::war3::render::ObjectKind;

  std::vector<Record> records;
  Stats incremental;
  const auto append = [&](Category category, Kind kind) {
    records.push_back({category, static_cast<uint8_t>(kind)});
    dxvk::war3::render::War3NoteShadowFallbackAppended(
        incremental, records.size(), records.back().category,
        records.back().objectKind);
    assertEqual(incremental, scan(records));
  };

  append(Category::Terrain, Kind::Unknown);
  append(Category::WorldObject, Kind::Unit); // Counts in both independent bins.
  append(Category::Effect, Kind::Effect);    // Effect belongs to world objects.
  append(Category::Terrain, Kind::Unit);     // Terrain and unit also overlap.
  append(Category::WorldObject, Kind::Building);
  append(Category::Unknown, Kind::Unknown);
  append(static_cast<Category>(0xffu), static_cast<Kind>(0xffu));
  append(Category::Terrain, Kind::Unknown);  // Duplicate records still count.

  assert(incremental.fallbackDrawCount == 8u);
  assert(incremental.fallbackDrawCountTerrain == 3u);
  assert(incremental.fallbackDrawCountWorldObject == 3u);
  assert(incremental.fallbackDrawCountUnitObject == 2u);

  // A prune is the only live-vector mutation that needs a full rebuild.
  records.erase(records.begin() + 1u, records.begin() + 4u);
  incremental = scan(records);
  assert(incremental.fallbackDrawCount == 5u);
  assert(incremental.fallbackDrawCountTerrain == 2u);
  assert(incremental.fallbackDrawCountWorldObject == 1u);
  assert(incremental.fallbackDrawCountUnitObject == 0u);

  records.clear();
  incremental = scan(records);
  assertEqual(incremental, Stats{});

  // The live total is taken from the post-append vector size, not inferred
  // from possibly stale diagnostics.
  incremental.fallbackDrawCount = 999u;
  dxvk::war3::render::War3NoteShadowFallbackAppended(
      incremental, 1u, Category::Unknown,
      static_cast<uint8_t>(Kind::Unknown));
  assert(incremental.fallbackDrawCount == 1u);

  return 0;
}
