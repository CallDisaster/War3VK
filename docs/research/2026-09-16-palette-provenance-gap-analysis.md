# 2026-09-16 — 蒙皮 palette：A 缓存复核 vs B 出版合同 缝隙分析（只读）

> 来源：合并台账 P1 项的子代理只读分析（grok-4.6），用户裁定"以 B 合同为基础、A 只查漏"的执行依据。
> 未改任何文件。A = dxvk（09-16 provenance 三元组缓存）；B = 本树（09-15/16 出版合同）。

## 结论摘要

A 修的是 **slot 号缓存的身份证明**；B 修的是 **矩阵字节的来源与新鲜度**。合同 ON 的
semantic owned/writer 路径已覆盖 A 的指针重用/frameTag 倒退/证明丢失三模式（且 frameTag
用"当前帧相等"比 A 的"不倒退"更严）。真正该从 A 学会的是 **B 里两处仍按"记住的 slot"
读 arena、且合同未设防的缓存**：

- **缝隙 B-S1（高）**：合同 OFF 时 B 的 useCachedEntry（d3d9_device.cpp:8633-8644）仍是
  A 修复前的形态——producer miss 时 return 缓存 slot，无 fail-visible。
- **缝隙 B-S2（高）**：war3_shadow_renderer_core.cpp:703-762 FindOrUpdatePaletteSlotCache
  是另一份 4096 缓存，合同开关管不到；currentSlotIndex 非法时直接返回上一帧缓存 slot
  （742-745），连 producer 都不问；tryEngineDirectPosePalette（6482-6496）用它乘 48 回读
  Game.dll+0xBC6BD0 arena。
- **缝隙 B-S3（中）**：CapturedWriter 在 IsSkinPaletteSelectionCurrent 只比 frame 相等，
  不重验生产者绑定（war3_model_hook.cpp:9355-9356）。
- B-S4（低）：QueryOwned 不校验 cell.renderablePart（ticket 已能挡旧 Selection）。
- B-S5（低）：live miss 回退 packet 旧 Selection（不回读 arena，风险低）。

## 移植裁定（P1 执行依据）

应补（按优先级）：
1. B-S2：ContractEnabled() 时禁用"非法 +0x08 用缓存 slot 读 arena"（742-745 改返回
   0xFFFFFFFF）；进一步可对标 A 的 producer 再确认。
2. B-S1：合同 OFF 的 useCachedEntry 移植 A 的证明丢失 fail-visible（A 7969-7982），
   或更干净地：producer miss 一律返回 0xFFFFFFFF；**不要**移植 A 的无证明快路径
   （A 7933-7936）。
3. B-S3（可选）：CapturedWriter 分支加 QueryRenderablePartPaletteSlot 再确认。
4. 诊断（可选）：mismatch 计数器接入 B 的 skin-selection 事件体系，而非照搬 A 的
   shadowStats 字段。

应放弃（B 已覆盖或方向相反）：
- TLS 三元组缓存进合同 ON 的 live 路径（B 不走该缓存，搬进=倒退）。
- frameTag 倒退谓词（B 的当前帧相等更严）。
- A 的 FROZEN"用 bindings 旧 slot 刷新 snapshot"（A 2610-2651；B 合同 ON 已否定）。
- A 快路径"+0x08 合法就不看证明"（A 7933-7936）。

## 详细证据

完整逐条 file:line 证据见会话记录（子代理报告全文）；关键锚点：
- A 实现：d3d9_device.cpp resolvePaletteSlotIndex（7933-7990）、折进 stats（21286-21293）；
  生产端 war3_model_hook.cpp QueryRenderablePartPaletteSlot（9208-9215）。
- B 合同：war3_skin_palette_selection.h（Source/Space/Domain/ticket/Usable/CanReplace）；
  集成 d3d9_device.cpp:8562-8579（合同 ON 只走 QueryOwned）、22977-22982（submit 新鲜度）、
  war3_model_hook.cpp:2499-2556（密封出版）、9351-9398（IsCurrent/OwnedSnapshotMatches）、
  war3_canonical_draw.cpp:262-268（canonical Usable 门）。
