"""Phase 7.105: 用 ETW / Process Cycles 测开局期间的渲染节奏。
对比 (4)MysticIsles vs (12)IceCrown：
  - 测 first 30s 内 Present 次数（间接反映渲染卡顿）
  - 同时拉 perf report 看 maxFrameTimeMs 和 inter-frame gap

不同于 perf monitor，本脚本利用 d3d9.dll 自己的 frame counter（control-plane 暴露的 frameIndex）
通过定时 polling 测 first-frame-after-launch 时间。
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


def measure(map_path: str, label: str):
    print(f"\n========== {label}: {map_path} ==========")
    t_start = time.time()
    launch = launch_war3_test(
        war3_dir=str(DEFAULT_WAR3_DIR),
        map_path=map_path,
        use_isolated_desktop=True,
        deploy_d3d9_before_launch=False,
        record_after_game_started=False,
        auto_perf_export_sec=60,
    )
    pid = launch.get("pid", 0)
    if not launch.get("ok"):
        return None
    t_launched = time.time()
    print(f"  launch: pid={pid} t={t_launched-t_start:.2f}s")

    # poll frameIndex 每 100ms，直到 ready 后再持续采 30s
    samples = []
    first_frame_seen_at = None
    ready_at = None
    poll_t0 = time.time()
    last_frame_idx = 0
    last_t = poll_t0
    while time.time() - t_start < 60:
        t = time.time()
        rs = _control_plane_request(pid=pid, command="get_runtime_status",
                                    timeout_sec=2.0)
        if not rs.get("transportOk"):
            time.sleep(0.05)
            continue
        result = rs.get("result", {})
        frame = result.get("frameIndex", 0)
        runtime = result.get("runtime", {})
        render = result.get("render", {})

        if first_frame_seen_at is None and frame > 0:
            first_frame_seen_at = t - t_launched
            print(f"  first-frame-rendered: at t+{first_frame_seen_at:.2f}s (frameIndex={frame})")

        if ready_at is None and runtime.get("gameStarted") and render.get("inGameRenderReady"):
            ready_at = t - t_launched
            print(f"  game-ready: at t+{ready_at:.2f}s (frameIndex={frame})")

        # 记录 inter-poll frame delta
        delta_t = t - last_t
        delta_f = frame - last_frame_idx
        samples.append({
            "t_launched_offset": round(t - t_launched, 3),
            "frameIndex": frame,
            "deltaT": round(delta_t, 3),
            "deltaFrames": delta_f,
            "fps_window": round(delta_f / max(0.001, delta_t), 1),
            "gameStarted": runtime.get("gameStarted"),
            "inGameRenderReady": render.get("inGameRenderReady"),
        })
        last_frame_idx = frame
        last_t = t

        # ready 之后再采 25s 就停
        if ready_at is not None and (t - t_launched - ready_at) > 25:
            break
        time.sleep(0.1)

    final_frame = last_frame_idx
    total_elapsed = time.time() - t_launched
    print(f"  total: {total_elapsed:.2f}s, last frame={final_frame}, ~avg fps {final_frame/max(0.1,total_elapsed):.1f}")

    # 检测 frame stagnation：连续多次 deltaFrames=0 而 deltaT 大
    stalls = []
    for s in samples:
        if s["deltaFrames"] == 0 and s["deltaT"] > 0.3:
            stalls.append(s)
    if stalls:
        print(f"  STALLS detected (frame stagnation > 300ms):")
        for st in stalls[:10]:
            print(f"    t+{st['t_launched_offset']:>6.2f}s  deltaT={st['deltaT']:>5.2f}s  frame={st['frameIndex']}  ready={st['inGameRenderReady']}")
        max_stall = max(s["deltaT"] for s in stalls)
        print(f"  MAX STALL: {max_stall:.2f}s")
    else:
        print(f"  no stall > 300ms detected")

    stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)

    return {
        "map": map_path, "label": label,
        "firstFrameSec": first_frame_seen_at,
        "readySec": ready_at,
        "totalSec": total_elapsed,
        "finalFrame": final_frame,
        "stalls": stalls,
        "maxStallSec": max(s["deltaT"] for s in stalls) if stalls else 0.0,
    }


maps = [
    (r"E:\Work\War3\Maps\(4)MysticIsles.w3m", "MysticIsles_4p"),
    (r"E:\Work\War3\Maps\(12)IceCrown.w3m", "IceCrown_12p"),
]

results = []
for mp, lbl in maps:
    r = measure(mp, lbl)
    if r:
        results.append(r)
    time.sleep(3)

print("\n\n========= SUMMARY =========")
for r in results:
    print(f"  {r['label']:18s} firstFrame={r['firstFrameSec']:>6.2f}s ready={r['readySec']:>6.2f}s maxStall={r['maxStallSec']:>5.2f}s total={r['totalSec']:>6.2f}s frames={r['finalFrame']}")
