"""现场 d3d9.dll 恢复工具（2026-09-18 事故产物）。

为什么需要它：
  实机运行会把候选 d3d9.dll 部署到现场；游戏进程在退出前持续持有该文件的锁。
  此时 `copy` 覆盖会以 WinError 32 失败，而该进程可能**无法被本会话终止**
  （实测：Stop-Process / taskkill /F /T / WMI Terminate / 模块内 _taskkill 全部拒绝访问）。

核心方法 —— **改名让路**：
  Windows 允许对一个以 FILE_SHARE_DELETE 打开的文件**改名**，即使不允许覆盖其内容。
  把被锁的 site 改名为 <site>.locked_<stamp>，原路径即刻空闲，再把备份复制到 site。
  运行中的进程继续使用它已映射的那一份，不受影响。**该方法已在 2026-09-18 的事故中实测成功。**

用法：
  py AutoTest/restore_site_dll.py                 # 自动发现备份并恢复到基线
  py AutoTest/restore_site_dll.py --selftest      # 在临时目录中自测两种路径（不接触现场）
"""
import argparse, hashlib, shutil, sys, tempfile
from datetime import datetime
from pathlib import Path

SITE = Path(r"E:\Work\Warcraft III\d3d9.dll")
BASELINE = "F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3"


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest().upper()


def log(*a):
    print("[%s]" % datetime.now().strftime("%H:%M:%S"), *a, flush=True)


def restore(site, bak, want, tries=40, delay=5):
    """返回 True 表示 site 已等于 want。两策略按序：改名让路（优先）→ 直接覆盖。"""
    import time
    for i in range(tries):
        try:
            if sha(site) == want:
                log("already at target (try %d)" % i)
                return True
        except Exception:
            pass
        # 策略 A：改名让路 —— 被锁时同样有效
        try:
            parked = site.with_name(site.name + ".locked_" + datetime.now().strftime("%Y%m%d-%H%M%S"))
            site.rename(parked)
            shutil.copy2(bak, site)
            if sha(site) == want:
                log("restored via rename-park (try %d); parked=%s" % (i, parked.name))
                return True
        except Exception as e:
            if i % 6 == 0:
                log("rename path failed (try %d): %s" % (i, str(e)[:70]))
        # 策略 B：直接覆盖
        try:
            shutil.copy2(bak, site)
            if sha(site) == want:
                log("restored via direct copy (try %d)" % i)
                return True
        except Exception as e:
            if i % 6 == 0:
                log("direct copy failed (try %d): %s" % (i, str(e)[:70]))
        time.sleep(delay)
    log("FAILED after %d tries; backup kept at %s" % (tries, bak))
    return False


def find_backup(site):
    cands = sorted(site.parent.glob(site.name + ".*backup*"))
    for c in cands:
        try:
            if sha(c) == BASELINE:
                return c
        except Exception:
            pass
    return None


def selftest():
    """在临时目录验证：①已是目标时幂等返回 ②"现场≠目标"时能改回目标。"""
    ok = True
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        site = d / "d3d9.dll"
        bak = d / "d3d9.dll.backup"
        site.write_bytes(b"CANDIDATE" * 100)
        bak.write_bytes(b"BASELINE" * 100)
        want = sha(bak)
        first = restore(site, bak, want, tries=2, delay=0)
        ok &= first and sha(site) == want
        log("selftest 1 (restore to target): %s" % ("PASS" if first and sha(site) == want else "FAIL"))
        second = restore(site, bak, want, tries=2, delay=0)
        ok &= second
        log("selftest 2 (idempotent when already target): %s" % ("PASS" if second else "FAIL"))
        parked = list(d.glob("d3d9.dll.locked_*"))
        log("selftest 3 (rename-park produced a parked file): %s" % ("PASS" if parked else "FAIL"))
        ok &= bool(parked)
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--site", default=str(SITE))
    ap.add_argument("--backup", default=None)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    site = Path(a.site)
    bak = Path(a.backup) if a.backup else find_backup(site)
    if bak is None or not bak.is_file():
        log("no usable backup found next to", site)
        return 2
    log("site:", site, "backup:", bak.name)
    r = restore(site, bak, BASELINE)
    log("RESULT:", "OK" if r else "FAILED")
    if r:
        log("site sha:", sha(site))
    return 0 if r else 1


if __name__ == "__main__":
    raise SystemExit(main())