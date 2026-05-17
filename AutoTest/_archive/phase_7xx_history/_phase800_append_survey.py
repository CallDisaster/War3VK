"""Phase 7.108b: 跑高压地图 30s 拉 shadowCasters append survey + ShadowProjector"""
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
        s = chars.decode('ascii')
        # is_path_blocker?
        is_blocker = s.startswith('YT') and s[2:3] in ('a', 'p', 'f', 'l') and s[3:4] in ('b', 'c')
        marker = " <<< PATH BLOCKER" if is_blocker else ""
        return f"0x{fcc:08X} ({s}){marker}"
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

print("\n=== Shadow append survey (实际入 shadowCasters 的 rawcode) ===")
print(f"  total appends:   {summary.get('shadowAppendTotalCount', 0)}")
print(f"  unique rawcodes: {summary.get('shadowAppendRawcodeUniqueCount', 0)}")
for i in range(16):
    fcc = summary.get(f"shadowAppendRawcode{i}", 0)
    if fcc:
        print(f"    [{i:2d}] {fmt_fcc(fcc)}")

print("\n=== ShadowProjector enter/blocked ===")
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

print("\n=== Path blocker reject (D3D9 mesh draw layer) ===")
keys2 = [
    "semanticSceneRejectedPathBlockerCount",
    "semanticSceneRejectedPathBlockerEarlyBypassCount",
    "semanticSceneSubmittedSkinned",
    "semanticSceneShadowMapDrawnCasters",
    "semanticSceneSubmittedDestructible",
    "semanticSceneSubmittedBuilding",
    "semanticSceneSubmittedRigid",
]
for k in keys2:
    print(f"  {k:55s} {summary.get(k, 'MISSING')}")

stop_war3(pid=pid, graceful_wait_sec=4)
