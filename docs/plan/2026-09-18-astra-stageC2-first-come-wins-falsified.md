# 阶段 C 第 2 步：「先到先得」被**证伪并撤回**；链型基础设施就位 — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本文件记录一次**失败的设计尝试及其撤回**，
> 以及仍然成立的结论。**树已回到全绿。**

## 1. 本轮目标

修掉复核的 `R → FirstSight` 缺陷：`NoteFirstSight` 对「已由拒绝建立的条目」**不 Insert**，
直接在该条目上发链首 ⇒ 拒绝恢复链被**改类**（实测 `inserted=0 / emitted=1`）。

## 2. 我尝试的方案（**已被证伪**）

**「先到先得」**：某键先由哪类入口建立条目，其后另一类入口**不得改类、也不得另建条目**，
只能显式计入 `droppedPayloadConflict`。我加了 `PaletteObjectChainType` + `Entry::chainType`
（建立时落定）+ 两条类型守卫。

## 3. 为什么它是错的 —— **Case 8 的 4 处失败**

```
FAIL: the first NoteFirstSight of an entry must be emitted
FAIL: a repeated NoteFirstSight on a LATER frame must NOT emit again (the chain head is per entry, not per frame)
FAIL: the repeated first sight must be counted as droppedDuplicatePerFrame
FAIL: exactly one first-sight emission per entry
[FAIL] 8 duplicate reject in one frame (21 checks, 4 failures)
```

⇒ 拒绝建立的条目**再也发不出观察链首** ⇒ 「先到先得」**把整条观察链丢掉了**。

**这与裁定的要求正好相反**：裁定要求 `FirstSight` **另建一条独立条目**（`inserted=1`），
两类链**各自独立存在**、同一对象可**同时**持有两条。因此"拒绝/放弃"作为过渡是**错误方向**，
不是"更保守的正确中间态" —— 它用**丢失观察**换取了**不改类**，而丢失观察同样是失真。

## 4. 撤回与保留

**已撤回**：两条类型守卫（连同其注释一并改写为"已证伪"的记录，避免后人重犯）。

**仍保留的（基础设施，**尚未参与查找**）**：
- `enum class PaletteObjectChainType { RejectionRecovery = 0u, Observation = 1u }`；
- `Entry::chainType`（**建立时落定**，字段注释写明"此后不得更改"及其理由）；
- `Insert(..., chainType)` 形参（两个调用点已分别传 `RejectionRecovery` / `Observation`）。

⚠️ **明确记档**：`chainType` 目前是**待用标签**，**没有**参与 `Find`/`Insert` 的键，
因此**不影响任何行为**。不得把它的存在当成"链型已实现"。

## 5. 结论：正确的修法必须一次做齐三件事

```
① 类型化查找键：Find/Insert 按 (对象键 × 链型) 匹配  —— 使两类链各自独立存在
② S/E/D/ObjectGone/CloseWindow 的**归属规则**        —— 二者并存时事件挂到哪条链，必须显式定义
③ 链型上 wire（v4：data[4]=chainType）+ 读方按版本接受 v4
```

**① 与 ③ 必须同时落地**：在链型尚未上 wire 之前让同一对象出现两条条目，
读方会按**对象键**把它们并成一条链 ⇒ 产生比现状更坏的假链。

**③ 是阻塞项**：因此本轮**不能**只做 ①。

## 6. 本轮状态（全绿）

```
ninja -C build32 -n : no work to do
meson               : Ok: 85  Fail: 0
AutoTest 全量静态    : 259 scripts, 0 failed
evidence test       : SUMMARY: 25 passed, 0 failed
wire roundtrip      : CHECKS=1141 FAILURES=0   CERTIFIED_SPEC_SATISFIED
候选 DLL            : 36,297,472 B
                      SHA-256 1646FAE8BCDB9B2C35CB5145E7C6B26F55AE86EC83959E5E005D7DE32E306E50
站点                : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                 : 无写操作
```

**一个附属修正**：我改了 `Insert` 的调用形状后，round 1 加的顺序断言锚定了旧字面
⇒ `test_semantic_build_thread_gate_static.py` 失败（`ValueError: substring not found`）。
按同一纪律**更新锚点、保留要求**（去掉尾部 `);`），现通过。

## 7. 不声称

- **不**声称链型已实现（`chainType` 未参与查找、未上 wire）；
- **不**声称 `R → FirstSight` 改类缺陷已修 —— **它仍未修**，且本轮证明了"用放弃来避免改类"是错的；
- **不**声称阶段 C 主体完成；wire 版本仍是 `firstSightUsed()` 动态 2/3。
