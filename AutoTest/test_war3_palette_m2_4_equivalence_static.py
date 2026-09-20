"""M2-4 palette compose-policy / env getter 余项迁移的等价性证据门禁
（fail-closed，独立成文件）。

背景：M2-4 把 d3d9_device.cpp 的 8 个调色板侧余项符号（9 个定义：
War3SemanticPaletteLooksModelLocal 两个重载）迁往
    src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}
迁出内容：
  A1 War3SemanticPaletteInPlaceAppendRuntime（env getter，inline）
  A2 War3SemanticDrawTimePoseRuntime（env getter）
  A3 War3SemanticPaletteStorageReadable（A4 supporting 纯判定）
  A4 War3SemanticPaletteLooksModelLocal（compose-policy 判定主体，两重载）
  A5 War3SemanticHashMatrix4（HashMatrixPalette 的姊妹）
  A6 War3SemanticTranslationFinite（A4 supporting）
  A7 War3SemanticTranslationDistanceSq（A4 supporting）
  A8 War3SemanticBoundsRadiusForObjectKind（A4 依赖的纯查表；另 6 个 bounds
     调用点文本未改，仍经 device.cpp 既有 using-directive 解析到模块）

为什么需要独立门禁：A3 / A4 / A5（以及 A6 / A7）**不匹配**预算门禁
（AutoTest/test_device_semantic_responsibility_budget_static.py）的
SEMANTIC_FAMILY_RE 语义选择命名族，因此判定 3（回流）对它们盲；M2-4 把它们
显式登记进 FROZEN_M2_4（budget=0）后只有判定 1 能咬住回流。本文件钉住
等价性证据本身，与 M2-1/M2-2/M2-3/M2-5 记录同型。

静态（fail-closed，任何一条不满足即失败）：
 1. 证据文件齐全：legacy 参考、入库生成器、预算门禁、模块 .h/.cpp、差分测试、
    meson 目标、M2-4 等价记录文档。
 2. legacy 参考 provenance：记录迁移前**工作树** d3d9_device.cpp 的 SHA-256、
    字节数、捕获时间，显式说明与 M1 git-blob provenance 的差异；文件自身
    SHA-256 / 字节数与登记值一致（故意改参考文本 -> 红）。
 3. 正文逐字节：本门禁**自己**从 .inc 抽取 9 个定义、去掉签名得到正文，与模块
    .cpp 每个 BEGIN/END 标记之间的正文逐字节比较（不依赖生成器自证）。
 4. device.cpp：8 个符号的行首站点必须为 0（用预算门禁同一条 DEF_RE 与同一条
    第 0 列规则在门禁内复算），且 9 个 M2-4 迁移注释锚必须逐条存在。
 5. A9 / A10 两条死代码的裁定落地状态（各自一次独立改动）：A10（未实例化
    function template）已**删除**；A9 War3SemanticBuildWorldPaletteIfNeeded
    已**迁出**到本门禁已在管的调色板模块。两条的定义/模板文本都不允许再出现
    在 device.cpp（反方向 fail-closed 断言；A9 的正文在模块里，由
    AutoTest/test_war3_palette_a9_migration_equivalence_static.py 与生成器
    --a9 双向校验）。
 6. FROZEN_M2_4 的键集合必须恰好等于 COVERED，每条 baseline 等于在 pre-M2-4
    快照上实测的行首站点数、budget 为 0；且原始 FROZEN 块里 A8 仍登记为
    (1, 1)（否则 M1 等价门禁会把 A8 误判成 M1 迁出）。
 7. meson：差分测试目标仍链接真实模块 .cpp；生成器 CLI 有 --m2-4。
 8. 模块 .h 必须逐条声明 8 个符号；模块 .cpp 每个函数签名必须恰好 1 处。

动态（可执行证据，构建产物存在时执行）：
 9. 差分测试在 4 组 env 矩阵下 exit 0、全过，总检查数 >= 各矩阵登记下限；
    并打印 M2-4 电池自身的检查数，要求 >= M24_MIN_CHECKS。
10. --probe-m2-4 在 2 组 env 下逐行对照**独立期望值表**（10 字段绝对值），
    并同时要求 module == legacy。期望值先按源码语义手工推导（hash 由门禁内
    独立实现的 FNV-1a + float32 位模式复算），再与实跑对照。

边界（本门禁不覆盖，不得据此宣称"职责已迁完"或"已稳定"）：
    - device.cpp 侧调用点编排（M2-5 的表 B：B1/B2/B4/B5/B6-B11）未迁移、也未被
      本门禁的差分覆盖；
    - A9 / A10 死代码的最终裁定由主线程给出（A10 删除、A9 迁出），两条均已落地；
      本门禁只钉住"两条定义文本都不再出现在 device.cpp"，A9 迁移的**等价证据**
      在独立门禁内（本门禁不覆盖 A9 的正文/差分）；
    - A8 的 6 个 bounds 调用点未被显式驱动（它们只是解析到模块定义，语义是纯
      switch 查表，由 device.cpp 全量构建与既有门禁覆盖）；
    - 差分只证明"同一输入 + 同一替身世界下两个实现一致"；IsReadableRange 在
      宿主机是替身，不证明生产内存原语本身；
    - 不覆盖 GPU / Vulkan / 实机画面与性能。
"""

from pathlib import Path
import hashlib
import os
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]

BUDGET_GATE = ROOT / "AutoTest/test_device_semantic_responsibility_budget_static.py"
GENERATOR = ROOT / "AutoTest/gen_war3_live_palette_selection_legacy_reference.py"
MODULE_H = ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.h"
MODULE_CPP = ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp"
M24_INC = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_m2_4_legacy_reference.inc"
)
TEST_CPP = (
    ROOT / "src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp"
)
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
MESON = ROOT / "src/d3d9/meson.build"
TEST_EXE = ROOT / "build32/src/d3d9/war3_live_palette_selection_test.exe"
RECORD_DOC = ROOT / "docs/plan/2026-09-18-m2-4-migration-equivalence-record.md"

# 迁移前（M2-5 迁移已应用、M2-4 未应用）的**未提交工作树** device.cpp 身份
# （= M2-5 记录的"迁移后 device.cpp"，同一 SHA，交叉验证闭合）。
PRE_M2_4_DEVICE_SHA256 = (
    "7391F3071F7D2CF32583C57FF1D914FC59AA765A5EDD5749F5D3A8FCBD8A0844"
)
PRE_M2_4_DEVICE_SIZE = 2_254_826
PRE_M2_4_CAPTURED_AT = "2026-09-18T03:34:55"
M24_INC_SHA256 = (
    "E9CB8931B427CB87172BCC96B03497103116DC30F1C1197D3A00FF218F2F5899"
)
M24_INC_SIZE = 9_397

# 本门禁覆盖的 M2-4 符号（= 预算门禁 FROZEN_M2_4 的键集合）与 baseline
# （在 pre-M2-4 快照上用预算门禁同一 DEF_RE / 同一第 0 列规则实测）。
COVERED = {
    "War3SemanticPaletteInPlaceAppendRuntime": 1,
    "War3SemanticDrawTimePoseRuntime": 1,
    "War3SemanticTranslationFinite": 1,
    "War3SemanticTranslationDistanceSq": 1,
    "War3SemanticPaletteStorageReadable": 1,
    "War3SemanticPaletteLooksModelLocal": 2,
    "War3SemanticBoundsRadiusForObjectKind": 1,
    "War3SemanticHashMatrix4": 1,
}

# (key, 设备侧签名, 模块侧签名, pre-M2-4 行号)。与生成器 --m2-4 同一张表，
# 但本门禁**独立**读取 .inc 与模块 .cpp 复算正文一致性。
SYMBOLS = [
    ("War3SemanticPaletteInPlaceAppendRuntime",
     "inline bool War3SemanticPaletteInPlaceAppendRuntime() {",
     "bool War3SemanticPaletteInPlaceAppendRuntime() {",
     (1302, 1309)),
    ("War3SemanticDrawTimePoseRuntime",
     "bool War3SemanticDrawTimePoseRuntime() {",
     "bool War3SemanticDrawTimePoseRuntime() {",
     (2079, 2087)),
    ("War3SemanticBoundsRadiusForObjectKind",
     "float War3SemanticBoundsRadiusForObjectKind(uint8_t objectKind) {",
     "float War3SemanticBoundsRadiusForObjectKind(uint8_t objectKind) {",
     (7144, 7160)),
    ("War3SemanticTranslationDistanceSq",
     "float War3SemanticTranslationDistanceSq(const Matrix4& a, const Matrix4& b) {",
     "float War3SemanticTranslationDistanceSq(const Matrix4& a, const Matrix4& b) {",
     (7177, 7182)),
    ("War3SemanticTranslationFinite",
     "bool War3SemanticTranslationFinite(const Matrix4& m) {",
     "bool War3SemanticTranslationFinite(const Matrix4& m) {",
     (7184, 7187)),
    ("War3SemanticPaletteStorageReadable",
     "bool War3SemanticPaletteStorageReadable(const std::vector<Matrix4>& palette) {",
     "bool War3SemanticPaletteStorageReadable(const std::vector<Matrix4>& palette) {",
     (7189, 7196)),
    ("War3SemanticPaletteLooksModelLocal[pointer]",
     "bool War3SemanticPaletteLooksModelLocal(\n"
     "    const Matrix4* palette,\n"
     "    uint32_t paletteCount,\n"
     "    const Matrix4& worldTransform,\n"
     "    uint8_t objectKind,\n"
     "    bool checkReadable = true) {",
     "bool War3SemanticPaletteLooksModelLocal(\n"
     "    const Matrix4* palette,\n"
     "    uint32_t paletteCount,\n"
     "    const Matrix4& worldTransform,\n"
     "    uint8_t objectKind,\n"
     "    bool checkReadable) {",
     (7209, 7263)),
    ("War3SemanticPaletteLooksModelLocal[vector]",
     "bool War3SemanticPaletteLooksModelLocal(\n"
     "    const std::vector<Matrix4>& palette,\n"
     "    const Matrix4& worldTransform,\n"
     "    uint8_t objectKind) {",
     "bool War3SemanticPaletteLooksModelLocal(\n"
     "    const std::vector<Matrix4>& palette,\n"
     "    const Matrix4& worldTransform,\n"
     "    uint8_t objectKind) {",
     (7265, 7274)),
    ("War3SemanticHashMatrix4",
     "uint64_t War3SemanticHashMatrix4(const Matrix4& matrix) {",
     "uint64_t War3SemanticHashMatrix4(const Matrix4& matrix) {",
     (7292, 7301)),
]

# device.cpp 里应留下的 9 个 M2-4 迁移注释锚（逐条必须存在）。
MARKERS = [
    "// M2-4: War3SemanticPaletteInPlaceAppendRuntime ->",
    "// M2-4: War3SemanticDrawTimePoseRuntime ->",
    "// M2-4: War3SemanticBoundsRadiusForObjectKind ->",
    "// M2-4: War3SemanticTranslationDistanceSq ->",
    "// M2-4: War3SemanticTranslationFinite ->",
    "// M2-4: War3SemanticPaletteStorageReadable ->",
    "// M2-4: War3SemanticPaletteLooksModelLocal[pointer] ->",
    "// M2-4: War3SemanticPaletteLooksModelLocal[vector] ->",
    "// M2-4: War3SemanticHashMatrix4 ->",
]

# A9 / A10 两条死代码的裁定落地状态（2026-09-18 主线程裁定，各自一次独立
# 改动）：
#   A10 War3SemanticVectorStorageReadable（未实例化 function template）：
#       **已删除** ⇒ 定义文本必须从 device.cpp 消失。
#   A9 War3SemanticBuildWorldPaletteIfNeeded：**已迁出**到本门禁已在管的
#       调色板模块（迁移不是删除：正文在模块 .cpp 里，与本 .inc 同源，由
#       AutoTest/test_war3_palette_a9_migration_equivalence_static.py 与
#       生成器 --a9 双向校验）。
# 两条都不允许再出现在 device.cpp（删除/迁移后的反方向 fail-closed 断言）。
DEAD_CODE_ABSENT_FROM_DEVICE = [
    "bool War3SemanticVectorStorageReadable(const std::vector<T>& values,",
    "[[maybe_unused]] void War3SemanticBuildWorldPaletteIfNeeded(",
]

PALETTE_DIAG = "DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS"
SKIN_CONTRACT = "DXVK_WAR3_SKIN_PALETTE_CONTRACT"

# 差分测试各 env 矩阵的总检查数下限（实测 default 37920643 / palette-diag
# 37920642 / contract 34432531 / contract-diag 34432530；按下限约 89% 登记，
# 留余量但仍能挡住整体空壳化）。M2-4 电池自身另有精确下限。
MATRIX_MIN_CHECKS = {
    "default": 33_700_000,
    "palette-diag": 33_700_000,
    "contract": 30_600_000,
    "contract-diag": 30_600_000,
}

# M2-4 电池自身检查数：7 个 palette variant x 2 可读性 x 8 world x 11 count x
# 256 kind x 2 checkReadable = 630,784 次调用；加 22 次 nullptr 场景与 200,000
# 次随机 = 830,806 次调用，每次 9 条（8 字段 + 1 次结构 memcmp）= 7,477,254，
# 加 3 次段末累计 memcmp = 7,477,257。登记下限取约 99%。
M24_MIN_CHECKS = 7_400_000
M24_EXACT_CHECKS = 7_477_257

CHECK_RE = re.compile(
    r"war3 live palette selection equivalence:\s*(\d+)/(\d+)\s*checks passed"
)
M24_BATTERY_RE = re.compile(r"M2-4 battery:\s*(\d+) checks,\s*(\d+) failures")
PROBE_RE = re.compile(
    r"probe-m2-4\s+(\S+)\s+module=([0-9,]+)\s+legacy=([0-9,]+)\s*$"
)

# 独立期望值表：每行 10 个字段，顺序 =
#   [A5 hash(world), A8 radiusBits(kind), A7 distBits(first, world),
#    A6 finite(world), A6 finite(paletteFirst), A3 readable,
#    A4 pointer 重载, A4 vector 重载, A1 env, A2 env]
# 前 8 个与 env 无关；最后 2 个按 env 矩阵登记（见 ENV_EXPECT）。
# 手工推导依据（float32 全为精确可表示值）：
#   * hash：门禁内独立实现 FNV-1a 64（offset 0xcbf29ce484222325，prime
#     0x100000001b3）对 4x4 float 位模式逐分量复算；
#   * radius：0=Unknown->0（调用方 fallback 260），1=Unit 260，2=Building 900，
#     3=Destructible 750，4=Item 220，5=Effect 900，255->0；
#   * dist：first/world 平移差平方，(1000-64)^2=876096、(1000-1000)^2=0、
#     (1e6-1000)^2 按 float32 舍入 = 1399348607 位模式、64^2=4096；
#   * A4 判定：guardRadius=max(384, r*1.5)，Unit -> 390，threshold=152100；
#     closestPaletteMagSq=64^2=4096 <= max(1024, 780)^2；
#     所以 distSq 876096 > 152100 -> true；world 平移到 1000 且 palette 同点 ->
#     distSq 0 -> false；palette 平移 1e6 -> mag 超 localMagLimit -> false。
PROBE_EXPECT = [
    ("a4-local-world-far",
     [9287544938805421157, 1132593152, 1230365696, 1, 1, 1, 1, 1, None, None]),
    ("a4-world-palette-near",
     [9287544938805421157, 1132593152, 0, 1, 1, 1, 0, 0, None, None]),
    ("a4-palette-too-far",
     [9287544938805421157, 1132593152, 1399348607, 1, 1, 1, 0, 0, None, None]),
    ("a4-palette-nan",
     [9287544938805421157, 1132593152, 2143289344, 1, 0, 1, 0, 0, None, None]),
    ("a4-palette-inf",
     [9287544938805421157, 1132593152, 2139095040, 1, 0, 1, 0, 0, None, None]),
    ("a4-empty-palette",
     [9287544938805421157, 1132593152, 1230365696, 1, 1, 0, 0, 0, None, None]),
    ("a4-count-257",
     [9287544938805421157, 1132593152, 1230365696, 1, 1, 0, 1, 0, None, None]),
    ("a4-unreadable",
     [9287544938805421157, 1132593152, 1230365696, 1, 1, 0, 0, 0, None, None]),
    ("a4-check-off-unreadable",
     [9287544938805421157, 1132593152, 1230365696, 1, 1, 0, 1, 0, None, None]),
    ("a4-null-ptr",
     [9287544938805421157, 1132593152, 1230365696, 1, 1, 0, 0, 0, None, None]),
    ("a4-world-nan",
     [2931326237932479589, 1132593152, 2143289344, 0, 1, 1, 0, 0, None, None]),
    ("a4-world-zero",
     [14920258192649708645, 1132593152, 1166016512, 1, 1, 1, 0, 0, None, None]),
    ("a8-kind-0-unknown",
     [9287544938805421157, 0, 1230365696, 1, 1, 1, 1, 1, None, None]),
    ("a8-kind-1-unit",
     [9287544938805421157, 1132593152, 1230365696, 1, 1, 1, 1, 1, None, None]),
    ("a8-kind-2-building",
     [9287544938805421157, 1147207680, 1230365696, 1, 1, 1, 0, 0, None, None]),
    ("a8-kind-3-destructible",
     [9287544938805421157, 1144750080, 1230365696, 1, 1, 1, 0, 0, None, None]),
    ("a8-kind-4-item",
     [9287544938805421157, 1130102784, 1230365696, 1, 1, 1, 1, 1, None, None]),
    ("a8-kind-5-effect",
     [9287544938805421157, 1147207680, 1230365696, 1, 1, 1, 0, 0, None, None]),
    ("a8-kind-255",
     [9287544938805421157, 0, 1230365696, 1, 1, 1, 1, 1, None, None]),
    ("a5-hash-identity",
     [14920258192649708645, 1132593152, 1166016512, 1, 1, 0, 0, 0, None, None]),
]

# env 矩阵 -> (A1 in-place append, A2 draw-time pose) 期望
ENV_EXPECT = {
    "default": (1, 0),
    "flipped": (0, 1),
}
PROBE_MATRICES = [
    ("default", {}),
    (
        "flipped",
        {
            "DXVK_WAR3_SEMANTIC_PALETTE_IN_PLACE_APPEND": "0",
            "DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE": "1",
        },
    ),
]


def fail(message):
    raise AssertionError(message)


def read(path):
    if not path.is_file():
        fail("fail-closed: 缺少必需文件 %s" % path)
    return path.read_text(encoding="utf-8")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest().upper()


def sanitized_env(overrides):
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("DXVK_WAR3_")
    }
    env.update(overrides)
    return env


def extract_definition(text, signature):
    start = 0
    while True:
        index = text.find(signature, start)
        if index < 0:
            fail("找不到定义锚：%r" % signature[:60])
        brace = text.index("{", index)
        semi = text.find(";", index, brace)
        if semi < 0:
            end = text.index("\n}\n", brace)
            line = text.count("\n", 0, index) + 1
            return line, text[index:end + 3]
        start = index + 1


def fnv1a_floats(rows):
    """独立于 C++ 的 FNV-1a 64 复算（float32 位模式，行主序）。"""
    import struct
    hash_value = 0xCBF29CE484222325
    for row in rows:
        for value in row:
            bits = struct.unpack("<I", struct.pack("<f", value))[0]
            hash_value = ((hash_value ^ bits) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return hash_value


def parse_frozen_m2_4(text):
    start = text.index("FROZEN_M2_4 = {")
    end = text.index("\n}", start)
    frozen = {}
    for match in re.finditer(
        r"\"([A-Za-z_][A-Za-z0-9_]*)\"\s*:\s*\((\d+),\s*(\d+)\)",
        text[start:end],
    ):
        frozen[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    return frozen


def parse_frozen_base_blocks(text):
    """解析**原始** FROZEN 块（到第一个行首 } 为止）。"""
    start = text.index("FROZEN = {")
    end = text.index("\n}", start)
    frozen = {}
    for match in re.finditer(
        r"\"([A-Za-z_][A-Za-z0-9_]*)\"\s*:\s*\((\d+),\s*(\d+)\)",
        text[start:end],
    ):
        frozen[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    return frozen


def check_static():
    budget_text = read(BUDGET_GATE)
    generator_text = read(GENERATOR)
    module_h = read(MODULE_H)
    module_cpp = read(MODULE_CPP)
    m24_text = read(M24_INC)
    test_text = read(TEST_CPP)
    device_text = read(DEVICE_CPP)
    meson_text = read(MESON)
    read(RECORD_DOC)

    # --- 1. legacy 参考 provenance 与自身身份 ---
    if PRE_M2_4_DEVICE_SHA256 not in m24_text:
        fail("legacy 参考缺少迁移前 d3d9_device.cpp 的 SHA-256 provenance")
    if str(PRE_M2_4_DEVICE_SIZE) not in m24_text:
        fail("legacy 参考缺少迁移前 device.cpp 的字节数 provenance")
    if PRE_M2_4_CAPTURED_AT not in m24_text:
        fail("legacy 参考缺少迁移前工作树文件的捕获时间 provenance")
    if "work-tree file hash, not a git blob hash" not in m24_text:
        fail("legacy 参考缺少 worktree-vs-git-blob 差异说明")
    if "gen_war3_live_palette_selection_legacy_reference.py" not in m24_text:
        fail("legacy 参考没有点名入库生成器")
    actual_inc_sha = sha256_bytes(M24_INC.read_bytes())
    if actual_inc_sha != M24_INC_SHA256:
        fail(
            "legacy 参考文件哈希与登记值不一致："
            "%s != %s" % (actual_inc_sha, M24_INC_SHA256)
        )
    if M24_INC.stat().st_size != M24_INC_SIZE:
        fail(
            "legacy 参考字节数与登记值不一致：%d != %d"
            % (M24_INC.stat().st_size, M24_INC_SIZE)
        )

    # --- 2. 正文逐字节：本门禁自己抽 .inc，与模块 BEGIN/END 之间比较 ---
    inc_lf = m24_text.replace("\r\n", "\n")
    for key, dev_sig, mod_sig, lines in SYMBOLS:
        if inc_lf.count(dev_sig) != 1:
            fail("legacy 参考里 %s 的设备签名不是恰好 1 处" % key)
        line, definition = extract_definition(inc_lf, dev_sig)
        span = (line - 0, line + definition.count("\n") - 1)
        if not definition.startswith(dev_sig):
            fail("legacy 参考里 %s 的定义不以签名开头" % key)
        body = definition[len(dev_sig):-2]
        if module_cpp.count(mod_sig) != 1:
            fail("模块 .cpp 里 %s 的签名不是恰好 1 处" % key)
        begin = (
            "    // --- BEGIN M2-4 body: %s (pre-M2-4 d3d9_device.cpp :%d-%d) ---"
            % (key, lines[0], lines[1])
        )
        end = "    // --- END M2-4 body: %s ---" % key
        if module_cpp.count(begin) != 1 or module_cpp.count(end) != 1:
            fail("模块 .cpp 里 %s 的 BEGIN/END 标记不是恰好 1 对" % key)
        begin_end = module_cpp.index("\n", module_cpp.index(begin)) + 1
        module_body = module_cpp[begin_end:module_cpp.index(end)]
        if module_body != body:
            fail(
                "模块 .cpp 的 %s 正文与 legacy 参考正文逐字节不一致"
                "（迁移文本已漂移）" % key
            )

    # --- 3. 模块 .h 声明逐条存在 ---
    declarations = [
        "bool War3SemanticPaletteInPlaceAppendRuntime();",
        "bool War3SemanticDrawTimePoseRuntime();",
        "float War3SemanticTranslationDistanceSq(const Matrix4& a, const Matrix4& b);",
        "bool War3SemanticTranslationFinite(const Matrix4& m);",
        "bool War3SemanticPaletteStorageReadable(const std::vector<Matrix4>& palette);",
        "float War3SemanticBoundsRadiusForObjectKind(uint8_t objectKind);",
        "uint64_t War3SemanticHashMatrix4(const Matrix4& matrix);",
    ]
    for declaration in declarations:
        if declaration not in module_h:
            fail("模块 .h 缺少声明：%s" % declaration)
    if module_h.count("bool War3SemanticPaletteLooksModelLocal(") != 2:
        fail("模块 .h 的 War3SemanticPaletteLooksModelLocal 声明不是 2 个重载")
    if "bool checkReadable = true);" not in module_h:
        fail("模块 .h 丢失 checkReadable 默认实参（M2-2 先例：默认值在声明上）")

    # --- 4. device.cpp：行首站点为 0 + 9 个迁移注释锚 + A9/A10 未被删除 ---
    budget_sites = read(BUDGET_GATE)
    def_start = budget_sites.index("DEF_RE = re.compile(")
    expr_start = budget_sites.index("(", def_start) + 1
    expr_end = budget_sites.index("\n)", expr_start)
    local_def_re = re.compile(
        eval("(" + budget_sites[expr_start:expr_end] + ")")
    )
    skip_src = re.search(
        r"SKIP_NAMES = \{(.*?)\}", budget_sites, re.DOTALL
    )
    if skip_src is None:
        fail("无法从预算门禁解析 SKIP_NAMES")
    skip_names = eval("{" + skip_src.group(1) + "}")
    sites = {}
    for line in device_text.replace("\r\n", "\n").splitlines():
        if not line or line[:1] in (" ", "\t"):
            continue
        if line.startswith(("//", "#", "/*", "*")):
            continue
        found = local_def_re.match(line)
        if not found:
            continue
        name = found.group(1)
        if name in skip_names:
            continue
        sites[name] = sites.get(name, 0) + 1
    for name in COVERED:
        if sites.get(name, 0) != 0:
            fail(
                "device.cpp 仍存在已迁出符号 %s 的行首站点 %d 个（回流/双实现）"
                % (name, sites.get(name, 0))
            )
    for marker in MARKERS:
        if marker not in device_text:
            fail("device.cpp 缺少 M2-4 迁移注释锚：%s" % marker)
    for dead in DEAD_CODE_ABSENT_FROM_DEVICE:
        if dead in device_text:
            fail(
                "device.cpp 里已裁定处置的死代码仍然存在（删除/迁移未落地或回流）："
                "%s" % dead
            )

    # --- 5. FROZEN_M2_4 键集合 / baseline / budget ---
    frozen = parse_frozen_m2_4(budget_text)
    if set(frozen) != set(COVERED):
        fail(
            "FROZEN_M2_4 与本门禁登记集合不一致：\n"
            "  仅在 FROZEN_M2_4: %s\n  仅在 COVERED: %s"
            % (sorted(set(frozen) - set(COVERED)), sorted(set(COVERED) - set(frozen)))
        )
    for name, baseline in COVERED.items():
        got = frozen[name]
        if got != (baseline, 0):
            fail(
                "FROZEN_M2_4[%s] = %s，期望 (%d, 0)（baseline 必须是在 pre-M2-4"
                " 快照上实测的行首站点数，budget 必须为 0）" % (name, got, baseline)
            )
    base_blocks = parse_frozen_base_blocks(budget_text)
    if base_blocks.get("War3SemanticBoundsRadiusForObjectKind") != (1, 1):
        fail(
            "原始 FROZEN 块里 A8 必须仍是 (1, 1)，否则 M1 等价门禁会把 A8 误判为"
            " M1 迁出而无等价证据"
        )

    # --- 6. FNV 期望值自证（独立实现必须复算出登记值） ---
    identity_rows = [
        (1.0, 0.0, 0.0, 0.0),
        (0.0, 1.0, 0.0, 0.0),
        (0.0, 0.0, 1.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    ]
    world_rows = [
        (1.0, 0.0, 0.0, 0.0),
        (0.0, 1.0, 0.0, 0.0),
        (0.0, 0.0, 1.0, 0.0),
        (1000.0, 0.0, 0.0, 1.0),
    ]
    if fnv1a_floats(world_rows) != PROBE_EXPECT[0][1][0]:
        fail("门禁内 FNV-1a 复算与登记的世界矩阵 hash 不一致")
    if fnv1a_floats(identity_rows) != PROBE_EXPECT[19][1][0]:
        fail("门禁内 FNV-1a 复算与登记的 identity hash 不一致")

    # --- 7. meson / 生成器 CLI / 差分测试引用 ---
    target = re.search(
        r"war3_live_palette_selection_test = executable\((.*?)\n\)",
        meson_text,
        re.DOTALL,
    )
    if target is None:
        fail("meson.build 里找不到 war3_live_palette_selection_test 目标")
    if "war3/semantic/war3_live_palette_selection.cpp" not in target.group(1):
        fail("差分测试目标没有链接真实模块 .cpp")
    if "--m2-4" not in generator_text:
        fail("入库生成器缺少 --m2-4 模式")
    if "war3_live_palette_selection_m2_4_legacy_reference.inc" not in test_text:
        fail("差分测试没有 include M2-4 legacy 参考")
    for primitive in ("RunProbeM24", "--probe-m2-4", "DiffM24Systematic",
                      "DiffM24Random", "M2-4 battery:"):
        if primitive not in test_text:
            fail("差分测试缺少必需原语：%s" % primitive)
    for name in COVERED:
        if ("sem::" + name) not in test_text:
            fail("差分测试没有驱动模块实现：%s" % name)


def check_dynamic():
    if not TEST_EXE.is_file():
        print(
            "NOTE 差分测试可执行文件不存在，跳过动态等价部分：%s"
            "（静态部分仍然 fail-closed）" % TEST_EXE.relative_to(ROOT)
        )
        return

    matrices = [
        ("default", {}),
        ("palette-diag", {PALETTE_DIAG: "1"}),
        ("contract", {SKIN_CONTRACT: "1"}),
        ("contract-diag", {SKIN_CONTRACT: "1", PALETTE_DIAG: "1"}),
    ]
    for name, overrides in matrices:
        proc = subprocess.run(
            [str(TEST_EXE)],
            cwd=str(ROOT),
            env=sanitized_env(overrides),
            capture_output=True,
            text=True,
            timeout=600,
        )
        match = CHECK_RE.search(proc.stdout)
        if proc.returncode != 0 or match is None:
            fail(
                "差分测试 env 矩阵 %s 失败 exit=%d\nstdout:\n%s\nstderr:\n%s"
                % (name, proc.returncode, proc.stdout[-4000:], proc.stderr[-4000:])
            )
        passed, total = int(match.group(1)), int(match.group(2))
        floor = MATRIX_MIN_CHECKS[name]
        if passed != total or total < floor:
            fail(
                "差分测试 env 矩阵 %s: %d/%d（要求全过且 >= %d）"
                % (name, passed, total, floor)
            )
        battery = M24_BATTERY_RE.search(proc.stdout)
        if battery is None:
            fail("差分测试 env 矩阵 %s 没有打印 M2-4 电池检查数" % name)
        battery_checks = int(battery.group(1))
        battery_failures = int(battery.group(2))
        if battery_failures != 0:
            fail(
                "差分测试 env 矩阵 %s: M2-4 电池有 %d 条失败"
                % (name, battery_failures)
            )
        if battery_checks != M24_EXACT_CHECKS or battery_checks < M24_MIN_CHECKS:
            fail(
                "差分测试 env 矩阵 %s: M2-4 电池检查数 %d != 登记值 %d"
                "（差分被删成空壳？）"
                % (name, battery_checks, M24_EXACT_CHECKS)
            )

    for name, overrides in PROBE_MATRICES:
        proc = subprocess.run(
            [str(TEST_EXE), "--probe-m2-4"],
            cwd=str(ROOT),
            env=sanitized_env(overrides),
            capture_output=True,
            text=True,
            timeout=300,
        )
        if proc.returncode != 0:
            fail(
                "--probe-m2-4 场景 %s exit=%d\nstdout:\n%s\nstderr:\n%s"
                % (name, proc.returncode, proc.stdout[-4000:], proc.stderr[-4000:])
            )
        env_in_place, env_draw_time_pose = ENV_EXPECT[name]
        observed = {}
        for line in proc.stdout.splitlines():
            match = PROBE_RE.match(line)
            if not match:
                continue
            scenario, module_fields, legacy_fields = match.groups()
            if module_fields != legacy_fields:
                fail(
                    "--probe-m2-4 场景 %s: %s module=%s legacy=%s"
                    % (name, scenario, module_fields, legacy_fields)
                )
            observed[scenario] = [int(f) for f in module_fields.split(",")]
        for scenario, expected in PROBE_EXPECT:
            fields = observed.get(scenario)
            if fields is None:
                fail("--probe-m2-4 场景 %s: 缺少 %s 的输出" % (name, scenario))
            expected = list(expected)
            expected[8] = env_in_place
            expected[9] = env_draw_time_pose
            if fields != expected:
                fail(
                    "--probe-m2-4 场景 %s: %s 期望 %s，实测 %s"
                    % (name, scenario, expected, fields)
                )


def main():
    check_static()
    check_dynamic()
    print(
        "war3 palette m2-4 equivalence gate static checks passed"
        "（M2-4 已迁出符号 %d 个 / 定义 %d 份；legacy 参考 %d B / SHA-256 %s；"
        "正文逐字节（门禁独立复算）；pre-M2-4 device.cpp %d B / %s；"
        "probe-m2-4 场景 %d × %d 组 env；M2-4 电池下限 %d（登记精确值 %d））"
        % (
            len(COVERED),
            len(SYMBOLS),
            M24_INC_SIZE,
            M24_INC_SHA256[:16],
            PRE_M2_4_DEVICE_SIZE,
            PRE_M2_4_DEVICE_SHA256[:16],
            len(PROBE_EXPECT),
            len(PROBE_MATRICES),
            M24_MIN_CHECKS,
            M24_EXACT_CHECKS,
        )
    )


if __name__ == "__main__":
    main()
