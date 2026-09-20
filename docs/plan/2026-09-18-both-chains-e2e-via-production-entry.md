# ✅ 两类链的生产入口端到端集成测试完成（场景 G / 版本 3）— 2026-09-18

> 补上外部复审明确要求的那一项：「用**实际生产入口**完成**两类链**的最小集成测试」。
> **完全不需要实机** —— 走的是宿主机往返（真实写方 → 真实导出 → 两个真实读方）。

## 1. 做了什么

在宿主机往返测试里新增**场景 G**（`war3_palette_object_wire_roundtrip_test.cpp`）：

```cpp
void ScenarioG(const ScenarioIO& io) {
  const PaletteObjectKey key = MakeKey(io.session, true, 0u);   // 生产形状键（弱身份、无生命周期身份）
  PaletteObjectEvidence& recorder = PaletteObjectRecorder();
  recorder.NoteFirstSight(key, FramesAt(kRejectFrame));          // 首次观察即建条目
  recorder.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0xB2ull,
                        FramesAt(kEnqueuedFrame), false);
}
```

与 A/B/C 的**关键区别**：G 里**没有任何 `NoteReject`**。这正是复审对"正常观察链"的定义。

**位置**：`RunScenario("G", ...)` 必须排在 F **之后** —— A..F 的序号被驱动的钉死值（session 世代等）依赖，
我第一版插在 E 之前，导致 6 处断言失败（E/F 的 session 世代被移动），已改到末尾。

## 2. 结果：两类链由生产入口端到端可区分

```
场景 A（拒绝恢复链）
  generic reader : ACCEPTED {"paletteObject": 2}
  palette parser : ACCEPTED formatVersion=2  recovered=[(...)]     <- 有"恢复"

场景 G（正常观察链，生产入口）
  generic reader : ACCEPTED {"paletteObject": 3}
  palette parser : ACCEPTED formatVersion=3  recovered=[]          <- 无"恢复"
```

**两点关键性质**：
1. G 的导出是**版本 3**，且被**两个读方都接受** —— 这正是此前缺失的读方注册；
2. G 的 `recovered` **为空** —— 正常完成**不会**被报成"恢复"。
   这直接对应复审的判据：「正常完成不能叫恢复」。

驱动最终输出：

```
CHECKS=1117 FAILURES=0
SPEC_SATISFIED=['A','B','C','D','E','F','G']  SPEC_UNMET=[]
ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED
```

## 3. 过程中改的第三处（驱动侧）

`test_palette_object_wire_roundtrip.py`：

| 改动 | 原因 |
| --- | --- |
| `SCENARIO_ORDER` 增加 `"G"` | 场景集合变了 |
| `parse_exports` 的正则 `SCENARIO=([A-F])` → `([A-G])` | 正则**硬编码了 A-F**，G 被静默过滤掉 |
| 提示文本去掉硬编码的 "A..F" | 否则报错信息会误导 |
| 新增 `check_scenario_g()` | 断言：通用读方接受为 3、解析器报 formatVersion 3、**recovered 必须为空**、恰好 1 条链 |

## 4. 全套件验证

```
ninja -C build32          -> 完成； ninja -n -> no work
全量静态脚本               -> 259 scripts, 0 failed
meson test                -> 85 Ok / 0 Fail   （含带场景 G 的往返目标）
```

## 5. 检查点身份更新（我改了 src/）

| 对象 | 旧 (c1) | **新** |
| --- | --- | --- |
| 源码指纹（932 文件） | `57D917F9…D4670ECD` | **`064B316AD6407E3C405B9CEBD00978B30CF8D432B318E7A9B14D2DB53B69DE25`** |
| 根配置指纹 | `612B91B4…B0E5C3` | 不变 |
| 候选 DLL | `BBEF3BBC…DE0DE357A`（36,297,472 B） | **不变**（改动只在测试 .cpp，不进 DLL） |

⇒ **现场部署用的候选二进制没有变**；变的只是测试源码。

## 6. 目标完成度

| 项 | 状态 |
| --- | --- |
| ① 检查点 | ✅（c0 + c1，身份已按本轮更新） |
| ② 三项缺陷 | ✅ |
| ③ 有版本正常观察链 | **✅ 本轮补上"生产入口端到端"这一环**（写方 + 读方注册 + 按版本 counters + 跨语言门禁 + **场景 G 往返**） |
| ④ 严格契约确认 + 真实入口对照 | 前三子问题 ✅；**第四子问题（是否误伤）仍缺运行期证据** |
| 套件 | 静态 259/0、meson 85/0、runnable 74/76；未跑 TDR/ABBA、前台门、**实机** |

## 7. 仍未闭合

- **仍无实机 v3 导出**：v3 现在在宿主机上端到端打通（生产写方 → 导出 → 两个读方），但**未经实机**；
- 第④问第四子问题（正常对象是否被误伤）：仍只有读码 + 纯谓词证据；
- 阻塞：`War3.exe` pid 18948 仍存活（需人工结束）⇒ 实机采集运行无法启动；
- 现场 DLL = 基线 `F275545B…5CF07FF3`，未触碰；未提交、未部署。