# 2026-09-18 阶段 D（批次 2，D3）：**头块单快照契约**的源码静态锁。
#
# 裁定：「HeaderJson 计数与版本字段存在性取同一快照」。
#
# 缺陷原本的形态 = **动态版本**（`firstSightUsed() ? 3 : 2`）：那是一次**独立的查询**，
# 其结果可以与计数快照不一致（版本说 3、计数按 2 的形状写）。
# round 10 已把版本改成字面常量 4，本锁**防止它退回动态形态**。
#
# 载荷断言是「快照查询恰好一次」：再出现第二次 QueryPaletteObjectEvidenceHeader，
# 就意味着又有了第二个快照 —— 这正是 D3 要防的东西。
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "src/d3d9/war3/tools/war3_frame_evidence.cpp").read_text(
    encoding="utf-8", errors="replace")

# 只取 PaletteObjectHeaderJson 的函数体（到下一个顶层 "json " 定义为止）。
_m = re.search(r"json PaletteObjectHeaderJson\(\) \{(.*?)\n\}", SRC, re.S)
failures = []
if _m is None:
    print("FAILURE: D3: PaletteObjectHeaderJson() not found")
    sys.exit(1)
BODY = _m.group(1)


def check(cond, message):
    if not cond:
        failures.append(message)


_snapshots = len(re.findall(r"QueryPaletteObjectEvidenceHeader\(", BODY))
check(_snapshots == 1,
      "D3: the header must be taken from EXACTLY ONE snapshot "
      "(found %d calls to QueryPaletteObjectEvidenceHeader); a second call is a second "
      "snapshot, which is the very defect D3 forbids" % _snapshots)

check(re.search(r'result\["version"\]\s*=\s*4\s*;', BODY) is not None,
      "D3: the block version must be the literal 4 (a computed/dynamic version is a "
      "second query whose result can disagree with the counters snapshot)")
check("?" not in re.search(r'result\["version"\][^;]*;', BODY).group(0),
      "D3: the version assignment must not contain a conditional")

check(re.search(r"const auto& c\s*=\s*header\.counters;", BODY) is not None,
      "D3: counters must be taken from the same header snapshot")
check("result[\"watchCount\"]=header.watchCount;" in BODY,
      "D3: watchCount must come from the same header snapshot")
check('result["counters"]=counters;' in BODY,
      "D3: the counters object must be the one built from that snapshot")

if failures:
    for message in failures:
        print("FAILURE: %s" % message)
    sys.exit(1)
print("palette object header single-snapshot static checks passed")