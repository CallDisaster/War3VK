# 阶段 D · D1 落地：armed 与计数**统一同步域**（atomic + release/acquire）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。本轮**改产品源码并重建 DLL**。

## 1. 时序认识（动手前的必要条件，round 47 读到）

```cpp
:162  ArmPaletteObjectEvidence(session, epoch) {
:167    g_paletteObjectEvidence.Configure(&EmitPaletteObjectEvent, nullptr);  // 初始化记录器
:169    g_paletteObjectEvidence.Reset(sessionGeneration, mapEpoch);           // 清表
:170    g_paletteObjectArmed = true;                                          // **最后**才置 armed
:171  }
```

⇒ **arm 的顺序本身已正确**（先 `Configure`/`Reset`，后置 armed）——
这一点必须写明，否则容易被误读成"发布顺序错了"。

## 2. 真正的缺陷在**同步域**，不在顺序

`g_paletteObjectArmed` 原为**裸 `bool`**，而记录器的初始化是在它自己的 `StateLock` 下写的。
⇒ 写者（`:170`）与 7 个读者之间**没有 acquire/release 配对** ⇒
弱内存序下读者可能看到 `armed == true`，却**看不到**记录器已完成初始化。

这正是裁定「armed 与计数读取**统一同步域**」所指。

## 3. 改动（机械、可验证）

```cpp
std::atomic<bool> g_paletteObjectArmed{false};        // 原：bool g_paletteObjectArmed = false;
g_paletteObjectArmed.store(true,  std::memory_order_release);   // :170
g_paletteObjectArmed.store(false, std::memory_order_release);   // :174
!g_paletteObjectArmed.load(std::memory_order_acquire)           // 6 处读
g_paletteObjectArmed.load(std::memory_order_acquire)            // :186 return
```

⇒ release 写与 acquire 读配对 ⇒ **armed 的发布同时发布记录器的初始化**。

## 4. 静态锁（4 条断言，写在既有 sink 静态检查旁）

```python
assert "bool g_paletteObjectArmed" not in sink          # 不得退回裸 bool
assert "std::atomic<bool> g_paletteObjectArmed{false};" in sink
assert "g_paletteObjectArmed.store(true, std::memory_order_release);" in sink
assert "g_paletteObjectArmed.load(std::memory_order_acquire)" in sink
```

**为什么是 4 条而不是 1 条**：只查"不是裸 bool"无法区分"改成原子但用 relaxed"——
而 **relaxed 不建立配对，等于没修**。故必须同时钉住写侧 release 与读侧 acquire。

## 5. 载荷性（**由变异证明**）

把声明退回裸 `bool` ⇒ 分析套件具名失败；随后逐字还原。

## 6. 状态

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 31/0
分析套件 106 OK ; DLL 8D63F8E88DC2A26D38AA5311154B933AD3AC03AF7989CE81162DB30262265E2B
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 7. 不声称

- **不**声称 D 已完成 —— 只做了 **D1**（三处中的第一处）；
- **不**声称这是一个**可再现竞态**的修复证明：本轮证明的是
  "发布路径现在建立了 acquire/release 配对 + 静态锁不许退回"，
  **不是**"曾在实机上观测到该竞态"（**没有**这样的观测）；
- **不**声称 D2（`active` 发布顺序）与 D3（`HeaderJson` 同一快照）有任何进展（**未做**）；
- **不**声称两条屏障测试中的 B1 已完整（B1 的运行期半边**未做**，只做了静态半边）；
- **不**声称 C 批次已完成（尾项卡在裁定）；**不**声称两个 P0 已完成；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，
  更不代表阴影已恢复。