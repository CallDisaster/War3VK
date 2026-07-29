# 构造、析构、帧更新与渲染调用图

## 1. 证据边界

本页只把真实 x86 控制流、直接/间接调用点和对象偏移写成 `Confirmed`。历史 IDA 名称中带
`Terrain`、`Shadow`、`Selection`、`Overlay` 的函数名可用于定位，但在 callee 自身尚未逐条完成
ASM/xref 审计前，不把名称中的业务词反向当作类归属证明。

## 2. 类族构造与析构

### 2.1 `CWorldFrameWar3`

```text
CGameUI ctor path @ 0x6F34A32A
  -> AllocMemory_Storm_401(0x668)
  -> CWorldFrameWar3 ctor @ 0x6F35EFB0
     -> primary frame/base construction
     -> install primary vptr 0x6F98DCD0
     -> install secondary CLayoutFrame vptr 0x6F98DDB8 at this+0xB4
     -> allocate three 0x1C-byte non-polymorphic WorldGroupRecordOwner objects
        -> store at this+0x16C/+0x170/+0x174
     -> CCinematicFilter_Ctor(this+0x254)            [embedded size 0xA4]
     -> initialize RallyIndicatorSmallVector16 / Int32SmallVector16 / Waypoint storage
     -> initialize six raw-pointer vectors and six CAgentPtr<T> vectors
     -> CFog_Create(0xD4) -> this+0x334
        -> CWorldFrameWar3_InitTerrainZFog(TerrainZFog Style/Color/Density/Start/End)
     -> CWorldFrameWar3_InitDncSpriteResources(terrainModelPath, unitModelPath)
        -> pooled CSpriteUber_ leaf -> this+0x338 / this+0x33C
     -> CLight_Create(0xDC) -> this+0x340
     -> resize TargetIndicatorVector @ +0x370 to 8 records
     -> CWorldFrameWar3_RefreshGameContextBindings
  -> store result at CGameUI+0x3BC
```

`push 0x668`、构造调用和 `CGameUI+0x3BC` 写入在同一分配路径中；两个 vptr 还由 RTTI COL 的
`offset=0` 与 `offset=0xB4` 独立闭合。内嵌 `CCinematicFilter` 另有 COL `0x6FA874AC`、
one-slot vtable `0x6F98ED34` 和 exact `0xA4` span；它是 composition，不是第三个 WorldFrame base。

主表 slot 1 `0x6F361130` 是 scalar-deleting 路径；完整析构链会对 `+0x16C/+0x170/+0x174`
三个 owner 的全部 constructed record 逐项 release `CSprite*`，再 free buffer/owner，并对
RCString/其他容器和多个尾部对象做对称释放。owner 的真实非多态 C++ 类名仍 Unknown。
base destruction `0x6F098F50` 还会先经 `[vt+0x6C]` 调 primary slot27；WorldFrame override
`0x6F367B40` 在 base `+0x30/+0x2C` 清理前释放派生 handles、arrays 和资源指针，因此这是
Confirmed 的派生 teardown 边界，不是普通帧 reset callback。其中 `+0x334/+0x340` 已精确为
`CFog*/CLight*`，`+0x338/+0x33C/+0x354` 为 pooled-Uber `CSprite*` strong refs；
`+0x370` TargetIndicator vector 会逐 record release `+0x04` strong ref 后释放 backing。

game-context refresh 的精确尾链为：

```text
CWorldFrameWar3_RefreshGameContextBindings @ 0x6F3618F0
  -> lazy CGameWar3 -> +0x178
  -> cache ordinal/object/mask/publications at +0x184/+0x188/+0x198/+0x1F4/+0x230/+0x32C
  -> initialize embedded metadata +0x1F8 and +0x214
  -> tail CWorldFrameWar3_UpdateIndicatorRuntimeObjectsForOrdinal @ 0x6F36A840
       -> RallyIndicator[0..count)
       -> singleton +0x588
       -> WaypointIndicator[0..count)
       -> each non-null runtimeObject08: sub_6F3393E0(object, EDX=ordinal)
```

ctor 在 `0x6F35F218` 调 `CCinematicFilter_Ctor @ 0x6F37F8F0`；WorldFrame dtor 在
`0x6F36060E` 调 `CCinematicFilter_Dtor @ 0x6F386650`。filter 自身唯一虚槽
`0x6F38A130` 是 scalar deleting dtor；内嵌场景使用 non-deleting dtor，不会单独 free
`this+0x254`。

### 2.1.1 environment objects、stage-0 sprite 与 indicator vectors

```text
CWorldFrameWar3_Ctor @ 0x6F35EFB0
  -> CFog_Create @ 0x6F191320
     -> HFOG allocation 0xD4
     -> CFog_Ctor @ 0x6F1904F0       [vptr 0x6F964ADC]
     -> publish strong ref at world+0x334
  -> CWorldFrameWar3_InitTerrainZFog @ 0x6F36C390
     -> read TerrainZFog Style/Color/Density/Start/End
     -> configure CFog
  -> CWorldFrameWar3_InitDncSpriteResources @ 0x6F366F10
     -> CSprite_CreatePooledMiniOrUber(ECX=1) -> pooled Uber leaf
     -> world+0x338 terrain DNC sprite
     -> world+0x33C unit DNC sprite
  -> CLight_Create @ 0x6F1913B0
     -> HLIGHT allocation 0xDC
     -> CLight_Ctor @ 0x6F190900     [vptr 0x6F964B08]
     -> publish strong ref at world+0x340
```

`0x6F35F99D..0x6F35F9AE` 先 push unit path、再 push terrain path 后调用 `retn 8` helper，
故 C++ 参数顺序是 `(terrainModelPath, unitModelPath)`。两条精确字符串分别是
`Environment\DNC\DNCLordaeron\DNCLordaeronTerrain\DNCLordaeronTerrain.mdl` 与
`Environment\DNC\DNCLordaeron\DNCLordaeronUnit\DNCLordaeronUnit.mdl`。`+0x338/+0x33C`
都由 factory 的 nonzero 分支创建，dynamic vptr 为 pooled Uber leaf `0x6F96485C`；这不是仅靠
文件名推测的类型。

另一路 `CWorldFrameWar3_SetStage0SpriteResource @ 0x6F36D620` 把 path 存到 RCString
`+0x348`，非空 path 同样以 `ECX=1` 创建 pooled Uber leaf并强持有于 `+0x354`。slot11 update
把针对该对象的 prepare/visibility helper 返回值写到 `+0x35C`；RenderScene 只有在
`+0x358 && +0x354 && +0x35C` 时发 stage 0。helper 返回值的公开枚举名仍 Unknown。

两张相邻 vector 是独立 owner：

```text
CPathingMapIndicatorRefVector @ world+0x360
  grow @ 0x6F361335..0x6F361387
  -> Reallocate @ 0x6F36A4D0  [acquire new refs, release old refs]
  -> Destroy @ 0x6F360080     [release count elements, free backing]

TargetIndicatorVector @ world+0x370
  -> Resize(8) @ 0x6F36CA00
  -> Reallocate @ 0x6F36A040
  -> Destroy @ 0x6F35FF00
  -> two producer sites advance world+0x37C as (cursor+1)&7
     and index data+cursor*0x18
```

数字 8 是 ctor 建立的 record 数量和 ring mask 合同，不把 `+0x378` 变成 inline array；
`+0x378` 是可重分配 data pointer。`TargetIndicator+0x04` 的 strong ref 具体动态类仍 Unknown。

### 2.2 `CWorldObjects` / `CDoodads` / `CBlightPuffs`

```text
CShowable_Ctor @ 0x6F74A950
  -> CWorldObjects_Ctor @ 0x6F74A970(this, CTerrain* terrain)
     -> initialize AUWOModel array header at +0x0C
     -> initialize remaining owned arrays/strings
     -> CDoodads_Ctor @ 0x6F74A860       [derived size 0x150]
     |  -> initialize +0xF4..+0x14F
     `-> CBlightPuffs_Ctor @ 0x6F74A7E0 [derived size 0xF8]
        -> read configuration section "Blight", key "PuffDuration"
```

- `CDoodads_EnsureSingleton @ 0x6F770FE0` 分配 `0x150`，构造后发布到
  `g_CDoodadsSingleton @ 0x6FBEE15C`。
- `CWorldObjects_Ctor` 的 direct-call xref 只有两个：CDoodads singleton 路径从
  `0x6F771060` 的 CTerrain lazy ctor/global 取得参数；BlightPuffs 在 CTerrain method
  `0x6F7296F0` 内传 `ESI=this`。所以 base `+0x94` 的类型是精确 `CTerrain*`。
- `CBlightPuffs` 的分配点 `0x6F729737` 压入 `0xF8` 后调用构造器。
- `CWorldObjects_Dtor @ 0x6F74BF70` 逐项析构 `0x188`-stride `AUWOModel`，再释放其余
  growable arrays、字符串和嵌套 owner。
- `CDoodads_Dtor @ 0x6F74BEB0` 先释放派生字段，再进入 base dtor；其 scalar deleting
  destructor 为 `0x6F74CCA0`。
- `CBlightPuffs` scalar deleting destructor `0x6F74CC40` 回落到 base dtor；当前没有证据表明
  `+0xF4` 是独立 heap owner。

### 2.3 `CWorldObjectsClippable`

```text
CWorldObjectsClippable_Ctor @ 0x6F74AB70(this, unknownKindOrMode)
  -> CClippable base ctor
  -> +4 = unknownKindOrMode
  -> zero array header +8..+14 and copied state +18

vslot 7 / Clone @ 0x6F752220
  -> allocate 0x1C
  -> construct with source+4 (enum-like value; only 1/2 recognized by vslot 6)
  -> allocate independent outer array, exact stride 0xB4
     -> raw-copy record +0x00..+0x20 and +0xA8..+0xB0
     -> copy-construct nested owner at +0x24 (confirmed owned substructures)
  -> copy source+0x18

CWorldObjectsClippable_Dtor @ 0x6F74C0B0
  -> destroy/free array owner at this+8
  -> restore CClippable vptr
```

这里没有任何构造链进入 `CWorldObjects`；`CWorldObjectsClippable` 是独立的
`CClippable` 分支。

`CBlightPuffs` owner 链也已闭合：`CTerrain+0x1F0` owns the puffs manager；
`0x6F7296F0` 分配/构造并发布，`0x6F721300` 以 vslot0 flags=1 销毁后清空。创建 helper
把 manager `puffDurationMs @ +0xF4` 写到 entry `+0x14C`；slot86 累加 entry elapsed-ms
`+0x148`，slot10 再执行 birth/stand/death 状态机。`CTerrain+0x1F4/+0x1F8/+0x1FC`
分别是独立 PuffInterval、owner elapsed-ms、PuffChance，不是 manager 派生字段。

## 3. `CFrame` 事件、有效态与 layout 交互

### 3.1 base observer 与 recursive frame pass

```text
CObserver_RegisterEventRecipient(this,key,eventTag,recipient)  [primary slot2]
  -> ensure ObserverRegistry at this+8
  -> insert/update ObserverEventReg(+8 recipient,+0x0C eventTag)

CFrame_RunRecursiveFramePass(this,context)                     [primary slot9]
  -> transform/save context
  -> self vslot11(float,float,context+0x18)
  -> self vslot12()                 [WorldFrame: RenderScene]
  -> self vslot13()                 [post-category cleanup]
  -> for child in this+0x1C: child vslot9(context)
```

slot2 的 incoming ECX 在 ctor caller `0x6F35F948..955` 和 callee 首个 helper call 中均被消费，
因此旧 `__stdcall` 自动型被真实 ABI 否定。slot9 caller 还独立证明 slot11 三栈参、slot12/13
零栈参。WorldFrame slot10 是 `CMouseEvent` hit-test；slot14 只构造 sprite、ghost-sprite、terrain
click 三种 event，不包含 track event。

### 3.2 event-record router

```text
event dispatcher @ 0x6F09B490
  0x400500C9 -> primary slot15 -> receiver vslot4
  0x400500CA -> primary slot16 -> generic vslot4 adapter
                               `-> unhandled: refresh GameUI children + indicator anchor
  0x400500CB -> primary slot17 -> optional global receiver -> vslot4
  0x400500CC -> primary slot18 -> receiver vslot4
  0x400500CD -> primary slot19 -> receiver vslot4
  0x40060067 -> primary slot20 -> receiver vslot4
  0x40060064 -> primary slot21 -> receiver vslot4
  0x40060065 -> primary slot22 -> receiver vslot4
  0x40060066 -> primary slot23 -> receiver vslot4
  0x40060068 -> primary slot24 -> receiver vslot4
```

slots15、18..24 的 raw body 都是 9-byte thunk：取 vptr 后 tail-jump `[eax+0x10]`。
`0x10` 是 byte offset，即 zero-based vslot4。base vslot4 `0x6F0562E0` 读取
`eventRecord+8` 的 event ID，再以 `(eventId,eventRecord)` 调 vslot5，最后 `retn 4`。
事件对象的具体自然语言名称与 optional global receiver 身份仍为 Unknown。

### 3.3 effective-active 与 `AUKeyboardFocus`

```text
CFrame effective-state recompute @ 0x6F099610
  inactive -> active   -> vslot25
                         -> if +0x7C/+0x8C registration counts require it
                            allocate AUKeyboardFocus(0x40)
                            attach +0x74/+0x80 registration sets
                            owner = this at focus+0x3C
                            this+0xB0 |= 0x10
  active -> inactive   -> vslot26
                         -> clear current global focus if this
                         -> detach/destroy AUKeyboardFocus
                         -> this+0xB0 &= ~0x10
  -> recursively recompute children
```

primary slots52/53 分别设置/清除 `+0xB0` hidden bit0，并在有效态边沿调用 slot26/25 后向
children 传播。slots31..33 则沿 `+0x1C` child 链传播 `+0x30` pointer、`+0x34` float 与
`+0x2C` pointer 三项 inherited property；属性具体类名仍 Unknown。

### 3.4 `CLayoutFrame@+0xB4`

次表 9 槽覆盖 rect-changing、resolve/commit、validate/commit、try-get rect、scale propagation、
scaled width/height 与 owner flag-bit3 query。secondary deleting dtor 的原始控制流是
`sub ecx,0xB4; jmp CWorldFrameWar3_ScalarDeletingDtor`；其余函数保持 secondary `this`，
需要 primary owner 时显式减 `0xB4`。这给出了次基调整和 layout mutation 的精确 ABI 边界，
但 rectangle 内逐分量自然语言名仍保留 Unknown。

## 4. `CWorldFrameWar3::RenderScene`

`0x6F3681C0` 是 `CWorldFrameWar3` 主 vtable slot 12，`__thiscall`，无栈参数。它先对
`+0x338/+0x33C` 执行 scene-slot state cleanup，并在 `+0x354` 非空时处理其对应 state；这三项
现在已证为 pooled `CSpriteUber_` leaf refs，不是 Unknown scene pointers。随后把
`+0x660/+0x664` 置为 `-1`，并读取
`activeQueue=this+0x31C`。随后执行以下精确序列：

| 顺序 | stage | `(renderMode, categoryMask, activeQueue)` | 条件/边界 |
|---:|---:|---|---|
| 1 | 0 | `(0, 1, 0)` | `+0x358 && +0x354 && +0x35C` |
| 2 | 1 | `(1, 2, +0x31C)` | 总是 |
| 3 | 13 | `(1, 2, +0x31C)` | 总是 |
| 4 | - | - | `RenderQueue_FlushAndReset` |
| 5 | 19 | `(1, 2, +0x31C)` | 总是 |
| 6 | 9 | `(1, 2, +0x31C)` | 总是 |
| 7 | 2 | `(1, 2, +0x31C)` | 总是 |
| 8 | 3 | `(1, 2, +0x31C)` | 总是 |
| 9 | 8 | `(1, 2, +0x31C)` | 总是 |
| 10 | 17 | `(1, 2, +0x31C)` | `+0x324 != 0` |
| 11 | 14 | `(2, 4, +0x31C)` | 总是 |
| 12 | 5 | `(2, 4, +0x31C)` | 总是 |
| 13 | 10 | `(2, 4, +0x31C)` | 总是 |
| 14 | 12 | `(2, 4, +0x31C)` | `+0x300 != -1`；调用前切换一次 shadow/group state |
| 15 | 11 | `(2, 4, +0x31C)` | 总是 |
| 16 | - | - | 第二次 `RenderQueue_FlushAndReset` |
| 17 | 4 | `(1, 2, +0x31C)` | 总是 |
| 18 | 7 | `(1, 2, +0x31C)` | 总是 |
| 19 | 6 | `(1, 2, +0x31C)` | 总是 |
| 20 | 20 | `(2, 4, +0x31C)` | 总是 |
| 21 | 15 | `(-1, -1, 0)` | 仅 `activeQueue==0` |
| 22 | 18 | `(2, 4, 0)` | 仅 `activeQueue==0` |
| 23 | 21 | `(-1, -1, 0)` | 仅 `activeQueue==0` |

最后按需关闭当前 category/mode，并再次把 `+0x664/+0x660` 置为 `-1`。这张表是调用次数
和顺序表；“22 stage”指 stage ID `0..21`，而一次 `RenderScene` 可含条件跳过，stage 12/13/11
也会在 dispatcher 内展开为多个 producer。

### 4.1 slot11 periodic 与 one-shot maintenance 状态流

```text
CWorldFrameWar3 primary slot11 @ 0x6F368480
  -> +0x35C = stage-0 sprite prepare/visibility result
  -> +0x380 += frame delta
  -> if +0x380 > 3.0f: +0x388 = 1
  -> if +0x38C:
       RenderGlobalPass_DispatchBySelector(8)
       Terrain_RenderExtraPass(...)
  -> sub_6F368E90
       if +0x388: run periodic maintenance cohort
       if +0x38C: register sub_6F36F4C0 via sub_6F02F3F0
       clear +0x38C after callback registration
  -> slot11 tail clears +0x388/+0x380/+0x384
```

阈值常量 `0x6F95FE7C` 的 raw bytes 是 `00 00 40 40`，即 exact `3.0f`。另有
`0x6F359470` 从 GameUI 取得当前 WorldFrame 后置 `+0x388=1`；`0x6F346440` 在自身
transform/delta 条件满足时经 `CGameUI+0x3BC` 置 `+0x38C=1`。`0x6F3617D0` 同时累加
`+0x384` 与 `+0x23C` 并比较同一阈值。这里能确认的是 periodic latch 与 one-shot
extra-terrain/deferred callback 的 producer/consumer/clear；`+0x384` 的业务单位和完整维护
cohort 自然名仍 Unknown。

## 5. `CWorldFrameWar3::DispatchStage`

`0x6F363020` 的真实 ABI：

```text
ECX       = CWorldFrameWar3*
[ESP+04]  = stageId
[ESP+08]  = renderMode
[ESP+0C]  = categoryMask
[ESP+10]  = activeQueue
return    = retn 0x10
```

若 `categoryMask != 0`，实际 render mode 被强制为 3；函数先按 `+0x664` 切 category，再按
`+0x660` 切 render mode。22 路 jump table 的低层目标如下：

| stage | 已确认的低层行为 | 语义置信度 |
|---:|---|---|
| 0 | `ECX=[world+0x354]` 调 `0x6F186300` | 调用 Confirmed；业务名 Inferred |
| 1 | `RenderGlobalPass_DispatchBySelector(selector=0)` | selector/control flow Confirmed；该 selector 落 terrain pass |
| 2 | 同上，selector 1 | Confirmed |
| 3 | 同上，selector 2 | Confirmed |
| 4 | 同上，selector 3 | Confirmed |
| 5 | 同上，selector 5 | Confirmed |
| 6 | 同上，selector 8 | Confirmed |
| 7 | 同上，selector 9 | Confirmed |
| 8 | 同上，selector 10 | Confirmed |
| 9 | 同上，selector 6 | Confirmed |
| 10 | 同上，selector 4 | Confirmed |
| 11 | global selector 12，随后 `WorldObjects_RenderGroup(0)` | Confirmed；两 producer 不可混称 |
| 12 | `WorldObjects_RenderGroup(1)` | Confirmed |
| 13 | `WorldObjects_RenderGroup(2)` | Confirmed |
| 14 | global selector 7（terrain path） | Confirmed |
| 15 | `0x6F367980(world)` | 调用 Confirmed；selection 语义 Inferred |
| 16 | feature bits `0x10/0x200/(0x20|0x40)/0x100` 分派四组 `CUnit` callback；bit `0x80` 再调 pathing/debug overlay | 类、ABI、分支与 owner Confirmed；四 bucket 的 Blizzard 业务名 Unknown |
| 17 | global selector 11（terrain path） | Confirmed |
| 18 | `+0x248`、UI-ready、`CBuildFrame* @ +0x250` 三门后执行 placement/construct visuals；UI-ready 时再走 `0x6F3ACFF0` | owner/type/control flow Confirmed |
| 19 | global selector 14（terrain path） | Confirmed |
| 20 | global selector 15 | Confirmed |
| 21 | static-root `CTextTagManager` -> optional `TerrainImage` index -> `CGameState+0x2C8` embedded `CTextTagManager` | 顺序、类型、对象偏移与 ABI Confirmed；static root 本身类型 Unknown |

`0x6F76F060` 的真实 ABI 是 `void __fastcall(int selector)`：ECX 是 0..16 selector，不是
`CWorld* this`。因此历史名 `CWorld_TerrainShadow_Dispatch` 已从 IDA 移除；多数 case 确实落到
terrain/shadow，但 case13 明确尾跳到文本标签 pass，不能用单域类名覆盖整个 switch。

### 5.1 stage 16：`CUnit` 四桶与 pathing/debug overlay

```text
CWorldFrameWar3_DispatchStage(case 16) @ 0x6F3631EA
  feature 0x10  -> CWorldFrameWar3_RenderStage16UnitBucket(world, 0)
  feature 0x200 -> CWorldFrameWar3_RenderStage16UnitBucket(world, 1)
  feature 0x60  -> CWorldFrameWar3_RenderStage16UnitBucket(world, 2)
  feature 0x100 -> CWorldFrameWar3_RenderStage16UnitBucket(world, 3)
  feature 0x80  -> RenderStage16PathingDebugOverlay()
```

`0x6F368A90` 设置 Gx skin mode 0 / output format 1，再选择
`0x6F36B8A0/0x6F36B920/0x6F36B4E0/0x6F36B7B0`。容器原始类型是
`CAgentPtr<CUnit>`，TD `0x6FBADC1C = .?AVCUnit@@`；callback ABI 为
`int __fastcall(CUnit* ECX, void* EDX)`，EDX 未用，四者均返回 1，并读取 `CUnit+0x16C/+0x170`。
这确认对象类和机械分桶，但四桶公开业务名仍 Unknown。

final helper `0x6F369560` 不消费 incoming ECX，只读 global root `0x6FBC5FB0`；root `+0x23C`
经 allocator string/vtable 闭合为 `NIpse::CAcceleratorMap*`，`+0x250` 为
`NIpse::CLrPathingAcc*`。它按 accelerator cells/pathing 查询发出 debug geometry；global root
动态类仍 Unknown，不能把它命名为 `CUnit` 或 `CWorldFrameWar3` 子对象。

### 5.2 stage 18：build placement / construct visuals

`world+0x250` 是精确 `CBuildFrame*`：primary vtable `0x6F9955B0`、secondary
`0x6F995698`、TD `.?AVCBuildFrame@@`、大小 `0x19C`。它由 `CBuildMode` 创建并发布，取消时在
active resources 销毁前清除，故对 WorldFrame 是 borrowed publication，不是本类 owned heap。
`CBuildFrame_RenderPlacementAndConstructVisuals @ 0x6F3C4330` 先调 owned
`CPlacementBox* @ +0x190`，再对非空 `CConstructUI* @ +0x18C` 调 vslot3。`+0x248` 是
secondary layout hit-test 建立、hover leave/build exit 清除的 gate；`+0x194/+0x198` 仍 Unknown。

### 5.3 stage 21：两个 TextTag owner 与 TerrainImage

严格顺序为：

```text
RenderGlobalPass_DispatchBySelector(13)
  -> StaticRoot_GetOrCreateTextTagManager()
  -> CTextTagManager_RenderPass(staticRoot+0x30)

if world+0x300 != -1 && world+0x2FC != 0
  -> RenderTerrainImageByIndex(ECX=index)
  -> CTerrain_RenderTerrainImageByIndex(terrain, index)
     record = [terrain+0x2CC] + index*0xA0

global CGameWar3* 0x6FBE4238
  -> CGameWar3_GetOrCreateGameState()  // +0x1C refcounted pointer
  -> CGameState_RenderEmbeddedTextTags()
  -> CTextTagManager_RenderPass(gameState+0x2C8)
```

`CTextTagManager` TD 为 `.?AVCTextTagManager@@`、vtable `0x6FA67A50`、精确大小 `0x88`；
render pass 的直接入口 xref 恰为上述两条 tail。`TerrainImage` TD `.?AUTerrainImage@@`，是
非多态 `0xA0` record；WorldFrame 保存 index/gate，未发现由其直接 release 记录的证据。
三条路径都在当前 dispatcher 调用栈同步完成，未见 worker 创建或 join。

## 6. WorldGroup 到 RenderQueue

```text
CWorldFrameWar3::DispatchStage
  stage 11 -> TerrainShadow selector 12
           -> CWorldFrameWar3_RenderWorldGroup(world, 0)
  stage 12 -> CWorldFrameWar3_RenderWorldGroup(world, 1)
  stage 13 -> CWorldFrameWar3_RenderWorldGroup(world, 2)

CWorldFrameWar3_RenderWorldGroup @ 0x6F368E30
  -> choose [world+0x16C/+0x170/+0x174]
  -> WorldGroupRecordOwner_GetRecords / GetActiveCount
  -> iterate WorldGroupRecord with stride 0x18
     -> CSprite* sprite = [record+0]
     -> CSprite_PrepareAndQueueAttachedRenderObject @ 0x6F184EE0
        -> if [sprite+0x20] == 0: return
        -> call sprite->vtable[5]
        -> tail-call RenderQueue_AddBatch(ECX=[sprite+0x20])
```

重要边界：`0x18` record 与被取出的 `CSprite*` 是两个对象；`+0x20` 属于 sprite，不属于
record。五张 raw RTTI/vtable 把运行时族闭合为 `CSprite/CSpriteMini_/CSpriteUber_` 和两个
pooled leaf。slot5 对 base/Mini 是精确 no-op；Uber 版本只消费两组 pending bit8 attached
state，真正 visibility/prepare dispatch 是 slot3。旧 `WorldObjectEntry_Render` 名及其
`PreRender` 解释均为 Contradicted，更不能映射成 `CWorldObjects::vslot5`。

owner add `0x6F0CB110` 对 sprite 加强引用，帧尾 clear `0x6F0CAB90` 逐项 release；归零会同步
进入 leaf vslot1，先非释放析构再回收到 Mini/Uber pool。clear 后 record 存储仍可复用，故
record 地址和 `CSprite*` 都不能跨该边界当稳定身份；add 先增 activeCount 再填 record，也没有
并发 publication 合同。

stage 11 先运行 selector 12，后运行 group 0；两者都可能写 RenderQueue。只有包围
`RenderWorldGroup(groupIdx=0)` 前后 queue count 的精确范围，才能证明某 record 来自 group 0。

## 7. Flush 与实际 consumer

`RenderQueue_FlushAndReset @ 0x6F139800` 无参数：

```text
RenderQueue_UpdateGxStages(force=1)
RenderQueue_FlushSortedItems()
RenderQueue_FlushTransparent()
RenderQueue_UpdateGxStages(force=1)
g_RenderQueue_NumOfElements = 0
g_AUCTransparent_Count = 0
```

排序队列再经 Common/Special dispatch、draw-state、dynamic vertex/index upload 和实际 DIP。
由于 flush 在 `DispatchStage` 返回之后才发生，dispatcher 栈上的 stage 参数已经不在 consumer
ABI 中。对 GPU skin 或任何按 stage 分类的 consumer，producer-time sidecar 是必需的；
flush-time ambient stage 不是证据。

## 8. WorldObjects slots28..52 生命周期、配置与 terrain-image 子图

以下分组来自真实 virtual callers，不是按地址相邻关系猜测。slots32/35/36 的 body 虽为空，
其 lifecycle 位置仍由 caller 给出；自然语言接口名保留为 Inferred：

```text
entry database/lifecycle path
  -> vslot28(entry)
       CDoodads: query CDoodadDB/CDestroyableDB by entry+4
       -> rewrite selected entry+84 bits
       -> vslot14(entry) + refresh helper
  -> vslot29(entry) -> CDoodads tail vslot30(entry)
  -> vslot30(entry) -> remove key0 auxiliary record from entry+164
  -> vslot31(entry,context) -> optional gg_dest_* pre-removal probe
  -> vslot32(entry)                  [shared no-op cleanup-begin hook]
  -> vslot33(entry) == true          [shared destroy predicate]
  -> vslot34(entry)                  [CDoodads activation callback/state refresh]
  -> vslot35() / vslot36()           [shared no-op completion hooks]
```

配置槽中，slot38 精确输出 `"stand"`；slots40/41/47/48 分别闭合 display name、pathing texture、
variation model 与 static-shadow resource 的来源。slots43..46 虽有确定常量返回和一个 caller
partition，业务接口仍为 Unknown。

```text
CWorldObjects_RegisterSelectionCircleImageForEntry @ 0x6F74DA00
  -> TerrainShadow_RegisterImageEntry(type=1, ...)
  -> entry+0x8C = returned handle
  -> this->vslot52(entry)            [initial visibility; family default false]
  -> bind exact config strings "SelectionCircle" / "ColorFriend"

TerrainShadow_ToggleStaticStampFromObject @ 0x6F74DB30
  -> this->vslot48(...)              [static-shadow resource]
  -> this->vslot49(entry)            [route predicate]
  -> this->vslot50(...)              [optional rectangle outputs]
  -> this->vslot51(entry)            [initial visibility]
  -> entry+0x88 = returned handle

CDoodads entry handle settlement
  build @ 0x6F7586AA..725
    -> entry[+0x88,+0x8C,+0x90,+0x94] = {-1,-1,-1,-1}
  movement/refresh
    -> 0x6F757D40 updates +0x88 unless entry flag 0x80000
    -> 0x6F757D10 updates +0x8C
    -> 0x6F757D80 updates +0x90
    -> 0x6F74D730 toggles +0x94 through its distinct CTerrain API
  explicit disable/destroy
    -> unregister valid +0x88/+0x90/+0x94 indices through CTerrain owner this+0x94
    -> write the corresponding entry handle back to -1
  full entry cleanup / CDoodads dtor
    -> 0x6F750FC0 -> per-entry 0x6F751770 -> unregister +0x8C only
    -> then AUWOModel_Dtor and free the array
    -> no direct +0x88/+0x90/+0x94 unregister in the reviewed dtor chain
```

`0x6F74DA00` 在旧数据库中未被识别为函数；原始 prologue/CFG 证明其最后有效指令为
`0x6F74DB21 retn 4`，IDA 现按尾部对齐记录为 `[0x6F74DA00,0x6F74DB24)`，其后为 `CC` padding。
但 IDA code/data xref 为 0；对原始 `.text` 的 direct-call 扫描以及全 PE 的 RVA/VA pointer 扫描
也均为 0。因此这里只确认 body 语义，不能宣称 selection-circle producer 在当前 binary 可达。

## 9. WorldObjects 高槽已证子图

下图只使用 slots78..102 已读到的真实 ASM；`UnknownHelper` 表示 helper 地址已知但业务接口名
仍缺 caller/type 证据：

```text
CWorldObjects::vslot81(model, out48)
  -> vslot74 maxPitch, vslot75 maxRoll, vslot76 radius/visibilityRadius
  -> CTerrain* @ this+0x94 -> sample/query paths
  -> build/copy 3x4 transform (48 bytes)
  -> set/clear model+0x84 bit 0x10000 from normalized scale

CDoodads::vslot87(..., point3, ...)
  -> gate point3 against CTerrain+0xC4..+0xD0
  -> choose CDoodadDB* @ +0xF4 or CDestructableDB* @ +0xF8
  -> database fallback/query paths
  -> explicit code reuse of CWorldObjects::vslot87 @ 0x6F74F540

CDoodads::vslot89(model, replacement)
  -> pre-helper(this, model, 0)
  -> set model+0x84 bit 0x4000
  -> copy model state; replace first dword
  -> owner helper(this, temporaryState, 0)
  -> clear bit; post-helper(this, model, 1)

CDoodads::vslot98()
  -> for each this+0x14[i], stride 0x188, count this+0x10
     -> UnknownHelper_6F75D8F0(this, entry, 0)
```

这些子图闭合的是参数、字段和调用顺序；slot74/75 的 DB 字段已精确为 maxPitch/maxRoll，
slot76/77 均读取 radius/visibilityRadius 但消费者不同，不能合并。公开接口原名、角度单位和
部分 query context 仍 Unknown。

## 10. 尚待逐类闭合的调用域

- `CWorldObjects/CDoodads` slot57、64..66 的高层接口名，slot61..64 的尾参数名，以及
  slots28..52/78..102 中缺 caller/type 证据的返回码、flag、DB helper 自然语言名。
- stage 16 四个 `CUnit` bucket 的公开业务 taxonomy，以及 pathing/debug global root 的动态类。
- stage 18 `CBuildFrame+0x194/+0x198` 和 stage 21 static root 的精确类与 owner 协议。
- UI/portrait/minimap、particle/ribbon/effect 的独立 RTTI 类与 CWorldFrame 的组合边。
- terrain/shadow selector 0..15 内的完整对象字段与 RenderQueue producer 划分。
