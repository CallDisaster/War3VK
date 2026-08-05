"""Phase 7.99c: 跑高压地图 30s 用 control plane 拉 path blocker reject 计数"""
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

# 关键 path blocker counter
keys = [k for k in (summary.keys() if isinstance(summary, dict) else []) if 'PathBlocker' in k or 'pathBlocker' in k]
print("=== path blocker counters ===")
for k in keys:
    print(f"  {k:65s} {summary.get(k)}")

print("\n=== widget identity counters ===")
widget_keys = [k for k in (summary.keys() if isinstance(summary, dict) else []) if 'widget' in k.lower()]
for k in widget_keys:
    print(f"  {k:55s} {summary.get(k)}")

print("\n=== shadow scene counters ===")
shadow_keys = [
    'semanticSceneSubmittedSkinned',
    'semanticSceneSubmittedRigid',
    'semanticSceneShadowMapDrawnCasters',
    'semanticSceneShadowCastersCount',
    'semanticSceneRejectedPathBlockerCount',
    'semanticSceneRejectedPathBlockerEarlyBypassCount',
    'semanticSceneRejectedPathBlockerEligibilityGateCount',
    'semanticSceneRejectedPathBlockerAppendEntryCount',
    'semanticSceneRejectedPathBlockerAppendEntryByJHandleCount',
    'semanticSceneRejectedPathBlockerAppendVbBlendCount',
    'semanticSceneRejectedPathBlockerFastAppendCount',
    'semanticSceneRejectedPathBlockerDirectGroupedCount',
    'semanticSceneRejectedPathBlockerProducerCount',
    'semanticSceneRejectedPathBlockerStaticSupplementCount',
    'semanticSceneRejectedPathBlockerLegacyCaptureCount',
    'semanticManifestCopyEnterCount',
    'semanticManifestCopySkipStableCount',
    'writeMaskRegionEnterCount',
    'writeMaskRegionRejectedIdx3Count',
    'writeMaskRegionPassFogCount',
    'writeMaskRegionPassLosCount',
    'writeMaskRegionPassPathCount',
    'writeMaskRegionPassOtherCount',
]
for k in shadow_keys:
    print(f"  {k:60s} {summary.get(k, 'MISSING')}")

stop_war3(pid=pid, graceful_wait_sec=4)
