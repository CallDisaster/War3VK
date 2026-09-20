# 阶段 C · 步骤⑦（读方半边）：`data[4]` 成为 **版本相关的链型槽** — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**只改读方（Python）**，未改产品源码。

## 1. 为什么先做读方

`chainType` 上 wire 之后，**同一对象会出现两条条目**（⑤ 的目标）。若读方仍按**对象键**分组，
两条链会被并成一条 —— 比现状更坏的假链。因此**读方必须先能表达链型**，C++ 半边才能安全跟进。

本轮完成读方契约，**C++ 仍写 0**（= `RejectionRecovery`，即当前唯一存在的链类），
所以生产形状**没有变化**，行为不变。

## 2. 改动（`AutoTest/analyze_palette_object_evidence.py`）

```python
CHAIN_TYPE_SLOT=('data',4)
CHAIN_TYPES={0:'RejectionRecovery',1:'Observation'}
```

- `reserved_data_indices(version)`：**版本 ≥4** 时把 `data[4]` 从保留集合中移除
  （该函数本就是版本相关的，这是唯一需要的钩子）；
- 事件解码新增 `'chainType'`：v4 从 `data[4]` 解码（**未知值 ⇒ 整份拒绝**）；
  v1/v2/v3 恒为 `'RejectionRecovery'` —— 那三个版本**没有**链型载体，
  这不是猜测而是它们的合同（`data[4]` 在那些版本上被校验为保留零）。

## 3. 新增用例 `test_chain_type_slot_stays_reserved_before_v4`（**两侧都钉**）

| 版本 | `data[4]=1` | 期望 |
| --- | --- | --- |
| 1 / 2 / 3 | 写入 | **必须拒绝，且理由必须是"保留槽"**（`assertRaisesRegex(…, "reserved")`） |
| 4 | 写入 | **必须接受**（否则"版本门控"退化成"一律拒绝 `data[4]`"） |

夹具 `event()` 新增 `chain_type` 形参（否则无法写出非零槽来证明旧版本不接受它）。

**⇒ 分析套件 `Ran 102 tests — OK`**（原 101 + 新 1）。

## 4. 状态

```
=== static ===
STATIC: 259 scripts, 0 failed
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== wire ===
exit=0
CHECKS=1160 FAILURES=0
=== DLL（本轮只改读方，应未变）===
36297544 B  79B3C229AE7715252BB1BE171BB38F0AED3FD080346233C7FC513CE642EC2C83
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 不声称

- **不**声称链型已在**生产**上 wire：C++ 的 sink **仍未写** `data[4]`，因此 `chainType` 恒为
  `RejectionRecovery`；本轮只是让读方**能**表达它；
- **不**声称 `Find`/`Insert` 已按链型分区；**不**声称同一对象可同时持有两条链
  （`R→FirstSight` 改类缺陷**仍未修**）；
- **不**声称读方已按「对象键 × 链型」分组（**未做**，属 ⑧）；
- **不**声称阶段 C 主体完成；K3（跨帧判序）未做。

## 6. 下一步（⑤⑥⑦C++⑧，必须一次做齐）

```
⑤ Find/Insert 按 (对象键 × 链型 × 窗口) 匹配
⑥ 显式归属规则：S/E/D/ObjectGone/CloseWindow 在两类链并存时挂到哪条
⑦(C++) sink 写 data[4]=record.chainType（记录结构已有该字段，见 round 13）
⑧ 读方链分组键加入 chainType（现按对象键）
⑨ 负向探针：同对象两类链各自结算；链型与事件形状不符必须拒绝
```
