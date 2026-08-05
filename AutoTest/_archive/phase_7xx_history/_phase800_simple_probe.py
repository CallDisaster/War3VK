"""Phase 7.105 simple probe to check status."""
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
print(f"ready ok={ready.get('ok')} elapsed={ready.get('elapsedSec')}s")
time.sleep(15)

# 试两种调用
rs1 = _control_plane_request(pid=pid, command="get_runtime_status",
                             timeout_sec=10.0)
print(f"\nget_runtime_status: ok={rs1.get('ok')} transportOk={rs1.get('transportOk')}")
result = rs1.get("result", {})
print(f"  frameIndex={result.get('frameIndex')}")
print(f"  module={result.get('module')}")
print(f"  runtime={result.get('runtime')}")
print(f"  render={result.get('render')}")

rs2 = _control_plane_request(pid=pid, command="get_shadow_runtime_summary",
                             timeout_sec=10.0)
s = rs2.get("result", {})
print(f"\n=== sprite host bind ===")
for k in [
    "spriteHostBindCount",
    "spriteHostBindResolvedIdentityCount",
    "spriteHostBindResolvedUnitCount",
    "spriteHostBindResolvedHandleCount",
    "spriteHostBindResolvedRawcodeCount",
    "semanticModelSpriteHostBindCalls",
    "semanticModelSpriteHostBindUs",
]:
    print(f"  {k:55s} {s.get(k, 'MISSING')}")

stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
