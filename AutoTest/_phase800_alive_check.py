"""Phase 7.105 是否 War3 进程还活着+CPU 使用情况。"""
import json
import sys
import time
import subprocess
sys.path.insert(0, '.')
from war3_autotest_mcp import (
    launch_war3_test,
    _control_plane_request,
    stop_war3,
    DEFAULT_WAR3_DIR,
)

map_path = r"E:\Work\War3\Maps\(12)IceCrown.w3m"
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
)
pid = launch.get("pid", 0)
print(f"launch ok={launch.get('ok')} pid={pid}")

t0 = time.time()
while time.time() - t0 < 30:
    time.sleep(2)
    elapsed = time.time() - t0
    rs = _control_plane_request(pid=pid, command="get_runtime_status",
                                timeout_sec=2.0)
    if rs.get("transportOk"):
        result = rs.get("result", {})
        frame = result.get("frameIndex", 0)
        runtime = result.get("runtime", {})
        render = result.get("render", {})
        # 同时通过 wmic 查 CPU usage
        cpu_pct = ""
        try:
            r = subprocess.run(["wmic", "process", "where", f"ProcessId={pid}",
                               "get", "Caption,WorkingSetSize"],
                             capture_output=True, text=True, timeout=3)
            cpu_pct = r.stdout.strip().split('\n')[-1] if r.returncode == 0 else "n/a"
        except Exception as e:
            cpu_pct = f"err({e})"
        print(f"t+{elapsed:>5.1f}s  frame={frame:>6}  inGame={render.get('isInGame')}  ready={render.get('inGameRenderReady')}  gameStarted={runtime.get('gameStarted')}  ws={cpu_pct[:60]}")
    else:
        print(f"t+{elapsed:>5.1f}s  control plane unresponsive")

stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
