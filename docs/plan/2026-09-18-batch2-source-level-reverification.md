# 批次 ② 修复的源码级实证复核（R2 / R3）— 2026-09-18

> 我的自我报告已被证伪两次（R3 泄漏、v3 只做写方），所以本轮**不看自己的叙述，直接读源码**核实批次 ② 是否真的落地。

## R2：生产采集点的 nativeKnown

`src/d3d9/d3d9_device.cpp:23514-23520`（生产实际路径）：

```cpp
const auto paletteObjectFrames =
    dxvk::war3::tools::evidence::MakePaletteObjectFrames(
        uint64_t(currentRenderFrameIndex), uint64_t(manifestFrame),
        // 2026-09-18 独立复审 R2：本处**拿不到** native 帧，必须记
        // nativeKnown=false（⇒ nativeUnknown=true）；此前的 true 把
        // 「未知」写成了「已知且等于 0」。
        0u, false);
```

✅ **修复在源码中真实存在**，理由就地写明。全文件 `nativeKnown` 只有这一处命中（即该注释），**无残留 `true` 调用点**。

## R3/(c)：诊断全局计数器的同步与位置

`src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp`：

```cpp
38: std::atomic<uint64_t> g_paletteObjectProductionInsertReached{0u};
39: std::atomic<uint64_t> g_paletteObjectProductionNoteCalled{0u};
196: g_paletteObjectProductionInsertReached.fetch_add(1u, std::memory_order_relaxed);
199: return g_paletteObjectProductionInsertReached.load(std::memory_order_relaxed);
```

✅ **已原子化**（渲染线程写 / 控制线程读不再无同步）。

**位置**：两个计数点都在 `d3d9_device.cpp:23505` 的 `if (PaletteObjectEvidenceEnabled())` **门内**：

```cpp
23505: if (dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled()) {
23506:   ...NotePaletteObjectProductionInsertReached();   // 门内
23523:   ...NotePaletteObjectProductionNoteCalled();      // 门内
```

⇒ 满足复审要求的「移入子门判断之后」。

## 附带确认：批次 3 的正常链**确实接在生产采集点上**

同处 `23528/23530`：

```cpp
paletteObjectRecorder.NoteFirstSight(paletteObjectKey, paletteObjectFrames);
paletteObjectRecorder.NoteEnqueued(paletteObjectKey,
    PaletteObjectSource::Unknown, 0u, paletteObjectFrames, false);
```

源码注释明确写出**为什么不用 NoteServed**：本处没有 selectedPalette，
发 NoteServed 等于「冒充 ServedCandidate」—— 符合复审约束「不伪造 Rejected / 不让 NoteEnqueued 自动插表」。

### 这条确认的一个**重要推论**

生产路径**每个对象**都会发 `NoteFirstSight` ⇒ 实机运行时 `firstSightUsed()` 必然为真 ⇒
**导出的块版本必然是 3**。

而这正是读方注册缺口致命的原因：修复前读方只注册 {1,2}，
**任何一次实机取证导出都会被整份拒绝** —— 不是「偶发」，是「必然」。
这个推论把该缺陷的严重性从「潜在」提升到「必然」，也说明场景 G 这条用例是必需的。

## 结论

| 项 | 源码级实证 |
| --- | --- |
| ②(a) R1 冷缓存 groupCount 检查 | ✅（此前已由共享谓词 `ProducerGroupCountCovers` + 纯谓词测试证明，两处调用） |
| ②(b) R2 nativeKnown=false | ✅ 本轮源码实证（23520 行实参 `false`） |
| ②(c) R3 计数器原子化 + 门内 | ✅ 本轮源码实证（atomic + 门内） |
| ③ 生产入口的正常链 | ✅ 本轮源码实证（23528 行 `NoteFirstSight`） |

## 仍未闭合

- **实机取证导出**：仍无（需先结束 `War3.exe` pid 18948）；
- 第④问第四子问题（正常对象是否被误伤）：仍只有读码 + 纯谓词证据；
- 现场 DLL = 基线 `F275545B…5CF07FF3`，未触碰；未提交、未部署。