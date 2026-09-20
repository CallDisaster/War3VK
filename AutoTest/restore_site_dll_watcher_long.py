import hashlib, shutil, time
from datetime import datetime
from pathlib import Path
SITE = Path(r"E:\Work\Warcraft III\d3d9.dll")
BAK  = Path(r"E:\Work\Warcraft III\d3d9.dll.contrast_backup_20260918-151332")
WANT = "F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3"
def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest().upper()
def log(*a): print("[%s]" % datetime.now().strftime("%H:%M:%S"), *a, flush=True)
log("long watcher start; will restore the moment the DLL unlocks")
if not BAK.is_file():
    log("WARNING: backup gone; nothing to restore")
    try:
        log("site sha", sha(SITE))
    except Exception as e:
        log("site unreadable", e)
    raise SystemExit(0)
for i in range(1440):
    try:
        if sha(SITE) == WANT:
            log("already baseline (iter %d)" % i); break
    except Exception:
        pass
    try:
        shutil.copy2(BAK, SITE)
        now = sha(SITE)
        if now == WANT:
            log("RESTORED at iter %d sha=%s" % (i, now))
            BAK.unlink(); log("backup removed; site is baseline"); break
        log("copy produced unexpected sha", now)
    except Exception as e:
        if i % 20 == 0:
            log("iter %d still locked: %s" % (i, str(e)[:70]))
    time.sleep(15)
try:
    final = sha(SITE)
except Exception as e:
    final = "ERR:%s" % e
log("FINAL SHA =", final)
log("FINAL OK  =", final == WANT)
log("backup exists =", BAK.exists())