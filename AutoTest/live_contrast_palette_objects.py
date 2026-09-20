"""批次 4 第④问 实机对照驱动（2026-09-18）。

安全设计（缺一不可）：
  1. 站点路径**硬编码**为 E:\\Work\\Warcraft III —— 绝不使用 E:/Work/War3（另一安装）。
  2. 部署前核对站点 DLL SHA 必须 == 基线；不符则中止，不做任何改动。
  3. 部署候选后核对 SHA == 候选。
  4. 无论成功/异常，finally 中恢复现场并核对 SHA 回到基线，且删除临时备份。
  5. deploy_d3d9_before_launch=False —— 部署由本脚本自己控制（不用 AutoTest 的部署路径）。
"""
import hashlib, json, shutil, sys, time, traceback
from datetime import datetime
from pathlib import Path

ROOT = Path(r"E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914")
SITE_DIR = Path(r"E:\Work\Warcraft III")
SITE_DLL = SITE_DIR / "d3d9.dll"
CAND_DLL = ROOT / "build32" / "src" / "d3d9" / "d3d9.dll"
MAP = Path(r"E:\Work\Warcraft III\Maps\(2)ConcealedHill.w3x")

BASE_SHA = "F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3"
# 2026-09-18 更新：原值 BBEF3BBC… 是 c1 时代（甚至早于 round 224 的 CFE40FE5…）的产物。
# 现更新为当前候选 77CAEED9…（含 P0 版本门控 / P1 deltaFrames / P1 完成判据按链型 /
# 写方修正①「首见去重按条目」+ 其多帧回归锁）。
#
# ⚠️ 教训（round 237 实测）：**只改注释也会改变 DLL 哈希** —— 注释行移动了其下方代码的
# 行号 ⇒ 调试信息变化（本构建 b_ndebug=false）。因此**每次重建都必须同步更新本常量**，
# 否则驱动会在部署前 fail-safe 中止（这是好事：绝不会部署与验证过的不同的产物）。
# 反向推论：哈希不同 ⇏ 语义变了；哈希相同 ⇒ 语义必然相同。
CAND_SHA = "5ADEDD5FE98228E193B1FB8203BE4F216650BCA3A3508D9C6F9710829A47918D"
RUN_SECONDS = 120  # 采集运行（整链已在 20s 验证中闭合）
ARM_LEAD_SECONDS = 20    # 触发前多久 arm（太早会被环冲刷掉）
ARM_CAPACITY = 262144    # 环容量上限，见 war3_frame_evidence.cpp:326

# 默认 false：前置检查仍然拒绝"已有 War3 在跑"的环境。
# 仅当**明确知道**孤儿进程持有的是 parked 文件（站点路径空闲）时才用 env 放行。
import os as _os
ALLOW_EXISTING_WAR3 = _os.environ.get("WARVK_ALLOW_EXISTING_WAR3", "") == "1"

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()

def log(*a):
    print("[%s]" % datetime.now().strftime("%H:%M:%S"), *a, flush=True)


def restore_site_dll(bak, site, want, tries=40, delay=5):
    """把现场 DLL 恢复回基线。**两种策略，按顺序尝试。**

    背景（2026-09-18 实测事故）：把恢复放在 finally 里**并不充分**。
    游戏进程退出前持续持有 d3d9.dll 的文件锁，`copy2` 会以 WinError 32 失败，
    于是 finally 走完、现场却停在候选版本；而且该进程在隔离桌面上**无法被本会话终止**
    （Stop-Process / taskkill /F /T / WMI Terminate 全部拒绝访问）。

    因此恢复**不得依赖"进程能结束"**：
      策略 A（首选，进程存活也可用）：**改名让路** ——
          Windows 允许对以 FILE_SHARE_DELETE 打开的文件改名（即使不允许覆盖内容）；
          把被锁的 site 改名为 <site>.locked_<stamp>，原路径即刻空闲，再把备份放到 site。
          运行中的进程继续用它已映射的那份，不受影响。**实测可行。**
      策略 B（兜底）：直接 copy 覆盖，适用于文件未被锁的情况。
    两种策略都先检查是否已是基线（幂等），并在成功后核对 SHA、删除备份。
    """
    import time as _t
    for i in range(tries):
        try:
            if sha(site) == want:
                log("restore: already baseline (try %d)" % i)
                return True
        except Exception:
            pass
        # 策略 A：改名让路（对被锁文件同样有效）
        try:
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            parked = site.with_name(site.name + ".locked_" + stamp)
            site.rename(parked)
            shutil.copy2(bak, site)
            if sha(site) == want:
                log("restore: OK via rename-park on try %d (parked as %s)" % (i, parked.name))
                bak.unlink()
                log("restore: backup removed; parked copy can be deleted once the game exits")
                return True
            log("restore: rename path produced unexpected sha; leaving parked file", parked.name)
        except Exception as e:
            if i % 6 == 0:
                log("restore: rename path failed (try %d): %s" % (i, str(e)[:70]))
        # 策略 B：直接覆盖
        try:
            shutil.copy2(bak, site)
            if sha(site) == want:
                log("restore: OK via direct copy on try %d" % i)
                bak.unlink()
                log("restore: backup removed")
                return True
        except Exception as e:
            if i % 6 == 0:
                log("restore: direct copy failed (try %d): %s" % (i, str(e)[:70]))
        _t.sleep(delay)
    log("restore: FAILED after %d tries -- MANUAL ACTION REQUIRED; backup at %s" % (tries, bak))
    return False


def _launch_env():
    """取证运行的环境变量。

    R-E A/B：允许用 WARVK_EXTRA_ENV 追加覆盖，实现"**同一 DLL、只切一个开关**"的干净对照。
    例：WARVK_EXTRA_ENV='{"DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT":"0"}'
    """
    # 2026-09-18 用户要求：**限制魔兽争霸3 的显卡占用**，否则前台应用（实测时用户在跑
    # destiny2，占 59.7% GPU）会被抢到无法运行。
    #
    # 手段：DXVK 的**内联配置**（config.cpp:1664-1666 支持 `$DXVK_CONFIG` 环境变量，
    # 按行解析，等同配置文件）⇒ **不落地任何文件、不动玩家安装**。
    # 键 `d3d9.maxFrameRate`（默认 -1 不限）由 D3D9 presenter 用来限帧。
    #
    # 取证只关心事件与链路，不关心帧率，所以默认限到 30 FPS；
    # 可用 WARVK_MAX_FPS 覆盖（例如 20 更省显卡，-1 表示不限）。
    _max_fps = _os.environ.get("WARVK_MAX_FPS", "30")
    env = {
        "DXVK_WAR3_FRAME_EVIDENCE": "1",
        "DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT": "1",
        "DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED": "0",
        # 2026-09-18 实机验证的**零代码**可观测性修复：
        # `_DRAWS` 控制 kind=18 DirectionalDraw（实测占环流量约 98%）。全开时 palette 3692 条
        # 中 3585 条被共享环按圈淘汰、导出只剩 107 条；关掉后 `evicted=0`、**3691 条全部保留**
        # （34 倍），且 32 条链首次呈现完整 FirstSight -> Enqueued -> Drawn。
        # 注意：`_OUTPUT` 是**输出目录路径**、不是布尔域门（设成 "0" 会让导出直接失败）。
        "DXVK_WAR3_FRAME_EVIDENCE_DRAWS": "0",
        "DXVK_WAR3_FRAME_EVIDENCE_CASTERS": "0",
        "DXVK_WAR3_FRAME_EVIDENCE_INPUTS": "0",
    }
    if str(_max_fps).strip() not in ("", "-1"):
        env["DXVK_CONFIG"] = "d3d9.maxFrameRate = %s" % str(_max_fps).strip()
    extra = _os.environ.get("WARVK_EXTRA_ENV", "")
    if extra:
        for k, v in json.loads(extra).items():
            env[str(k)] = str(v)
    return env


def find_session(obj):
    """在控制面响应里**递归**找 session token。

    2026-09-18 采集教训：`war3_frame_evidence.cpp:104` 把 session 放在 **result 内层**
    （`{"session": "...", "processId": ...}`），不是响应顶层。我第一版只在顶层 `get("session")`，
    于是取到 None ⇒ export 不带 session ⇒ `session mismatch`（trigger/arm 明明都成功）。
    """
    if isinstance(obj, dict):
        v = obj.get("session")
        if isinstance(v, str) and v:
            return v
        for sub in obj.values():
            got = find_session(sub)
            if got:
                return got
    elif isinstance(obj, list):
        for sub in obj:
            got = find_session(sub)
            if got:
                return got
    return None


def control(pid, action, session=None, timeout=8.0, capacity=None):
    """通过取证控制面 CLI 发送 arm/status/trigger/freeze/export/discard。"""
    import subprocess as _sp
    cmd = ["py", str(ROOT / "AutoTest" / "frame_evidence_control.py"),
           "--pid", str(pid), action, "--timeout", str(timeout)]
    if session:
        cmd += ["--session", str(session)]
    if capacity is not None:
        cmd += ["--capacity", str(capacity)]
    try:
        out = _sp.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout + 20)
    except Exception as e:
        return {"ok": False, "exc": str(e)[:120]}
    try:
        return json.loads(out.stdout)
    except Exception:
        return {"ok": False, "stdout": out.stdout[:300], "stderr": out.stderr[:300]}


def require_no_war3_running():
    """前置：环境必须干净。

    教训：上一轮在遗留实例仍持有文件锁的情况下继续操作，使恢复失败、现场停在候选版本。
    因此"启动前必须没有任何 War3/WorldEditor 在跑"这条检查要由**代码**强制，而不是靠人记住。
    """
    import subprocess as _sp
    bad = []
    for name in ("War3.exe", "war3.exe", "WorldEditor.exe"):
        out = _sp.run(["tasklist", "/FI", "IMAGENAME eq %s" % name, "/NH"],
                      capture_output=True, text=True, errors="replace").stdout
        for line in out.splitlines():
            if name.lower() in line.lower():
                bad.append(line.strip()[:90])
    if bad and ALLOW_EXISTING_WAR3:
        # 2026-09-18 实测后的判断：孤儿进程已**不再威胁站点路径** ——
        #   * rename-park 恢复已在实机验证：进程存活时也能把现场换回基线；
        #   * 孤儿持有的是 <site>.locked_<stamp>（改名后的 parked 文件），**不是** site 本身；
        # 因此"已有 War3 在跑"从**站点完整性风险**降级为**操作噪音**。
        # 默认仍然拒绝（见下）；只有显式设置 WARVK_ALLOW_EXISTING_WAR3=1 才放行，
        # 以免有人无意中在脏环境里启动。
        log("WARNING: existing game process(es) present; WARVK_ALLOW_EXISTING_WAR3=1 so continuing:")
        for b in bad:
            log("   ", b)
        log("   note: the site path is protected by rename-park restore, not by process exit.")
        return True
    if bad:
        log("ABORT: a game/editor process is already running; refusing to start a dirty run:")
        for b in bad:
            log("   ", b)
        log("   finish or kill it first (taskkill /F /PID <pid>), then retry.")
        return False
    log("preflight: no War3/WorldEditor running")
    return True


def wait_for_process_exit(pid, timeout_s=60):
    """停止请求之后，确认进程**真的**退出了。

    教训：stop_war3 返回 stopped=true 不代表进程已退出（实测曾返回 `forced:true` 而进程仍存活）。
    进程不退 ⇒ DLL 锁不释放 ⇒ 恢复不可能成功。因此这一步必须显式验证并如实报告。
    """
    import subprocess as _sp
    import time as _t
    deadline = _t.time() + timeout_s
    while _t.time() < deadline:
        out = _sp.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH"],
                      capture_output=True, text=True, errors="replace").stdout
        if "War3" not in out and str(pid) not in out:
            log("process %d exited" % pid)
            return True
        _t.sleep(5)
    log("WARNING: process %d still alive after %ds -- the DLL stays locked and the site", pid, timeout_s)
    log("WARNING: will NOT return to baseline until it exits. Kill it manually (taskkill /F /PID %d)." % pid)
    return False

def main():
    log("site", SITE_DLL)
    if not SITE_DLL.is_file():
        log("ABORT: site dll missing"); return 2
    if not CAND_DLL.is_file():
        log("ABORT: candidate missing"); return 2
    if not MAP.is_file():
        log("ABORT: map missing", MAP); return 2
    pre = sha(SITE_DLL); cand = sha(CAND_DLL)
    log("pre  site sha", pre)
    log("cand sha     ", cand)
    if pre != BASE_SHA:
        log("ABORT: site DLL is not the baseline; refusing to touch anything"); return 2
    if cand != CAND_SHA:
        log("ABORT: built candidate hash != expected; rebuild first"); return 2
    if not require_no_war3_running():
        return 2

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    bak = SITE_DIR / ("d3d9.dll.contrast_backup_%s" % stamp)
    shutil.copy2(SITE_DLL, bak)
    log("backup", bak.name)
    try:
        shutil.copy2(CAND_DLL, SITE_DLL)
        got = sha(SITE_DLL)
        log("deployed sha", got, "verified", got == CAND_SHA)
        if got != CAND_SHA:
            raise RuntimeError("deployment verification failed")

        sys.path.insert(0, str(ROOT / "AutoTest"))
        import war3_autotest_mcp as war3
        log("launching on isolated desktop ...")
        launch = war3.launch_war3_test(
            war3_dir=str(SITE_DIR), map_path=str(MAP),
            launcher_mode="direct", use_isolated_desktop=True,
            desktop_name="WarVK-P0", windowed=True,
            deploy_d3d9_before_launch=False,
            enforce_video_baseline=False, auto_perf_record=True,
            # R-E A/B：允许从环境变量追加/覆盖 env，用于"同一 DLL、只切一个开关"的对照。
            # 例如 WARVK_EXTRA_ENV='{"DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT":"0"}'
            env_overrides_json=json.dumps(_launch_env()))
        log("launch result:", json.dumps(launch, default=str)[:600])
        _pid = int(launch.get("pid") or launch.get("gamePid") or 0) if isinstance(launch, dict) else 0
        if _pid:
            time.sleep(25)   # 等游戏进图
            _st = control(_pid, "status")
            log("status:", json.dumps(_st, default=str)[:400])
            # 2026-09-18 采集教训：**不要**在这里 arm。
            # 第一版在运行开始就 arm(capacity=8192)，而 120s 内产生了 610 万事件，
            # 8192 格的环被冲刷干净 ⇒ 导出里每条链只剩 1 个事件、
            # 解析器正确判为 chainComplete=false / stageStreamTruncated / Uncovered。
            # 正确做法：**临近触发再 arm**，并用允许的最大容量（262144，见
            # war3_frame_evidence.cpp:326 的 capacity 上界）。
            _lead = max(0, RUN_SECONDS - 25 - ARM_LEAD_SECONDS)
            if _lead:
                time.sleep(_lead)
            log("arm:", json.dumps(control(_pid, "arm", None, capacity=ARM_CAPACITY),
                                   default=str)[:300])
        time.sleep(ARM_LEAD_SECONDS)
        log("run window elapsed (%ds)" % RUN_SECONDS)
        # 取证：trigger + export 必须在**游戏仍在运行时**做。
        # 教训（2026-09-18 采集运行）：我第一版把这段放在 stop_war3 **之后**，
        # 结果 trigger/export 全部 WaitNamedPipeW failed:2 —— 那时控制面已被关掉，
        # arm 明明成功（ok:true）却拿不到任何导出。顺序错了，不是功能坏了。
        if _pid:
            log("trigger:", json.dumps(control(_pid, "trigger"), default=str)[:300])
            time.sleep(4)
            _st2 = control(_pid, "status")
            log("status2:", json.dumps(_st2, default=str)[:400])
            _sess = find_session(_st2)
            log("session token:", repr(_sess))
            # 协议顺序（2026-09-18 逐次试错得出）：arm -> trigger -> **freeze** -> export。
            # 只 trigger 后立刻 export 会得到 "freeze before export" —— 环必须先冻结。
            if _sess:
                log("freeze:", json.dumps(control(_pid, "freeze", _sess), default=str)[:300])
                time.sleep(2)
            log("export:", json.dumps(control(_pid, "export", _sess), default=str)[:600])
        for name in ("stop_war3", "stop_war3_test"):
            fn = getattr(war3, name, None)
            if callable(fn):
                try:
                    log("calling", name)
                    log("stop result:", json.dumps(fn(), default=str)[:400])
                except Exception as exc:
                    log("stop failed:", repr(exc))
                break
        # 教训：stop 返回成功 != 进程已退出。不验证这一步，恢复必然失败。
        _pid = int(launch.get("pid") or launch.get("gamePid") or 0) if isinstance(launch, dict) else 0
        if _pid:
            wait_for_process_exit(_pid, timeout_s=60)
        ev = SITE_DIR / "WarVK" / "Log" / "FrameEvidence"
        log("evidence dir exists:", ev.is_dir(), ev)
        if ev.is_dir():
            files = sorted(ev.glob("*.json"), key=lambda p: p.stat().st_mtime)[-12:]
            for f in files:
                log("  export", f.name, f.stat().st_size, "B")
    except Exception:
        log("EXCEPTION during run:")
        traceback.print_exc()
    finally:
        ok = restore_site_dll(bak, SITE_DLL, BASE_SHA)
        log("RESTORE OK  ", ok)
        if not ok:
            log("*** SITE DLL IS NOT AT BASELINE ***")
            log("*** backup kept at: %s" % bak)
            log("*** kill any War3.exe, then re-run restore, or let a watcher do it")
    log("DONE")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())