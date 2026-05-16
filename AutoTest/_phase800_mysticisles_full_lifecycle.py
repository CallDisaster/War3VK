"""Phase 7.102 / Task 4：MysticIsles 全周期开局诊断 — 在 game-ready 信号之前
就启动 perf record，捕获用户报告的"开局 4-5 秒卡顿"。"""
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

map_path = r"E:\Work\War3\Maps\(4)MysticIsles.w3m"

print(f"[fullcycle] launching {map_path}")
launch_t0 = time.time()
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
)
pid = launch.get("pid", 0)
launch_elapsed = time.time() - launch_t0
print(f"  ok={launch.get('ok')} pid={pid}")
if not launch.get("ok"):
    sys.exit(1)

# 进图后立刻开 perf。先轮询 control-plane 直到 ready。
print("[fullcycle] polling for ready, perf will start immediately on first response")
ready_t0 = time.time()
perf_started = False
ready = None
while time.time() - ready_t0 < 60:
    if not perf_started:
        # 立即开 perf（不管 ready 没 ready，d3d9 module 加载就能开）
        resp = _control_plane_request(
            pid=pid, command="start_war3_perf_record",
            payload={"resetTracker": True}, timeout_sec=2.0,
        )
        if resp.get("transportOk"):
            perf_started = True
            print(f"  perf record started @ {time.time()-launch_t0:.2f}s after launch ok={resp.get('ok')}")

    ready = wait_for_game_ready(timeout_sec=3, pid=pid)
    if ready.get("ok"):
        break
    time.sleep(0.5)
ready_elapsed = time.time() - ready_t0
print(f"[fullcycle] ready={ready.get('ok')} ready_elapsed={ready_elapsed:.2f}s")

# 等 25 秒采集（这样总样本 = 启动期 + 25s稳定期）
print("[fullcycle] sampling 25s of post-ready perf...")
time.sleep(25)

stop_resp = _control_plane_request(
    pid=pid, command="stop_war3_perf_record",
    payload={"writeReport": True}, timeout_sec=10.0,
)
report_path = (stop_resp.get("result") or {}).get("reportPath")
print(f"[fullcycle] perf stopped, reportPath={report_path}")

# 拉一次 summary 看 stats
summary_resp = _control_plane_request(
    pid=pid, command="get_shadow_runtime_summary", timeout_sec=10.0,
)
summary = summary_resp.get("result", summary_resp.get("summary", summary_resp))

print("\n=== key counters ===")
for k in [
    'widgetIdentityEnterCount', 'widgetIdentityCacheSize', 'widgetIdentityMagicMatchedCount',
    'semanticManifestCopyEnterCount', 'semanticManifestCopySkipStableCount',
    'semanticSceneSubmittedSkinned', 'semanticSceneShadowMapDrawnCasters',
    'writeMaskRegionEnterCount', 'writeMaskRegionPassFogCount',
    'writeMaskRegionPassLosCount', 'writeMaskRegionPassPathCount', 'writeMaskRegionPassOtherCount',
    'semanticSceneRejectedPathBlockerCount',
]:
    print(f"  {k:55s} {summary.get(k, 'MISSING')}")

stop_war3(pid=pid, graceful_wait_sec=4)
print(f"\n[fullcycle] perf report: {report_path}")
