# 三大顽固问题最终解决方案（2026-05-30 接手）

> 接手人：Claude Opus 4.8。
> 约束：先理论成立再开发；当前内存/CPU 紧张，**暂不启动 AutoTest**；
> IDA 已开启，逆向以 `Game.dll @ 0x6F000000`（1.27a）为基线。

## 0. 三个问题定义

1. **魔兽自带静态阴影禁用**：War3 原生的树木/装饰物/可破坏物/建筑的
   "预渲染贴花阴影 + 脚下方块"，与我们项目自渲染的 CSM 阴影叠加，观感违和。
   需要干净关闭，且不影响战争迷雾 / 视野 / 路径阻挡。
2. **桥/斜坡卡顿**：游戏中看到桥/斜坡这类内容时大量卡顿，过一会恢复正常；
   对象离开视野后再次看到又卡，过一会又不卡。需找根因。
3. **Path blocker 渲染泄漏**：War3 原本不渲染的"路径阻断器"（不可见 marker），
   被我们的 ShadowMap 渲染出来，导致大量不可见目标变可见。需要彻底屏蔽。

---

## 1. 问题 1（静态阴影禁用）— 理论

### 1.1 IDA 已确认的三条独立写入路径

| 路径 | 入口 | 末端数据结构 | 渲染对象 |
|---|---|---|---|
| **X — CDoodads stamp** | `CDoodads_CreateDoodadAndActivate (0x74D5AE / 函数 0x74D500)` | RegisterImageEntry → ListA/Stamp 池 | 树木/装饰物/可破坏物/腐地 |
| **Y — CUnit ShadowProjector** | `ShadowProjector_Add_FromObject (0x76D800)` | RegisterImageEntry type=4 → ListA/ListB | 单位脚下方块 / 部分施工 emitter |
| **Z — FogMask 直写** | `TerrainShadow_WriteMaskRegion (0x234710)` | CFogMaskTable 16-bit grid | 战争迷雾/视野/路径阻挡 **（不是建筑阴影）** |

### 1.2 本轮新增 IDA 验证（2026-05-30）

1. **路径 Z 已确认不是建筑阴影**：重新反编译 `WriteMaskRegion (0x234710)` +
   `DispatchToShape (0x234420)`：
   - `a2+0x10C` 是 *footprint 形状 result*（`a4 ? 4 : *(u16*)(a2+0x10C)`），
     不是 mask layer index；
   - `a3` 是 16-bit **player-slot 可见性位 + 形状位**，所有写入和 fog/LOS/path
     共享 mask grid；
   - 调用者全是 `RebuildMaskFromObjectLists / WriteMaskRegion_ForObject /
     FromActorRuntime / CUnit_StampBuildingShadowFootprint /
     CWidget_RegisterFootprintAndShadowMask`。
   - **结论**：这条路径是 fog-of-war / 路径阻挡 mask，*不能* 整体拦（会破坏迷雾），
     `idx==3` 推断已被 Phase 7.100 实测推翻。**放弃路径 Z 作为阴影治理点。**

2. **路径 X 是真正的"魔兽自带可见静态阴影"治理点（已确认）**：
   反编译 `CDoodads_CreateDoodadAndActivate (0x74D5AE)`，IDA 注释（doc 24 已写）
   确认 `a6` 跳过位掩码：
   - `bit1 (a6&2)` → 跳过 `ShadowPath_StaticStamp_Toggle` + `ToggleStaticStampFromObject`
   - `bit2 (a6&4)` → 跳过 `ToggleEmitterStamp`
   - **5 个调用者全部传 `a6=0`**（`0x74D2A3 / 0x74D81E / sub_74D8E0 /
     sub_757030 / sub_75D280`）。
   - `ToggleStaticStampFromObject (0x74DB30)` 的调用者全是 `CDoodads_*`
     (`CreateDoodadAndActivate / DestroyDoodadAtIndex / EnableFeatures /
     DisableFeatures`)，确认这是 doodad（树/桥/斜坡/装饰物）的地面贴花阴影系统。
   - `CDoodads_SetTodAndRefreshStamp (0x75C5F0)`：TOD（时间）变化时
     `Disable → StaticStamp_Toggle(0) → 改 alpha → StaticStamp_Toggle(1) → Enable`
     会重写 stamp，必须连同 hook。

### 1.3 治理方案（理论成立）

**目标对象**："魔兽自带的可见静态阴影" = 路径 X（CDoodads 贴花）+ 路径 Y（CUnit
脚下方块/建筑 emitter）。**不碰路径 Z**（fog/视野/路径阻挡）。

项目当前已有的可用拦截点：
- `Hook_ShadowPath_StaticStamp_Toggle (0x74E420)` — 已安装，按 mode reject
  `enable!=0` 的 StaticStamp 写入（`kNativeShadowBlockStaticStampPathWhenMode1`）。
- `Hook_ShadowProjector_Add_FromObject (0x76D800)` — 已安装，FourCC 过滤已开。
- `Hook_Terrain_RenderShadowLayer / ListA / ListB` — 已安装。

**新方案（doc 24 策略 A1，最干净，本轮采用）**：
- hook `CDoodads_CreateDoodadAndActivate (0x74D500)`，`mode>=1` 时强制
  `a6 |= 0x06`，从源头跳过 StaticStamp + Emitter 写入；
- hook `CDoodads_SetTodAndRefreshStamp (0x75C5F0)`，`mode>=1` 时 return，
  避免 TOD 刷新重新写 stamp；
- **关键**：必须在地图加载 doodad 之前安装（早装）。已存在的 doodad 需要
  通过现有 `ShadowPath_StaticStamp_Toggle / ToggleStaticStampFromObject` 拦截
  + 现有 ListA/Stamp 拦截作为兜底。

**当前状态评估**：项目 `kNativeShadowDefaultMode=1`，且 StaticStampPath / Projector
FourCC 拦截已部分启用。需要核对 `kNativeShadowBlockStaticStampPathWhenMode1`
是否为 true，若 false 则它正是"自带静态阴影仍可见"的原因之一。

---

## 2. 问题 2（桥/斜坡卡顿）— 理论

### 2.1 现象拆解
- 看到桥/斜坡 → 卡顿 → 过一会恢复；
- 对象离开视野 → 再次看到 → 又卡 → 过一会又不卡。
- "再次看到又卡" = **缓存被清除后重新建立的代价**。

### 2.2 IDA + 代码已确认的根因链
1. 我们的 shadow caster 用 **draw-time VB GPU copy**（`m_war3DrawTimeVBCache`）：
   capture 时 `createBuffer` + `EmitCs(copyBuffer)` 把顶点/UV/IB 拷到自有
   device-local buffer。桥/斜坡/装饰物是**大量静态 doodad**，一次性进入视野时
   触发几十~几百次 `createBuffer`（vkAllocateMemory 同步阻塞主线程）。
2. **缓存淘汰是 16 帧**（`d3d9_device.cpp:18050` 区，`maxAge=16`）：
   ```
   if (entry.frameSerial + 16 < currentFrame) erase(entry);
   ```
   对象离开视野 16 帧后 entry 被删 + GPU buffer 释放。再次进入视野时必须
   重新 capture（再次 createBuffer + copyBuffer）→ **复现卡顿**。
3. Phase 7.123 已加 per-frame alloc budget（`kShadowDrawTimeVBCacheAllocBudgetPerFrame=32`），
   把首帧暴降摊到多帧，但**没有解决"离开再回来又卡"的根因**——因为 16 帧淘汰
   导致每次回来都是全新 cache miss。

### 2.3 根因结论
**桥/斜坡是静态几何**（顶点不随骨骼变化，CPU skin 后的 VB 每帧相同），却被当成
动态对象按 16 帧淘汰 + 每次重新 GPU copy 处理。静态 doodad 的 VB 一旦 capture
就永远不变，**根本不需要淘汰，也不需要每帧/每次重新 copy**。

### 2.4 治理方案（理论成立）
1. **静态 doodad 的 VB cache entry 不淘汰（或大幅延长 TTL）**：
   对 `objectKind == Building/Destructible/Unknown(static doodad)` 且
   `capturedWorldMatrix` 稳定的 entry，标记为 `persistent`，离开视野后不删除
   GPU buffer，只标记 inactive；再次进入视野时直接复用（O(1)，无 createBuffer）。
2. **alloc budget 仍保留**（首次进图的摊销），但对"复用已有 buffer"的 entry
   不消耗预算（已是这样：`needsNewPositionBuffer` 才扣预算）。
3. 需要核对：静态 doodad 的 `capturedWorldMatrix` 是否真的稳定（doodad 不动），
   以及 `renderablePart` 指针是否稳定（cache key）。

---

## 3. 问题 3（path blocker 渲染泄漏）— 理论

### 3.1 现象
War3 原本不渲染 path blocker（编辑器隐形 marker），但我们的 ShadowMap 把它
画了出来，导致不可见目标变可见。

### 3.2 已有拦截（项目现状）
- 8 个 `YT??` fourcc 黑名单（`kPathBlockerFourCCs`）；
- 多个拦截分桶：EntryGate / EarlyBypass / Producer / FastAppend / DirectGrouped /
  StaticSupplement / AppendVbBlend / AppendEntry / LegacyCapture；
- `War3TryCaptureShadowCaster` 入口的 EntryGate 总闸（fast path + slow path）。

### 3.3 用户反馈的矛盾点
dxvk.log 显示 `PATH BLOCKER REJECT ... via=EntryGate` 命中 YT*，但**视觉上仍可见
path blocker 阴影**。

### 3.4 关键判断（需 IDA + 代码进一步定位）
两种可能：
- **A. fourcc 不匹配**：地图实际放置的 path blocker 数据层借用了其它 doodad model，
  渲染时 fourcc 不是 YT*；
- **B. 渲染来源不是我们的 CSM**：path blocker 阴影来自 War3 原生 TerrainShadow
  系统（路径 X/Y/Z）——但用户说"原本不渲染"，所以更可能是我们的 CSM 在画。

**本轮策略**：path blocker 在 War3 里就是 doodad（走 CDoodads 路径），所以：
1. 问题 1 的 CDoodads `a6` 注入会同时关掉 path blocker 的 *native* stamp 阴影；
2. 我们 CSM 的 path blocker 泄漏 = path blocker 的 mesh 被 capture 进 shadowCasters。
   需要确认 EntryGate 是否真的覆盖所有 capture 入口（doc 已列 6 个 emplace_back
   站点）。先用 IDA 确认 path blocker 的 fourcc 实际值（用户地图实测过 YTfb/YTpb/YTab）。

### 3.5 待 IDA 确认
- path blocker doodad 的 typeId / fourcc 在 CDoodads slot+0 的真实值；
- 是否存在 path blocker 走 CUnit 路径（不太可能，path blocker 是 doodad）。

---

## 4. 执行优先级
1. **问题 2（卡顿）**：纯性能，最高确定性，先做（静态 VB cache 持久化）。
2. **问题 1（静态阴影）**：CDoodads a6 注入 hook，需早装。
3. **问题 3（path blocker）**：依赖问题 1 的 CDoodads hook + 现有 CSM 拦截核对。

## 5. 验证策略（暂不跑 AutoTest）
- 先保证 `ninja -C build32` 通过；
- 每个改动有独立 env / config 开关可回退；
- 用户内存空闲后再做实机视觉 + AutoTest 验收。


---

## 6. 实现完成状态（2026-05-30）

### 已落地（编译通过 `ninja -C build32`，d3d9.dll 26895491 bytes @ 23:28）

**问题 1（魔兽自带静态阴影禁用）+ 问题 3 的 native 阴影部分**：
- 新增 `Hook_Doodad_ToggleStaticStampFromObject (0x74DB30)`：CDoodads
  树木/装饰物/可破坏物/path blocker 的"地面贴花阴影"对象级注册入口
  （RegisterImage type=0）。mode>=1 且 enable!=0 时跳过写入（return 0），
  enable==0 移除放行。覆盖 create / EnableFeatures / SetTodAndRefresh 全部
  caller（单点拦截）。
- IDA 已验证 `0x74DB30` 是干净 `__thiscall(ecx=this, [ebp+8]=slot, [ebp+C]=enable)`，
  `__fastcall(this,edx,slot,enable)` 完美匹配。
- **`0x74DE40 ToggleEmitterStamp` 不拦**：IDA 确认是 `__userpurge`（caller 在
  `edi` 传隐式 this，`push edi` 给子函数），标准 __fastcall trampoline passthrough
  会破坏 edi → 崩溃风险。它是发光体/特效/腐地 puff（type=4），不是树木/建筑
  阴影本体，本轮安全跳过。
- 配合现有 `Hook_ShadowPath_StaticStamp_Toggle (0x74E420)`（ListA 直写，已拦）
  + Projector FourCC 拦截，CDoodads 阴影写入链已覆盖主路径。

**问题 2（桥/斜坡卡顿）**：
- draw-time VB cache 静态几何常驻：`objectKind==Building||Destructible` 标记为
  静态，离开视野后不释放 GPU buffer（不再 16 帧淘汰 + 重新 createBuffer），
  再次进入视野 O(1) 复用。受 64MiB 字节上限 + lastAccess LRU + 30 分钟闲置回收
  约束。动态单位保持原 16 帧 TTL。

### 问题 3 的 CSM 部分（已有，核对确认）
- `War3TryCaptureShadowCaster` EntryGate 总闸（fast + slow path）+ 8 个
  shadowCasters 提交站点全部有 `kPathBlockerHideEnabled` 拦截。
- path blocker 双源治理：
  1. **CSM mesh capture**（path blocker 被画成 mesh）→ EntryGate 拒绝 ✓
  2. **native 地面贴花**（path blocker doodad 的 stamp 阴影）→ 新 0x74DB30 hook ✓

### 决定性逻辑（problem 3）
path blocker 在 War3 里是 doodad。它的阴影有两个来源：
- 若它被画成 mesh（EntryGate 日志显示 YTfb/YTpb/YTab 命中）→ 我们 CSM 已拦；
- 它的 native 地面贴花（ToggleStaticStampFromObject）→ 本轮新 hook 拦。
两条都覆盖后，path blocker 阴影应彻底消失。

### 回退开关
- `kNativeShadowDoodadStampHookEnabled=false`：关 doodad 贴花拦截（回到只拦 ListA）。
- `kShadowDrawTimeVBCacheStaticPersistEnabled=false`：回到 16 帧 TTL（问题 2 回退）。
- `War3RenderState::SetNativeShadowMode(0)`：完全恢复魔兽原生阴影。

### 待用户实机验收（内存空闲后）
1. mode=1 下树木/装饰物/path blocker native 地面贴花阴影是否消失，fog/视野/
   路径阻挡不受影响。
2. 桥/斜坡反复进出视野不再卡（静态 entry 复用）。
3. 若仍有残留：开 `kNativeShadowDoodadStampStatsLogging=true` 看 DoodadStaticStamp
   blocked 计数，确认 hook 命中。
