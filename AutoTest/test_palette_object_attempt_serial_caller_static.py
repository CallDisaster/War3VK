# 2026-09-18 阶段 C（K3 生产调用方传参测试）+ P0-5 修正：生产调用点必须**真的**给出 attemptSerial。
#
# 裁定：「跨帧判序改为按一次明确关联的尝试（attemptSerial 按值携带），不采用每帧清零」。
#
# 为什么必须是源码锁：机制（MarkStage 的尝试内判序）早已就位，但生产站点最初全都传默认哨兵
# ⇒ 实机判序仍是终身单调。「机制已实现」与「生产线已接线」是两件不同的事，本锁钉后者。
#
# 2026-09-18 P0-5 修正（独立复审指出的**门禁盲区**）：
#   旧版本只读 d3d9_device.cpp + war3_shadow_renderer_core.cpp 并断言 live_calls == 3 ——
#   而全树活的生产调用点实为 4（漏了 d3d9_war3_shadow.cpp 的 D 点）。
#   它还用字符串包含判断接线（draw.shadowRecordFrameSerial);），结构上既看不见第 4 个站点，
#   也无法发现「省略第 5 个实参」。
#   ⇒ 现在：**全树扫描** + **按括号配平数顶层实参**（必须正好 5 个）+ 第 5 实参不得是哨兵。
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


failures = []


def check(cond, message):
    if not cond:
        failures.append(message)


CAPTURE = read(ROOT / "src/d3d9/war3/tools/war3_palette_object_capture.h")

check(re.search(r"bool nativeKnown,\s*uint64_t attemptSerial\s*=\s*~0ull\)", CAPTURE) is not None,
      "K3: MakePaletteObjectFrames must take attemptSerial (default = unknown sentinel ~0ull)")
check("frames.attemptSerial = attemptSerial;" in CAPTURE,
      "K3: the factory must actually write attemptSerial into the frames it returns")


def top_level_args(text, open_paren):
    depth = 0
    args = []
    current = []
    i = open_paren
    while i < len(text):
        ch = text[i]
        if ch == "(":
            depth += 1
            if depth > 1:
                current.append(ch)
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(current).strip())
                return args
            current.append(ch)
        elif ch == "," and depth == 1:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
        i += 1
    return None


def strip_comments(text):
    """去注释/去字符串字面量（否则注释里的逗号会变成**幻影实参**，注释里的调用会变成**幻影站点**）。

    2026-09-18 P0-5（复审实测）：旧版对原文裸扫，'/* x, y */' 能让 4 实参调用判成 5 实参，
    纯注释里的调用能把站点数变成 5 ⇒ 双向都能骗。"""
    out = []
    i = 0
    n = len(text)
    quote = ""
    while i < n:
        ch = text[i]
        if quote:
            if ch == "\\" and i + 1 < n:
                i += 2
                continue
            if ch == quote:
                quote = ""
            i += 1
            continue
        if ch in "\"'":
            quote = ch
            i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        out.append(ch)
        i += 1
    return "".join(out)


# 已知的**未知哨兵拼法**（大小写/常量别名都要挡住 —— 复审实测 7 种写法曾全部逃逸）。
_UNKNOWN_ATTEMPT_SPELLINGS = ("~0ull", "~0ULL", "~0uLL", "UINT64_MAX", "0u", "0")

call_sites = []
for path in sorted(SRC.rglob("*.cpp")) + sorted(SRC.rglob("*.h")):
    rel = path.relative_to(ROOT).as_posix()
    if "/render/tests/" in "/" + rel:
        continue
    if rel.endswith("war3_palette_object_capture.h"):
        continue
    # 2026-09-18 P0-5（复审实测修正）：**必须先去掉注释与字符串字面量** ——
    # 旧版裸扫原文；注释里的逗号会变成**幻影实参**（合法 4 实参调用被判成 5 实参），
    # 注释里的调用会变成**幻影站点**（把门禁假打红）：双向都能骗。
    text = strip_comments(read(path))
    for match in re.finditer(r"MakePaletteObjectFrames", text):
        # 2026-09-18 P0-5（复审实测修正）：**取地址/函数指针逃逸** ——
        # `auto fn = &…::MakePaletteObjectFrames; fn(a,b,c,d)` 是真实的 4 实参生产调用，
        # 旧版只匹配标识符紧跟 '(' 的形态，实测完全逃逸。现在要求后面必须紧跟 '('。
        rest = text[match.end():].lstrip()
        check(rest.startswith("("),
              "K3/P0-5: %s 里 MakePaletteObjectFrames 不是直接调用（取地址/别名会绕过本门禁）" % rel)
        if not rest.startswith("("):
            continue
        args = top_level_args(text, match.end())
        call_sites.append((rel, args))
        check(args is not None,
              "K3/P0-5: unbalanced parentheses at a MakePaletteObjectFrames call: %s" % rel)

check(len(call_sites) == 4,
      "K3/P0-5: expected exactly 4 live production call sites (2 x d3d9_device.cpp = Served/FirstSight, "
      "1 x d3d9_war3_shadow.cpp = Drawn, 1 x war3_shadow_renderer_core.cpp = Reject); found %d: %s"
      % (len(call_sites), [c[0] for c in call_sites]))


def is_bare_unknown_attempt(expr):
    """整个第 5 实参**就是**一个未知/零哨兵字面量（而不是把它用在归一化表达式里）。

    2026-09-18 P0-5（复审实测）：旧版用子串检查 `'~0ull' not in arg`，于是 `0u` / `~0ULL` /
    `UINT64_MAX` 全部逃逸 —— 而 `0u` 恰恰是 D 点在 6+ 条不写该字段的 append 路径上**实际取到的值**，
    并被记录器明令禁止当作未知（会与「第一次尝试」混淆、错误重置尝试基线）。
    注意：**显式**传命名常量 `kPaletteObjectUnknownAttempt` 是允许的（那是「如实说未知」），
    本检查只拦「整实参就是裸字面量哨兵」。
    """
    e = "".join(expr.split())
    return e in ("~0ull", "~0ULL", "~0uLL", "UINT64_MAX", "0", "0u", "0ull", "0ULL")


for rel, args in call_sites:
    check(args is not None and len(args) == 5,
          "K3/P0-5: %s must pass attemptSerial explicitly as the 5th argument (omitting it silently "
          "yields the unknown sentinel => mechanism implemented but production line NOT wired); got %s"
          % (rel, None if args is None else len(args)))
    if args and len(args) == 5:
        check(not is_bare_unknown_attempt(args[4]),
              "K3/P0-5: %s 的第 5 实参是**裸哨兵/裸零字面量**；0 是记录器明令禁止的冒充值，"
               "拿不到真实序号时必须显式传 kPaletteObjectUnknownAttempt" % rel)
        check(args[4].strip() != "", "K3/P0-5: %s has an empty 5th argument" % rel)

if failures:
    for message in failures:
        print("FAILURE: %s" % message)
    sys.exit(1)
print("palette object attempt-serial production-caller static checks passed "
      "(%d live call sites, all passing an explicit serial)" % len(call_sites))
