# 30 - War3 1.27a `CWorld` 相关类族全量逆向

> 数据库：`E:\Work\War3\Game.dll.i64`
>
> 模块：`Game.dll`，image base `0x6F000000`，x86
>
> 状态：持续取证中；本文只把逐条证据闭合的内容标为 Confirmed，绝不因旧文档自称“完整”而升级置信度。

## 1. 本卷解决的命名问题

历史资料把下列不同对象都写成过 `CWorld` 或 `WorldObject`：

1. `CWorldFrameWar3`：一个由 `CGameUI` 拥有的 UI/world frame；对象大小 `0x668`，同时含主表和位于 `+0xB4` 的 `CLayoutFrame` 次表。
2. `CWorldObjects`：一个独立的 world-object 容器/管理基类；对象大小 `0xF4`，只有 `CShowable` 单继承。
3. `CWorldObjectsClippable`：一个独立的 `CClippable` 派生类；对象大小 `0x1C`，持有 `0xB4` 字节裁剪记录数组。它不是 `CWorldObjects` 或 `CDoodads` 的基类。
4. `CDoodads`：`CWorldObjects` 的派生类；对象大小 `0x150`，有独立全局单例。
5. `CBlightPuffs`：`CWorldObjects` 的另一个派生类；对象大小 `0xF8`。
6. `CSprite_PrepareAndQueueAttachedRenderObject @ 0x6F184EE0`：精确接收 `CSprite*`；三个
   world-group 的 `0x18` record 只在 `+0` 强持有该指针。旧名 `WorldObjectEntry_Render`
   以及“它接收 record/AUWOModel”的解释已被真实 ASM、RTTI 与引用计数链推翻。

因此本卷只在地址已经证明属于 `CWorldFrameWar3` 时使用该类名；旧函数名中的 `CWorld_*` 暂作为历史标签，直到 RTTI、构造链或明确的 `this` 来源把类归属闭合。

```text
CWorldFrameWar3 (0x668)
├─ CFrame @ +0x00
│  ├─ CLayer @ +0x00
│  │  └─ CObserver @ +0x00
│  │     └─ TRefCnt @ +0x00
├─ CLayoutFrame @ +0xB4
├─ contains CCinematicFilter @ +0x254 (0xA4)
├─ owns CFog @ +0x334, CLight @ +0x340
├─ owns pooled CSpriteUber_ leaf refs @ +0x338/+0x33C/+0x354
├─ owns CPathingMapIndicator strong-ref vector @ +0x360
└─ owns TargetIndicator vector/ring @ +0x370/+0x37C

CHandleObject
└─ CDataMgr
   └─ CEnvEffect (base extent 0x20)
      ├─ CFog (0xD4)
      └─ CLight (0xDC)
         └─ COmniLight (0x104)

CShowable (0x0C)
└─ CWorldObjects (0xF4)
   ├─ CDoodads (0x150)
   └─ CBlightPuffs (0xF8)

CClippable
└─ CWorldObjectsClippable (0x1C)   [独立分支]

CSprite (base extent 0x64)
├─ CSpriteMini_ (0x9C)
│  └─ pooled TAllocatedHandleObjectLeaf<CSpriteMini_> (0x9C)
└─ CSpriteUber_ (0x1D4)
   └─ pooled TAllocatedHandleObjectLeaf<CSpriteUber_> (0x1D4)
```

## 2. 当前 authoritative inventory

| 类 | vftable | 槽数 | COL | 直接基类 | 主/次偏移 | 精确大小 | 状态 |
|---|---:|---:|---:|---|---|---:|---|
| `CWorldFrameWar3` | `0x6F98DCD0` | 57 | `0x6FA86BD8` | `CFrame` | `+0x00` | `0x668` | Confirmed |
| `CWorldFrameWar3::{for CLayoutFrame}` | `0x6F98DDB8` | 9 | `0x6FA86C34` | `CLayoutFrame` | `+0xB4` | 同上 | Confirmed |
| `CCinematicFilter` | `0x6F98ED34` | 1 | `0x6FA874AC` | RTTI 仅 self BCD | WorldFrame composed `+0x254` | `0xA4` | type/size/vslot/lifetime Confirmed |
| `CShowable` | `0x6FA59AAC` | 6 | `0x6FACBC04` | 无 | `+0x00` | `0x0C` | Confirmed |
| `CWorldObjects` | `0x6FA59AC8` | 103 | `0x6FACBD24` | `CShowable` | `+0x00` | `0xF4` | Confirmed |
| `CClippable` | `0x6FA59E1C` | 10 | `0x6FACBB74` | 无 | `+0x00` | `0x04` | ctor/dtor 与派生首字段边界 Confirmed；standalone alloc 未见 |
| `CWorldObjectsClippable` | `0x6FA5A2EC` | 10 | `0x6FACBD38` | `CClippable` | `+0x00` | `0x1C` | Confirmed |
| `CDoodads` | `0x6FA59C7C` | 103 | `0x6FACBC4C` | `CWorldObjects` | `+0x00` | `0x150` | Confirmed |
| `CBlightPuffs` | `0x6FA59E48` | 103 | `0x6FACBCD4` | `CWorldObjects` | `+0x00` | `0xF8` | Confirmed |
| `CSprite` | `0x6F96467C` | 29 | `0x6FA74360` | `CDataMgr` | `+0x00` | base extent `0x64` | extent Confirmed；standalone allocation Unknown |
| `CSpriteMini_` | `0x6F9646F4` | 29 | `0x6FA743B0` | `CSprite` | `+0x00` | `0x9C` | Confirmed |
| `CSpriteUber_` | `0x6F9647BC` | 29 | `0x6FA7445C` | `CSprite` | `+0x00` | `0x1D4` | Confirmed |
| pooled Mini leaf | `0x6F9648D4` | 29 | `0x6FA74404` | `CSpriteMini_` | `+0x00` | `0x9C` | Confirmed |
| pooled Uber leaf | `0x6F96485C` | 29 | `0x6FA744B0` | `CSpriteUber_` | `+0x00` | `0x1D4` | Confirmed |
| `CEnvEffect` | `0x6F964AC4` | 5 | `0x6FA74898` | `CDataMgr` | `+0x00` | base extent `0x20` | extent Confirmed；standalone allocation Unknown |
| `CFog` | `0x6F964ADC` | 5 | `0x6FA748E8` | `CEnvEffect` | `+0x00` | `0xD4` | RTTI/vtable/factory size Confirmed |
| `CLight` | `0x6F964B08` | 5 | `0x6FA7493C` | `CEnvEffect` | `+0x00` | `0xDC` | RTTI/vtable/factory size Confirmed |
| `COmniLight` | `0x6F964B20` | 5 | `0x6FA74990` | `CLight` | `+0x00` | `0x104` | RTTI/vtable/factory size Confirmed |

大小证据不是由相邻符号猜出：`CWorldFrameWar3` 来自 `0x6F34A32A push 668h` 后直接调用构造器；`CDoodads` 来自 `0x6F771015 push 150h`；`CBlightPuffs` 来自 `0x6F729737 push 0F8h`；`CWorldObjectsClippable::Clone` 在 `0x6F752230` 分配 `0x1C`。`CWorldObjects` 的两个已证派生类都从 `+0xF4` 开始写派生字段。

## 3. 文档分卷

- [RTTI 与继承](rtti_inheritance.md)：COL/CHD/BCA/BCD 原始地址、PMD、对象大小证据。
- [字段与生命周期](fields.md)：按偏移记录宽度、初始化、读写点、所有权；未知字段保持 Unknown。
- [vtable](vtables.md)：所有已发现表及逐槽地址；未闭合 ABI/语义明确写 Unknown。
- [CWorldObjects 家族逐槽 ABI](worldobjects_vtable_abi.md)：103 槽的栈清理、this 消费、已证行为与 Unknown 边界。
- [调用图](callgraphs.md)：构造/析构、clone、单例和渲染主链。
- [并发与蒙皮边界](concurrency_lifecycle.md)：producer、不可变窗口、flush/join、reset/retirement。
- [冲突审计](conflicts.md)：旧 17/19/24/overnight/AGENTS 的过度声明与被推翻结论。
- [证据台账](evidence_ledger.md)：Confirmed / Inferred / Unknown / Contradicted 逐项出处。
- [IDA 写回日志](ida_writeback.md)：每批 read-before-write、写回、读回与保存结果。

## 4. 当前已完成批次

第一批读取了真实 x86 ASM、vtable data xref、MSVC RTTI 原始 dword 和分配指令，随后才写回 IDA：

- `CShowable`、`CWorldObjects`、`CWorldObjectsClippable`、`CDoodads`、`CBlightPuffs` 的构造/析构或 scalar deleting destructor；
- `CWorldObjectsClippable::Clone @ 0x6F752220`；
- `CDoodads_EnsureSingleton @ 0x6F770FE0` 与 `g_CDoodadsSingleton @ 0x6FBEE15C`；
- 四张目标 vftable 的 RTTI/继承注释。

所有名字、类型、注释均已读回；`ida_loader.save_database(..., 0xFFFFFFFF)` 返回 true，保存目标为 `E:\Work\War3\Game.dll.i64`。详见 [IDA 写回日志](ida_writeback.md)。

随后二十一批又完成：

- 把历史 `CWorld_RenderScene/DispatchStage/WorldObjects_RenderGroup` 纠正为
  `CWorldFrameWar3_RenderScene/DispatchStage/RenderWorldGroup`，并删除无 ASM 证据的 group
  gameplay taxonomy；
- 写回 WorldFrame ctor/dtor/scalar deleting dtor 的 canonical 名和精确 `__thiscall` ABI；
- 把 `CWorldObjectsClippable+0x04` 从错误的 owner 指针纠正为 enum-like kind/mode；
- 由全部 ctor xref 把 `CWorldObjects+0x94` 和三个 ctor 参数闭合为 `CTerrain*`；
- 将 `CDoodads+0x118` 的专用 hash-table ctor/dtor 定名并写入精确 ABI，同时在真实 store
  指令上闭合两个 borrowed DB、`+0x10C` scalar config/mode 与 Blight duration；
- 给 `AUWOModel` 建立 opaque ABI 锚，写回 RenderQueue 提交、model-color 解析和 Blight
  毫秒累计函数，并在 18 条真实指令上标注 partial entry field map；
- 逐槽闭合 embedded `ModelColorHash` 四槽 vtable、node `0x1C` header 生命周期和
  `AUModelColorHash+0x18` color raw cache 字段；
- 追到 debug description raw table，把 `CDoodads+0x10C` 从未知 scalar 升级为
  `Destructibles use height map` cached toggle，并闭合 vslot37 refresh 行为；
- 逐槽闭合 WorldFrame primary slots34..56 与 `CLayoutFrame@+0xB4` 全部 9 槽，确认 hover、
  hidden/visible propagation、rectangle/layout query 以及精确 secondary deleting-dtor
  adjustor ABI；
- 将 primary slots15..24 从错误的“layout pass”纠正为十个 event-record forwarder，并由原始
  `AUKeyboardFocus` type descriptor 闭合 slots25/26 focus attach/detach，再闭合派生 teardown
  和三项 inherited property propagation；
- 完成 primary slots0..14 的真实基表 owner/ABI：slot2 从错误 `__stdcall` 纠正为
  `CObserver` 三参 registration，slot10/14 分别闭合为 mouse hit-test/world-click router，
  并由 recursive caller 精确闭合 slots11/12/13 参数边界。
- 由 ctor/setter/commit 的真实 ASM 闭合 `CLayoutFrame@+0xB4` 的 `0x68` 字节次对象：
  `+0xF8..+0x104` cached rectangle、`+0x108` authoritative valid、`+0x10C/+0x110`
  unscaled width/height、`+0x114` inherited scale 与 `+0x118` publication latch；scale setter
  只失效 valid，不清旧 rect bytes，所有消费者都必须先验 valid。
- 对三张 103 槽表的 slots78..102 读取完整函数边界、raw bytes、data/code xref 和真实 ASM，
  将 39 个唯一 target 的 conservative name、精确机械 ABI 与双份注释写回 IDA；slot81 的
  48-byte transform、slot82 bounds、slot87 terrain-bounded query、slot89 mutation bracket 和
  slot98 `0x188` entry sweep 已闭合，返回码/DB helper 的业务名仍保持 Unknown。
- slots28..52 也已由真实 lifecycle caller、DB/string dataflow 与 terrain-image 注册链闭合逐槽
  保守行为，39 个唯一 target 已全量读回（38 个新写 conservative name/type，1 个保留既有
  精确内容）；另恢复旧 IDB 漏识别的函数 `0x6F74DA00`，确认 entry
  `+0x88/+0x8C` 分别承载 static-shadow/selection-circle image handle。slots43..46 等缺业务证据
  的接口仍保留 Unknown。
- 进一步闭合 `AUWOModel` ctor/dtor/copy 与 `+0x88..+0x94` 四连 terrain handle 的 `-1`
  初始化、移动更新和注销边界，并完成第 14 批 IDA 写回；其中 `0x6F74DA00` 的 body 语义虽然
  Confirmed，但三类静态引用扫描均为 0，producer reachability 明确保留 Unknown。
- 由真实 jump table/RTTI/alloc/caller 闭合 stage16 四个 `CUnit` bucket 与 pathing owner、
  stage18 `CBuildFrame/CPlacementBox/CConstructUI`、stage21 两个不同 owner 的
  `CTextTagManager` 和 `TerrainImage`；第15批同时把 `0x6F76F060` 从错误 TerrainShadow
  thiscall 修正为 `ECX=selector` 的 global fastcall。
- 完成 WorldObjects family slots53..77 的 45 个独立 target：逐项闭合 cleanup、caller、
  change-mask、DB 字段和 entry producer-consumer，并完成第16批 conservative name/type/双注释
  写回；slot57、64..66、公开枚举名与缺 caller 的返回码仍保留 Unknown。
- 完成 `CClippable/CWorldObjectsClippable` 两张 10 槽表的全 target 写回，并恢复旧 IDB 缺失的
  `CClippable` ctor/dtor 函数边界；Clone 收窄为 independent outer array + 已证 nested-owner
  copy。第17批还把 `CBlightPuffs+0xF4` 与 entry `+0x148/+0x14C` 闭合为毫秒域，并写回
  birth/stand/death 状态机。
- 第18批用 `0x6F184EE0` 的原始字节、两个 direct xref、五张 29 槽 raw vtable 与 RTTI
  关闭了旧 `WorldObjectEntry` 假设：consumer 是 `CSprite*`，slot5 对 base/Mini 是 no-op，
  对 Uber 只提交两组 pending attached state；真正 visibility/prepare dispatch 是 slot3。
  同批建立 `WorldGroupRecord` (`0x18`) 与无 vptr owner (`0x1C`) 的 IDA type，写回 add/clear/
  pool-return ABI，闭合三个 group 的强引用、帧清理、最终析构和 record reuse 边界。
- 第19批把 WorldFrame `+0x3B0..+0x65F` 从大块 Unknown 拆成 exact
  `RallyIndicatorSmallVector16`、`Int32SmallVector16`、0x100 项 Waypoint storage、六组 raw
  pointer vector 与六组 `CAgentPtr<T>` vector；所有 header、stride、inline span、reserve/
  growth quantum 和 backing/element ownership 均由 ctor/dtor/resize/descriptor ASM 闭合。
- 第20批闭合 `+0x178..+0x337` 的 game-context writer、ordinal/mask、两块 `0x1C` embedded
  metadata、transient state、stage18/21 publication 与 scene gates；同时确认
  `CCinematicFilter @ +0x254..+0x2F7` 的 RTTI、`0xA4` footprint、ctor/dtor 和 owned-buffer
  teardown。`+0x24C` 仍保留 Unknown，因为旧 slot16 reader 实际访问的是 `CGameUI+0x24C`。
- 第21批按 `vt[-1]`、raw dword 与下一 `CAllianceSlot` COL 闭合 `CCinematicFilter` 的 exact
  一槽 vtable；`0x6F38A130` 已写回 scalar deleting dtor 的 `__thiscall` ABI、双注释并读回。
- 第22批继续把 WorldFrame `+0x334..+0x38C` 从 stale Unknown 拆开：闭合
  `CEnvEffect/CFog/CLight/COmniLight` RTTI、5 槽表与 `0xD4/0xDC/0x104` factory size；确认
  `+0x334/+0x340` 为 exact `CFog*/CLight*`，三个 DNC/stage0 sprite ref 的动态类都是 pooled
  `CSpriteUber_` leaf；再闭合 `CPathingMapIndicator` strong-ref vector、stride-`0x18`
  `TargetIndicator` vector/ring，以及 `3.0f` periodic accumulator 与 one-shot deferred latch。
  `+0x344`、TargetIndicator 业务字段名和 `+0x384` 单位仍保留 Unknown。

第21批最终 IDB 保存大小为 `251,786,383` bytes；第22批最终 readback/save 与大小以
[IDA 写回日志](ida_writeback.md) 的同批结算记录为准，不能由文档更新提前宣称。

## 5. 仍不能声明完成的范围

- `CWorldObjects/CDoodads/CBlightPuffs` 的 103 个虚槽已有全地址、机械 ABI 和逐槽 raw
  behavior；slots0..77、78..102 均已按可得 caller/DB/string 证据收口，但 slot57、64..66、
  若干返回码/flag、第三参数和 DB helper 的公开业务名仍是 Unknown。
- `CWorldFrameWar3` primary 57 槽与 secondary 9 槽的地址、cleanup 和最保守行为均已逐槽
  闭合；`+0x178..+0x337` 与 `+0x3B0..+0x65F` 已按 reader/writer 拆分，但其中多项动态类、
  container element 业务 taxonomy、三个 global channel、部分 event 自然语言名、layout scalar
  与 inherited property 具体类型仍必须保持 Unknown。`+0x334..+0x38C` 的结构边界虽已显著
  收口，`+0x344`、TargetIndicator payload/state 的公开语义、`+0x384` 业务单位仍未完成。
- `CEnvEffect/CFog/CLight/COmniLight` 的 RTTI、大小与五槽机械行为已闭合；这不等于它们
  `+0x20` 之后的完整字段表或 native fog/light record 的公开字段名已经全部完成。
- 22 stage 的函数级调度已知，stage16/18/21 的主要 owner 也已闭合；这仍不等于 UI/particle/
  effect/terrain/shadow 的每个关联类、字段和生命周期均已完成。
- `CSprite` 五张 29 槽表的 raw 地址已确认；当前只把 slots0/1/3/5/25 的 owner/行为闭合，
  其余槽仍需逐函数补 ABI、caller 与业务语义。旧 `WorldObjectEntry_Render` 不再是 Unknown 类，
  也绝不能外推成 `CWorldObjects::vslot5`。
- Game.dll 渲染路径的线程亲和与最晚 join 仍需 caller/线程创建点的静态闭合；当前只确认 producer 与 flush 的同线程顺序合同。

## 6. 操作约束

本任务只做静态逆向和 IDA 数据库增量更新；不启动 War3、不 build/deploy、不运行 AutoTest。工作树存在大量其他改动，本文只编辑 30 号目录、必要回链和最小 AGENTS 交接段。
