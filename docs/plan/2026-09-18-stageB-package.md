# 阶段 B 单独出包记录 — 2026-09-18（round 54）

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。
> 裁定要求「**每阶段单独出包、单独验证**」。本轮完成 **B 阶段**的出包。

## 1. 交付物

```
E:\Work\WarVK-delivery-20260918-stageB-gate.zip
  77,926 B
  SHA-256  EB0B805922EB4F84E22626687A9F14EB420F004A5A9ECA682E3CFD09430AEA6F
```

包内（8 项，已逐条列出核对）：

```
artifacts/test_palette_object_wire_roundtrip.py         77,785 B   CB75971D…
artifacts/analyze_palette_object_evidence.py            64,931 B   1C3671C4…
artifacts/analyze_frame_evidence.py                     16,050 B   5D3DED9D…
artifacts/war3_palette_object_wire_roundtrip_test.cpp   37,803 B   056FD900…
docs/2026-09-18-astra-stageB-gate-restored.md            3,864 B   D66E528A…
docs/2026-09-18-astra-stageC1-reject-order-and-two-reds.md 6,869 B  AE0D4121…
docs/2026-09-18-astra-stageC1-complete-both-reds-resolved.md 5,702 B 374F8AF2…
MANIFEST.md                                              3,621 B
```

## 2. 为什么现在可以出 B 而不可以出 D

| 阶段 | 可否出包 | 理由 |
| --- | --- | --- |
| **B** | ✅ **可以** | 阶段**已全部完成**：`check_scenario_g` 恢复、`REQUIRED_SCENARIOS` 硬条件、5 个可失败性探针 |
| **D** | ❌ **不可以** | **B1 运行期半边未做**（卡裁定：宿主测试不链接 sink TU）⇒ 出包会把**未完成**阶段伪装成完成（round 50 的结论） |
| **C** | ❌ **不可以** | 两项尾项卡裁定（K3 生产侧、窗口维度） |

## 3. 包内 MANIFEST 的硬约束（已写明，不靠"读者自己注意"）

- 它是 **B 批次（1-a）**，**不是**代码修复版；
- **不是**稳定候选、**不是**发布；
- **不**证明任何**实机**行为改善；
- **不**声称"全门禁通过"——只列各条各自的结果；
- **不**含任何 `git` 写操作产物；
- 与 A 阶段 `doc-r1` **互不替代**（那个是文档更正版）；
- **不含** `d3d9.dll`（B 阶段是测试/门禁改动，不产出产品二进制）；
- **不含** C/D 批次产物。

## 4. 已知限制（包内已列）

1. `test_analyze_frame_evidence.py` **不在** `test_*_static.py` 通配内 ⇒ 不在 261 门禁中；
2. 门禁运行需 `DXVK_WAR3_FRAME_EVIDENCE=1` 与 `DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1`；
3. 包内哈希为**交付时**的值，验证时应复核（见 round 49 的教训：**哈希变化不等于源码变化**）。

## 5. 状态

```
STATIC=261/0 ; meson 85/0 ; no-work ; evidence 31/0 ; wire CHECKS=1160 FAILURES=0
DLL : 41A70AA50AEABE4245F3D9F640ECC79501D403720AF7F1CE4DACE06A7E9BF951（本包不含它）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作（本包由文件复制生成，未触碰任何仓库状态）
```

## 6. 不声称

- **不**声称 B 阶段的包在**实机**上被验证过（全是离线/宿主机工件）；
- **不**声称"全门禁通过"（裁定禁止）；
- **不**声称 C 或 D 阶段完成；**不**声称两个 P0 完成 ⇒
  **不新增实机因果结论、不晋升稳定候选**；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。