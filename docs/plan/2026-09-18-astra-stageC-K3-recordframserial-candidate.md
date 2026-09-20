# 阶段 C · K3 §4①（续）：可能**不需要**新字段 —— `recordFrameSerial` 已按值携带 — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读侦察，本轮无代码改动。**

## 1. 发现

工厂签名（`war3_palette_object_capture.h:176-188`）：

```cpp
inline PaletteObjectFrames MakePaletteObjectFrames(
    uint64_t renderFrame, uint64_t recordFrameSerial, uint64_t nativeFrameTag,
    bool nativeKnown) noexcept {
  PaletteObjectFrames frames{};
  frames.renderFrame = renderFrame;
  frames.manifestFrameSerial = 0u;
  frames.manifestPublishRevision = 0u;
  frames.recordFrameSerial = recordFrameSerial;   // ← 已按值携带的"每次记录"序号
  frames.nativeFrameTag = nativeKnown ? nativeFrameTag : 0u;
  frames.manifestUnknown = true;
  frames.nativeUnknown = !nativeKnown;
  return frames;
}
```

而**拒绝点**（`war3_shadow_renderer_core.cpp:6821-6829`）传的是：

```cpp
MakePaletteObjectFrames(
    uint64_t(RenderState::instance().getFrameIndex()),   // renderFrame
    renderable.frameSerial,                              // ← recordFrameSerial
    uint64_t(recheckEvidence.currentPaletteFrameTag),    // nativeFrameTag
    recheckEvidence.currentPaletteFrameTag != 0u)        // nativeKnown
```

⇒ `recordFrameSerial` 取的是 **`renderable.frameSerial`** ——
**每个 renderable 各自的序号**，不是帧号。

## 2. 为什么这可能让 K3 **不需要新字段**

裁定要求「**attemptSerial 按值携带**」。若 `recordFrameSerial` 在四个构造点上确实满足：

1. 对**同一个对象**的**同一次尝试**，R/S/E/D 拿到**同一个值**；
2. 对**同一个对象**的**下一次尝试**，拿到**不同的值**；

那么它**就是**裁定说的 attemptSerial，K3 只需在 `MarkStage` 里改判序作用域，
**不必**新增字段、不必改工厂签名、不必改四个调用点 —— 改动面从"跨文件"收缩到**一个函数**。

## 3. 但**尚未证实**（本轮刻意停在这里）

已知的只有拒绝点那一处取值。**尚未读**：

- `d3d9_device.cpp:22121/22139`（`NoteServed` 路径）传的第三个参数是什么；
- `d3d9_device.cpp:23515`（`NoteFirstSight` 路径）同上；
- `d3d9_war3_shadow.cpp:5241` 同上（该点无相邻 Note*，角色仍未明）。

**必须核对的关键风险**：若服务路径传的是 `renderFrame`（帧号）而拒绝路径传的是
`renderable.frameSerial`（对象序号），那么**同一个对象**的 S 与 R 会拿到**不同体系**的值 ⇒
`MarkStage` 的"尝试号变化即重置"会被**错误触发**（每次交替都重置，等于把检查废掉）。
这正是 round 31 §3 标记的风险，本轮仍未排除。

## 4. 下一步

```
① 读 d3d9_device.cpp:22100-22160 与 23490-23540，确认第三/第二参数各自是什么；
② 读 d3d9_war3_shadow.cpp:5241 的上下文；
③ 若四处**同体系** ⇒ K3 = 只改 MarkStage（判序作用域），无需新字段；
   若**不同体系** ⇒ 必须显式引入 attemptSerial 并统一编号（改动面回到跨文件）；
④ 只有 ③ 定清后才动代码。
```

## 5. 状态（无代码改动）

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 30/0
分析套件 105 OK ; DLL 未变（01230C1F…）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 不声称

- **不**声称 K3 不需要新字段（**§3 明确尚未证实**）；
- **不**声称 `recordFrameSerial` 的语义已确定（只核对了**一个**构造点）；
- **不**声称服务路径与拒绝路径同体系（**恰恰相反：这是我标记的主要风险**）；
- **不**声称 K3 有进展（无代码改动，终态仍 Unclosed）；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。