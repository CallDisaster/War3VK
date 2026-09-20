"""实机对照驱动的**安全不变量**静态门禁（2026-09-18 事故产物）。

背景：2026-09-18 的一次实机运行把候选 DLL 部署到现场后，游戏进程无法终止、文件被锁定，
恢复失败，现场一度停在候选版本。事后查明两处缺口并修复。本门禁把这些修复**钉死**，
防止日后有人（包括我）在重构驱动时无意中削弱它们。

只读源码；不启动游戏、不接触现场。
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVER = (ROOT / "AutoTest/live_contrast_palette_objects.py").read_text(encoding="utf-8")
TOOL = (ROOT / "AutoTest/restore_site_dll.py").read_text(encoding="utf-8")

# ---- 1. 站点路径必须是唯一且正确的那个安装 ----
assert 'SITE_DIR = Path(r"E:\\Work\\Warcraft III")' in DRIVER, (
    "站点必须硬编码为玩家测试站点")
# 注意：docstring 里**提到** E:/Work/War3 作为"禁止使用"的对照是允许的，
# 因此这里只检查**赋值与调用目标**，而不是任意出现。
for line in DRIVER.splitlines():
    stripped = line.strip()
    if stripped.startswith("#") or stripped.startswith("\\"):
        continue
    is_target = ("SITE_DIR" in line or "war3_dir=" in line or "SITE =" in line) and "=" in line
    if is_target:
        for forbidden in ("Work\\\\War3", "Work/War3"):
            assert forbidden not in line, (
                "驱动不得把 E:/Work/War3 当成目标 —— 那是另一个安装，混用会动错现场: " + stripped[:80])

# ---- 2. 动手之前必须核对两个 SHA ----
assert "if pre != BASE_SHA:" in DRIVER, "部署前必须核对现场是否等于基线"
assert "if cand != CAND_SHA:" in DRIVER, "部署前必须核对候选是否等于期望"
assert "refusing to touch anything" in DRIVER, "SHA 不符时必须中止且不做任何改动"

# ---- 3. 环境必须干净（事故直接教训之一）----
assert "def require_no_war3_running()" in DRIVER
assert "if not require_no_war3_running():" in DRIVER, "前置检查必须被真正调用"
# 它必须排在备份/部署之前
_pre = DRIVER.index("require_no_war3_running():", DRIVER.index("def main"))
_bak = DRIVER.index("shutil.copy2(SITE_DLL, bak)")
assert _pre < _bak, "干净环境检查必须发生在备份与部署之前"

# ---- 4. 恢复必须有两种策略，且以「改名让路」为首选（事故核心教训）----
assert "def restore_site_dll(" in DRIVER, "驱动必须内联恢复函数"
assert "site.rename(parked)" in DRIVER, "策略 A：改名让路（进程存活/文件被锁时同样有效）"
assert "shutil.copy2(bak, site)" in DRIVER, "策略 B：直接覆盖"
_a = DRIVER.index("site.rename(parked)")
_b = DRIVER.index("shutil.copy2(bak, site)", _a)
assert _a < _b, "改名让路必须是首选策略，直接覆盖只作兜底"
assert "ok = restore_site_dll(bak, SITE_DLL, BASE_SHA)" in DRIVER, "finally 必须调用恢复并核对基线"
assert "SITE DLL IS NOT AT BASELINE" in DRIVER, "恢复失败必须显式告警，不得静默"

# ---- 5. 停止之后必须验证进程真的退出 ----
assert "def wait_for_process_exit(pid" in DRIVER
assert "wait_for_process_exit(_pid" in DRIVER, "停止后必须验证进程退出，否则恢复必然失败"

# ---- 6. 独立补救工具：只在备份等于基线时才动手 ----
assert "def find_backup(site)" in TOOL
assert "if sha(c) == BASELINE:" in TOOL, "只接受 SHA 等于基线的备份，绝不把任意旧版本当基线"
assert "def selftest()" in TOOL and "--selftest" in TOOL, "工具必须自带可离线运行的 selftest"
assert "site.rename(parked)" in TOOL, "补救工具同样以改名让路为首选"

# ---- 7. 不得出现"只加更多次相同尝试"式的伪加固而不换手段 ----
assert "rename-park" in DRIVER or "rename-park" in TOOL, "必须保留改名让路的可识别标记"

print("live driver safety static: PASS (site pinned, SHA guards, clean-env preflight, rename-park restore, exit verification)")