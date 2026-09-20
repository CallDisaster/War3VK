# 阶段 D · D2 落地：先装预冻结钩子，再发布 active — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。本轮**改产品源码并重建 DLL**。

## 1. 缺陷（实测到的原始顺序）

```cpp
// war3_frame_evidence.cpp（arm 分支）
:356  ++s->generation; s->lossAtArm=s->lost.load();
:357  s->active.store(s->generation,std::memory_order_release);   // ← **发布 active**
:360  ArmPaletteObjectEvidence(s->generation,0u);                 // ← **arm（palette 侧发布）**
:363  s->ring.setPreFreezeHook(&PaletteObjectPreFreezeHook);      // ← 钩子**在发布之后**才装
```

⇒ `:357` 与 `:360` 之后、`:363` 之前存在一个**窗口**：`active` 已发布、记录器已 arm，
但**预冻结钩子尚未安装**。若在此窗口内发生自动冻结（post-window / 容量 / 序列回绕），
终态**不会被结算**，因而**进不了证据环** —— 正是 sink `:146` 注释所描述的后果。

这正是裁定「**先初始化记录器与预冻结钩子再 release 发布 `active`**」所指。

## 2. 修法（最小、语义唯一）

把钩子块（含其两条说明注释）**移到所有发布动作之前**：

```cpp
s->ring.setPreFreezeHook(&PaletteObjectPreFreezeHook);   // 现在**最先**
++s->generation; s->lossAtArm=s->lost.load();
s->active.store(s->generation,std::memory_order_release);
ArmPaletteObjectEvidence(s->generation,0u);
```

⇒ 顺序不变量：`hook < active.store < ArmPaletteObjectEvidence`。

## 3. 为什么用**源码顺序锁**而不是运行期用例

缺陷是"两个发布动作与钩子安装之间的**窗口**"。在单线程夹具里**无法稳定复现**，
而顺序本身是**源码可判定**的事实。故新建独立静态测试文件
`AutoTest/test_palette_object_arm_order_static.py`（文件名匹配 `test_*_static.py` ⇒
**自动进入 259 门禁**，无需改动任何既有测试的锚点）。

它断言三条存在性 + 两条顺序：`hook < active.store`、`hook < ArmPaletteObjectEvidence`。

## 4. 载荷性（**由变异证明**）

把钩子块移回 arm 之后 ⇒ 两条顺序断言**同时具名失败**：

```
FAILURE: D2: the pre-freeze hook MUST be installed BEFORE active is published
         (otherwise a freeze in between settles no terminal state)
FAILURE: D2: the pre-freeze hook MUST be installed BEFORE the recorder is armed
```

随后源码已逐字还原。

## 5. 状态

```
STATIC=260/0（259 + 本轮新增 1 个静态测试文件）; meson 85/0 ; no-work
wire CHECKS=1160 FAILURES=0 ; evidence 31/0 ; 分析套件 106 OK
DLL 8D63F8E8… → 8786DE4F150F8E231902C5EAA4F0B655DB781A4E3353D40FDAF307D4833A609B
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 不声称

- **不**声称 D 已完成：**D3（`HeaderJson` 同一快照）未做**，三条屏障测试中 **B1 的运行期半边**、
  **B3** 也未做；
- **不**声称该窗口在**实机**上发生过冻结（这是一个**结构性窗口**，没有观测支持）；
- **不**声称顺序锁能代替运行期验证（它只证"源码文本的次序"）；
- **不**声称 C 批次已完成（尾项卡在裁定）；**不**声称两个 P0 已完成 ⇒
  **不新增实机因果结论、不晋升稳定候选**；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。