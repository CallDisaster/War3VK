"""Phase 7.105 验证默认禁用 SpriteHostBind 后两图都健康。"""
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


def measure(map_path, label):
    print(f"\n========== {label} ==========")
    launch = launch_war3_test(
        war3_dir=str(DEFAULT_WAR3_DIR),
        map_path=map_path,
        use_isolated_desktop=True,
        deploy_d3d9_before_launch=False,
    )
    pid = launch.get("pid", 0)
    if not launch.get("ok"):
        return None
    t_launched = time.time()
    last_frame = 0
    samples = []
    while time.time() - t_launched < 30:
        time.sleep(0.5)
        rs = _control_plane_request(pid=pid, command="get_runtime_status",
                                    timeout_sec=2.0)
        if rs.get("transportOk"):
            frame = rs.get("result", {}).get("frameIndex", last_frame)
            samples.append({"t": round(time.time() - t_launched, 1),
                           "frame": frame, "df": frame - last_frame})
            last_frame = frame

    elapsed = time.time() - t_launched
    fps = last_frame / max(0.1, elapsed)
    stuck = sum(1 for s in samples if s["df"] == 0)
    print(f"  total: {elapsed:.1f}s, last_frame={last_frame}, avg fps={fps:.1f}")
    print(f"  stuck samples (df=0): {stuck}/{len(samples)}")

    s = _control_plane_request(pid=pid, command="get_shadow_runtime_summary",
                               timeout_sec=10.0).get("result", {})
    print(f"  spriteHostBindCount={s.get('spriteHostBindCount')} (game's hook fires)")
    print(f"  spriteHostBindOpeningSkipCount={s.get('spriteHostBindOpeningSkipCount')} (our skip count)")
    print(f"  semanticModelSpriteHostBindCalls={s.get('semanticModelSpriteHostBindCalls')} (我们 RecordSpriteHostOwnerBinding 调用次数)")
    bindUs = s.get('semanticModelSpriteHostBindUs', 0) or 0
    print(f"  semanticModelSpriteHostBindUs={bindUs/1000:.1f}ms total")

    stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
    return fps


fps_4p = measure(r"E:\Work\War3\Maps\(4)MysticIsles.w3m", "MysticIsles_4p")
time.sleep(3)
fps_12p = measure(r"E:\Work\War3\Maps\(12)IceCrown.w3m", "IceCrown_12p")

print(f"\n==== SUMMARY ====")
print(f"  4-人 MysticIsles: {fps_4p}")
print(f"  12-人 IceCrown:  {fps_12p}")
