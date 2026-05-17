"""Phase 7.105: IceCrown 30s perf with full record，看 semantic/shadow 内部哪个 scope 最贵。"""
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
import os

map_path = r"E:\Work\War3\Maps\(12)IceCrown.w3m"

t_start = time.time()
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
    record_after_game_started=False,
    auto_perf_export_sec=40,
    env_overrides_json=json.dumps({
        # 强制 perf 记录器开启
        "DXVK_WAR3_PERF_RECORD_ON_START": "1",
        "DXVK_WAR3_PERF_AUTO_EXPORT_SEC": "40",
        "DXVK_WAR3_PERF_HISTORY_FRAMES": "10000",
    }),
)
pid = launch.get("pid", 0)
print(f"launch ok={launch.get('ok')} pid={pid}")

ready = wait_for_game_ready(timeout_sec=60, pid=pid)
print(f"game ready ok={ready.get('ok')} elapsed={ready.get('elapsedSec')}s")

print("waiting 35s for perf to capture full opening...")
time.sleep(35)

# 通过 control-plane 强制导出报告
exp_resp = _control_plane_request(
    pid=pid, command="invoke_test_command",
    payload={"command": "perf.export", "payload": {}},
    timeout_sec=10.0,
)
print(f"perf export = {exp_resp}")

# 拉 control-plane summary
summary_resp = _control_plane_request(
    pid=pid, command="get_shadow_runtime_summary", timeout_sec=10.0,
)
summary = summary_resp.get("result", summary_resp.get("summary", summary_resp))

print("\n=== Key counters during IceCrown opening ===")
for k in [
    'semanticManifestCopyEnterCount',
    'semanticManifestCopySkipStableCount',
    'semanticSceneSubmittedSkinned',
    'semanticSceneShadowMapDrawnCasters',
    'widgetIdentityEnterCount',
    'widgetIdentityCacheSize',
    'writeMaskRegionEnterCount',
]:
    print(f"  {k:55s} {summary.get(k, 'MISSING')}")

stop_war3(pid=pid, graceful_wait_sec=4, avoid_foreground_switch=True)
