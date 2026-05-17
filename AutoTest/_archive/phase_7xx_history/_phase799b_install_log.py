"""Phase 7.99b: 启动游戏，等 30s 拉所有 OutputDebugString 事件，找 hook 安装日志"""
import json
import sys
import time
sys.path.insert(0, '.')
from war3_autotest_mcp import (
    launch_war3_test,
    wait_for_game_ready,
    get_runtime_events,
    stop_war3,
    DEFAULT_WAR3_DIR,
)

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

# Wait 30s for any widget lifecycle events
time.sleep(30)

# Get all events
events_resp = get_runtime_events(since_id=0, limit=2000, contains="")
events = events_resp.get("events", [])
print(f"[probe] total events: {len(events)}")

print("\n=== events with 'Widget' or 'Shadow' or 'Hook' ===")
for e in events:
    msg = e.get("message", "")
    if any(k in msg for k in ["Widget", "widget", "Hook", "hook", "Shadow", "Install"]):
        print(f"  [{e.get('id', '?')}] {msg.rstrip()}")

print("\n=== ALL events (first 50, just to see real content) ===")
for e in events[:50]:
    msg = e.get("message", "").rstrip()
    if msg:
        print(f"  [{e.get('id', '?')}] {msg[:200]}")

stop_war3(pid=pid, graceful_wait_sec=4)
