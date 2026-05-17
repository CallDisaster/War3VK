"""Phase 7.116: 验证 DispatchToShape hook 是否拦截建筑/装饰物原生静态阴影"""
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

print("\n[probe] querying shadow_runtime_summary...")
summary_resp = _control_plane_request(
    pid=pid, command="get_shadow_runtime_summary", timeout_sec=10.0,
)
summary = summary_resp.get("result", summary_resp.get("summary", summary_resp))
if not isinstance(summary, dict):
    print(f"unexpected summary: {type(summary)}")
    stop_war3(pid=pid, graceful_wait_sec=4)
    sys.exit(1)

print("\n=== DispatchToShape (Phase 7.116 建筑/装饰物 shadow 拦截) ===")
keys = [
    "dispatchToShapeEnterCount",
    "dispatchToShapeRejectedCount",
    "dispatchToShapeFromRebuildMaskCount",
    "dispatchToShapeFromShadowSetupCount",
    "dispatchToShapeFromOtherCallerCount",
]
for k in keys:
    v = summary.get(k, "MISSING")
    print(f"  {k:55s} {v}")

print("\n=== sanity: shadow scene + path blocker still working ===")
keys2 = [
    "semanticSceneSubmittedSkinned",
    "semanticSceneSubmittedDestructible",
    "semanticSceneSubmittedBuilding",
    "semanticSceneShadowMapDrawnCasters",
    "semanticSceneRejectedPathBlockerEarlyBypassCount",
    "writeMaskRegionEnterCount",
    "writeMaskRegionPassFogCount",
    "writeMaskRegionPassLosCount",
    "writeMaskRegionPassPathCount",
    "writeMaskRegionPassOtherCount",
]
for k in keys2:
    v = summary.get(k, "MISSING")
    print(f"  {k:55s} {v}")

stop_war3(pid=pid, graceful_wait_sec=4)
