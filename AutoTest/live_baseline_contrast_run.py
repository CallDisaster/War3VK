#!/usr/bin/env python3
"""R-E 对照：用**基线 DLL**（站点现状，不部署候选）跑一次同条件取证。

与候选运行的差别只有一条：**不部署**。其余（地图、时长、控制面顺序）保持一致。
脚本自身核对站点 SHA，前后都不允许变化。
"""
import hashlib, json, subprocess, sys, time
from pathlib import Path

BASE = "F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3"
SITE = Path(r"E:\Work\Warcraft III\d3d9.dll")
MAP = Path(r"E:\Work\Warcraft III\Maps\(2)ConcealedHill.w3x")
RUN_SECONDS = 120
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "AutoTest"))

def sha(p):
    h = hashlib.sha256()
    h.update(Path(p).read_bytes())
    return h.hexdigest().upper()

def log(*a):
    print("[%s]" % time.strftime("%H:%M:%S"), *a, flush=True)

def control(pid, action, session=None, timeout=8.0):
    cmd = ["py", str(ROOT / "AutoTest" / "frame_evidence_control.py"),
           "--pid", str(pid), action, "--timeout", str(timeout)]
    if session:
        cmd += ["--session", str(session)]
    out = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout + 20)
    try:
        return json.loads(out.stdout)
    except Exception:
        return {"ok": False, "stdout": out.stdout[:200]}

def find_session(obj):
    if isinstance(obj, dict):
        v = obj.get("session")
        if isinstance(v, str) and v:
            return v
        for s in obj.values():
            g = find_session(s)
            if g: return g
    elif isinstance(obj, list):
        for s in obj:
            g = find_session(s)
            if g: return g
    return None

import war3_autotest_mcp as war3

pre = sha(SITE)
log("site sha (pre):", pre)
if pre != BASE:
    log("ABORT: site is not the baseline; refusing")
    raise SystemExit(2)
if subprocess.run(["tasklist", "/FI", "IMAGENAME eq War3.exe", "/NH"],
                  capture_output=True, text=True).stdout.count("War3.exe"):
    log("ABORT: War3 already running")
    raise SystemExit(2)

launch = war3.launch_war3_test(
    war3_dir=r"E:\Work\Warcraft III",
    map_path=str(MAP),
    launcher_mode="direct",
    use_isolated_desktop=True,
    desktop_name="WarVK-P0",
    windowed=True,
    deploy_d3d9_before_launch=False,          # <<< 与候选运行的唯一差别
    enforce_video_baseline=False,
    auto_perf_record=False,
    env_overrides_json=json.dumps({
        "DXVK_WAR3_FRAME_EVIDENCE": "1",
        "DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT": "1",
        "DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED": "0",
    }),
)
pid = int(launch.get("pid") or launch.get("gamePid") or 0)
log("launch pid:", pid, "ok:", launch.get("ok"))
try:
    time.sleep(25)
    log("status:", json.dumps(control(pid, "status"), default=str)[:300])
    log("arm:", json.dumps(control(pid, "arm"), default=str)[:200])
    time.sleep(RUN_SECONDS)
    log("trigger:", json.dumps(control(pid, "trigger"), default=str)[:200])
    time.sleep(4)
    st = control(pid, "status")
    sess = find_session(st)
    log("session:", repr(sess))
    if sess:
        log("freeze:", json.dumps(control(pid, "freeze", sess), default=str)[:200])
        time.sleep(2)
    log("export:", json.dumps(control(pid, "export", sess), default=str)[:400])
finally:
    for name in ("stop_war3", "stop_war3_test"):
        fn = getattr(war3, name, None)
        if callable(fn):
            try: log("stop:", json.dumps(fn(), default=str)[:200])
            except Exception as e: log("stop failed:", repr(e)[:120])
            break
    time.sleep(5)
    post = sha(SITE)
    log("site sha (post):", post, "unchanged:", post == BASE)
    log("DONE")