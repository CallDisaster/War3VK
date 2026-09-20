# 🎯 ④ 的「真实蒙皮选择入口」证据：4021 万次检查的等价性电池全通过 — 2026-09-18

## 1. 发现

我此前把 ④ 的"契约逻辑层"证据限定为 `war3_skin_palette_admission_test`（6 用例 / 33 checks），
并据此说"实机对照层仍缺场景"。**这个限定不完整。**

实际存在一个**直接跑真实蒙皮选择入口**的等价性测试：

```
src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp
   └─ war3_live_palette_selection_legacy_reference.inc
   └─ war3_live_palette_selection_chain_legacy_reference.inc   <- 含 ②(a) 的冷/热缓存数量规则
   └─ war3_live_palette_selection_motion_legacy_reference.inc
   └─ war3_live_palette_selection_m2_4_legacy_reference.inc
   └─ war3_live_palette_selection_a9_legacy_reference.inc
```

`war3_live_palette_selection_chain_legacy_reference.inc:23` 的注释正是目标 ②(a) 的缺陷描述：

> 冷缓存首次查询原先不校验 producer 记录的 groupCount，与热缓存路径的数量规则不一致：
> 同一对象、同一数量条件会在冷缓存下被接受、热缓存下被拒绝

且 `:401` 调用 `ProducerGroupCountCovers`（②(a) 引入的共享谓词）。

## 2. 实测结果

```
$ build32\...\war3_live_palette_selection_test.exe
M2-4 battery : 7,477,257 checks, 0 failures
A9 battery   :   947,458 checks, 0 failures
M2-5B battery: 1,347,808 checks, 0 failures
war3 live palette selection equivalence: 40,211,653/40,211,653 checks passed
EXIT=0
```

⇒ **40,211,653 次检查，0 失败**，全部在**真实蒙皮选择入口**的实现上（非合成副本）。

## 3. 这意味着什么

| # | 结论 |
| --- | --- |
| 1 | ②(a) 的修复**不是**只被"共享谓词的 6 条单元断言"覆盖，而是被**等价性电池**覆盖 —— 新路径对旧参考实现逐项比对 |
| 2 | 等价性覆盖**冷缓存与热缓存两条路径**，正是该缺陷的所在（原先两者数量规则不一致） |
| 3 | 因此 ④ 的"契约逻辑层"证据强度被显著抬高：不再是 33 checks，而是 **4021 万次检查 + 6 用例 33 checks** |

## 4. 但**仍然不能**就此说 ④ 完成 —— 必须分清

| ④ 的子问题 | 是否已被该测试回答 |
| --- | --- |
| **选中什么数据** | ❌ 测试用**合成** Selection；真实场景里"选中什么"仍需实机 |
| **为何允许** | ✅ 逻辑层已覆盖（`liveRuntimeGroupPaletteReady` + `IsSkinPaletteSelectionCurrent` + `ProducerGroupCountCovers`） |
| **提交时是否被换掉** | ✅ 逻辑层已覆盖（`:20868-20870` 的清空条件）；实机仍未知 |
| **正常对象是否被误伤** | ⚠️ **部分**：等价性测试证明"正常条件下两路径一致"；但"真实场景里正常对象是否被误伤"需要**真实 Selection** 才能答 |

⇒ **逻辑层结论**：契约在真实入口上行为正确且与参考实现等价；
⇒ **实机层缺口**：仍然需要**一个已知会触发蒙皮选材的场景**，才能回答"真实数据里选中什么 / 是否误伤"。

## 5. ④ 状态更新

| 项 | 状态 |
| --- | --- |
| 第一半：构建启用严格契约 | ✅（19 处活调用） |
| 第二半·契约逻辑层 | ✅ **6 用例 33 checks + 真实入口 4021 万次等价性检查（0 失败）** |
| 第二半·实机对照层 | ⏳ **仍阻塞于"缺一个会触发蒙皮选材的场景"**（round 251-255 已把通道、测量、含义、5 项排除全部做完） |

## 6. 现场

```
站点     : E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
候选 DLL : 36,289,280 B  SHA-256 5ADEDD5FE98228E193B1FB8203BE4F216650BCA3A3508D9C6F9710829A47918D
本轮未改任何文件（只读 + 运行既有测试二进制）。未提交、未部署。
```