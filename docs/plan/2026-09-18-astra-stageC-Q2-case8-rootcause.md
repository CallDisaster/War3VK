# 阶段 C · Q2：Case 8 根因**完全确认**（分区实现自身的缺陷）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读诊断，本轮无代码改动。**

## 1. 三个假设的裁决

| 假设 | 内容 | 裁决 | 证据 |
| --- | --- | --- | --- |
| A | 新建条目路径**没有**置位 sawFirstSight | **排除** | sawFirstSight = true 在 :484，位于 if (e == nullptr) { … } **之外** ⇒ 两条路径都置位 |
| B | NoteFirstSight 体内有**第二处** Find | **排除** | 全文件 Find(key) 共 6 处（:343/441/510/557/602/645），NoteFirstSight 只占 :441 一处 |
| C | 探测/墓碑让 Find 只返回**先插入**的条目 | **确认** | 见 §2 的 Find 实现 |

## 2. 根因（代码级，已核对原文）

war3_palette_object_evidence.h:926-941 的 Find：从 HashKey(key) % kWatchCapacity 起探测，
遇到第一个 SameKey(e.key, key) 就 **return &e** —— 与链型无关。

⇒ Find 是「每个对象键 ⇒ 至多一条条目」这一**旧不变量的实现**。
分区之后同一键有两条条目，而它们**共享同一个 HashKey** ⇒ 起点相同
⇒ Find **永远返回先插入的那条**（= NoteReject 建立的拒绝条目）。

我 round 16 的实现是 FindChain = Find(key) + 事后类型检查：
FindChain(key, Observation) **恒为 nullptr**（只要该对象已有拒绝条目）
⇒ NoteFirstSight **每次都走新建路径并发射**
⇒ Case 8 的 :941 / :943 / :945 三条同时失败。

**⇒ 分区实现自身的缺陷**：不是夹具旧期望，也不是被掩盖的既有缺陷
（round 17 的假设 A 曾是「既有缺陷」候选，已被 :484 的位置排除）。

## 3. 正确的修法（下一轮实施，必须与读方分组键同一次落地）

链型进入**匹配谓词**，而不是事后过滤：

- if (SameKey(e.key, key) && e.chainType == type) return &e;
- ★ 同键但**链型不同**必须 **continue 继续探测**，不得当作「不存在」而返回 nullptr
  （目标条目可能在**更后的探测位**上）；
- tombstone 仍必须 continue（既有语义）；
- 任何**有链型归属**的 Note* 都不得再用类型无关的 Find(key)；
- Insert 已能放置同键第二条（它找第一个 !used 槽）⇒ 无需改；
- 读方 chain_group_identity 必须同一次加入 chainType（否则同对象两条链被并成一条）；
- 落地后**重跑 Case 8** —— 它正是「跨帧重复不得再发」的回归锁，分区正确时应当**通过**。

## 4. 状态（无代码改动）

```
静态 259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 26/0 ; 读方 OK
候选 DLL : 36,297,544 B  1C85084115534EF3312F259E5AAE4B668A689A00664FFF6C3E89187D71E5F333
站点     : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git      : 无写操作
```

## 5. 不声称

- **不**声称 ⑤/⑥/⑧ 已实现（仍**未落地**）；
- **不**声称修法已验证（§3 是**设计**，尚未编译与运行）；
- **不**声称阶段 C 主体完成；K3（跨帧判序）未做；
- 本轮产出：**排除两个假设、确认第三个，并给出代码级根因与修法设计** ——
  这是诊断结论，**不是**功能进展。