"""Phase 7.105 验证 DXVK_WAR3_SPRITE_HOST_BIND_DISABLE=1 是否恢复 12-人图 FPS。"""
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


def measure(map_path, label, env_overrides):
    print(f"\n========== {label} ==========")
    launch = launch_war3_test(
        war3_dir=str(DEFAULT_WAR3_DIR),
        map_path=map_path,
        use_isolated_desktop=True,
        deploy_d3d9_before_launch=False,
        env_overrides_json=json.dumps(env_overrides),
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
    print(f"  total: {elapsed:.1f}s, last_frame={last_frame}, avg fps={fps:.1f}")

    # 显示 stalls
    stuck_periods = 0
    for i in range(2, len(samples)):
        if samples[i]["df"] == 0 and samples[i-1]["df"] == 0:
            stuck_periods += 1
    print(f"  stuck samples (df=0 consecutive): {stuck_periods}/{len(samples)}")

    s = _control_plane_request(pid=pid, command="get_shadow_runtime_summary",
                               timeout_sec=10.0).get("result", {})
    print(f"  spriteHostBindCount={s.get('spriteHostBindCount')}")
    print(f"  semanticModelSpriteHostBindCalls={s.get('semanticModelSpriteHostBindCalls')}")
    bindUs = s.get('semanticModelSpriteHostBindUs', 0) or 0
    print(f"  semanticModelSpriteHostBindUs={bindUs}ms")

    stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
    return fps


# A/B: 默认 vs DXVK_WAR3_SPRITE_HOST_BIND_DISABLE=1
fps_default = measure(r"E:\Work\War3\Maps\(12)IceCrown.w3m",
                      "IceCrown_12p_default", {})
time.sleep(3)
fps_disabled = measure(r"E:\Work\War3\Maps\(12)IceCrown.w3m",
                       "IceCrown_12p_DISABLED",
                       {"DXVK_WAR3_SPRITE_HOST_BIND_DISABLE": "1"})

print(f"\n==== DELTA ====")
print(f"  default:  {fps_default} fps")
print(f"  disabled: {fps_disabled} fps")
