# 阶段 D · D3：**头块单快照**契约 —— 已满足（结构性），本轮**加锁** — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**本轮未改产品语义**，只加静态锁。

## 1. 裁定条目与实测

裁定：「`HeaderJson` 计数与版本字段存在性取**同一快照**」。

`war3_frame_evidence.cpp` 的 `PaletteObjectHeaderJson()`：

```cpp
:38-40  PaletteObjectEvidenceHeader header{};
        QueryPaletteObjectEvidenceHeader(header);        // ← **唯一**一次快照查询
        const auto& c=header.counters;                   // ← 计数取自该快照
:85     result["version"]=4;                              // ← **字面常量**（不查任何东西）
:87     result["watchCount"]=header.watchCount;           // ← 同一快照
:88     result["counters"]=counters;                      // ← 同一快照
```

⇒ **version / watchCount / counters 全部来自同一个快照**，且 version 是**字面常量** ⇒
**不存在**能与计数快照不一致的第二个来源 ⇒ 该条目**已经满足**（比要求更强）。

## 2. 为什么这个缺陷"原本存在但已消失"——值得记下来

D3 的缺陷形态就是**动态版本**：`result["version"] = firstSightUsed() ? 3 : 2;`。
那是一次**独立于计数的查询**，其结果可以与计数快照冲突
（版本报 3、而计数按 2 的形状写，或反之）。

**round 10 把版本改成恒定 4 时，顺带修掉了 D3** —— 但当时并不知道自己修了这个。
⇒ 本轮把它**显式登记并加锁**，使它不再是"碰巧满足"。

## 3. 锁（新建 `AutoTest/test_palette_object_header_snapshot_static.py`）

| 断言 | 防什么 |
| --- | --- |
| 函数体内 `QueryPaletteObjectEvidenceHeader(` **恰好 1 次** | **第二个快照**（D3 的缺陷本体） |
| 版本是字面量 `4` 且赋值中**不含 `?`** | 退回动态/计算版本 |
| `const auto& c=header.counters;` 存在 | 计数改从别处取 |
| `watchCount` / `counters` 来自该快照 | 头块三字段来源分裂 |

**载荷断言是第一条** ——"恰好一次快照"正是 D3 的要害；其余三条防的是同一件事的其它入口。

## 4. 载荷性（**两个变异探针**，都具名失败）

```
变异 A（插入第二次 QueryPaletteObjectEvidenceHeader 调用）:
  FAILURE: D3: the header must be taken from EXACTLY ONE snapshot (found 2 calls ...)
变异 B（版本改回 firstSightUsed() ? 3 : 2 形态）:
  FAILURE: D3: the block version must be the literal 4 ...
  FAILURE: D3: the version assignment must not contain a conditional
```

源码已逐字还原（`restored: true`）。

## 5. ⚠️ 一个**未解释的观察**（不掩饰）

本轮**源码逐字还原**，但 DLL 哈希由 round 48 的
`8786DE4F150F8E231902C5EAA4F0B655DB781A4E3353D40FDAF307D4833A609B`
变为 `41A70AA50AEABE4245F3D9F640ECC79501D403720AF7F1CE4DACE06A7E9BF951`。

**可能的原因**（我**未**核实）：构建嵌入了时间戳/版本串，使二进制不可复现。
⇒ **因此不能用"哈希未变"当作"源码未变"的证据**（反之亦然）。
本轮"源码未变"的依据是**文件内容逐字比较**（`restored: true`），不是哈希。

## 6. 状态

```
STATIC=261/0（259 + D2 的 1 个 + 本轮的 1 个静态文件）; meson 85/0 ; no-work
wire CHECKS=1160 FAILURES=0 ; evidence 31/0 ; 分析套件 106 OK
DLL: 41A70AA50AEABE4245F3D9F640ECC79501D403720AF7F1CE4DACE06A7E9BF951（源码与 round 48 逐字相同）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 7. 不声称

- **不**声称 D3 是"被本轮修好的"——它是**已被 round 10 顺带修掉**的，本轮只是**加锁并登记**；
- **不**声称 B1 的运行期半边、B3 的必要性已完成：D 的三条屏障测试里
  **B1 只做了静态半边**、**B2 已做（D2）**、**B3 尚未裁定是否必要**
  ——因为 D3 已被证明是结构性满足的；
- **不**声称 DLL 哈希可作源码同一性的证据（见 §5）；
- **不**声称 C 批次已完成（尾项卡在裁定）；**不**声称两个 P0 已完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。