"""Phase 7.105 对比 4-人 vs 12-人 SpriteHostBind cost。"""
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
        print("launch failed")
        return None
    ready = wait_for_game_ready(timeout_sec=60, pid=pid)
    print(f"ready={ready.get('ok')} elapsed={ready.get('elapsedSec')}s")
    time.sleep(30)
    s = _control_plane_request(pid=pid, command="get_shadow_runtime_summary",
                               timeout_sec=10.0).get("result", {})
    rs = _control_plane_request(pid=pid, command="get_runtime_status",
                                timeout_sec=5.0).get("result", {})
    print(f"frameIndex={rs.get('frameIndex')}")
    print(f"  spriteHostBindCount={s.get('spriteHostBindCount')}")
    print(f"  spriteHostBindOpeningSkipCount={s.get('spriteHostBindOpeningSkipCount')}")
    print(f"  runtimePaletteTreeOpeningSkipCount={s.get('runtimePaletteTreeOpeningSkipCount')}")
    print(f"  semanticModelSpriteHostBindCalls={s.get('semanticModelSpriteHostBindCalls')}")
    bindUs = s.get('semanticModelSpriteHostBindUs', 0) or 0
    calls = s.get('semanticModelSpriteHostBindCalls', 0) or 0
    avgUs = bindUs / max(1, calls)
    print(f"  semanticModelSpriteHostBindUs={bindUs} (avg={avgUs:.0f}us = {avgUs/1000:.1f}ms)")

    stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
    return s


measure(r"E:\Work\War3\Maps\(4)MysticIsles.w3m", "MysticIsles_4p")
time.sleep(3)
measure(r"E:\Work\War3\Maps\(12)IceCrown.w3m", "IceCrown_12p")
