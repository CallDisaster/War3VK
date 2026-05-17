"""Phase 7.105 binary-search bisect: find which module(s) cause IceCrown opening freeze.

测同一张 12 人图 (IceCrown)，禁用不同模块组合，看哪个组合让 first-30s frame counter
不再卡住（即 frames > 1000）。
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


def measure(map_path: str, label: str, disable_modules: str = ""):
    print(f"\n========== {label}: disable=[{disable_modules}] ==========")
    launch = launch_war3_test(
        war3_dir=str(DEFAULT_WAR3_DIR),
        map_path=map_path,
        use_isolated_desktop=True,
        deploy_d3d9_before_launch=False,
        disable_modules=disable_modules,
        record_after_game_started=False,
        auto_perf_export_sec=60,
    )
    pid = launch.get("pid", 0)
    if not launch.get("ok"):
        print(f"  launch FAILED: {launch}")
        return None
    t_launched = time.time()

    last_frame = 0
    last_t = t_launched
    longest_stall_sec = 0.0
    stall_start = None
    stall_count_300ms = 0

    while time.time() - t_launched < 30:
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

        # 检测 stall
        if delta_f == 0 and delta_t > 0.3:
            if stall_start is None:
                stall_start = last_t
        else:
            if stall_start is not None:
                stall_dur = last_t - stall_start
                if stall_dur > longest_stall_sec:
                    longest_stall_sec = stall_dur
                if stall_dur > 0.3:
                    stall_count_300ms += 1
                stall_start = None

        last_frame = frame
        last_t = t
        time.sleep(0.2)

    final_frame = last_frame
    elapsed = time.time() - t_launched
    print(f"  total: {elapsed:.1f}s, final_frame={final_frame}, avg_fps={final_frame/elapsed:.1f}")
    print(f"  longest stall: {longest_stall_sec:.2f}s, total stalls (>300ms)={stall_count_300ms}")

    stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
    return {
        "label": label,
        "disable": disable_modules,
        "finalFrame": final_frame,
        "elapsed": elapsed,
        "avgFps": final_frame / elapsed,
        "longestStallSec": longest_stall_sec,
        "stallCount300ms": stall_count_300ms,
    }


map_path = r"E:\Work\War3\Maps\(12)IceCrown.w3m"

# A/B group: 渐进禁用模块组
configs = [
    ("default_full", ""),
    ("disable_semantic.data", "semantic.data"),
    ("disable_shadow.all", "shadow.capture,shadow.map,shadow.receiver,shadow.taa"),
    ("disable_shadow+semantic", "semantic.data,shadow.capture,shadow.map,shadow.receiver,shadow.taa"),
    ("disable_render.queue", "render.queue"),
    ("disable_postfx_aa_ssao", "postfx,ssao,aa"),
    ("disable_hook.render", "hook.render"),
    ("disable_diag", "diag"),
]

results = []
for label, dis in configs:
    r = measure(map_path, label, dis)
    if r:
        results.append(r)
    time.sleep(3)

print("\n\n========= SUMMARY =========")
print(f"  {'config':40s} {'finalFrame':>10s} {'avgFps':>8s} {'longestStall':>13s} {'stalls':>7s}")
for r in results:
    print(f"  {r['label']:40s} {r['finalFrame']:>10}  {r['avgFps']:>7.1f} {r['longestStallSec']:>12.2f}s {r['stallCount300ms']:>7}")
