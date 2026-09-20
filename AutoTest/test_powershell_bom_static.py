# 2026-09-18/19 阶段 E 实机半发现的**真实缺陷类**：PowerShell 脚本缺 UTF-8 BOM。
#
# 现象（实测）：`AutoTest/capture_startup_environment.ps1` 无 BOM ⇒ Windows PowerShell 5.1 按 ANSI 解码，
# 脚本内中文变乱码 ⇒ **解析错误**（`The '<' operator is reserved` / 字符串未终止 / 赋值非法），
# 于是「保存完整启动环境」这一步**静默失效**（错误只在 stderr，退出码仍是 0）。
# 只有 PowerShell 7（harness 的 `pwsh`）默认按 UTF-8 读取，所以交互跑是好的、无人值守跑是坏的。
#
# 判据：任何**含非 ASCII** 的 AutoTest/*.ps1 都**必须**带 UTF-8 BOM。
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOM = b'\xef\xbb\xbf'
failures = []
checked = 0
for path in sorted((ROOT / 'AutoTest').glob('*.ps1')):
    raw = path.read_bytes()
    checked += 1
    text = raw.decode('utf-8', errors='replace')
    non_ascii = any(ord(ch) > 127 for ch in text)
    if non_ascii and not raw.startswith(BOM):
        failures.append('%s contains non-ASCII but has no UTF-8 BOM (Windows PowerShell 5.1 will mis-decode it)' % path.name)

if failures:
    for message in failures:
        print('FAILURE: %s' % message)
    sys.exit(1)
print('powershell BOM static checks passed (%d scripts)' % checked)
