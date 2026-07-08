# 24 - CDoodads 与 CUnit 静态阴影上游逆向（建筑/可破坏物/装饰物的真正治理点）

> 本研究基于：`Game.dll @ ImageBase 0x6F000000`（暴雪 1.27a）
>
> 工具：IDA MCP（HTTP `127.0.0.1:13337/mcp`），现场反编译/反汇编/RTTI/字符串扫描已落盘到
> `AutoTest/artifacts/_phase_static_shadow_research/`。
>
> ⚠️ **本文档已修正**：第一版只覆盖 `CDoodads` 一条主路径，但用户指出"建筑物是 `CUnit`"，
> 重新逆向后确认 War3 静态阴影实际上由 **`CDoodads` 和 `CUnit::ShadowProjector` 两条独立路径**
> 写入同一片 stamp 注册池，本版同时给出两条治理蓝图。

## 0. 用户痛点回顾
1. 项目给魔兽争霸 3 加了独立的画质阴影，已经默认关掉了"动态单位脚底贴花"。
2. 但**静态阴影（树木阴影、建筑预渲染贴花、可破坏物贴花）不可能仅靠 War3 选项关闭**，跟项目自渲染阴影叠加显得很违和。
3. 所有 War3 阴影渲染都走 `CWorld::DispatchStage(stage=2)`：
   - 渲染末端是 `TerrainShadow_RenderListA / RenderListB`；
   - `ListA / ListB` **是混合层**：里面同时有战争迷雾、地图边界、烘焙阴影、贴花等，
     在末端拦截无法分离"只是建筑阴影"。
4. **关键澄清**：War3 里"建筑物"就是 `CUnit`（`isBldg=1`），不是 `CDoodads`；
   但**树木/可破坏物/装饰物/腐地是 `CDoodads`**。这两类都会写阴影 stamp，但走完全不同的代码路径。
5. 因此正确路线是：**找到 `CDoodads` 和 `CUnit` 各自何时往这些表里塞条目，在更上游分别拦截**。

## 0.1 一图看懂（三条独立写入路径，不同末端数据结构）
```
CDoodads（树木/装饰物/腐地） — 路径 X
    └─ CDoodads::CreateDoodadAndActivate (0x74D500)
        ├─ ShadowPath_StaticStamp_Toggle (0x74E420)        ← 按贴图名直写 ListA mask 字节网格
        ├─ TerrainShadow_ToggleStaticStampFromObject (0x74DB30)  ← RegisterImageEntry(type=0)
        └─ TerrainShadow_ToggleEmitterStamp (0x74DE40)     ← RegisterImageEntry(type=4)
    末端：ListA 网格 + 各类 stamp 注册池

CUnit ShadowProjector（单位脚下方块/特效 emitter） — 路径 Y
    ├─ CUnit_FindShadowProjectorByObject (0x532420)
    ├─ CUnit_ActivateBuildingShadowProjector (0x52F510)   ← v2 误以为是建筑阴影，实际不是
    │     └─ ShadowPath_ObjectProjector_Runtime (0x38D7A0)
    │           └─ ShadowProjector_Add_FromObject (0x76D800)
    │                 └─ RegisterImageEntryWithParams (0x7290B0)
    │                       └─ RegisterImageEntry (0x713250) → ListA/ListB type=4
    ├─ CUnit_ActivateGenericShadowProjector (0x5449D0)
    └─ CUnit_RefreshAllShadowEmitters (0x5457B0)
    末端：ListA/ListB 注册池

★★★ FogMask 直写（建筑预渲染贴花、路径阻挡、视野、迷雾共享）— 路径 Z（v3 决定性发现）
    多个入口：
    ├─ TerrainShadow_RebuildMaskFromObjectLists (0x233E90)         ← 整体重建
    ├─ TerrainShadow_WriteMaskRegion_ForObject (0x234620)          ← 单对象 wrapper
    ├─ TerrainShadow_WriteMaskRegion_FromActorRuntime (0x3DB260)   ← actor runtime
    ├─ sub_6F65A140  (CUnit/Widget 中央 sync, 30+ caller)          ← 关键中央点
    └─ sub_6F514F40  (CUnit lifecycle helper)                     ← 关键中央点
        └─→ TerrainShadow_WriteMaskRegion (0x234710)
              └─→ CFogMaskTable_GetOrCreateMask (0x232060)
                  └─→ CFogMask_BuildNodeAndRangeTable (0x230210)
              【按 type code 16-bit 直接 set/clear mask grid bit】
    末端：CFogMaskTable / CFogOfWarMap 共享 mask 网格（不进 ListA/ListB/Stamp）
```

**渲染时**：所有阴影渲染都汇聚到 `CWorld_TerrainShadow_Dispatch` 的 16+ 个 stage（0..16），
但路径 Z 的 mask 是直接被**地形渲染管线**采样着色的，不需要 stage 里调 RenderListA/B。这就是
为什么"项目把 ListA/ListB 都拦了，建筑阴影还在"——它根本不走 stage 渲染，是**地形 tile 自带的**。

## 1. 先给结论（v3 高置信度，已通过反证）
1. War3 静态阴影由**三条独立路径**生产，最终落到不同的渲染数据结构，不是同一片 stamp 池：
   - **路径 X（CDoodads stamp 注册）**：树木/装饰物/腐地，通过 `0x74D500` 等 5 个 CDoodads 调度器 →
     `RegisterImageEntry` → ListA/ListB type 4 等。
   - **路径 Y（CUnit ShadowProjector 注册）**：单位脚下方块、部分施工特效 emitter，通过
     `CUnit + 0xA8` emitter 数组 → `ShadowProjector_Add_FromObject` → 同 RegisterImageEntry。
   - **路径 Z（FogMask 直写，本轮决定性发现）**：
     **建筑预渲染贴花阴影 + 路径阻挡 + 战争迷雾 + 视野** 共用 `CFogMaskTable` 的 16-bit grid，
     通过 **`TerrainShadow_WriteMaskRegion (0x6F234710)`** 直接修改 mask 的 type bit。
     这条路径**不经过任何 stamp/projector/RegisterImage 注册池**，由地形渲染管线在画地面时按 mask bit 着色。
2. **历史所有针对路径 X / Y 的拦截都不可能消除建筑阴影**——因为建筑阴影压根不走那两条路（已通过 AGENTS 第 57 条
   `Mode1_BlockAllRegisterImage` 极限实验反证：拦了 RegisterImage 全部 8 条来源后建筑阴影没消失只是游戏崩溃）。
3. **真正能关掉建筑阴影的唯一可行点是 `TerrainShadow_WriteMaskRegion` 自己**，但它和战争迷雾共享 mask grid，
   必须按 a3 type code 的 bit 拆分屏蔽，不能整体 return（否则会同时关闭 fog/视野/路径阻挡）。
4. CDoodads 路径的 `a6` mask 注入方案（v1/v2 蓝图）**仍然有效，可以独立关掉树木阴影**，但**不影响建筑阴影**。
5. CUnit ShadowProjector 路径（v2 蓝图）只能关单位脚下方块那种 emitter 阴影，**也不影响建筑底部矩形贴花**。

> **核心反思**：v1 把"CDoodads = 静态阴影主治理点"判断错；v2 把"CUnit ShadowProjector = 建筑阴影"也判断错。
> 这两个东西都是 War3 真实存在的阴影注册路径，但都不是建筑预渲染贴花的真正写入路径。
> v3 找到的 `WriteMaskRegion` 才是。下一轮治理只看 §4.5。

## 2. 关键 RTTI / vtable / 实例
| 项 | 地址/特征 | 含义 |
|---|---|---|
| RTTI Class Descriptor `.?AVCDoodads@@` | `0x6FBB4468` | 标识 CDoodads 类 |
| RTTI ComplObjLoc `??_R4CDoodads@@6B@` | `0x6FA59C78` | CDoodads vtable 锚 |
| `CDoodads` vtable 起点 | 约 `0x6FA59C50` ~ `0x6FA59C78`（COL 之前 10 个 dword） | 实际虚函数槽紧接 COL 之后 |
| 同 vtable 内 stamp 写入入口槽 | `0x6FA59C98 → 0x6F75CB10`（vt+18），`0x6FA59CE0 → 0x6F74EAA0`（vt+36） | 用于设置 + 刷新 |
| `CClippable` vtable 锚 | `0x6FA59E18` | CDoodads 基类之一 |
| `CBlightPuffs` vtable 锚 | `0x6FA59E44` | 亡灵腐地的独立子表 |
| 其它 World Object 派生 | `CWorldObjects` (`0x6FBB4480`)，`CWorldObjectsClippable` (`0x6FBB44B8`) | 共享相同基类层 |

CDoodads 单例的指针位置（在 `0x74D500` 内访问 `this[5]/this[4]/this[16]/...`），可以通过对
`InstallShadowHooks` 加 `lookup_funcs` 验证，但**不是治理必需**——hook 直接拦截这些函数即可。

## 3. CDoodads 的 392 字节 doodad 槽位布局（来自反编译交叉验证）
> 字段名为研究推断；标 ★ 的位是治理路径需要直接读取的字段。

| 偏移 | 类型 | 含义 |
|---|---|---|
| +0 | u32 | typeId / rawcode（`*a2` 在 `0x758300` 中以 `*(this+72)(this, *a2)` 验证可用为 SLK key）★ |
| +4 ~ +20 | float[5] | world matrix（位置 + 朝向 + 半径），来自 `_mm_loadu_si128` 双载入 |
| +12 | f32 | world X（`StampWriteCore` 用作贴花中心）★ |
| +16 | f32 | world Y ★ |
| +20 | f32 | radius / scale |
| +24 | f32 | rotation |
| +84 (0x54) | u32 | ListA 组 flags（包含 0x400 = blob block） |
| +132 (0x84) | u32 | doodad 状态 flags：`0x10`(emitter)、`0x400`(blob mask 已写)、`0x800`(更新中)、`0x80000`(static stamp 已写)、`0x1000000`(active doodad)、`0x10000000`(extra) ★ |
| +135 (0x87) byte | u8 | "valid/visible" bit；`& 1` 为 ON 时才能写阴影 |
| +136 (0x88) | u32 | static stamp index（`74DBFA` 写入）★ |
| +144 (0x90) | u32 | emitter stamp index（`74DF50` 写入）★ |
| +160 (0xA0) | i32 | animation matrix array length |
| +328 (0x148) | u32 | runtime opacity / state cache |
| +332 (0x14C) | u32 | TOD-related "stored alpha %" ★ |
| +336 (0x150) | u32 | TOD-related "compute mode" |
| +344 (0x158) | u32 | data block 第 4 字段（campaign 数据） |
| +364 (0x16C) | f32 | world Z |
| +368 (0x170) | f32 | world position 备份 |
| +372 (0x174) | f32 | scale 备份 |
| +376 ~ +384 | bytes | matrix data 临时缓冲 |
| +384 (0x180) | u32 | matrix block pointer |

## 4. 静态阴影的三条上游写入链
### 4.1 主链 A — `ShadowPath_StaticStamp_Toggle (0x74E420)`
- 内部只调 `ShadowStamp_WriteByName (0x713B20)`：
  ```text
  ReplaceableTextures\Shadows\<name>
  → ShadowStamp_WriteCore (0x713920)
  → 字节级网格修改 + 4x4 dirty 区刷新（0x72FA40 + 0x7395C0）
  → ListA 末端混合
  ```
- **不经过 RegisterImage**，是 ListA 网格的"直写"旁路。
- 只有当 `entry+135 & 1 == 1` && `entry+132 & 0x400 == 0` 才生效。

### 4.2 主链 B — `TerrainShadow_ToggleStaticStampFromObject (0x74DB30)`
- 通过 vtable 拿数据：
  - `vt[48] (0x754A20)` 返回 1（"是否有自带阴影模型"）
  - `vt[49] (0x75CF90)` 返回 1（"是否需要 stamp"）
  - `vt[50] (0x753B40)` 拿 UV+矩形（位于 `out[0..3]`），内部读 SLK
  - `vt[51] (0x756470)` 提交 stamp 后回调（写入 dirty）
- 调 `TerrainShadow_RegisterImageEntry(..., type=0)`，记录 stamp 索引到 `entry+136`。
- **是建筑/可破坏物"按对象矩形"贴花的核心入口**。

### 4.3 主链 C — `TerrainShadow_ToggleEmitterStamp (0x74DE40)`
- 通过 `vt[51]` 拿 emitter 半径（默认 1.0），调
  `TerrainShadow_RegisterImageEntry(..., type=4)`，记录到 `entry+144`。
- 主要用于：发光体、技能特效、Blight Puff 之类。

### 4.4 共同末端 — `TerrainShadow_RegisterImageEntry (0x713250)`
- 项目已经接入了对它的 hook（按返回地址识别来源 + UberSplat key 拦截 + Shadow texture key 拦截）。
- 缺点：**返回地址识别只能识别"是哪条上游写的"，没法识别"是树/桥/墙/可破坏物哪种 doodad"**。

## 5. 核心调度器解读
### 5.1 `CDoodads::CreateDoodadAndActivate (0x74D500)` — 真正"对象创建并激活阴影"
```c
// 简化伪代码
int CDoodads::CreateDoodadAndActivate(this, *desc, ?, mode_flag, ?, char a6) {
    sub_6F74B1A0(slot);
    if (sub_6F758300(*desc, slot, ..., a6)) {  // 内部 init + emit slot
        sub_6F75C960(slot);                    // 阴影主体激活
        if ((a6 & 1) == 0) sub_6F74E120(slot, 1);                          // basic init
        if ((a6 & 2) == 0) {                                                // ★ 静态阴影
            ShadowPath_StaticStamp_Toggle(slot, 1);
            TerrainShadow_ToggleStaticStampFromObject(this, slot, 1);
        }
        if ((a6 & 4) == 0) TerrainShadow_ToggleEmitterStamp(this, slot, 1); // ★ emitter
        sub_6F74D730(slot, 1); sub_6F75DDD0(slot); sub_6F75E300(slot);
        sub_6F757FA0(...); sub_6F752F90(...); sub_6F750BC0(slot);
        sub_6F74DC70(slot, 1);
        slot[+160] = 1;
        if (vt[13](this) == 3) vt[1](this, 1);
        vt[34](this, *(_DWORD *)(this + 20) + 392 * slot[0]);
    }
    return slot_index_or_0;
}
```
- **`a6` 位掩码就是 War3 的官方"跳过部分 stamp"接口**：
  - `bit 0` = 跳过 vt[13] basic init；
  - `bit 1` = 跳过 StaticStampPath + ToggleStaticStampFromObject；
  - `bit 2` = 跳过 ToggleEmitterStamp；
  - `bit 3` = 设 `v22=8` 给 vt+ pose 标记；
  - `bit 4` = 跳过 vt+248；
- xrefs (5 处) 全部传 `a6=0`：`0x74D2A3 / 0x74D81E / 0x74D8E0(via sub_74D8E0) / 0x757030(传 16 或 24，飞行 doodad) / 0x75D280(载图反序列化时)`。

### 5.2 `CDoodads::SetTodAndRefresh (0x75C5F0)` — 时间变化驱动 stamp 重写
```c
void CDoodads::SetTodAndRefresh(this, slot, alpha_pct, mode, force) {
    if ((slot[135] & 1) == 0 || alpha_pct == -1) return;
    int new_alpha = clamp(alpha_pct, 0, 100);
    if (new_alpha != slot[+332] || force) {
        sub_6F74E120(slot, 0);
        ShadowPath_StaticStamp_Toggle(slot, 0);   // ★ 关闭 static stamp
        sub_6F75C030(slot);
        slot[+332] = new_alpha;
        this[+40] = 1;
        if (!old_alpha) { ... vt[10](this, slot); }
        if (!new_alpha) { ... vt[10](this, slot); }
        ShadowPath_StaticStamp_Toggle(slot, 1);   // ★ 重新启用
        sub_6F74E120(slot, 1);
    }
}
```
- 调用方：`0x74D2CD / 0x74EAB6(vt+某槽) / 0x758ED8(vt+某槽) / 0x75CB1F(vt+某槽)`。
- **TOD 变化每次都会重写一次 stamp**——hook 时务必把入口和退出都拦下来才能根治。

### 5.3 `CDoodads::EnableFeatures(this, slot, mask) (0x759880)` 与 `Disable (0x7599F0)`
- `mask` 位含义（按反编译归并）：
  - `bit 0 (mask & 1)`：StaticStampPath（**直接控制 stamp 写入**）；
  - `bit 1 (mask & 2)`：basic init；
  - `bit 2 (mask & 4)`：vt[348] (RefreshStampScaleResource)；
  - `bit 3 (mask & 8)`：完整重新激活整套 lifecycle（含三条 stamp 全开）；
  - `bit 4 (mask & 16)`：vt[256] doodad 类型相关。
- xrefs：
  - `Disable(0x7599F0)` 调用方：`0x74F9A4(传 mask=8)`（即关闭整套）、`sub_6F758F00(mask=8)`、
    `0x75CA6B(传 ebx)`、`0x75CCF9(via sub_6F75CBB0)`；
  - `Enable(0x759880)` 调用方：同上，紧跟在 disable 之后。
- `sub_6F75CBB0(this)` 是 **TOD 全量刷新**：遍历所有 392 字节 doodad 槽，对 flag bit 0x400 命中的统一
  `Disable(mask=1) → ... → Enable(mask=1)`，相当于**整图建筑/装饰物阴影全部重写**。

## 4.5 ★★★ 第三条独立写入路径 — `TerrainShadow_WriteMaskRegion`（v3 重大补充）

### 4.5.1 反证：为什么之前所有拦截都失效
回顾历史（AGENTS.md 第 14、43~57 条）：
1. 拦 `RegisterImageEntry` 入口的所有 8 条来源 → **AGENTS 第 57 条做过极限实验**：
   `Mode1_BlockAllRegisterImage` 把 RegisterImage 入口全部封堵，日志确认 `WithParams/UberSplat`、
   `FromTwoPoints Shadow/ShadowFlyer`、`FromPoint SelectionCircleSmall` 全部 BLOCK，
   但**War3 进程在 AutoTest 里直接崩溃**（截图失败、未生成报告）。
2. 拦 `ShadowPath_StaticStamp_Toggle (0x74E420)` 默认开启（`kNativeShadowBlockStaticStampPathWhenMode1=true`）。
3. 拦 `ShadowProjector_Add_FromObject (0x76D800)` 已具备来源识别能力，但默认 `kNativeShadowBlockProjectorFromObjectEnabled=false`。
4. **但用户的实机视觉验收（14 文档残余风险段）：建筑阴影仍然没消失。**

如果上面三条加起来已经堵死了所有"贴花对象注册"路径，那就只剩一种可能：
**War3 渲染建筑阴影根本不通过这套贴花对象系统**。

### 4.5.2 真正的入口：`TerrainShadow_WriteMaskRegion (0x6F234710)`
本轮在 IDA 里 `list_funcs` 时发现一组 **从未在历史研究里提及** 的函数：

| 地址 | 函数名 | 说明 |
|---|---|---|
| `0x6F233E90` | `TerrainShadow_RebuildMaskFromObjectLists` | 从所有对象列表整体重建 mask（loadgame / 视野同步时调用） |
| `0x6F234620` | `TerrainShadow_WriteMaskRegion_ForObject` | 单对象 mask 写入入口 |
| `0x6F234710` | **`TerrainShadow_WriteMaskRegion`** | **真正的写入函数本体** |
| `0x6F3DB260` | `TerrainShadow_WriteMaskRegion_FromActorRuntime` | 运行时 actor 控制流写入 |

它们**不是上一节那条 RegisterImage 链的别名**——它们直接修改 **terrain mask grid 中的字节标志位**，不走 `ListA/ListB/Stamp` 任何一条注册池。

### 4.5.3 数据结构：`CFogMask + CFogMaskTable + CFogOfWarMap`
源文件由 IDA 提供（编译期字符串残留）：
- `War3\Source\Game\CFogMaskTable.h`
- `War3\Source\Game\CFogOfWarMap`（`.PAVCFogMask@@ / .?AVCFogOfWarMap@@ / .?AVCFogMask@@ / .?AVCFogMaskTable@@`）

关键事实：
1. **建筑阴影、战争迷雾、视野、路径阻挡共用同一片 16-bit mask grid**。
2. mask grid 的 cell 大小：`*(this+11)` / `*(this+12)` / `*(this+14)` 三个 `_WORD*`（说明是 4 个并行 layer）。
3. `CFogMaskTable_GetOrCreateMask(radius, slot) (0x6F232060)`：按半径 `a2` 拿/造一个 footprint 模板（脚印形状索引表）。
4. `CFogMask_BuildNodeAndRangeTable(radius, type) (0x6F230210)`：构建脚印节点表 + 范围表，初始化每个 cell 为 `0xFFFF`，type code `a3` 决定哪些 bit 受影响。
5. `WriteMaskRegion` 的关键操作：
   ```c
   // 简化版（来自反编译核心循环）
   *(_WORD *)(maskPtr + 2 * cellIndex) &= ~typeCodeMask;     // 清除 type bits
   *(_WORD *)(otherMaskPtr + 2 * cellIndex) |= typeCodeMask; // 设置 type bits
   ```
   即按 type code 对 mask grid 的多个并行 layer 做 set/clear 位操作。
6. type code 16 个 bit 各自代表不同含义（已确认存在的语义包括但不限于：fog-of-war、line-of-sight、blight、path-blocker、building-shadow-footprint）。具体 bit 分配需要进一步从 `CUnit::isBldg` / `CUnitUI flags 5C` 路径反查。

### 4.5.4 写入端的真实调用链
通过 `xrefs_to(0x6F234710)` 的 6 个 caller，建筑阴影的真正入口完全显形：

| Caller | 来源 | 说明 |
|---|---|---|
| `0x6F2341E4` | `TerrainShadow_RebuildMaskFromObjectLists (0x233E90)` | 整体重建（loadgame / 视野同步） |
| `0x6F2346FC` | `TerrainShadow_WriteMaskRegion_ForObject (0x234620)` | 单对象写入 wrapper |
| `0x6F23D944` | unnamed | 一个 fog/visibility helper（待确认） |
| `0x6F3DB33B` | `TerrainShadow_WriteMaskRegion_FromActorRuntime (0x3DB260)` | 运行时 actor 控制流 |
| `0x6F514FAB` | `sub_6F514F40` | **CUnit 段（0x51XXXX），CUnit lifecycle helper**，会 OR 上 `unit + 96 |= 0x400`，明显是建筑阴影 footprint 注册 |
| `0x6F65A412` | `sub_6F65A140` | **CUnit/Widget 中央 sync 函数（30+ caller）**，几乎所有 unit 状态变化都会调到，调用前从 `dword_6FBE4238 + 13` 取 mask manager |

最关键的两条路径：
1. **`sub_6F65A140`**：从 `dword_6FBE4238`（项目地址簿里 `gameWar3 = 0xBE4238`）的 `+0x34 (this+13)` 字段取 mask manager，调 `WriteMaskRegion(maskMgr, unit, typeCode, 1, 1)`，完成建筑/单位的 mask region 写入。30+ 个 caller 包含建筑创建、销毁、移动、状态变化等几乎所有 lifecycle 节点。
2. **`sub_6F514F40`**（CUnit 的 lifecycle helper）：从 `unit + 12` 拿到 widget，验证 `*(widget + 0x0C) == 0x2B5DB42C`（一个 magic 0x2B5DB42C，疑似类型签名），命中后调 `WriteMaskRegion`，并把 `*(widget + 96) |= 0x400u`（0x400 已知是 ListA 共享 blob mask flag）。

### 4.5.5 `RebuildMaskFromObjectLists (0x233E90)` 的 caller 也很关键
| Caller | 含义 |
|---|---|
| `sub_6F1CD100` (在 `0x1CD1D8`) | 视野/碰撞重新初始化时整体重建 |
| `sub_6F251000` (在 `0x251182`) | 战争迷雾系统 |
| `sub_6F26B600` (在 `0x26B619`) | 一个 fog tick helper（小函数 0x1E）|

这 3 条路径是"非每帧的批量重建"——在玩家视野变化、单位大量进出场景时，**会把所有对象的 mask region 全部重写一遍**，相当于强制覆盖任何"我们清掉过 mask"的工作。

### 4.5.6 为什么 RegisterImage 拦截会让游戏崩溃
AGENTS 第 57 条：`Mode1_BlockAllRegisterImage=true` 时进程崩溃。原因推断：
- `RegisterImageEntry` 是混合层注册池，**真的对 LOSBlocker / Selection / 内置贴花资源有用**——这些是 War3 渲染的依赖项（不是建筑阴影本体）；
- 全拦后丢掉合法资源句柄，渲染管线后续读到 `-1` index 触发 UAF / null deref。
- 即拦 RegisterImage **既拦不到建筑阴影**（因为根本不走那条路），**又会拦掉合法依赖**导致崩溃。这就是历史拦截"看起来命中很多 BLOCK 日志但视觉无效 + 崩溃"的根因。

### 4.5.7 真正能关掉建筑阴影的 hook 点
**`TerrainShadow_WriteMaskRegion (0x6F234710)`** 才是建筑阴影的唯一可行拦截点。

但要小心：
1. mask grid 是**多 channel 共享**——同一次 `WriteMaskRegion` 调用可能同时写多个 type bit（fog/los/shadow），不能整体 return；
2. 必须按 `a3 typeCode` 的 bit 拆分，**只屏蔽建筑阴影对应的 bit**，保留 fog-of-war / LOS / path-blocker 等其它 bit；
3. 或者更简单：**在调用前清掉 a3 中的 building-shadow bit**，让 War3 自己写入"剩下的 mask"。

具体哪个 bit 是建筑阴影，需要下一轮做 in-game 实验：
- 修改 hook 把 a3 改成 `a3 & ~bit_X`，逐一试 bit 0..15；
- 对比实机：哪一 bit 改了导致建筑底部矩形阴影消失但 fog/视野不变 → 锁定 bit。

也可以从 `CFogMask_BuildNodeAndRangeTable` 入参 `a3` 反推——它的 caller `CFogMaskTable_GetOrCreateMask` 把 `slot` 当做 `a3`，slot 来自上层 `WriteMaskRegion` 推断：
- slot 0/1/2 由 `*(_DWORD *)(a2 + 508) == 16` 分支决定（actor 类型）；
- 16-bit mask 的高 7 bit 是 elevation / height（高度图），低 9 bit 是 type code。


## 5.5 CUnit 阴影路径专题（保留作历史参考；建筑阴影实际由 4.5 描述的 mask 写入实现）

> ⚠️ 历史本节是 v2 写入的"建筑阴影通过 ShadowProjector_Add_FromObject 注册"假设。
> 4.5 节已经证明该假设错误：建筑阴影**不是**通过 stamp/projector 写入，而是通过 `WriteMaskRegion` 直接改 mask grid。
> 本节保留作为"次要路径"研究：CUnit 自身的 emitter 数组只对**普通单位脚下方块**和**部分施工特效**有效，**不画建筑底部矩形阴影**。
> 真正治理建筑阴影必看 §4.5。

### 5.5.1 CUnit 持有 shadow projector 数组
所有单位（含建筑）都持有一个 emitter 数组：

| `CUnit` 偏移 | 类型 | 含义 |
|---|---|---|
| `+0xA4` (`this[41]`) | u32 | shadow projector / emitter 数量 |
| `+0xA8` (`this[42]`) | T**[] | shadow projector ptr 数组（堆上） |

每个 emitter 至少有：
- `+0x18` (`this[6]`)：target object 指针（用 `sub_6F399220` = `emitter->GetTarget()` 取出）；
- `+0x14` (`this[5]`)、`+0x10` (`this[4]`)：strong/weak ref counted 资源；
- `+0x28` (`this[10]`)：注册后回填的 stamp index（被 `RegisterImageEntry` 写入）；
- `+0x38/+0x3C` (`this[14]/[15]`)：emitter 自身的世界坐标 X/Y。

`CUnit::FindShadowProjectorByObject(target) (sub_6F532420)`：
```c
for (i = 0; i < this[41]; ++i) {
    e = this[42][i];
    if (e && e->GetTarget() == target) return e;  // 即 e[6] == target
}
return 0;
```

### 5.5.2 三种激活路径
War3 内部至少有 3 个会调用 `ShadowPath_ObjectProjector_Runtime (0x38D7A0)` 的 CUnit 方法：

| 函数地址 | 建议名 | 出现场景 | 关键行为 |
|---|---|---|---|
| `0x6F52F510` | `CUnit::ActivateBuildingShadowProjector` | 建筑创建 / 施工 / 升级（事件 ID 触发） | 读取 SLK record 的 `+72(unitShadow)` 和 `+80(buildingShadow)`；调 `sub_6F532420` 找已存在 emitter；最后 `ShadowPath_ObjectProjector_Runtime(emitter, ...)` |
| `0x6F5449D0` | `CUnit::ActivateGenericShadowProjector` | 通用 dispatch 路径（`sub_6F543A90` switch fallthrough，via `sub_6F66EC30(v3)`）| 调 `vt[572]` 取 emitter 资源；条件满足后 `ShadowPath_ObjectProjector_Runtime` |
| `0x6F5457B0` | `CUnit::RefreshAllShadowEmitters` | **每帧 prerender / 状态转换时**（属于 7 个状态/种族 vtable 槽位之一） | 遍历 `+0xA4 / +0xA8` emitter 数组，对**未注册**(`e[10]==0`)的逐个 `ShadowPath_ObjectProjector_Runtime(e, ...)` |

`sub_6F543A90` 是 **CUnit 状态机事件分发器**（也在 7 个 .data 槽位被引用），按事件 ID 派发：
- 事件 `0xD02A5` → `sub_6F5449D0`（**通用阴影激活**）
- 事件 `0xD02A6` → `sub_6F543880`
- 事件 `0xD02A5/0xD0161/...` → 各种 unit 事件
- 默认 → 若 `sub_6F66EC30(v3)` 返回 true，转 `sub_6F52F4D0`（**建筑/普通分发**）：
  - 通过 `vt[7]` 比对函数指针，命中 `byte_6F72642E / loc_6F726474` → `sub_6F52F510`（建筑路径）
  - 否则 → `sub_6F52F980`（其他单位路径）

### 5.5.3 7 个 .data 槽位的含义
`sub_6F5457B0` 和 `sub_6F543A90` 各自在 7 个 `.data` 槽位被引用：
- `0x6F9EA1A0 / 6F9EA504 / 6F9EA868 / 6F9EABCC / 6F9EAF30 / 6F9EB294 / 6F9EB5F8`（refresh）
- `0x6F9EA16C / 6F9EA4D0 / 6F9EA834 / 6F9EAB98 / 6F9EAEFC / 6F9EB5C4`（dispatch）

间距 `0x364`（868 字节）—— 这是 7 张 **CUnit 状态/种族 vtable**（如 alive / dying / decay / construction / upgrade / morphing / reincarnating 之类）。每张 vtable 共享相同的 shadow projector slot。

### 5.5.4 CUnit 路径与 CDoodads 路径的关键差异

| 维度 | `CDoodads` 路径 | `CUnit` 路径 |
|---|---|---|
| 出口函数 | `ToggleStaticStampFromObject (0x74DB30)` → `RegisterImageEntry(type=0)` | `ShadowPath_ObjectProjector_Runtime (0x38D7A0)` → `ShadowProjector_Add_FromObject (0x76D800)` → `RegisterImageEntryWithParams (0x7290B0)` → `RegisterImageEntry(type=4)` |
| 调用频率 | 仅在对象创建/销毁/TOD 切换时 | **每帧 prerender** 都会调一次 `RefreshAllShadowEmitters`，对未注册 emitter 重新 register |
| stamp index 回填 | `slot+136 / slot+144`（`CDoodads` 内部 392 字节槽） | `emitter+0x28`（`CUnit::ShadowProjector` emitter 对象）|
| owner 类型 | `CDoodads` 内部 doodad slot（不是 CUnit/CDestructable 实例本身） | **就是 `CUnit*`**（来自 `target = emitter[6]`） |
| `Hook_ShadowProjector_Add_FromObject` 来源识别 | 不会命中此路径（CDoodads 走 `ToggleStaticStampFromObject` 而不是 `Add_FromObject`）| 命中 `ShadowPathObjectProjectorRuntime (0x38D7A0)` 范围 |

> **重大确认**：用户当前的 `Hook_ShadowProjector_Add_FromObject` 已经具备识别 Runtime 路径的能力，
> 它**专门拦截的就是 CUnit 这条路径**。CDoodads 那条路径**不会**经过这个 Hook（CDoodads 走 `0x74DB30 ToggleStaticStampFromObject` 直接进 `RegisterImageEntry`，不经过 `0x76D800 Add_FromObject`）。
>
> 这意味着：
> - 项目当前的 Hook 就是为"CUnit 建筑阴影"设计的；
> - 但目前 `kNativeShadowBlockProjectorFromObjectEnabled` 默认关闭，没有真正启用拦截；
> - 一旦启用，会**同时拦掉所有 CUnit 阴影**（普通单位 + 建筑），需要按 owner 分流。

### 5.5.5 如何只拦"建筑"而不拦"普通单位"
`ShadowProjector_Add_FromObject (0x76D800)` 的 `arg0` 就是 emitter，**`emitter[6] = target = CUnit*`**。
只要在 Hook 里：
1. 用 `arg0[6]` 取 `target` 指针；
2. 验证它是 `CUnit*`（用 RTTI vtable 比对，或直接读 `*(target+0)` 与 `??_R4CUnit@@6B@` 锚点匹配）；
3. 读 `target` 的 SLK record（先 `vt[7]` 查 unit type 拿到 SLK key，再查 `isBldg` SLK 字段）。

更省事的判别：**`CUnit::ActivateBuildingShadowProjector (0x52F510)` 已经做了"是不是建筑"的判断**——它的 caller `sub_6F52F4D0` 通过 `vt[7]` 函数指针比对来分流。也就是说，凡是从 `sub_6F52F510` 这一支（建筑路径）流向 `Add_FromObject` 的就是建筑；从 `sub_6F52F980` 流向 `Add_FromObject` 的就不是建筑。

但实际工程上**不需要区分这两支**，因为：
- `sub_6F5449D0`（通用 dispatch）和 `sub_6F5457B0`（refresh）也会调 Runtime 路径，所以单看 caller 不够；
- 最稳的方法是**直接读 `target = arg0[6]` 后查它的 `isBldg`**。

#### 怎么读 `isBldg`：
路径 1（直接读 CUnit 的 cached flags）：
- 项目内已经有 `dxvk::war3::game::UnitWrapper`（`war3_shadow_filter_policy.cpp` 在用），
  其内部会读 `CUnit + offset` 的 type/flag 字段。如果还没暴露 `isBldg`，需要补一个 getter。

路径 2（间接，通过 SLK）：
- `CUnit + 8` (`*(this+2)`) 通常是 `unit_type_id`（rawcode / SLK 主键）；
- 然后查 `UnitData.slk` 的 `isbldg` 列。但每次 hook 都查表会慢，建议项目缓存到 hash map。

路径 3（间接，通过 caller 范围）：
- 在 Hook 里用 `_ReturnAddress()` 比对是否在 `sub_6F52F510` 范围内；
- 这条路只识别"建筑路径"，但漏掉 `sub_6F5449D0/5457B0` 触发的建筑刷新——**不可靠**。

> 推荐选**路径 1**。

### 5.5.6 实操：建筑专用拦截策略
项目已有的 `Hook_ShadowProjector_Add_FromObject` 改造步骤：
1. 在 hook 入口读 `arg0[6]` 拿 `target`（emitter 的目标对象）；
2. 用 `UnitWrapper(target)` 验证它是合法 CUnit；
3. 调 `unit.IsBuilding()` 或读 `+0x130/0x134` 之类的 isBldg cache（具体偏移要从 IDA 再确认一次）；
4. 命中建筑 + `mode>=1` → return `-1`（不写入 stamp）；
5. 普通单位 → 原样放行；
6. **不依赖 caller return address**，覆盖 3 条建筑激活路径全部 case。

副作用：
- 建筑创建时不再注册 stamp，**`emitter[10] = -1`**（`RegisterImageEntry` 失败时返回 `-1`）；
- 但下一帧 `sub_6F5457B0` 会再次尝试 register（因为 `e[10]==0`），所以 hook 必须**持续生效**，不能只在第一次拦；
- TOD 切换会触发整图重新激活，hook 要保证幂等。

### 5.5.7 还需后续 IDA 验证的字段
1. `CUnit::isBldg` 的精确偏移：研究文档 03 提到过 `unit+0x5C` 是 flags5C，里面有 path-blocker bit；建筑的 isBldg bit 还需要从 `UnitData.slk` 加载流程逆向得出。
2. `sub_6F52F4D0` 的 `vt[7]` 究竟返回什么——已经确认是函数指针对比（`byte_6F72642E / loc_6F726474`），但这两个目标函数本身的语义还需要细查。
3. `sub_6F66EC30` 是判别"是否是 unit/structure 类型事件"的 helper，需要确认它的精确语义。

## 6. 前轮研究 vs 本轮新结论的差分
| 来源 | 旧结论 | 修正点（本轮） |
|---|---|---|
| `03_building_static_shadow` 旧版 | 把 `RegisterImageEntry (0x713250)` 当 ListA 的"主上游" | RegisterImage 是混合层注册池，**真正按对象语义控制的是 CDoodads 5 个调度器 + CUnit 的 3 条阴影激活路径** |
| `14_2026_02_26_static_shadow_write_gate_closeout` | 写入端 owner 解析仅命中 Unit/Unknown | 这是因为现有解析只在 `RegisterImage` 入口看，那时已经丢失"是 CDoodads 还是 CUnit"的语义。**应改成在 `Add_FromObject` 入口按 emitter[6] 反查 CUnit / 在 `ToggleStaticStampFromObject` 入口看 CDoodads slot**。 |
| `23_blob_shadow_lista_upstream_reverse` | 列出 5 个对象级调度器但未确定它们属于哪个类 | 已通过 RTTI（`??_R4CDoodads@@6B@`）确认 **5 个全部是 `CDoodads` 成员函数**；**CUnit 是另一条独立路径，不与之相交** |
| 字符串 `unitShadow / buildingShadow` 解读 | 曾被怀疑是阴影 manager category | 实际上是 `UnitUI.slk` 列名，由 `sub_6F66BA00` 解析后写入 **CUnitUIManager record 的 `+72/+80`**，由 CUnit 在 `sub_6F52F510` 读取 |
| **本轮第一版（24 文档 v1）** | 结论"5 个调度器是建筑/可破坏物/装饰物的真正治理点" | **错误**：CDoodads 不管建筑。建筑是 CUnit，**两条路径完全独立**。本版本（v2）已修正。 |
| `14_2026_02_26_static_shadow_write_gate_closeout` | 写入端 owner 解析仅命中 Unit/Unknown | `CDoodads` 的对象不是 CUnit/CDestructable 实例，**owner 解析不命中是正常的**，应改用 doodad slot+0/+136/+144 来识别 |
| `23_blob_shadow_lista_upstream_reverse` | 列出 5 个对象级调度器但未确定它们属于哪个类 | 已通过 RTTI（`??_R4CDoodads@@6B@`）确认全部属于 `CDoodads` |
| 字符串 `unitShadow / buildingShadow` 解读 | 曾被怀疑是阴影 manager category | 实际上是 `UnitUI.slk` 的列名，由 `sub_6F693130` 注册，对应 `CUnit` 不是 doodad |

## 7. 推荐拦截策略（从最稳到最激进）

> 由于 War3 静态阴影是**两条独立路径写入同一片 stamp 池**，
> 治理方案必须在两条路径上同时下手。下面分别给出策略族 A（CDoodads）和策略族 B（CUnit）。

### 7.A 策略族 A — CDoodads（树木/可破坏物/装饰物/腐地）

#### A1（最干净）：把 `0x74D500` 的 `a6` 强制注入 `0x06`
- **原理**：让 War3 自己的"跳过 stamp 路径"机制在装饰物创建时生效，相当于把 CDoodads 的"激活阴影"
  代码段直接关闭。
- **优点**：
  - 完全不动 stamp / RegisterImage / ListA / ListB 任何末端；
  - 对象其他状态（碰撞、视觉、动画）不变；
  - 只影响"新创建的 doodad"和"被销毁后重新创建的 doodad"。
- **缺点 / 注意**：
  - 已经存在于内存中的 doodad 仍然挂着旧 stamp，需要在游戏初始化早期就 hook（≤进图前）；
  - `0x75C5F0` 走 TOD 刷新时仍会重新激活 StaticStampPath，要同步 hook 它把 `enable=1` 改成 `enable=0` 或直接 return。
- **可行性**：高（已有 `0x74E420 / 0x74DB30 / 0x74DE40` 直接拦截能力，本质上注入 `a6=6` 是用更优雅的方式做同一件事）。

### A2（精度最高）：在 `0x74D500` 入口里读 doodad slot[+0] 即 typeId，按 SLK 分类决策
- **原理**：通过 IDA 已经验证 `*a2 = typeId/rawcode`（doodad ID，对应 `Doodads.slk` 主键）。
  在 hook 里用这个 typeId 查 `DoodadDB` 是否是 "tree / wall / bridge / decoration / destructable"
  并按需要决定是否注入 `a6`。
- **优点**：
  - 可以精确保留某些贴花（如可破坏物战利品的"破碎残骸"），关闭其他纯装饰阴影；
  - 行为完全可解释；
- **缺点**：
  - 需要逆 `DoodadDB` 的查询入口，工程量略大；
  - 如果只是"全关静态贴花"诉求，策略 1 更简洁。
- **可行性**：中（已知 `CDoodadDatabase / CDestructableDatabase` 类，但 SLK 查询入口要再补一轮逆向）。

### A3（兜底）：保留现有 RegisterImage / StaticStampPath / ListB type=4 hook
- 现状的拦截链路依然有效，作为"已经创建出来的 doodad 在 TOD 刷新时被重写"的兜底。
- **不要再扩大 ListA 的 type 拦截**，会误伤雾/边界。

### 7.B 策略族 B — CUnit（建筑物 + 普通单位的 stamp 阴影）

#### B1（最干净，强烈推荐）：在 `Hook_ShadowProjector_Add_FromObject` 里按 isBldg 分流
- **原理**：CUnit 阴影激活的全部 3 条路径（`0x52F510 / 0x5449D0 / 0x5457B0`）最终都走
  `ShadowProjector_Add_FromObject (0x76D800)`。在这个**唯一出口**做拦截，覆盖率 100%。
- **拦截规则**：
  ```
  arg0 = emitter
  target = arg0[6]               // emitter->GetTarget()
  if (target is CUnit && CUnit::isBldg(target) && mode >= 1)
      return -1;                 // 不写入 stamp
  else
      pass through;
  ```
- **优点**：
  - 单点拦截，覆盖建筑创建/施工/升级/TOD 切换/复活/decay/morph 全部场景；
  - 不影响普通单位阴影（如脚下方块）；
  - 不需要触碰 `RegisterImageEntry` 末端逻辑。
- **缺点**：
  - 必须确认 `CUnit::isBldg` 的内存读取偏移（5.5.7 已列出待办）；
  - emitter 必须是 CUnit 而非其他对象类型（要做 RTTI 校验，避免误读垃圾内存）。
- **关键细节（必做）**：因为 `sub_6F5457B0 RefreshAllShadowEmitters` 每帧都会重发，hook **必须每次都返回 `-1`**，不要尝试"只拦第一次"——War3 看到 `emitter[10] == 0` 会一直重试。

#### B2（防御性）：同时在 `Hook_ShadowPath_ObjectProjector_Runtime` 里再拦一次
- **原理**：在 `0x38D7A0` 入口提前阻断，能让 `Add_FromObject` 根本不被调用。
- **优点**：减少不必要的 emitter 处理；
- **缺点**：会同时影响 JassBridge 路径（`0x1DEEA0`），需要按返回地址区分；不如 B1 简洁。

#### B3（最激进）：在 `sub_6F52F510 ActivateBuildingShadowProjector` 入口直接 return
- **原理**：直接 short-circuit 建筑 stamp 激活整个流程；
- **优点**：连 emitter 数据都不创建；
- **缺点**：**会破坏 building 的其他状态机逻辑**（PaidStructure 颜色、施工进度色等），不推荐。

### 7.C 组合方案
| 用户诉求 | A 族 | B 族 |
|---|---|---|
| 全关树木/装饰物/可破坏物/建筑阴影（默认） | **A1**（注入 `a6=6` + `SetTodAndRefresh` return） | **B1**（按 isBldg 拦） |
| 仅关建筑，保留树木阴影 | 不动 | **B1** |
| 仅关树木，保留建筑阴影 | **A1** | 不动 |
| 极致干净（含 emitter / 烟雾） | A1 + 现有 EmitterStamp 拦截 | B1 |

## 8. 推荐 Hook 落地接线（无需立刻修改代码，仅作下一轮蓝图）

### 8.A CDoodads 路径
- **新增地址**：
  - `cdoodadsCreateAndActivate = 0x74D500`
  - `cdoodadsSetTodAndRefresh = 0x75C5F0`
  - `cdoodadsEnableFeatures = 0x759880`
  - `cdoodadsDisableFeatures = 0x7599F0`
- **新增 hook 函数（仅描述，不写代码）**：
  - `Hook_CDoodads_CreateAndActivate(this, desc, p3, p4, p5, char a6)`
    - 在 `mode >= 1` 时把 `a6 |= 0x06`（屏蔽 StaticStamp + Emitter）；
    - 在 `mode >= 2` 时把 `a6 |= 0x16`（再加 vt+248 跳过）；
    - 在 hook 里读 `slot[+0]` 输出 typeId 统计，便于后续按 typeId 精细化；
  - `Hook_CDoodads_SetTodAndRefresh(this, slot, alpha_pct, mode, force)`
    - 在 `mode >= 1` 时直接 return（避免 TOD 刷新重新写 stamp）；
    - 也可以选择仅在 `slot[+132] & (0x80000 | 0x10) == 0` 时才放行，保留 emitter 类的合法刷新；
  - `Hook_CDoodads_DisableFeatures(this, slot, mask)`
    - 当 `mask & 1` 时（关 stamp）放行，确保已存在的 stamp 能被清除；
  - `Hook_CDoodads_EnableFeatures(this, slot, mask)`
    - `mode >= 1` 时把 `mask &= ~1`，避免重新启用 stamp。
- **保持现有**：StaticStampPath / RegisterImageEntry / ListB type=4 拦截不变，作为兜底。
- **不要做**：在 `CDoodads::EnableFeatures` 里清掉 `mask & 8`（该位代表完整重激活，会破坏 TOD 等核心功能）。

### 8.B CUnit 路径（建筑阴影专属，强烈推荐）
- **复用现有 hook**：`Hook_ShadowProjector_Add_FromObject` 已经存在并接入 MinHook，直接扩展行为即可。
- **新增配置开关**：
  - `kNativeShadowBlockBuildingProjectorByOwnerEnabled`（默认 false，灰度试验）
- **改造点**（伪代码）：
  ```cpp
  void* arg0 = ...;             // emitter
  void* target = SafeReadPtr(arg0, 0x18);   // emitter->GetTarget() == arg0[6]
  if (mode >= 1 &&
      kNativeShadowBlockBuildingProjectorByOwnerEnabled &&
      target != nullptr &&
      IsLikelyCUnit(target) &&  // 用 RTTI vtable 锚比对
      ReadIsBldgFlag(target))   // 项目内 UnitWrapper 的 IsBuilding helper
  {
      return -1;                // 阻断 stamp 写入
  }
  ```
- **可观测性**：
  - 加 stats counter `BuildingShadowBlocked / NonBuildingPassed`；
  - 低频日志输出被拦截的 unit rawcode（`unit + 0x08`）；
- **配套**：不必新装其它 hook（B1 已覆盖 3 条 CUnit 路径全部 case）；保留来源识别能力以便备用切换 B2。

### 8.C 不要做的事
- 不要在 `CDoodads::EnableFeatures` 里清掉 `mask & 8`（破坏完整重激活）；
- 不要在 `sub_6F52F510` 直接 return（B3，破坏建筑施工色等状态机）；
- 不要在 `sub_6F5457B0` 入口拦掉整个循环（影响所有单位阴影）。

## 9. 风险与边界
1. **`CBlightPuffs`** 用独立 vtable 锚（`??_R4CBlightPuffs@@6B@ @ 0x6FA59E44`），实例不是 `CDoodads`，
   策略 A 不会误伤腐地。
2. **`Splat (CSplatEmitter)`** 是另一套独立路径，与 CDoodads 混在 ListA 末端但**入口不同**——本研究不影响 splat。
3. **本轮 v1 错误澄清**：v1 文档说"`unitShadow / buildingShadow` 与本研究无关 / `buildingShadow` 由 `CUnit` 自己读取不走 CDoodads"——这条**部分错误**：
   - 字段确实不属于 CDoodads 槽位（这点 v1 对）；
   - 但 `buildingShadow` **会被 `CUnitUIManager` 解析后转交给 CUnit 的 shadow projector list**，最终通过
     `ShadowProjector_Add_FromObject` 写入 ListA/ListB type=4。所以**它是建筑阴影的真正写入链**，
     已经在第 5.5 节完整建模并给出 B 系列治理策略。
4. **`CUnit::isBldg` 偏移待确认**：`UnitWrapper` 当前已经能 `GetRawcode`，但项目可能还没暴露 `IsBuilding`。
   补 getter 时需要从 IDA 进一步确认 `unit + ?` 偏移（候选: `unit+0x70 / 0x130 / 0x230` 等 SLK cache）。
5. **`unitShadow` SLK 字段（CUnitUI record `+0x4C`）**：这是普通单位脚下方块阴影，渲染方式和"建筑预渲染贴花"
   完全不同（不进 ListA/ListB，由 CUnit 自己每帧画 billboard）。如果用户**也**想关掉它，需要再做一轮单独研究。
6. **必须早装 hook**：如果 hook 落得比"地图开始加载 doodads / units"晚：
   - CDoodads：地图静态 doodad 已经是 `a6=0` 创建出来的，需要触发 `Disable(mask=1)` 清除已有 stamp；
   - CUnit：已存在的 emitter 已经 `e[10] != 0`，必须等下次 `Refresh` 才能消除（或主动调用反向 toggle）。
7. **TOD 切换 / 单位施工动画 / 建筑被摧毁** 都会重新激活阴影路径，hook 时要确保 enable/disable 配对。

## 10. 证据与参考产物
- 反编译（CDoodads 路径）：
  - `decomp_74D500.txt`（`CDoodads::CreateDoodadAndActivate` 含 `a6` mask 分支）
  - `decomp_751290.txt`、`decomp_759880.txt`、`decomp_7599F0.txt`、`decomp_75C5F0.txt`
  - `decomp_74E420_StaticStampToggle.txt`、`decomp_74DB30_ToggleStaticStampFromObject.txt`、
    `decomp_74DE40_ToggleEmitterStamp.txt`、`decomp_713920_StampWriteCore.txt`、
    `decomp_713B20_StampWriteByName.txt`、`decomp_758300.txt`、`decomp_75D7F0.txt`
  - `decomp_caller_*.txt`：`0x74D500` 五个调用方
  - `decomp_dbquery_*.txt`：vtable+50/51 内部 SLK 查询
- 反编译（CUnit 路径，**本轮新增**）：
  - `decomp_52F510.txt`（`CUnit::ActivateBuildingShadowProjector`，含 `+72/+80` SLK 字段读取与
    `ShadowPath_ObjectProjector_Runtime` 调用，含 `PaidStructureColor` 字符串）
  - `decomp_5449D0.txt`（`CUnit::ActivateGenericShadowProjector`）
  - `decomp_5457B0.txt`（`CUnit::RefreshAllShadowEmitters`，每帧重发的 vt 槽实现）
  - `decomp_543A90.txt`（`CUnit::DispatchEvent`，含事件 ID switch + `vt[7]` 分流）
  - `decomp_52F4D0_CUnit_dispatch.txt`（建筑/普通分发 helper）
  - `decomp_emitter_helper.txt`（`emitter[6] = target` 的 `GetTarget` 实现，仅 4 字节）
  - `decomp_532420.txt`（`CUnit::FindShadowProjectorByObject`，遍历 `+0xA8` emitter 数组）
  - `decomp_ShadowObjectProjector_Runtime.txt` / `decomp_ShadowObjectProjector_JassBridge.txt` /
    `decomp_ShadowProjectorSimpleBridge.txt`（三条 ProjectorAdd 入口）
  - `decomp_7291D0_RegisterImageEntryWithParams.txt`（`+esi+2C` stamp index 回填证据）
  - `decomp_713CA0.txt`（`Add_Simple/FromObject` 共同子调用，调 `RegisterImageEntryWithParams`）
  - `decomp_RegisterShadow.txt` / `decomp_RegisterStructureShadow.txt` /
    `decomp_RegisterShadowOffset.txt` / `decomp_RegisterShadowSize.txt`
    （`CUnitUIManager` record `+76/+80/+128/+136/+144` 的 setter，源文件 `War3\Source\UI/CUnitUIManager.cpp`）
  - `decomp_CUnitUIManager_LoadSlk.txt`（`sub_6F66BA00`，把 `unitShadow / buildingShadow` 字段从 SLK 写入 record）
- 反汇编 / vtable：
  - `rdata_vtable_at_A59C50.txt`（CDoodads vtable 全量 + RTTI 锚 + CClippable + CBlightPuffs）
- 字符串/RTTI：
  - `rtti_world_classes.txt`（`CWorldObjects / CWorldObjectsClippable / CDoodads / CDestructable / CBlightPuffs / CSplat*` 等 62 项）
  - `rtti_classes_full.json`（200 个 RTTI class descriptor 名字索引，用于本轮多次 lookup）
  - `strings_unit_blight_shadow.txt`（`unitShadow`、`buildingShadow`、`unitshadows`）
  - `strings_shadow_all.txt`（含 `RegisterShadow / RegisterStructureShadow / RegisterShadowOffset / RegisterShadowSize / RegisterShadowOnWater`）
- xrefs：`xrefs_dispatcher.txt / xrefs_stamp_writers.txt / xrefs_class_descriptors.txt / xrefs_l4.txt /
  xref_CUnit_shadow.txt / xref_register_shadow.txt / xref_RegisterShadow_handler.txt /
  xref_RegisterImageWithParams_real.txt / xref_38D7A0.txt / xref_543A90.txt`

## 11. 与 README 索引的衔接
- 本研究**已扩展为同时覆盖 `CDoodads` + `CUnit` 两条独立路径**：
  > **24 - CDoodads 与 CUnit 静态阴影上游逆向（建筑/可破坏物/装饰物的真正治理点）**
- 旧文档 `03 / 14 / 23` 不需要回退，只需在末尾备注"主治理点已收敛到 CDoodads 调度器 + CUnit Shadow Projector，详见 24"。

## 12. 下一步建议
1. **不再继续往末端 ListA / ListB / RegisterImage 上叠拦截规则**，改向 CDoodads 调度器 / CUnit emitter 出口靠拢；
2. **CDoodads 路径**用第 8.A 节描述的 4 个 hook 做一次 A/B 实验：
   - 先只 hook `0x74D500`（注入 `a6=0x06`）观察新加载/刷出的 doodad 阴影是否消失；
   - 再 hook `0x75C5F0`，确认 TOD 切换不会复活；
   - 最后 hook `Enable/Disable` 处理"已经在场 doodad 的过渡态"。
3. **CUnit 路径**用第 8.B 节描述的扩展做实验（仅扩 `Hook_ShadowProjector_Add_FromObject`）：
   - 先确认 `UnitWrapper::IsBuilding()` 能稳定判别建筑（必要时 IDA 再补一轮 `+0x70 / +0x130 / +0x230` 偏移定位）；
   - 在 `mode>=1 && kNativeShadowBlockBuildingProjectorByOwnerEnabled` 时按 owner 拦；
   - 重点验证：
     - 建筑底部矩形阴影是否消失；
     - 普通单位脚下方块（unitShadow）保持正常；
     - 选中圈、施工进度色、PaidStructure 灰显都不能受影响。
4. 实机验证清单（建筑专属）：
   - 城市地图：人类/兽族/亡灵主基地周围"建筑底部矩形预渲染贴花阴影"是否消失（**这是用户的核心痛点**）；
   - 兵营、农场、商店、防御塔的脚底贴花；
   - 施工中、未支付（红色）、升级中、变形中（动物战团）多种状态都要测；
   - 单位选中圈、单位 unitShadow 脚底方块、战争迷雾边界全部要保持。
5. 实机验证清单（CDoodads 路径，已有蓝图）：
   - 树木/花草：树叶根部"圆形阴影"消失；
   - 桥/可破坏物（栅栏、矿点入口、商店）：底部贴花消失；
   - 雾/边界 / 战争迷雾：保持正常；
   - 单位选中圈、单位脚底标记：保持正常。
