"""Phase 7.99d: 启动高压地图 + 60s 后截图，看 path blocker 是否还能看到阴影"""
import json
import sys
import time
sys.path.insert(0, '.')
from war3_autotest_mcp import (
    launch_war3_test,
    wait_for_game_ready,
    capture_war3_screenshot,
    stop_war3,
    DEFAULT_WAR3_DIR,
)
from pathlib import Path

map_path = r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x"

print(f"[probe] launching {map_path}")
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
)
pid = launch.get("pid", 0)
print(f"  ok={launch.get('ok')} pid={pid}")
if not launch.get("ok"):
    sys.exit(1)

ready = wait_for_game_ready(timeout_sec=120, pid=pid)
print(f"[probe] game ready ok={ready.get('ok')}")
time.sleep(60)

out_dir = Path(r"E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk\AutoTest\artifacts\phase799_screenshots")
out_dir.mkdir(parents=True, exist_ok=True)
out_path = out_dir / f"highpressure_after60s.png"

shot = capture_war3_screenshot(output_path=str(out_path), pid=pid)
print("=== screenshot ===")
print(json.dumps(shot, indent=2, ensure_ascii=False))

stop_war3(pid=pid, graceful_wait_sec=4)
