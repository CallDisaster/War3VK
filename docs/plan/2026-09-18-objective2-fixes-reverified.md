# 复核：目标② 三项缺陷修复**仍在位**（多轮 ③ 改动后未被回归）— 2026-09-18

## 为什么要做这次复核

我连续多轮专注目标 ③（版本门控、deltaFrames、完成判据、写方修正①、实机 A/B、10 条链定案、守卫用例），
期间改了 `AutoTest/` 与 `src/d3d9/war3/tools/war3_palette_object_evidence.h`。
**长时间单点推进有回归风险**，而 ② 的三项修复是此前轮次的成果，不能假设它们还在。

⇒ 本轮**逐项取证**，而非凭记忆断言。

## 核查结果（全部在位）

### (a) legacy palette 选择「冷缓存首次查询」缺 groupCount 数量检查

```
共享谓词 ProducerGroupCountCovers 在 war3_live_palette_selection.cpp 的调用点: 2 处  → 339, 391
残留字面比较 producerGroupCount >= required                        : 0 处
```

✅ 两处（冷/暖缓存）都走同一个谓词；没有任何地方绕过它做字面比较。

### (b) 生产采集点 nativeKnown 误传 true

`src/d3d9/d3d9_device.cpp:23515-23520`：

```cpp
const auto paletteObjectFrames =
    dxvk::war3::tools::evidence::MakePaletteObjectFrames(
        uint64_t(currentRenderFrameIndex), uint64_t(manifestFrame),
        // 2026-09-18 独立复审 R2：本处**拿不到** native 帧，必须记
        // nativeKnown=false（⇒ nativeUnknown=true）；此前的 true 把
        // "未知"写成了"已知且等于 0"。
        0u, false);
```

✅ 实参确为 `0u, false`，且注释记录了"为什么必须 false"。

（另一处 `:22139` 走的是 draw-time 路径，**能**拿到 native 帧，因此那里用条件变量
`paletteObjectNativeKnown = !drawTimeVBOverrideApplied && selectedPalette.frameTag != 0u;` —— 
这不是 R2 的缺陷点，不受影响。）

### (c) 诊断全局计数器：原子性 + 位置

**原子性**：`src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp`

```
g_paletteObjectProductionInsertReached / g_paletteObjectProductionNoteCalled
  声明  : 38, 39      使用: 196, 199
```

✅ 已是原子变量（渲染线程写 / 控制线程读）。

**位置**：`src/d3d9/d3d9_device.cpp:23505-23509`

```cpp
23505:    if (dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled()) {   // 子门
23506:      dxvk::war3::tools::evidence::NotePaletteObjectProductionInsertReached();  // 在门内 ✅
23507:      const uint64_t evidenceSession =
23508:          dxvk::war3::tools::evidence::ActiveSession();
23509:      if (evidenceSession != 0u) {                                          // session 门
...
23523:      dxvk::war3::tools::evidence::NotePaletteObjectProductionNoteCalled();
```

✅ 第一个计数在子门**之内**（此前的缺陷是它在子门判断**之前**、关闭后仍更新）；
第二个在 session 门之内。

## 结论

**目标 ② 的三项修复全部在位，未被后续 ③ 的改动回归。**

## 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读取证）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```