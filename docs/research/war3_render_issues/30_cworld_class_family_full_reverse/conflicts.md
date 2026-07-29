# 历史资料冲突审计

## 1. 审计结论

已逐份读取指定的 17、两个 19、overnight plan、RTTI、GPU-skin contract、native header 与相关
研究分卷。旧资料真正较完整覆盖的是 `CWorldFrameWar3` 的主渲染拓扑，不是
`CWorldObjects` 类族。文档标题里的“完整链路/完整引擎”不能作为字段、vtable、ABI 或类归属
完成度的证据。

本卷采用以下迁移规则：

- 地址、原始 vtable 指针、精确分配大小和直接字段读写可作为重新核验入口。
- 业务函数名、stage 内容、group 内容、Queue/SceneNode 字段必须重新读取 ASM/xref 后导入。
- 互相冲突或仅由任务卡要求“填满”的字段进入隔离区，不择一猜测。
- 旧文档未覆盖的字段保持 `Unknown`。

## 2. `CWorld` 命名歧义

| 历史写法 | 现在的裁决 | 状态 |
|---|---|---|
| 把 `0x668` UI/world frame 泛称 `CWorld` | 精确 RTTI 类为 `CWorldFrameWar3`；`CGameUI+0x3BC` 持有它 | Contradicted old naming |
| 把 `CWorld_WorldObjects_RenderGroup` 当 `CWorldObjects` 方法 | `ECX` 是同一个 `CWorldFrameWar3*`，读取 `+16C/+170/+174` | Contradicted |
| 把 `0x6F184EE0` 写成 `WorldObjectEntry_Render` 或 `CWorldObjects` 虚函数 | 两个 caller 都从 `0x18` record `+0` 取 `CSprite*`；RTTI/vtable 闭合为 CSprite 族 | Contradicted；canonical `CSprite_PrepareAndQueueAttachedRenderObject` |
| 把 `CSprite::vslot5` 写成通用 PreRender/pose update | base/Mini target `0x6F1877C0` 是 no-op；Uber `0x6F1877D0` 只 flush 两组 pending attached state；visibility/prepare wrapper 调 slot3 | Contradicted |
| 把 `CWorldObjectsClippable` 当 `CDoodads` 的次基类 | RTTI 显示它只派生 `CClippable`，是独立分支 | Contradicted |

`CWorldObjects` 的 RTTI 是 `.?AVCWorldObjects@@`，主表 `0x6FA59AC8`；它与
`CWorldFrameWar3` 的主表 `0x6F98DCD0` 没有继承关系。

## 3. 24 号 CDoodads 文档的确定错误

| 旧声明 | raw RTTI/vtable 结果 | 裁决 |
|---|---|---|
| `0x6FA59C78` 是 CDoodads vtable | 它是 `vt[-1]` 的 COL pointer cell；表从 `0x6FA59C7C` 开始 | Contradicted |
| `0x6FA59E18` 是 CDoodads 的 CClippable base table | 它是独立 `CClippable` 的 COL cell；表从 `0x6FA59E1C` 开始 | Contradicted |
| `0x6FA59E44` 是 CBlightPuffs vtable | 它是 COL cell；表从 `0x6FA59E48` 开始 | Contradicted |
| TD `0x6FBB4468/4480/44B8` | 这些是各 TD 内 `+8` 的 name string；TD 起点分别为 `4460/4478/44B0` | Contradicted |
| slots 48..51=`754A20/75CF90/753B40/756470` | 这些实际是 slots 13/16/17/18；真 48..51 为 `754810/75E560/754870/75CF50` | Contradicted |
| “CDoodads 392 字节布局” | `0x188` 是 `CWorldObjects+0x14` 指向的 `AUWOModel` entry stride；类对象大小为 `0x150` | Contradicted |

`CWorldObjects[102] @ 0x6FA59C60` 后的 `0x6FA59C64` 已是
`TSHashTable<CDoodads::ModelColorHash,HASHKEY_STRI>` 的 COL；`CDoodads[102] @ 0x6FA59E14`
后的 `0x6FA59E18` 已是 `CClippable` COL；`CBlightPuffs[102] @ 0x6FA59FE0` 后立即是
`PuffDuration` 字符串。三表的 103 槽边界均已由相邻对象闭合。

## 4. WorldFrame 旧字段/容器冲突

| 旧声明 | 当前证据 | 裁决 |
|---|---|---|
| activeQueue/mode/category 为 `+0x320/+0x66C/+0x664/+0x668` | RenderScene/Dispatcher 直接读写 `+0x31C/+0x660/+0x664`；对象尾为 `0x668` | Contradicted |
| `+0x16C` 同时命名 visibility manager 与 group 0 | `RenderWorldGroup(0)` 直接读取它；其他业务名未证 | 仅保留 group owner 角色 |
| 三个 group owner 是 `0x18` 对象 | ctor 对每个 `push 0x1C`，构造 helper 写到 `+0x18` | Contradicted |
| `0x18` record 本身是可渲染对象/稳定 identity | record `+0` 只是 strong `CSprite*`；帧尾 release 后 record buffer 仍可复用 | Contradicted |
| owner `activeCount` 是并发发布计数 | add 先增加 count、随后才 acquire/store record | Contradicted；无 lock-free publication contract |
| group0/1/2 精确等于单位/建筑/装饰/特效 | ASM 只证明 stage 11/12/13 与 group index/list pointer | Inferred-only |
| `+0x19C` 必然是 camera | 当前只闭合为 retained/refcounted runtime object；具体动态类未由 RTTI 证明 | camera 名 Contradicted；type 保持 Unknown |
| `+0x588` 是 TargetPointConfirm | ctor/init/dataflow 证明 `+0x588` 是 RallyIndicator source；TargetPointConfirm 位于 `+0x58C` | Contradicted offset/type pairing |
| `+0x254..+0x2F7` 是 Unknown embedded owner | 独立 TD/COL、one-slot vtable、ctor/dtor 和相邻字段共同证明 `CCinematicFilter`，size `0xA4` | Contradicted；升级 exact composed type |
| `+0x334/+0x340` 只能写 Unknown runtime objects，或把 `+0x340` 泛称 `COmniLight` | factory/alloc/ctor vptr 分别闭合为 `CFog(0xD4)` 与 exact base `CLight(0xDC)`；`COmniLight` 有独立 `0x104` factory/vtable | Contradicted；不能由同族派生类反推字段动态类 |
| `+0x338/+0x33C/+0x354` 仍是无类型 scene-state pointers | 三个非空创建点都以 `ECX=1` 走 pooled-Uber factory，最终 vptr `0x6F96485C`；`+0x338/+0x33C` 还有 exact DNC model paths | Contradicted；exact strong `CSprite*` / pooled Uber leaf |
| `+0x378` 是“owner 内固定 8 个 stride-0x18 handles” | `+0x370/+0x374/+0x378` 是 `{capacity,count,data}`；`+0x378` 为 heap data pointer，ctor resize 到 8，且有 reallocate/destroy | Contradicted；8 是初始 count/ring mask 合同 |
| `+0x35C..+0x38F` 可整体保留 Unknown block | `+0x35C` stage0 prepare result；两张 vector、`+0x37C` ring cursor、`+0x380/+0x384` accumulators、`+0x388/+0x38C` latches 均有独立 reader/writer | Contradicted block boundary；成员业务名按证据分别保留 Unknown/Inferred |
| slot16 的 `+0x24C` reader 可给 WorldFrame 同偏移命名 | 该 reader 的 ECX 是 `CGameUI*`，不是 `CWorldFrameWar3*` | Contradicted cross-object offset migration；WorldFrame `+0x24C` 仍 Unknown |
| slots15..26 是 begin/end layout pass | slots15..24 由 event-ID dispatcher 映射为 event-record forwarders；slots25/26 由 raw `AUKeyboardFocus` type descriptor 闭合为有效态 attach/detach | Contradicted |
| `[vt+0x10]` 是 slot16 | `0x10` 是字节偏移，即 zero-based vslot4；target `0x6F0562E0` 是 event adapter | Contradicted |
| slot27 是普通 resource/reset callback | base destruction 在清基类字段前虚调该槽，override 逐项 release 派生资源 | 纠正为 destructor-time teardown |
| slots34/35 只是 enter/leave lifecycle candidate | WorldFrame override 的 caller/body 已闭合为 hover enter/leave | 纠正为 hover callbacks |
| primary slot0 是 WorldFrame scalar deleting destructor | 地址与 `TRefCnt` vslot0 完全共享；其行为是非空时以 flags=1 调 vslot1 | Contradicted；scalar deleting dtor 是 slot1 |
| primary slot2 自动型 `__stdcall` | ctor caller 明确 `ECX=this` 后 push 三参，callee 首先消费 incoming ECX | Contradicted；`CObserver` `__thiscall` |
| primary slot10 是 frame pre-update | caller 构造 raw `CMouseEvent`，callee 做 layout/world-pick/fallback bool test | Contradicted；mouse hit-test |
| primary slot14 是 click/track router | 全函数只构造 `CSpriteClickEvent/CGhostSpriteClickEvent/CTerrainClickEvent`，没有 TrackEvent ctor | 收窄为 world click router |
| `0x6F76F060` 是 `CWorld_TerrainShadow_Dispatch(this)`，selector13 也属于 TerrainShadow | 函数直接把 ECX 当 0..16 selector；case13 `jmp 0x6F766C70`，后者 lazy-get `CTextTagManager` 并尾调 render pass | Contradicted；改名 `RenderGlobalPass_DispatchBySelector` |

`war3_native_renderer.h` 把 `+0x08..+0x14B` 整体 padding，会吞掉 RTTI/ctor 已证的
`CLayoutFrame` 次 vptr `+0xB4`；该声明不能作为可替换的精确 C++ layout。

## 5. RenderQueue 与渲染算法冲突

| 冲突 | 旧资料表现 | 当前处理 |
|---|---|---|
| `RenderQueue_AddBatch` ABI | 一处写 `this=SceneNode*`，另一处写 queue+stack sceneNode | 只采用真实 caller/callee ASM；旧签名隔离 |
| flush 次数 | 一处称每帧一次 | RenderScene 内已证两次，另有 immediate/tail flush |
| Common/Special wrapper | header 写普通 3/2 参数 wrapper | native 为 ECX+EDX+3/2 stack 的混合 ABI；wrapper 不是 Game ABI |
| SceneNode `+0x9C` | 同时被称 list head 和 renderable count | 保持 Unknown，逐 reader/writer 重建 |
| 透明 entry 布局/排序方向/type 数 | 多份文档互相矛盾 | 不导入本类族字段表 |
| special 判定位 | `(flags&3)==3` 与 `flags&2` 两种结论 | 重新读 ASM，旧结论不择一 |
| upload 对 DIP | 有“一次 upload 对下一个 DIP”假设 | 原生允许 0/1/N fan-out，按 exact mutable tuple 关联 |

IDA 中 `0x6F7519E0/0x6F75BB20/0x6F757300` 被自动命名为 `_DllMain@12_*`，但真实函数只是
`mov eax,1; retn 0xC` 的 family virtual stubs；`0x6F756390` 的 PPL lock 名同样是误识别。
自动库名不能覆盖 ASM 行为。

## 6. 模型、蒙皮、雾与 UI 的旧冲突

- `CModel+0x60` 同时被写成 pointer 和 inline flexible array；后者还与 `+0x64` 重叠。
- 旧文档把 decimal 148 当 hex `0x148`，与已列 `+0x94` flags 自冲突。
- 两套 `CGeosetData` 和 RenderablePart layout 偏移完全不同。
- 单权重、group 级加权和“CPU kernel 尚未逆向”三种结论互相冲突；当前 GPU-skin contract 的
  真实 kernel/ABI 取代旧推测。
- FogMask `+0x38/+0x3C`、`+0x68` 分别被赋予互斥语义；idx 3 “已确认”又在同文档承认是推断。
- UI 分卷把 `0x184F00/0x0CAA90/0x0CB480` 命名成 UI 路径，而其他分卷把同地址证明为
  WorldObject list/visibility；整条旧 UI 命名必须隔离。
- 粒子、Ribbon、CEffect 多为通用描述，未提供本目标类族的 RTTI/vtable/逐槽 ASM 证据。

## 7. 两个 19 目录与 overnight 的覆盖度过度声明

两个 19 目录提供有价值的主链拓扑和 stage 线索，但其正文同时列出 Common/Special/Gx/shadow、
SceneNode、CAnimComplex、CModel、stage16、preview、portrait 等未完成项；因此应定位为“主链
透视/汇总”，不是全类字段与全虚函数逆向。

overnight master 声称十章与全部渲染域完成，而同目录 plan 明确多章仍是 stub 或未完成。旧任务卡
还要求字段/bit “不能留 ?”，这与当前基于证据保留 Unknown 的方法冲突。历史 IDA rename/comment
API 返回成功只证明写入成功，不证明语义正确。

## 8. 可保留的高价值旧入口

经本轮 raw/ASM 重核后可以保留：

- `CWorldFrameWar3` 大小 `0x668`、主表 57 槽、`CLayoutFrame` 次表 9 槽与偏移 `+0xB4`；
- slot 11 frame update、slot 12 RenderScene 的地址；slot27 地址保留但语义已纠正为
  destructor-time derived teardown，slots34/35 已纠正为 hover enter/leave；
- RenderScene 精确 stage 顺序、两次 flush；
- `+0x16C/+0x170/+0x174` group owner、`+0x31C` activeQueue、`+0x660/+0x664`
  mode/category；
- 17 号文档的 RenderQueue 函数级边界仍可作历史入口；其 `WorldObjectEntry` Unknown 声明已被
  第18批 `CSprite` RTTI/record/refcount 证据 supersede。

所有其他旧名字都只是下一轮 xref/ASM 的搜索入口，不自动继承 Confirmed 状态。
