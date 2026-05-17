"""Phase 7.105 long IceCrown test, 60s of frame counter polling."""
import json
import sys
import time
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
t_launched = time.time()
last_frame = 0
samples = []
while time.time() - t_launched < 60:
    time.sleep(0.5)
    rs = _control_plane_request(pid=pid, command="get_runtime_status",
                                timeout_sec=2.0)
    elapsed = round(time.time() - t_launched, 1)
    if rs.get("transportOk"):
        frame = rs.get("result", {}).get("frameIndex", last_frame)
        delta = frame - last_frame
        samples.append({"t": elapsed, "frame": frame, "df": delta})
        if delta == 0 or elapsed % 2 < 0.5:
            print(f"t+{elapsed:>5.1f}s frame={frame:>6} df={delta:>4}")
        last_frame = frame

stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
total = time.time() - t_launched
print(f"\nTotal: {total:.1f}s, last frame={last_frame}, avg fps={last_frame/total:.1f}")
