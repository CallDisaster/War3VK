# 2026-09-18 阶段 D（批次 2，D2）：**arm 顺序契约**的源码静态锁。
#
# 裁定：「先初始化记录器与预冻结钩子再 release 发布 active」。
#
# 为什么必须是顺序锁（而不是运行期用例）：缺陷是"两个发布动作与钩子安装之间的窗口"，
# 在单线程夹具里**无法稳定复现**；而顺序本身是**源码可判定**的事实。
# 这与本项目其它"契约型"约束（11p/11q/11r(d)）同类。
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "src/d3d9/war3/tools/war3_frame_evidence.cpp").read_text(
    encoding="utf-8", errors="replace")

failures = []


def check(cond, message):
    if not cond:
        failures.append(message)


def _index(pattern):
    m = re.search(pattern, SRC)
    return m.start() if m else None


_hook = _index(r"setPreFreezeHook\(&PaletteObjectPreFreezeHook\)")
_active = _index(r"s->active\.store\(s->generation,std::memory_order_release\)")
_arm = _index(r"ArmPaletteObjectEvidence\(s->generation,0u\)")

check(_hook is not None, "D2: the pre-freeze hook installation is missing from the arm path")
check(_active is not None, "D2: the release publication of active is missing from the arm path")
check(_arm is not None, "D2: ArmPaletteObjectEvidence is missing from the arm path")

if _hook is not None and _active is not None:
    check(_hook < _active,
          "D2: the pre-freeze hook MUST be installed BEFORE active is published "
          "(otherwise a freeze in between settles no terminal state)")
if _hook is not None and _arm is not None:
    check(_hook < _arm,
          "D2: the pre-freeze hook MUST be installed BEFORE the recorder is armed")
if _arm is not None and _active is not None:
    # 2026-09-18 P0-1（Astra 实测修正）：这一条**此前缺失** —— 旧锁只锁了
    # hook<active 与 hook<arm，**恰好漏掉**了裁定要求的另一半：
    # 「先初始化记录器与预冻结钩子**再** release 发布 active」。
    # 实测后果：发布后、初始化前到达的事件被未初始化记录器接纳，随后 Reset 清掉
    # 且不落丢失计数 ⇒ 无声丢失。
    check(_arm < _active,
          "D2/P0-1: the palette recorder MUST be initialized BEFORE active is published "
          "(otherwise events arriving in that window are silently lost: accepted, then wiped "
          "by Reset without any loss counter)")

if failures:
    for message in failures:
        print("FAILURE: %s" % message)
    sys.exit(1)
print("palette object arm-order static checks passed")