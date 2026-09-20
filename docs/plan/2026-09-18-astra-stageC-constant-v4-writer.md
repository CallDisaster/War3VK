# 阶段 C：写方改为**恒定 v4**（删动态版本选择）— 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**改产品源码并重建 DLL**。

## 1. 改动（`src/d3d9/war3/tools/war3_frame_evidence.cpp`）

| 位置 | 原 | 现 |
| --- | --- | --- |
| `:66` | `if (firstSightUsed()) { counters["firstSightInserted"/"firstSightEmitted"] = … }` | **无条件写入**（v4 的 counters 集恒含这两个键） |
| `:79` | `result["version"] = firstSightUsed() ? 3 : 2;` | `result["version"] = 4;` |

**理由**：原实现让**同一个二进制**产生两种块形状（2 或 3），读方必须同时接受，
而"版本"本应是**契约**而不是**内容摘要**。裁定要求：新写方一律输出 v4。

## 2. 级联（预期内，全部具名）

只重建 DLL 时门禁**假绿**（`CHECKS=1141 FAILURES=0`）—— 因为 wire 测试可执行文件**仍用旧写方**。
**全量重建后**才暴露真实差异：

```
CHECKS=1153 FAILURES=17
FAILURE[1/17] A: the production export must declare extension version 2 (got 4)
FAILURE[2/17] A: the generic root reader must report the registered version-2 extension …
FAILURE[3/17] A: the palette reader must agree with the generic reader on the extension version (got 4 / …)
… (A–F 各 3 条) + G 2 条
```

**这本身就是一条教训**：我起初只构建了 DLL，门禁于是**没有在测新写方**。
"改了产品代码却只重建一部分产物"会让门禁给出**假绿**。

## 3. 夹具/期望更新（**产品契约变更，不是放宽**）

`AutoTest/test_palette_object_wire_roundtrip.py`：新增 `PRODUCTION_VERSION=4`，
`check_header_block` 默认值、generic/palette 读方一致性断言、`decode_event` 版本、
以及 `check_scenario_g` 自身的两条断言全部改用该常量。

⇒ **`CHECKS=1153 FAILURES=0`**（由 1141 增加：版本断言现在真的在检查）。

## 4. 两个静态门禁的收紧（**比原断言更强**）

| 文件 | 原断言 | 现断言 |
| --- | --- | --- |
| `test_first_sight_observation_chain_static.py:49` | 必须存在 `firstSightUsed() ? 3 : 2` | 必须存在 `version=4`，且 **`firstSightUsed() ? 3 : 2` 必须不存在** |
| 同上 `:91` | 同上 | `version=4` + **读方必须登记 `PALETTE_OBJECT_CHAIN_TYPED_VERSION`** |
| `test_semantic_build_thread_gate_static.py:2216` | 「字面 2」**或**「条件表达式 3:2」 | **必须恰好 `version=4`**，且**禁止动态选择回归** |

三者都**加强了**要求（从"接受两种写法"变为"只接受恒定 4 并禁止回归"）。

## 5. 状态

```
AutoTest 全量静态     : STATIC: 259 scripts, 0 failed
meson                : Ok: 85  Fail: 0
ninja -C build32 -n  : no work to do
wire roundtrip       : CHECKS=1153 FAILURES=0   CERTIFIED_SPEC_SATISFIED
evidence test        : SUMMARY: 25 passed, 0 failed
读方套件              : test_palette_object_evidence_analysis_static 100 OK ; test_analyze_frame_evidence 55 OK
候选 DLL             : 36,297,544 B
                       SHA-256 54EDAFB6BCE2587F8ED923341336A37E0CA933A7BE28F1CE537234C9FE4E7E7C
站点                 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                  : 无写操作
```

## 6. 不声称

- **不**声称 v4 已完整实现：本轮的 v4 = v3 的字段集 + **恒定版本号**；
  **`ObservationClosed=7` 尚未上 wire**（C++ 枚举未加）、**链型未上 wire**、
  **链型仍未参与查找**；
- **不**声称阶段 C 主体完成：①类型化查找键 ②事件归属规则 ③链型导出 均**未做**；
- **不**声称 K2（未拒绝链被标 `Recovered`）、K3（混合链/跨帧语义）已修；
- **不**声称旧产物读取行为有变 —— v1/v2/v3 仍按各自契约解析（本轮只改**写方**）。
