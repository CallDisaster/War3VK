"""Phase 7.105 second pass: per-second FPS during 12-player IceCrown opening.
Probe at 200ms granularity for 30s post-ready, log FPS / frame deltas to find the actual stall window.
"""
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


def measure(map_path: str, label: str, profile: str = ""):
    print(f"\n========== {label} (profile={profile or 'default'}) ==========")
    extra = {}
    if profile:
        extra["profile"] = profile
    launch = launch_war3_test(
        war3_dir=str(DEFAULT_WAR3_DIR),
        map_path=map_path,
        use_isolated_desktop=True,
        deploy_d3d9_before_launch=False,
        record_after_game_started=False,
        auto_perf_export_sec=60,
        **extra,
    )
    pid = launch.get("pid", 0)
    if not launch.get("ok"):
        return None
    t_launched = time.time()

    samples = []
    last_frame = 0
    last_t = t_launched
    poll_t0 = time.time()

    while time.time() - t_launched < 35:
        t = time.time()
        rs = _control_plane_request(pid=pid, command="get_runtime_status",
                                    timeout_sec=2.0)
        if not rs.get("transportOk"):
            time.sleep(0.05)
            continue
        result = rs.get("result", {})
        frame = result.get("frameIndex", 0)
        delta_t = t - last_t
        delta_f = frame - last_frame
        if delta_t > 0:
            samples.append({
                "t": round(t - t_launched, 2),
                "frame": frame,
                "dt": round(delta_t * 1000.0, 1),  # ms
                "df": delta_f,
                "fps": round(delta_f / max(0.001, delta_t), 1),
            })
        last_frame = frame
        last_t = t
        time.sleep(0.2)

    print(f"  Final: {samples[-1]['frame']} frames in ~{samples[-1]['t']:.1f}s")
    print(f"  Per-200ms FPS samples (showing only when fps<60 or first 30 samples):")
    shown = 0
    for s in samples:
        if shown < 30 or s["fps"] < 60:
            print(f"    t+{s['t']:>5.1f}s  frame={s['frame']:>6}  dt={s['dt']:>6.1f}ms  df={s['df']:>3}  fps={s['fps']:>6.1f}")
            shown += 1

    stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
    return samples


maps = [
    (r"E:\Work\War3\Maps\(12)IceCrown.w3m", "IceCrown_12p"),
    (r"E:\Work\War3\Maps\(4)MysticIsles.w3m", "MysticIsles_4p"),
]

for mp, lbl in maps:
    measure(mp, lbl)
    time.sleep(3)
