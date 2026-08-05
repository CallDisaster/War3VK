"""Phase 7.108: 跑高压地图 30s 用 control plane 拉 ShadowProjector 真实数据"""
import json
import sys
import time
sys.path.insert(0, '.')
from war3_autotest_mcp import (
    launch_war3_test,
    wait_for_game_ready,
    _control_plane_request,
    stop_war3,
    DEFAULT_WAR3_DIR,
)

def fmt_fcc(fcc):
    if fcc == 0:
        return "(0)"
    chars = bytes([(fcc >> 24) & 0xFF, (fcc >> 16) & 0xFF, (fcc >> 8) & 0xFF, fcc & 0xFF])
    try:
        return f"0x{fcc:08X} ({chars.decode('ascii')})"
    except Exception:
        return f"0x{fcc:08X}"

map_path = r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x"
print(f"[probe] launching {map_path}")
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
)
pid = launch.get("pid", 0)
print(f"  ok={launch.get('ok')} pid={pid}")
if not launch.get("ok"):
    sys.exit(1)

ready = wait_for_game_ready(timeout_sec=120, pid=pid)
print(f"[probe] game ready ok={ready.get('ok')}")
time.sleep(30)

print("[probe] querying shadow_runtime_summary...")
summary_resp = _control_plane_request(
    pid=pid, command="get_shadow_runtime_summary", timeout_sec=10.0,
)
summary = summary_resp.get("result", summary_resp.get("summary", summary_resp))
if not isinstance(summary, dict):
    print(f"unexpected summary: {summary}")
    stop_war3(pid=pid, graceful_wait_sec=4)
    sys.exit(1)

print("\n=== ShadowProjector counters ===")
keys = [
    "projectorAddFromObjectEnterCount",
    "projectorAddFromObjectBlockedCount",
    "projectorAddFromObjectFourCCExtractedCount",
    "projectorAddFromObjectFourCCMissCount",
    "projectorAddFromObjectBlockedFourCCCount",
    "projectorAddSimpleEnterCount",
    "projectorAddSimpleBlockedCount",
]
for k in keys:
    print(f"  {k:55s} {summary.get(k, 'MISSING')}")

print("\n=== ShadowProjector observed fourcc samples ===")
for i in range(8):
    fcc = summary.get(f"projectorObservedFourCCSample{i}", 0)
    if fcc:
        print(f"  observed[{i}]  {fmt_fcc(fcc)}")

print("\n=== ShadowProjector blocked fourcc samples ===")
for i in range(8):
    fcc = summary.get(f"projectorBlockedFourCCSample{i}", 0)
    if fcc:
        print(f"  blocked[{i}]   {fmt_fcc(fcc)}")

print("\n=== Path blocker reject (D3D9 mesh draw layer) ===")
keys2 = [
    "semanticSceneRejectedPathBlockerCount",
    "semanticSceneRejectedPathBlockerEarlyBypassCount",
    "semanticSceneRejectedPathBlockerAppendVbBlendCount",
    "semanticSceneRejectedPathBlockerFastAppendCount",
    "semanticSceneRejectedPathBlockerProducerCount",
    "semanticSceneRejectedPathBlockerDirectGroupedCount",
    "semanticSceneSubmittedSkinned",
    "semanticSceneShadowMapDrawnCasters",
]
for k in keys2:
    print(f"  {k:55s} {summary.get(k, 'MISSING')}")

print("\n=== WriteMaskRegion (independent grid mask system) ===")
keys3 = [
    "writeMaskRegionEnterCount",
    "writeMaskRegionPassFogCount",
    "writeMaskRegionPassLosCount",
    "writeMaskRegionPassPathCount",
    "writeMaskRegionPassOtherCount",
]
for k in keys3:
    print(f"  {k:55s} {summary.get(k, 'MISSING')}")

stop_war3(pid=pid, graceful_wait_sec=4)
