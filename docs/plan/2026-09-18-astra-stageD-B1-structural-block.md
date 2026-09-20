# 阶段 D · B1 运行期半边：**结构性阻塞已查明** — 2026-09-18（round 52）

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**本轮无前进性改动**，但把阻塞从"失败"推进为"结构性原因"。

## 1. 三层错误，逐层查明

| 层 | 现象 | 真正原因 |
| --- | --- | --- |
| 第 1 层 | `ArmPaletteObjectEvidence was not declared` | **我的 include 插入静默失败**：测试文件的 include 字面是 `#include "../../tools/war3_palette_object_evidence.h"`（带相对前缀），而我猜的是 `#include "war3_palette_object_evidence.h"` ⇒ `replace` 什么也没做 |
| 第 2 层 | `did you mean \'PaletteObjectEvidence\'?` | **类型名也需限定**：该测试文件没有 `using namespace`，`PaletteObjectEvidenceHeader` 等非限定名找不到（namespace 本身是对的：`dxvk::war3::tools::evidence`，sink.h `:13`） |
| 第 3 层 | `collect2.exe: error: ld returned 1 exit status` | ★ **宿主测试目标不链接 sink 的编译单元** —— `ArmPaletteObjectEvidence` 等实现在 `war3_palette_object_evidence_sink.cpp`，该 TU 不在宿主测试的链接集里 |

## 2. ★ 结构性结论（本轮真正的产出）

**B1 的运行期半边不能放在 `war3_palette_object_evidence_test.cpp` 里。**
两个可行方向：

| 方向 | 代价 |
| --- | --- |
| **A** 放进 `war3_palette_object_wire_roundtrip_test.cpp` | 该目标**已经链接 sink**（它调用 `ArmPaletteObjectEvidence`）⇒ 无需改构建；但那是"生产编码往返"的载体，混入生命周期断言会让两类测试的职责不清（与 round 40 分列两清单的立场冲突） |
| **B** 把 sink TU 链接进宿主测试目标 | 需要改 meson 构建定义 ⇒ **扩大构建面**，且会让宿主测试从"纯 CPU 记录器测试"变成"链接生产 sink 的测试" |

⇒ **这是一个需要裁定的取舍**（职责清晰 vs 构建面），不是我能自行选定的。
本轮**不替裁定做这个选择**。

## 3. 我在本轮犯的错（同一类第 4 次）

1. **猜 include 字面**（第 1 层）—— 正确做法是**读出来再复用同一前缀**（我后来这么做了，一次成功）；
2. 限定替换时 `Disarm…` 含 `Arm…` 子串（round 51 已记，本轮未再犯，因为我改用**直接写全新代码**而不是毯式替换）。

⇒ 第 4 次同类错误了（round 16 块边界 / 34 锚点 / 51 子串 / 52 猜字面）。
**规律已足够清楚**：凡"我以为某段文本长什么样"的地方，一律**先读出来**。

## 4. 处置与状态

Case 32 与其 include 已逐字移除（`Case32` = false、`_sink.h` = false）。

```
STATIC=261/0 ; meson 85/0 ; no-work ; evidence 31/0 ; wire CHECKS=1160 FAILURES=0 ; 读方 106 OK
DLL : 41A70AA50AEABE4245F3D9F640ECC79501D403720AF7F1CE4DACE06A7E9BF951（未变）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 不声称

- **不**声称 B1 运行期半边有进展（**两次尝试均回退**）；B1 仍**只有静态半边**；
- **不**声称方向 A 或 B 是正确选择（**需裁定**）；
- **不**声称 D 阶段可出包（B1 未齐）；**不**声称 C 批次已完成（尾项卡裁定）；
- **不**声称两个 P0 已完成 ⇒ **不新增实机因果结论、不晋升稳定候选**；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。