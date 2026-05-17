"""Phase 7.105 验证 skip counter 是否累加。"""
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

map_path = r"E:\Work\War3\Maps\(12)IceCrown.w3m"
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
)
pid = launch.get("pid", 0)
print(f"launch ok={launch.get('ok')} pid={pid}")
ready = wait_for_game_ready(timeout_sec=60, pid=pid)
print(f"ready={ready.get('ok')} elapsed={ready.get('elapsedSec')}s")
time.sleep(30)

s = _control_plane_request(pid=pid, command="get_shadow_runtime_summary",
                           timeout_sec=10.0).get("result", {})

print("\n=== sprite host bind + skip diagnostic ===")
for k in [
    "spriteHostBindCount",
    "spriteHostBindOpeningSkipCount",
    "runtimePaletteTreeOpeningSkipCount",
    "semanticModelSpriteHostBindCalls",
    "semanticModelSpriteHostBindUs",
]:
    print(f"  {k:55s} {s.get(k, 'MISSING')}")

stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
