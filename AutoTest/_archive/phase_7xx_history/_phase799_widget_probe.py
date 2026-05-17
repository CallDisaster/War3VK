"""Phase 7.99: 用 control plane 拉 widget identity stats（mini probe）

启动高压地图 → 等进图 → 等 30s → 拉一次 shadow_runtime_summary → 关游戏
"""
import json
import sys
import time
sys.path.insert(0, '.')
from war3_autotest_mcp import (
    launch_war3_test,
    wait_for_game_ready,
    _control_plane_request,
    stop_war3,
    STATE,
    DEFAULT_WAR3_DIR,
)

map_path = r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x"

print(f"[probe] launching {map_path}")
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
)
print(json.dumps({"ok": launch.get("ok"), "pid": launch.get("pid")}, ensure_ascii=False))

if not launch.get("ok"):
    sys.exit(1)

pid = launch.get("pid", 0)

print("[probe] waiting for game ready...")
ready = wait_for_game_ready(timeout_sec=120, pid=pid)
print(json.dumps({"ok": ready.get("ok"), "stage": ready.get("stage"),
                  "elapsedSec": ready.get("elapsedSec")}, ensure_ascii=False))

if not ready.get("ok"):
    stop_war3(pid=pid, graceful_wait_sec=4)
    sys.exit(1)

print("[probe] sleeping 60s for stable scene + widget lifecycle...")
time.sleep(60)

print("[probe] querying shadow_runtime_summary...")
summary_resp = _control_plane_request(
    pid=pid,
    command="get_shadow_runtime_summary",
    timeout_sec=10.0,
)

stats_keys = [
    'widgetIdentityEnterCount',
    'widgetIdentityMagicMatchedCount',
    'widgetIdentityMagicMismatchCount',
    'widgetIdentityCacheInsertCount',
    'widgetIdentityCacheUpdateCount',
    'widgetIdentityHandleResolvedCount',
    'widgetIdentityHandleMissingCount',
    'widgetIdentityCacheSize',
    'widgetIdentityInstallAttempted',
    'widgetIdentityInstallSucceeded',
    'widgetIdentityInstallFailedAddrNull',
    'widgetIdentityInstallFailedEnvDisabled',
    'widgetIdentityInstallFailedMinHook',
    # 同时拉 ManifestCopy 数据继续诊断 perf
    'semanticManifestCopyEnterCount',
    'semanticManifestCopyTotalScanned',
    'semanticManifestCopyMaxScanned',
    'semanticManifestCopyTotalChronoNs',
    'semanticManifestCopyMaxChronoNs',
]

summary = summary_resp.get("summary", summary_resp)
print("=== widget identity / manifest copy stats ===")
# 调试: 看实际 summary keys 长啥样
print("summary type:", type(summary).__name__)
if isinstance(summary, dict):
    print(f"keys count={len(summary)}, sample first 5: {list(summary.keys())[:5]}")
    # 尝试在 result 里找
    if "result" in summary:
        summary = summary["result"]
        print(f"unwrapped to result.keys count={len(summary)}, sample first 5: {list(summary.keys())[:5]}")
for k in stats_keys:
    print(f"  {k:50s} {summary.get(k, 'MISSING')}")

print("[probe] stopping war3...")
stop_war3(pid=pid, graceful_wait_sec=4)
