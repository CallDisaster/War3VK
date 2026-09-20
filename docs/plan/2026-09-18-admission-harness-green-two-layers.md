# 目标④：现成准入对照 harness 全绿；把"契约逻辑层"与"实机对照层"分清 — 2026-09-18

## 1. `war3_skin_palette_admission_test.cpp` 的用例与 ④ 子问题的对应

| # | 用例 | 覆盖 ④ 的哪个子问题 | checks |
| --- | --- | --- | --- |
| 1 | `normal admission and count-short refusal` | **正常对象是否被误伤**（正常准入）+ **为何允许**（计数不足时拒绝） | 9 |
| 2 | `replaced mid-submission (slot/frameTag)` | **提交时是否被换掉** | 3 |
| 3 | `object or map switch (epoch/model/part)` | 跨对象/跨地图不得误用（与硬约束"跨地图不得复用"一致） | 4 |
| 4 | `preconditions (ticket/hash/source/space/domain)` | **为何允许**（前置条件的**具体字段**：publicationTicket / hash / source / space / domain） | 5 |
| 5 | `submission-time replacement rule (CanReplace)` | **提交时是否被换掉**（规则本身） | 6 |
| 6 | `shared producer-group-count admission rule` | **groupCount 数量规则** —— 即目标 ②(a) 那条共享谓词的锁 | 6 |

## 2. 实测

```
$ .\build32\...\war3_skin_palette_admission_test.exe
[PASS] 1 ... (9 checks, 0 failures)
[PASS] 2 ... (3 checks, 0 failures)
[PASS] 3 ... (4 checks, 0 failures)
[PASS] 4 ... (5 checks, 0 failures)
[PASS] 5 ... (6 checks, 0 failures)
[PASS] 6 ... (6 checks, 0 failures)
SUMMARY: 6 passed, 0 failed          （共 33 checks）
EXIT=0
```

## 3. ⚠️ 把 ④ 的"两个层次"分清（避免过度声称）

目标 ④ 的原话是「围绕**真实蒙皮选择入口**做正常/异常对照验证」，并列出四个子问题。
本轮核实后发现必须分成两层，**不能混为一谈**：

| 层次 | 内容 | 状态 |
| --- | --- | --- |
| **契约逻辑层** | `Selection` 的字段、`ContractEnabled()`、`GroupRange()`、准入/替换/跨对象规则 | ✅ **已有 6 用例、33 checks 全绿**（本轮实测） |
| **实机对照层** | 在真实游戏里用 `DXVK_WAR3_SKIN_PALETTE_CONTRACT=1` vs `=0` 两次启动，观察 `skin-selection/v1` 事件中 `source`/`slot`/`actualGroupCount` 的分布，以及正常对象在两种配置下是否被区别对待 | ⏳ **未做** |

### 为什么不能声称 ④ 已完成

契约逻辑层全绿**只证明**"给定 `Selection`，契约判定正确"。
它**不能**证明：

1. 生产路径**真的**产生符合契约的 `Selection`（字段来源是否正确）；
2. 关闭契约（`=0`）时**正常对象是否被误伤**（这是"异常对照"要回答的）；
3. 真实数据里 `source` 的分布（`LegacySlotCache` vs `CapturedWriter` 等各占多少）。

⇒ **"契约逻辑正确"≠"实机行为符合预期"。** 这正是目标④第二半要补的。

## 4. 下一轮（具体、可执行）

```
① 先做**两次进程启动**的实机对照（因 ContractEnabled 用 static const 锁存，中途改无效）：
     run A: DXVK_WAR3_SKIN_PALETTE_CONTRACT=1   （= 本构建默认）
     run B: DXVK_WAR3_SKIN_PALETTE_CONTRACT=0
② 两次都开启 DXVK_WAR3_FRAME_EVIDENCE_INPUTS=1（skin-selection/v1 受 InputsEnabled() 门控，
   见 d3d9_device.cpp:20970/20977），其余域维持 DRAWS=0 等固化默认。
③ 对比：skin-selection/v1 事件条数、source 分布、slot/actualGroupCount 分布，
   以及两次运行中"正常对象"（两次都应出现者）的差异 ⇒ 回答"是否被误伤"。
④ 记录时明确：这是**隔离桌面的功能对照**，**不是前台性能数据**（硬约束）。
```

## 5. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读 + 运行既有测试二进制）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```