"""Phase 7.102 / Task 4: (4)MysticIsles 对战图开局卡顿调查。

用户报告：进入对战图开局会卡 4-5 秒。本脚本：
  1) 启动 War3 进 (4)MysticIsles.w3m
  2) 测量 launch -> game_ready 的真实秒数
  3) 进图 5 秒后再做 25 秒 perf record，看开局之后 FPS 稳定值
  4) 输出关键 counter（widget identity / path blocker / writeMaskRegion / manifestCopy）
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
    DEFAULT_WAR3_DIR,
)

map_path = r"E:\Work\War3\Maps\(4)MysticIsles.w3m"

print(f"[startup] launching {map_path}")
launch_t0 = time.time()
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
)
pid = launch.get("pid", 0)
launch_elapsed = time.time() - launch_t0
print(f"  ok={launch.get('ok')} pid={pid} launch_elapsed={launch_elapsed:.2f}s")
if not launch.get("ok"):
    print(json.dumps(launch, indent=2, ensure_ascii=False))
    sys.exit(1)

ready_t0 = time.time()
ready = wait_for_game_ready(timeout_sec=180, pid=pid)
ready_elapsed = time.time() - ready_t0
print(f"[startup] game ready ok={ready.get('ok')} ready_elapsed={ready_elapsed:.2f}s")
print(f"  mode={ready.get('mode')} reportedElapsed={ready.get('elapsedSec')}")

# 等开局过 5 秒，然后采 25 秒 perf
print("[startup] waiting 5s for opening to settle...")
time.sleep(5)

print("[startup] starting 25s perf record...")
start_resp = _control_plane_request(
    pid=pid, command="start_war3_perf_record",
    payload={"resetTracker": True}, timeout_sec=10.0,
)
print(f"  start_perf={start_resp.get('ok')}")
time.sleep(25)
stop_resp = _control_plane_request(
    pid=pid, command="stop_war3_perf_record",
    payload={"writeReport": True}, timeout_sec=10.0,
)
print(f"  stop_perf={stop_resp.get('ok')}")
report_path = (stop_resp.get("result") or {}).get("reportPath")
print(f"  reportPath={report_path}")

print("[startup] querying shadow_runtime_summary...")
summary_resp = _control_plane_request(
    pid=pid, command="get_shadow_runtime_summary", timeout_sec=10.0,
)
summary = summary_resp.get("result", summary_resp.get("summary", summary_resp))

print("\n=== widget identity counters ===")
widget_keys = [k for k in (summary.keys() if isinstance(summary, dict) else []) if 'widget' in k.lower()]
for k in widget_keys:
    print(f"  {k:55s} {summary.get(k)}")

print("\n=== path blocker reject counters ===")
pb_keys = [k for k in (summary.keys() if isinstance(summary, dict) else []) if 'PathBlocker' in k or 'pathBlocker' in k]
for k in sorted(pb_keys):
    print(f"  {k:65s} {summary.get(k)}")

print("\n=== writeMaskRegion counters (diagnostic) ===")
wmr_keys = [k for k in (summary.keys() if isinstance(summary, dict) else []) if 'writeMaskRegion' in k]
for k in sorted(wmr_keys):
    print(f"  {k:50s} {summary.get(k)}")

print("\n=== manifest copy / scene counters ===")
scene_keys = [
    'semanticManifestCopyEnterCount',
    'semanticManifestCopySkipStableCount',
    'semanticSceneSubmittedSkinned',
    'semanticSceneShadowMapDrawnCasters',
]
for k in scene_keys:
    print(f"  {k:55s} {summary.get(k, 'MISSING')}")

print("\n=== summary ===")
print(f"  launch_elapsed={launch_elapsed:.2f}s")
print(f"  game_ready_elapsed={ready_elapsed:.2f}s (control-plane reported {ready.get('elapsedSec')}s)")
print(f"  perf_report={report_path}")

stop_war3(pid=pid, graceful_wait_sec=4)
