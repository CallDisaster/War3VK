"""Phase 7.105 验证 my skip counter 是否真的累加了。"""
import json
import sys
import time
import os
sys.path.insert(0, '.')
os.environ["DXVK_WAR3_SPRITE_HOST_BIND_OPENING_SKIP"] = "1"
os.environ["DXVK_WAR3_PALETTE_TREE_OPENING_SKIP"] = "1"
os.environ["DXVK_WAR3_PALETTE_TREE_DEFER_MATRIX"] = "1"

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
    env_overrides_json=json.dumps({
        "DXVK_WAR3_SPRITE_HOST_BIND_OPENING_SKIP": "1",
        "DXVK_WAR3_PALETTE_TREE_OPENING_SKIP": "1",
        "DXVK_WAR3_PALETTE_TREE_DEFER_MATRIX": "1",
    }),
)
pid = launch.get("pid", 0)
print(f"launch ok={launch.get('ok')} pid={pid}")

# 持续 polling 30s
t0 = time.time()
last_report = 0
while time.time() - t0 < 30:
    time.sleep(2)
    s = _control_plane_request(pid=pid, command="get_shadow_runtime_summary",
                               timeout_sec=5.0).get("result", {})
    rs = _control_plane_request(pid=pid, command="get_runtime_status",
                                timeout_sec=5.0).get("result", {})
    elapsed = time.time() - t0
    print(f"\nt+{elapsed:.1f}s:")
    print(f"  isInGame={rs.get('render', {}).get('isInGame')}")
    print(f"  inGameRenderReady={rs.get('render', {}).get('inGameRenderReady')}")
    print(f"  spriteHostBindCount={s.get('spriteHostBindCount')}")
    print(f"  semanticModelSpriteHostBindCalls={s.get('semanticModelSpriteHostBindCalls')}")
    print(f"  semanticModelSpriteHostBindUs={s.get('semanticModelSpriteHostBindUs')}")

stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
