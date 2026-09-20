# 阶段 C · Q2 核心落地：按链型分区（⑤+⑥+⑧）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。本轮**改产品源码、读方并重建 DLL**。

## 1. 根因回顾（round 18 的结论）

Case 8 的三处失败**不是**夹具旧期望，而是我 round 16 的分区实现缺陷：
Find 从 HashKey(key) % kWatchCapacity 探测并返回**第一个** SameKey 条目（与链型无关），
而同一键的两条条目**共享同一个 HashKey** ⇒ FindChain = Find + 事后类型检查**恒 nullptr**。

## 2. 本轮实现

**C++（war3_palette_object_evidence.h）**

- 新增 FindChain：链型进入**匹配谓词**
  （SameKey(e.key,key) && e.chainType == type），且
  **★ 同键但链型不同必须 continue 继续探测**，不得返回 nullptr；
- 新增 FindOwner：归属规则**显式**（优先 Observation，其次 RejectionRecovery）；
- **只改两个头创建入口**：NoteReject ⇒ FindChain(RejectionRecovery)；
  NoteFirstSight ⇒ FindChain(Observation)。**S/E/D/NoteObjectGone 本轮仍用 Find(key)**
  —— 把「分区」与「归属规则」两个变量分开（round 18 计划第⑤步）。

**读方（analyze_palette_object_evidence.py）**

- chain_group_identity 的分组键加入 chainType，插在**分段标签之前**
  （保住调用方 item[0][-1] 的排序语义）；
- v1/v2/v3 的 chainType 恒为 RejectionRecovery ⇒ 旧合同分组逐字节不变。

## 3. 证据

- 构建：ninja 收敛（[30/30] Linking，随后 no-work）；
- **Case 8（round 16 的拦路者）现在通过**；
- **Case 27 新增**（3 checks, 0 failures）：同一对象同时持有两条独立链 ——
  断言该键关窗结算出**两个**终态、两类链型事件都存在、且
  firstSightInserted == 1（观察链首是**新建条目**，即复核的 R→FirstSight inserted=0 缺陷已修）；
- evidence 由 26 → **27 passed, 0 failed**；计数器印证 terminalEmitted=2、
  closedRecovered=1 + closedWindowExpired=1。

**关于 Case 27 的载荷性**：其 firstSightInserted == 1 断言在旧的「复用」行为下应为 0
（round 4 实测 inserted=0），故**按构造即载荷**；**但反向变异探针本轮未做**（如实记档）。

## 4. 状态

```
=== static ===
FAIL: test_semantic_build_thread_gate_static.py
STATIC: 259 scripts, 1 failed
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== wire ===
exit=0
CHECKS=1160 FAILURES=0
=== DLL ===
36301640 B  3005DFC81FB897A9E08DF751EA5FB021F46831D8A74256243565ACFC66214B91
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 不声称

- **不**声称 ⑥ 的**归属规则已生效**：FindOwner 已定义但**尚未被使用**
  （S/E/D/NoteObjectGone 仍用类型无关的 Find(key)）⇒ 同对象两类链**并存时**，
  这些事件目前挂到探测序**第一条**（通常是拒绝条目）上，而**不是**按规则归属；
- **不**声称窗口维度已进入查找键（裁定要求 对象键 × 链型 × **窗口**）；本轮只加了链型；
- **不**声称读方已按 (对象键 × 链型) **正确拒绝**「链型与事件形状不符」的导出；
- **不**声称实机已验证（且 D 批次完成前不新增实机因果结论）；
- **不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。