"""固定门禁：诊断/候选构建配置不得冒充发布默认。

背景（2026-09-16，架构复审目标项"建立防止回退的固定门禁"）：
    项目纪律要求"候选不得冒充稳定"、"发布候选须在关闭诊断选项后重建"。
    本门禁钉住 meson 诊断/候选选项的**默认值必须为 false**，以及源码层宏缺省必须为 0，
    防止有人把候选/诊断配置"焊进"树里，使普通构建实际上变成诊断构建。

本测试只读取文本，不参与构建；它不证明行为正确，只防止默认值被悄悄翻转。
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
OPTIONS = (ROOT / "meson_options.txt").read_text(encoding="utf-8")
D3D9_MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")
SKIN_HEADER = (
    ROOT / "src/d3d9/war3/render/war3_skin_palette_selection.h"
).read_text(encoding="utf-8")

OPT_RE = re.compile(
    r"option\(\s*'([^']+)'\s*,\s*type\s*:\s*'boolean'\s*,\s*value\s*:\s*(true|false)"
)

# 1) 已知诊断/候选选项：必须存在且默认 false
MUST_BE_OFF = [
    "warvk_data_collection_tree_dev",
    "warvk_internal_frame_recorder",
    "warvk_skin_palette_contract_candidate",
    "warvk_shadow_observers_dev",
    "warvk_rts_shadow_candidate_dev",
    "warvk_coherent_up_index_trim_dev",
    "warvk_current_up_shadow_replay_dev",
    "warvk_coherent_real_index_trim_dev",
    "warvk_coherent_real_perf_candidate_dev",
    "warvk_device_address_binding_report_dev",
]

found = {m.group(1): m.group(2) for m in OPT_RE.finditer(OPTIONS)}

for name in MUST_BE_OFF:
    assert name in found, (
        f"meson_options.txt 缺少诊断/候选选项 {name}（若已重命名，请同步更新本门禁）"
    )
    assert found[name] == "false", (
        f"{name} 必须默认 false（诊断/候选配置不得冒充发布默认），当前 {found[name]}"
    )

# 2) 通用规则：任何 warvk_* 且含 _dev / candidate 的布尔选项都不得默认 true
for name, value in found.items():
    if name.startswith("warvk_") and ("_dev" in name or "candidate" in name):
        assert value == "false", (
            f"{name} 属诊断/候选选项，必须默认 false，当前 {value}"
        )

# 3) 源码层宏缺省：skin palette 合同宏在头文件中的 fallback 必须是 0
m = re.search(
    r"#\s*define\s+WARVK_SKIN_PALETTE_CONTRACT_DEFAULT\s+(\S+)", SKIN_HEADER
)
assert m, "war3_skin_palette_selection.h 缺少 WARVK_SKIN_PALETTE_CONTRACT_DEFAULT 的 fallback 定义"
assert m.group(1).rstrip() == "0", (
    "WARVK_SKIN_PALETTE_CONTRACT_DEFAULT 的源码缺省必须为 0，当前 " + m.group(1)
)

# 4) meson 只允许在显式 option 守卫下开启对应宏
GUARDED = [
    ("-DWARVK_SKIN_PALETTE_CONTRACT_DEFAULT=1", "warvk_skin_palette_contract_candidate"),
    ("-DWARVK_DATA_COLLECTION_TREE_DEV=1", "warvk_data_collection_tree_dev"),
]
lines = D3D9_MESON.split("\n")
for needle, option in GUARDED:
    # 只检查**生产 d3d9.dll 参数**（d3d9_cpp_args）：测试可执行目标允许直接开 DEV
    # 以便对 DEV=1 实现做真实编译与运行验证。
    prod = [
        i for i, l in enumerate(lines) if needle in l and "d3d9_cpp_args" in l
    ]
    assert prod, f"src/d3d9/meson.build 未找到受守卫的生产启用点 {needle}（d3d9_cpp_args）"
    for i in prod:
        window = "\n".join(lines[max(0, i - 3):i])
        assert f"get_option('{option}')" in window, (
            f"生产启用点 {needle} 必须位于 if get_option('{option}') 守卫内，而不是无条件启用"
        )

print("diagnostic build options default-off static checks passed")
