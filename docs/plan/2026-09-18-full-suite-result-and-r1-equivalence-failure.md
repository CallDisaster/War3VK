# 完整测试套件结果与 R1 引发的等价门禁失败 — 2026-09-18（**必须处理**）

> 外部复审要求「再在同一最终检查点运行完整测试」。本轮执行后**套件并未全绿**。
> **本文不得被解读为"门禁通过"**；此前的门禁结论均针对**受影响的子集**，不是全量。

## 1. 实测结果（全量重建后、最新二进制）

```
ninja -C build32 -n                  -> no work
全量静态脚本                          -> 258 个脚本，7 失败
meson test                            -> 85 中 83 ok / 2 FAIL
```

### 1.1 静态失败清单（7）

- `test_semantic_build_thread_gate_static.py`
- `test_war3_live_palette_motion_equivalence_static.py`
- `test_war3_live_palette_selection_equivalence_static.py`
- `test_war3_palette_a9_migration_equivalence_static.py`
- `test_war3_palette_m2_4_equivalence_static.py`
- `test_war3_palette_m2_5b_equivalence_static.py`
- `test_war3_palette_taxonomy_equivalence_static.py`

### 1.2 meson 失败清单（2）

| 目标 | 状态 | 备注 |
| --- | --- | --- |
| `warvk:war3_palette_object_wire_roundtrip` | FAIL exit 1 | **直接运行（显式设置 `DXVK_WAR3_FRAME_EVIDENCE=1` + `..._PALETTE_OBJECT=1`）为 116/116 PASS** ⇒ 很可能是**环境变量前置差异**（meson 不注入该 env），需判定是既有问题还是回归 |
| `warvk:war3_live_palette_selection` | FAIL exit 1 | **判定为 R1 引入的真实差异**，见 §2 |

## 2. R1 引发的等价门禁失败（主因）

`test_war3_live_palette_selection_equivalence_static.py` 的差分输出：

```
DIFF line 2181 chain.slotIndex:     module=8          legacy=4294967295
DIFF line 2181 chain.servedDelta:   module=1          legacy=0
DIFF line 2181 chain.rejectedDelta: module=0          legacy=1
DIFF line 2181 chain.paletteWord:   module=14917...   legacy=0
```

机制：该门禁把**迁移后的模块实现**与 `war3_live_palette_selection_chain_legacy_reference.inc`（由 `AutoTest/gen_war3_live_palette_selection_legacy_reference.py` 从 pre-M2 快照生成）在同一输入域上逐字段对照。
**R1 有意改变了冷缓存路径的准入行为**，因此二者必然不同 ⇒ 门禁失败。

### 2.1 但差分方向与预期**相反**，必须先查清

R1 的效果应是**更严**（数量不足即拒绝）。而实测是：**模块接受（slotIndex=8）而 legacy 拒绝（0xFFFFFFFF）**。
这与"更严"方向相反，存在两种可能，**尚未判定**：

1. 该差分场景与 R1 无关，而是由**其他**差异造成（需逐场景定位）；
2. R1 的改动在此场景产生了**非预期**效果（例如 `queryProducerBindingSlot` 在某些路径下不填充 `producerGroupCount`，或插入位置改变了控制流）。

⇒ **在查清方向相反的原因之前，不得宣称 R1"行为符合预期"。**

## 3. 依外部复审原则应如何处理

复审已裁定：「搬代码时，应证明新旧行为相同；**修错误时，应证明新行为符合预期。不能要求修复继续与旧错误完全等价。**」

⇒ 正确做法**不是**回退 R1 以迎合旧参考，而是：

1. 先查清 §2.1 的方向问题（可能暴露 R1 的真实缺陷）；
2. 再为"有意的行为差异"建立**显式声明机制**（在等价门禁中登记该差异及其预期方向），而不是让门禁静默变红；
3. 其余 6 个失败门禁逐一判定属于"同一 R1 根因"还是"独立问题"。

## 4. 对既有结论的更正

- 此前多轮报告中「门禁通过」均指**受影响子集**（roundtrip / cost / runtime / memory / lifecycle / geometry / 静态门禁若干），**不是全量**；
- 检查点 c0 记录中「该树在诊断改动前有全门禁全绿记录（静态 256/256、meson 84/84）」引自更早的次班独立审核，**不代表当前树**；当前树为 **258 脚本 7 失败 / 85 中 2 失败**；
- 因此**目标尚不能标记完成**。

## 5. 边界

- 未提交、未部署；现场 DLL 仍是 `F275545B…`（`519AFA69…` 备份在位）；未改用户视频设置；
- 本轮新增的 `war3_skin_palette_admission_test`（5 passed / 0 failed）只覆盖**纯谓词**，不覆盖含表与内存读取的完整准入函数。