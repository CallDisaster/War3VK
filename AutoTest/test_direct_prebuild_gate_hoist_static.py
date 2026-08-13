from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

populate = DEVICE.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
)[1].split("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1)[0]

gate = populate.index("const bool drawTimePrebuildBypassEnabled")
for token in (
    "War3DrawTimeVBCacheRuntime()",
    "War3SemanticDrawTimePrebuildBypassRuntime()",
    "War3SemanticDrawTimeFastAppendRuntime()",
):
    assert token in populate[gate : gate + 300]

lambda_start = populate.index("auto tryBuildDrawTimePrebuildBypassEligible", gate)
lambda_end = populate.index(
    "War3PopulateReadableRegionCache currentDrawGroupRangeCache", lambda_start
)
body = populate[lambda_start:lambda_end]
assert "if (!drawTimePrebuildBypassEnabled)" in body
assert "War3DrawTimeVBCacheRuntime()" not in body
assert "War3SemanticDrawTimePrebuildBypassRuntime()" not in body
assert "War3SemanticDrawTimeFastAppendRuntime()" not in body
loop = populate.index("for (size_t buildIndex = 0u;", lambda_end)
assert gate < lambda_start < lambda_end < loop

print("direct prebuild gate hoist static checks passed")
