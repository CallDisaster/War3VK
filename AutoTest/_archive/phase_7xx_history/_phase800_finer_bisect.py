"""Phase 7.105 finer bisect: 找到 semantic.data + shadow 内具体哪个子模块。
通过组合不同 disable 列表来定位（基于上一轮发现：semantic+shadow 必须同时禁用才恢复 FPS）。
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


def measure(map_path, label, disable_modules=""):
    print(f"\n========== {label}: disable=[{disable_modules}] ==========")
    launch = launch_war3_test(
        war3_dir=str(DEFAULT_WAR3_DIR),
        map_path=map_path,
        use_isolated_desktop=True,
        deploy_d3d9_before_launch=False,
        disable_modules=disable_modules,
    )
    pid = launch.get("pid", 0)
    if not launch.get("ok"):
        return None
    t_launched = time.time()
    last_frame = 0
    while time.time() - t_launched < 30:
        time.sleep(0.5)
        rs = _control_plane_request(pid=pid, command="get_runtime_status",
                                    timeout_sec=2.0)
        if rs.get("transportOk"):
            last_frame = rs.get("result", {}).get("frameIndex", last_frame)
    elapsed = time.time() - t_launched
    fps = last_frame / max(0.1, elapsed)
    print(f"  frames={last_frame} elapsed={elapsed:.1f}s fps={fps:.1f}")
    stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
    return {"label": label, "frames": last_frame, "fps": fps}


map_path = r"E:\Work\War3\Maps\(12)IceCrown.w3m"

# 定位：semantic + shadow 中的具体子模块
configs = [
    ("baseline_default", ""),
    # 单独禁用 shadow 的子模块
    ("dis_shadow.capture", "shadow.capture"),
    ("dis_shadow.map", "shadow.map"),
    ("dis_shadow.receiver", "shadow.receiver"),
    ("dis_shadow.taa", "shadow.taa"),
    # semantic.data 和 shadow.taa 组合
    ("dis_semantic+shadow.taa", "semantic.data,shadow.taa"),
    # semantic.data 和 shadow.map 组合
    ("dis_semantic+shadow.map", "semantic.data,shadow.map"),
    # semantic.data 和 shadow.receiver 组合
    ("dis_semantic+shadow.receiver", "semantic.data,shadow.receiver"),
    # semantic.data 和 shadow.capture 组合
    ("dis_semantic+shadow.capture", "semantic.data,shadow.capture"),
]

results = []
for lbl, dis in configs:
    r = measure(map_path, lbl, dis)
    if r:
        results.append(r)
    time.sleep(2)

print("\n\n========= SUMMARY =========")
print(f"  {'config':40s} {'frames':>8s} {'fps':>8s}")
for r in results:
    print(f"  {r['label']:40s} {r['frames']:>8} {r['fps']:>7.1f}")
