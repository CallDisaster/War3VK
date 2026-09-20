from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# 2026-09-16 P0：palette 记忆槽位复核计数器（Gap A device 2 个 +
# Gap B shadow-core 3 个）必须完整接线到报告出口。这是实机度量
# "合法对象有替代路径"与"失败后可恢复"两项证明的前提；断线即失败。

DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
CORE = (ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp").read_text(encoding="utf-8")
CORE_H = (ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.h").read_text(encoding="utf-8")
BRIDGE_H = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.h").read_text(encoding="utf-8")
BRIDGE = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp").read_text(encoding="utf-8")
HUB_H = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h").read_text(encoding="utf-8")
HUB = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp").read_text(encoding="utf-8")
CP = (ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp").read_text(encoding="utf-8")
PM = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(encoding="utf-8")

DEVICE_SERVED = "semanticSceneSkinnedPaletteSlotCacheDeviceServedAfterConfirmCount"
DEVICE_REJECTED = "semanticSceneSkinnedPaletteSlotCacheDeviceRejectedStaleCount"
CORE_SERVED = "semanticSceneSkinnedPaletteSlotCacheShadowCoreServedAfterConfirmCount"
CORE_REJECTED = "semanticSceneSkinnedPaletteSlotCacheShadowCoreRejectedStaleCount"
CORE_SNAPSHOT = "semanticSceneSkinnedPaletteSlotCacheShadowCoreProducerSnapshotFallbackCount"
# 2026-09-17 Gap B 补强新增 5 个（全链帧证明 + 四类细分拒绝）。
CORE_FRAME_PROOF = "semanticSceneSkinnedPaletteSlotCacheShadowCoreFrameProofServedCount"
CORE_GROUP_SHORT = "semanticSceneSkinnedPaletteSlotCacheShadowCoreGroupShortRejectedCount"
CORE_BINDING_STALE = "semanticSceneSkinnedPaletteSlotCacheShadowCoreBindingFrameStaleRejectedCount"
CORE_RANGE_STALE = "semanticSceneSkinnedPaletteSlotCacheShadowCoreSlotRangeStaleRejectedCount"
CORE_SNAPSHOT_STALE = "semanticSceneSkinnedPaletteSlotCacheShadowCoreSnapshotFrameStaleRejectedCount"
FIELDS = [DEVICE_SERVED, DEVICE_REJECTED, CORE_SERVED, CORE_REJECTED, CORE_SNAPSHOT,
          CORE_FRAME_PROOF, CORE_GROUP_SHORT, CORE_BINDING_STALE, CORE_RANGE_STALE,
          CORE_SNAPSHOT_STALE]

# ---- 1. 计数器本体与 Query 访问器 ----
assert "g_devicePaletteSlotCacheServedAfterConfirmCount" in DEVICE
assert "g_devicePaletteSlotCacheRejectedStaleCount" in DEVICE
assert "uint64_t QueryDevicePaletteSlotCacheServedAfterConfirmCount()" in DEVICE
assert "uint64_t QueryDevicePaletteSlotCacheRejectedStaleCount()" in DEVICE

for name in ("QueryPaletteSlotCacheServedAfterConfirmCount",
             "QueryPaletteSlotCacheRejectedStaleCount",
             "QueryPaletteSlotCacheProducerSnapshotFallbackCount",
             "QueryPaletteSlotCacheFrameProofServedCount",
             "QueryPaletteSlotCacheGroupShortRejectCount",
             "QueryPaletteSlotCacheBindingFrameStaleRejectCount",
             "QueryPaletteSlotCacheSlotRangeStaleRejectCount",
             "QueryPaletteSlotCacheSnapshotFrameStaleRejectCount"):
    assert f"uint64_t {name}();" in CORE_H, name
    assert f"uint64_t {name}() {{" in CORE, name

# ---- 2. bridge summary 结构体字段 + 填充 ----
for f in FIELDS:
    assert f"uint64_t {f} = 0;" in BRIDGE_H, f
    # 填充允许折行（summary\n    .field），只要求字段名出现在 bridge.cpp。
    assert f in BRIDGE, f
assert "QueryDevicePaletteSlotCacheServedAfterConfirmCount();" in BRIDGE
assert "shadow::QueryPaletteSlotCacheServedAfterConfirmCount()" in BRIDGE
assert "shadow::QueryPaletteSlotCacheRejectedStaleCount()" in BRIDGE
assert "shadow::QueryPaletteSlotCacheProducerSnapshotFallbackCount()" in BRIDGE
assert "shadow::QueryPaletteSlotCacheFrameProofServedCount()" in BRIDGE
assert "shadow::QueryPaletteSlotCacheGroupShortRejectCount()" in BRIDGE
assert "shadow::QueryPaletteSlotCacheBindingFrameStaleRejectCount()" in BRIDGE
assert "shadow::QueryPaletteSlotCacheSlotRangeStaleRejectCount()" in BRIDGE
assert "shadow::QueryPaletteSlotCacheSnapshotFrameStaleRejectCount()" in BRIDGE

# ---- 3. diagnostics hub：字段 + 填充 + JSON ----
for f in FIELDS:
    assert f"uint64_t {f} = 0;" in HUB_H, f
    assert f in HUB, f
    assert f'{{"{f}"' in HUB, f

# ---- 4. control plane JSON ----
for f in FIELDS:
    assert f'{{"{f}"' in CP, f

# ---- 5. perf monitor JSON（shadowRuntimeV2Summary 直读最新 summary）----
# C++ 源码中 JSON 键以转义形式 \"key\" 出现。
for f in FIELDS:
    assert f'\\"{f}\\"' in PM, f
assert "runtimeSummary" in PM

print("palette slot cache counter export static checks passed")
