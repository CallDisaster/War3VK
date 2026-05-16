"""Phase 7.105: 测量 launch -> first-frame-rendered 时间。
对比 default profile vs dxvk_only profile，找开局卡顿的真正瓶颈。
"""
import json
import sys
import time
import subprocess
sys.path.insert(0, '.')
from war3_autotest_mcp import (
    launch_war3_test,
    wait_for_game_ready,
    _control_plane_request,
    stop_war3,
    DEFAULT_WAR3_DIR,
)


def measure_launch_window(map_path: str, profile: str = ""):
    print(f"\n=== Map: {map_path} | profile={profile or 'default'} ===")
    t_start = time.time()

    extra = {}
    if profile:
        extra["profile"] = profile

    launch = launch_war3_test(
        war3_dir=str(DEFAULT_WAR3_DIR),
        map_path=map_path,
        use_isolated_desktop=True,
        deploy_d3d9_before_launch=False,
        **extra,
    )
    pid = launch.get("pid", 0)
    t_launched = time.time()
    print(f"  ok={launch.get('ok')} pid={pid} launch_proc_t={t_launched - t_start:.2f}s")
    if not launch.get("ok"):
        return {"ok": False, "stage": "launch"}

    # 不停轮询 control-plane 看 game ready 时间
    poll_start = time.time()
    ready = wait_for_game_ready(timeout_sec=120, pid=pid)
    poll_end = time.time()
    cp_ready_elapsed = float(ready.get("elapsedSec", 0))
    poll_total = poll_end - t_launched

    print(f"  ready={ready.get('ok')} cp_reported={cp_ready_elapsed:.2f}s poll_observed={poll_total:.2f}s")

    # 让游戏稳定 5s
    time.sleep(5)

    # 拉 runtime status 看 d3d9 module / shadow runtime 状态
    rs = _control_plane_request(pid=pid, command="get_runtime_status",
                                timeout_sec=5.0)
    runtime = rs.get("result", {})

    stop_war3(pid=pid, graceful_wait_sec=4, avoid_foreground_switch=True)

    return {
        "ok": True,
        "map": map_path,
        "profile": profile or "default",
        "launchProcSec": round(t_launched - t_start, 3),
        "cpReadyElapsedSec": round(cp_ready_elapsed, 3),
        "pollObservedSec": round(poll_total, 3),
        "totalElapsedSec": round(poll_end - t_start, 3),
        "runtime": runtime,
    }


# 4 测试场景
maps = [
    r"E:\Work\War3\Maps\(4)MysticIsles.w3m",
    r"E:\Work\War3\Maps\(12)IceCrown.w3m",
]
results = []

for map_path in maps:
    # default profile（含我们所有渲染增强）
    r1 = measure_launch_window(map_path, profile="")
    results.append(r1)
    time.sleep(3)
    # dxvk_only profile（关掉我们的所有渲染增强，只保留基础 DXVK）
    r2 = measure_launch_window(map_path, profile="dxvk_only")
    results.append(r2)
    time.sleep(3)

print("\n\n========= SUMMARY =========")
for r in results:
    print(f"  {r['map']:50s} profile={r['profile']:12s} cp_ready={r.get('cpReadyElapsedSec'):>7.2f}s poll={r.get('pollObservedSec'):>7.2f}s")
