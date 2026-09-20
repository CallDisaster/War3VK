import hashlib, shutil, time, sys
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
if not BAK.is_file():
    log("backup missing; abort"); raise SystemExit(1)
for i in range(200):
    try:
        cur = sha(SITE)
    except Exception as e:
        cur = "ERR:%s" % e
    if cur == WANT:
        log("already baseline (iter %d)" % i); break
    try:
        shutil.copy2(BAK, SITE)
        now = sha(SITE)
        log("copy ok (iter %d) sha=%s" % (i, now))
        if now == WANT:
            log("RESTORE OK"); BAK.unlink(); log("backup removed"); break
    except Exception as e:
        if i % 6 == 0:
            log("iter %d still locked: %s" % (i, str(e)[:80]))
    time.sleep(10)
try:
    final = sha(SITE)
except Exception as e:
    final = "ERR:%s" % e
log("FINAL SHA =", final)
log("FINAL OK  =", final == WANT)
log("backup exists =", BAK.exists())