# 阶段 C 第 1 步：`NoteReject` 预算预检提前（完成+已锁）＋ 两条**未决红点** — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。阶段 A（doc-r1）、B（门禁恢复）已完成。
> **本轮是阶段 C 的第 1 步**，结果：**代码修复成立且已锁**，但**留下两个必须由下一轮解决的问题**。

## 1. 完成的修复（复核 P1，Astra 第 65 键反例）

**问题**：`NoteReject` **先 `Insert`（`:307`）后判非终态预算（`:348`）** ⇒ 预算拒发时留下**无链首条目**，
它最终只有一个 `stage=Drawn` 的终态。因此"表里有条目 ⇒ 链首必发过"只对观察链成立。

**修复**（`war3_palette_object_evidence.h` 的 `NoteReject`）——新顺序为：

```
去重 → 表满（TableFull 属**终态**预算，F1）→ 非终态预算预检 → Insert → 发射
```

**顺序中两个不可交换的要点**：
1. **非终态预算必须先于 `Insert`**：否则预算拒发就留下无链首条目（本条修复）；
2. **表满判定必须先于非终态预算**：`TableFull` 是**终态**（`CanEmit(true)`，受 512 预留约束、不受
   每帧 64 与非终态预算约束，见 F1）。若先判非终态预算就返回，会在非终态预算耗尽时**悄悄不再公告表满**。
   这一点是我在重排时**主动发现**的，否则会引入一个新的静默丢失。

## 2. 锁定与验证（三处，均通过）

| # | 手段 | 结果 |
| --- | --- | --- |
| 1 | 新增宿主用例 `Case 25 budget-refused reject must not create a headless entry` | `[PASS] 25 … (6 checks, 0 failures)`；`SUMMARY: 25 passed, 0 failed` |
| 2 | **反向变异探针**：把预算检查移回 `Insert` 之后 | `[FAIL] 25 … (6 checks, 2 failures)`、`SUMMARY: 24 passed, 1 failed`；`terminalEmitted=65`（修复后 64）、`emitted=129`（128）⇒ **第 65 键确实留下了条目与终态**，测试**载荷有效** |
| 3 | 静态门禁新增两条**顺序**断言（`test_semantic_build_thread_gate_static.py`） | 通过；同时把一条**文本锚定**断言由字面 `if (e->lastRejectFrame == frames.renderFrame) {` 改为检查**要求本身**（我去重逻辑加了空指针守卫，行为未变） |

**Case 25 不能被既有 `Case7Budgets(a)` 代替**：那里 100 个条目是在**各自独立帧**建立的，
所以"建条目时预算被拒"这条路径从未被走到（旧断言 `watchCount()==100` 恒真）。这一点是**本轮的实质发现**。

## 3. 🔴 未决红点 1：成本测试的终态预留期望与修复冲突（**已具名，未解决**）

`meson` 由 **85 Ok / 0 Fail** 变为 **84 Ok / 1 Fail**：

```
83/85 warvk:war3_palette_object_evidence_cost   FAIL
[FAIL] CloseWindow must emit exactly the remaining terminals allowed by the 512 reserve
COST_VERDICT=FAIL checks=38 failures=2
```

**原因**：该断言 `emittedTerminals == kTerminalReserve - reserveConsumed` 与
`droppedReserve == kCapacity - emittedTerminals` **编码了修复前的行为**——那时预算被拒也照样建条目，
所以"填充 kCapacity 个键"必然得到 kCapacity 个存活条目、关窗必然把 512 预留用满。
修掉那条路径后存活条目数下降，该期望随之失效。**这是正确性修正的正当后果，不是回归。**

**但我写不出正确的替代断言** —— 实测数据与我对终态预留的记账模型**不符**：

```
CLOSE reserveConsumed=0   emittedTerminals=512  droppedTerminalReserve=512  emittedBeforeClose=1024
CLOSE reserveConsumed=448 emittedTerminals=288  droppedTerminalReserve=736  emittedBeforeClose=1472
CLOSE reserveConsumed=512 emittedTerminals=256  droppedTerminalReserve=768  emittedBeforeClose=1536
```

`kTerminalReserve = 512`，但 `reserveConsumed=448` 时 **448 + 288 = 736** 个终态被发出 ⇒
**我对"终态预留如何封顶"的理解是错的**。我因此**撤回**了自己写的替代断言（不理解的公式不得用于换绿色），
并用**已验证未改动的交付副本**（doc-r1 包内 `src 966|039F78B3…`，指纹已核）恢复了该测试源码。

**🔴 未解决的具体问题**：终态预留与 `kTotalBudget`/`kNormalBudget`、`droppedTerminalReserve` 的
精确关系是什么？为什么 `reserveConsumed=448` 时仍能再发 288 个终态？**必须先答这个问题**，
才能写出正确的断言（或判断修复本身需要调整）。

## 4. 🔴 未决红点 2：构建状态与源码不一致（**我先无法调和**）

恢复成本测试源码后：

| 观测 | 值 |
| --- | --- |
| 源码（与 doc-r1 纯净副本逐字节相同） | `39024` 字节，**不含**我的编辑（`orig-assert`） |
| `ninja -C build32 -n` | `no work to do` |
| 但运行该测试二进制 | 仍打印**我编辑过**的断言措辞、`checks=44` |

⇒ "源码已还原 + 增量构建认为无事可做" 与 "产物仍是旧编辑" **互相矛盾**。
**我没有继续试（例如强制全量重建）**，因为：①上下文已接近耗尽；②在未搞清楚前继续改动会掩盖问题。
**下一轮第一件事就是查清它**（可能方向：meson 目标实际引用的源文件路径、`build32_safe.cmd` 与
`ninja` 的产物路径是否不同、是否存在第二份成本测试源码）。

## 5. 状态（截至本轮结束）

```
改动文件（2，均在 src/ 内）：
  src/d3d9/war3/tools/war3_palette_object_evidence.h        NoteReject 重排
  src/d3d9/war3/render/tests/war3_palette_object_evidence_test.cpp  新增 Case 25
  AutoTest/test_semantic_build_thread_gate_static.py        锚点更正 + 两条顺序断言
  src/d3d9/war3/render/tests/war3_palette_object_evidence_cost_test.cpp  已恢复为纯净副本（无净改动）

已核实绿：
  war3_palette_object_evidence_test   : SUMMARY: 25 passed, 0 failed
  wire roundtrip                      : CHECKS=1141 FAILURES=0  CERTIFIED_SPEC_SATISFIED
  AutoTest 全量静态                    : 259 scripts, 0 failed
  宿主反例探针（反向变异）              : 如预期失败（证明 Case 25 载荷有效）

已核实红（**未解决**）：
  meson 84 Ok / 1 Fail  ← war3_palette_object_evidence_cost（见第 3 节）
  构建状态与源码不一致                                    （见第 4 节）

候选 DLL（**已变**，本阶段允许）：36,289,280 B
  SHA-256 881B0325D89B5F8F7ADB8457514CEB4799E9D7CFD17B045C8EA26B1B5E417928
  （阶段 A/B 时为 5ADEDD5F…）

站点：E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）。git：无写操作。
```

## 6. 不声称

- **不**声称阶段 C 的第一步"完整完成" —— 它留下两个具名红点；
- **不**声称全门禁通过（meson 明确 1 Fail）；
- **不**用我不理解的终态预留公式换取绿色；
- **不**在红点 2（构建状态不一致）查清前继续改动该测试。
