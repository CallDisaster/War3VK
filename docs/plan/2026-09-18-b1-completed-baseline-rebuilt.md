# B1 完成：迁移后契约补丁使等价基线重建，全套件回到可控状态 — 2026-09-18

> 承接 `...-b1-premise-verified.md`（前提实证）。本文记录实施、结果与**精确**的剩余项。

## 1. 实施的补丁（具名 / 定点 / 可复算）

在生成器 `AutoTest/gen_war3_live_palette_selection_legacy_reference.py` 中新增：

```python
POST_MIGRATION_CONTRACT_PATCHES = [
    {"name": "R1-cold-cache-groupcount",
     "date": "2026-09-18",
     "rationale": ("冷缓存首次查询原先不校验 producer 记录的 groupCount，与热缓存路径的"
                   "数量规则不一致 … 依据 selection.cpp:325-332 既有契约与批次 4 步骤①的行为测试"),
     "old": "      currentSlotIndex = producerSlotIndex;\n",
     "new": <插入 ProducerGroupCountCovers 检查 + 拒绝>}
]

def apply_post_migration_contract_patches(text):
    # 锚点必须**恰好匹配一次**，否则 raise SystemExit —— 绝不静默产出错误的参考
```

并在 `main_m2_2` 中：先 `body, applied = apply_post_migration_contract_patches(chr(10).join(chunks))`，
再组装 header；header 新增 provenance 段列出已应用的补丁（名称/日期/依据）；
`{chr(10).join(chunks)}` 改为 `{body}`。

## 2. 结果

### 2.1 参考文件身份变更（**有据可查**）

| | 大小 | SHA-256 |
| --- | --- | --- |
| 补丁前 | 36,025 B | `02FF8AFE5A30E3710D651BC300D1A7BB507B4DF34E6E6E394D6FA3CFD1D6410C` |
| **补丁后** | **37,375 B** | **`3D258F48355353B948ADDAB5266524DD07EEBF849B186C329DB2C54528274C1B`** |

新 `.inc` 中补丁可被直接审阅：provenance 在 **line 19-23**，本体在 **line 398-401**
（`// --- POST-MIGRATION CONTRACT PATCH [R1-cold-cache-groupcount] (2026-09-18) ---` 与
`if (!dxvk::war3::render::surface::ProducerGroupCountCovers(...))` 的位置见文件）。

### 2.2 差分**清零**

```
test_war3_live_palette_selection_equivalence_static.py
  diffLines = 0
  EXIT      = 0   -> "war3 live palette selection equivalence gate static checks passed"
```

⇒ **行为差异已消除，且是靠"参照实现带上同一契约"消除的**，不是放行差异。
逐位等价比较的强度**未降低**。

### 2.3 同步的两处钉死断言

| 文件 | 改动 |
| --- | --- |
| `test_war3_live_palette_selection_equivalence_static.py` | `CHAIN_LEGACY_INC_SHA256` 更新为新 SHA（该文件是唯一钉死旧 SHA 的地方） |
| `test_device_palette_slot_cache_producer_confirmation_static.py` | 热缓存复核断言由**字面** `producerGroupCount >= requiredPaletteCount` 改为**调用共享规则** `ProducerGroupCountCovers(`，并新增"不得再出现字面比较" |

## 3. 全套件结果

```
全量静态脚本 : 258 scripts, 0 failed        <-- 本轮由 7 failed 降至 0
meson test   : 84 ok / 1 FAIL
               FAIL = warvk:war3_palette_object_wire_roundtrip（env 相关，见 §4）
ninja -C build32 -n : no work
```

B1 如预测一并解决了：5 个等价门禁包装器 + `m2_4_equivalence`（同一二进制）+ meson `war3_live_palette_selection`。

## 4. ⚠️ 仍剩 1 项未判定（不得含糊）

**meson `war3_palette_object_wire_roundtrip` FAIL**：该测试在**显式设置** `DXVK_WAR3_FRAME_EVIDENCE=1` 与
`DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1` 时**116/116 PASS**（多次实测）。
meson 不注入这两个 env，因此这是"**测试需要 env 前置而 meson 未提供**"的问题。
**尚未判定**它是否在本轮之前就已如此（即是否既有问题）。判定方法：
查该目标在 `meson.build` 中的 `test(...)` 是否带 `env:`；若不带，则与本次改动无关。

**在判定之前，不得声称"全门禁通过"。** 准确表述是：
**全量静态 258/258 通过；meson 84/85，剩 1 个环境前置相关问题待判定。**

## 5. 硬约束遵守情况

| 约束 | 状态 |
| --- | --- |
| 未声称全门禁通过 | ✅（§4 给出精确表述） |
| 未提交 / 未部署 | ✅ |
| 未用 git 写 | ✅（仅文件编辑与生成器重跑） |
| 修复不要求与旧错误等价 | ✅（重建基线而非放行差异，并已先完成步骤①的行为证明） |
| 未把隔离桌面数据当前台性能 | ✅（本轮无实机运行） |
| 现场 DLL / 用户视频设置 | `F275545B…` 未动；`519AFA69…` 备份在位；未改设置 |

## 6. 目标状态

批次 ①②③④ 的主体均已完成并验证；**唯一未闭合项**是 §4 的 meson env 判定。
**本轮不标记目标完成**，待该项判定后再评估。