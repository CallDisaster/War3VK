# 阶段 D · B1 运行期半边：**尝试后回退**（命名空间解析受阻）— 2026-09-18（round 51）

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**本轮没有留下前进性改动。**

## 1. 我尝试了什么

按 round 50 的计划补 B1 的**运行期**半边：在 `war3_palette_object_evidence_test.cpp` 新增
`Case 32`，用 sink 的公开接口断言：

```
· 未 arm ⇒ PaletteObjectEvidenceArmed() 必须为假（且这是**正确**契约：子门关闭时 Arm 是故意零操作）
· armed 为真 ⇒ 头块必须来自**已初始化**的记录器（watchCount 有界）且计数自洽
  （terminalEmitted ⊆ emitted）
· disarm ⇒ 必须发布 armed=false
```

并**打印 armed 的真假**，使"子门关闭导致真空通过"**可见**而非静默。

## 2. 为什么失败（两次，同一根因）

| 尝试 | 错误 |
| --- | --- |
| 直接用 `ArmPaletteObjectEvidence(...)` | `was not declared in this scope` |
| 加 `dxvk::war3::tools::evidence::` 限定 | `did you mean 'PaletteObjectEvidence'?` |

⇒ 这些名字**不在**我假定的那个 namespace 里（该命名空间只有 `PaletteObjectEvidence` 类）。
**我没有在当轮查明 sink 接口的真实命名空间**，因此按纪律回退。

## 3. 我还犯了一个**脚本化替换**的错误（记录在案）

限定替换时我按名字循环替换 `X(` → `限定::X(`，但 **`DisarmPaletteObjectEvidence(` 含有
`ArmPaletteObjectEvidence(` 作为子串** ⇒ 被替换成 `Dis` + 限定名，产生了 `Disdxvk::…`。
虽然随后修回，但这是**同一类错误的第三次**（round 16 块边界、round 34 锚点、本轮子串嵌套）。

**规律**：脚本化替换前必须检查**待替换串是否为其它标识符的子串**。

## 4. 处置与状态

- Case 32 与其 include 已**逐字移除**（残留检查：`Case32` = false）；
- 树已回到 round 50 的已验证状态：**31 passed, 0 failed**、**STATIC=261/0**、`no-work`；

```
STATIC=261/0 ; meson 85/0 ; no-work ; evidence 31/0 ; wire 1160/0 ; 读方 106 OK
DLL : 41A70AA50AEABE4245F3D9F640ECC79501D403720AF7F1CE4DACE06A7E9BF951（与 round 50 同源）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 下一步（一次定向查证，然后立刻实现）

```
① 读 war3_palette_object_evidence_sink.h 的 namespace 声明（一行即可）；
② 同一轮内直接用正确的限定名写 Case 32；
③ 构建 + 运行 + 反向变异（使 armed 的读写退回裸 bool ⇒ 静态锁失败；
   本运行期用例的载荷性则来自"disarm 后 armed 必须为假"这条无条件断言）。
```

## 6. 不声称

- **不**声称 B1 的运行期半边有任何进展（**回退**）；**B1 仍只有静态半边**；
- **不**声称 D 阶段可以出包（B1 未齐 ⇒ 出包会把未完成阶段伪装成完成，同 round 50 的结论）；
- **不**声称 C 批次已完成（尾项卡在裁定）；**不**声称两个 P0 已完成 ⇒
  **不新增实机因果结论、不晋升稳定候选**；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。