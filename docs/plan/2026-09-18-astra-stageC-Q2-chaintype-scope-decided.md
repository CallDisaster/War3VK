# 阶段 C · 步骤④ 定案：`PaletteObjectChainType` 的 scope 查明并统一 — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。

## 1. 查明结果

`PaletteObjectChainType` 与它的三个同类枚举**同在 namespace scope**：

| 枚举 | 位置 |
| --- | --- |
| `PaletteObjectTerminal` | `war3_palette_object_evidence.h:79` |
| `PaletteObjectRejectReason` | `:89` |
| `PaletteObjectIdentityProofKind` | `:100` |
| **`PaletteObjectChainType`** | `:105` |

四者都在 `namespace dxvk::war3::tools::evidence {`（`:26`）之内，
**不是** `class PaletteObjectEvidence`（`:146`）的成员。

⇒ round 12 我猜的两种写法：
- `PaletteObjectChainType`（非限定）—— 错，测试没有 `using namespace`；
- `PaletteObjectEvidence::PaletteObjectChainType` —— **错，它不是类成员**。

## 2. 正式解决（取代 round 12 的底层值回避）

在测试文件里加**文件内 using 声明**（放在 `Case 26` 之前，与其它测试的可见性一致）：

```cpp
using dxvk::war3::tools::evidence::PaletteObjectChainType;
```

并把 round 12 的两处底层值比较改回**按名字**比较：

```cpp
g_collected[i].chainType != PaletteObjectChainType::Observation
g_collected[i].chainType != PaletteObjectChainType::RejectionRecovery
```

**为什么这不是美化**：底层值比较在**枚举子改名时不会报错**（`!= 1u` 依然编译），
按名字比较会让编译器立刻拦下。round 12 的写法是"让构建通过"，本轮是"让约束成立"。

**⇒ `Case 26` = 8 checks, 0 failures。**

## 3. 状态

```
=== no-work ===
ninja: no work to do.
=== static ===
STATIC: 259 scripts, 0 failed
=== meson ===
Ok:                85
Fail:              0
=== wire ===
exit=0
CHECKS=1160 FAILURES=0
=== DLL ===
36297544 B  79B3C229AE7715252BB1BE171BB38F0AED3FD080346233C7FC513CE642EC2C83
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 4. Q2 剩余（⑤–⑨）

```
⑤ Find/Insert 按 (对象键 × 链型 × 窗口) 匹配 ⇒ 两类链各自独立、同一对象可同时持有两条
⑥ 显式定义 S/E/D/ObjectGone/CloseWindow 在两类链并存时的归属规则（不得默认）
⑦ 链型上 wire：sink 写 data[4]；读方 RESERVED_DATA 必须**按版本**排除 4（现存版本无关用法）
   + 新增 data[4] 的链型解码
⑧ 读方链分组键由"仅对象键"改为「对象键 × 链型」
⑨ 负向探针：同对象两类链各自结算；链型与事件形状不符必须拒绝；v1/v2/v3 的 data[4] 必须为零
```

## 5. 不声称

- **不**声称链型已上 wire；**不**声称 `Find`/`Insert` 已按链型分区；
- **不**声称同一对象可同时持有两条链 —— `R→FirstSight` **改类缺陷仍未修**；
- **不**声称阶段 C 主体完成；K3（跨帧判序）未做；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。
