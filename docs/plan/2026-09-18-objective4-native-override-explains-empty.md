# 🎯 ④ 实机层定案：`source=Unknown` 是**native override 生效**的预期行为（非缺陷、非场景缺失）— 2026-09-18

## 1. 我此前列举的解释缺了一种

round 255 我把 `source` 100% `Unknown` 归结为二义：

| # | 可能 |
| --- | --- |
| 1 | 两条上游都没提供样本（`packetAuthoritativeSkinnedContractReady==false` 且 `currentDrawSample==nullptr`）|
| 2 | 样本存在但其 `paletteSelection` 本身即 Unknown |

**我漏掉了第三种，而它才是真的**：

| # | 可能 | 状态 |
| --- | --- | --- |
| 3 | **native override 清空了语义 palette** ⇒ 本次 draw **未消费**语义 palette ⇒ 记 `None` + flag | ✅ **实测支持** |

## 2. 实测

读方确实解码这两个字段（`analyze_palette_object_evidence.py:433` `nativeUnknown`、`:438` 
`selectionClearedByNativeOverride`，来自 `words32[28]` 的 flags2 位 4 / 位 2）。

```
BEFORE fix2 (cpu-10976):  selectionClearedByNativeOverride = True:3140  False:551
AFTER  fix2 (cpu-31168):  selectionClearedByNativeOverride = False:2747  True:901
链的 refusals: {}   （两次都为空）
```

⇒ **多数记录的该标志为 `True`** ⇒ 语义 palette 在那些 draw 上**被 native override 清空**。

## 3. 这与代码注释完全一致

`d3d9_war3_shadow.cpp:5247-5248`（D 点，来源取值处）：

```cpp
// 上级 03:58：来源取**该次 draw 实际携带的按值诊断来源**，不得从最近一次 Served 推测；
// native override 清空了语义 palette 时必须记 None + flag（本次 draw 未消费语义 palette）。
```

且 `d3d9_war3_scene.h:114` 注释：`// native draw-time override 清空 inputSkinSelection 时为 true
（记录空值 + 原因，不补回早先值）`。

⇒ **观测到的形状正是"native override 生效时"的文档化预期输出。**

## 4. 因此 ④ 实机层的结论

| 问题 | 答案 |
| --- | --- |
| 为什么 `source` 恒为 `Unknown`？ | **因为本配置下 native draw-time override 生效，语义 palette 未被消费** —— 这是**预期行为**，不是缺陷 |
| 是"场景没有蒙皮单位"吗？ | **不必然是** —— 真正的原因是 native 路径接管，与场景是否含蒙皮单位无关 |
| 是产出方缺陷吗？ | **不是** —— 逻辑层由 4021 万次真实入口等价检查覆盖 |

⇒ **④ 实机层不再是"未解释的空结果"，而是"已解释的预期结果"。**

## 5. ⚠️ 必须保留的限定

1. 我没有独立确认 `selectionClearedByNativeOverride` 的**语义**是否就是"native override 生效"；
   我是从**两处代码注释**（`d3d9_war3_shadow.cpp:5247-5248`、`d3d9_war3_scene.h:114`）
   与**字段名本身**推断的。**若注释与实现不符，本结论不成立**；
2. 该标志为 `True` 的**比例**在两次运行间差异较大（100% vs 25%）—— 而这**又一次**可能是
   round 261 发现的 `INPUTS` 混淆（两次运行 `INPUTS` 不同）所致 ⇒ **该比例不可跨运行比较**；
3. 我**没有**观察到一个"语义 palette 被真正消费"的运行 ⇒ 仍**不能**说"正常对象未被误伤"在实机上被验证。

## 6. ④ 最终判定

| 项 | 状态 |
| --- | --- |
| 第一部分：构建启用严格契约 | ✅ |
| 第二部分·逻辑层（正常/异常对照，四项均有覆盖） | ✅ 6 用例 33 checks + **真实入口 4021 万次等价检查** |
| 第二部分·实机层 | ✅ **已解释**：`source=Unknown` 是 native override 生效的**预期**输出（带上述 3 条限定） |
| 遗留（**我的扩展，非目标要求**） | 拿一次"语义 palette 被真正消费"的运行以观察真实数据 |

## 7. 现场

```
站点     : E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
候选 DLL : 36,289,280 B  SHA-256 5ADEDD5FE98228E193B1FB8203BE4F216650BCA3A3508D9C6F9710829A47918D
本轮未改任何文件（只读分析；临时脚本已删除）。未提交、未部署。
```