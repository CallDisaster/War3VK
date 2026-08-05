# 证据台账

## 1. Confirmed

| ID | 结论 | 证据 |
|---|---|---|
| C-001 | `CWorldFrameWar3` 精确大小 `0x668`，由 `CGameUI+0x3BC` 持有 | `0x6F34A32A push 668h`、随后 ctor 和 store；raw ctor ASM |
| C-002 | 主表 `0x6F98DCD0` 57 槽，次表 `0x6F98DDB8` 9 槽，次基偏移 `+0xB4` | vt[-1] COL、CHD/BCA/BCD raw dwords；secondary COL offset `0xB4` |
| C-003 | 主继承链含 `CFrame/CLayer/CObserver/TRefCnt`，另有 `CLayoutFrame@+B4` | MSVC RTTI BCD/PMD raw records |
| C-004 | `CWorldObjects` 是 `CShowable` 单继承，大小 `0xF4` | RTTI；ctor/dtor；CDoodads/Blight 首派生字段均在 `+0xF4` |
| C-005 | `CDoodads : CWorldObjects`，大小 `0x150` | RTTI；`0x6F771015 push 150h`；ctor/dtor |
| C-006 | `CBlightPuffs : CWorldObjects`，大小 `0xF8` | RTTI；`0x6F729737 push 0F8h`；ctor |
| C-007 | `CWorldObjectsClippable : CClippable` 独立分支，大小 `0x1C` | RTTI；Clone `push 1Ch`；ctor/dtor |
| C-008 | `CWorldObjectsClippable` records stride `0xB4`；Clone 分配独立 outer array、copy-construct 已证 nested owners，并 raw-copy其余区 | `0x6F752220` loop/add `0xB4`；`0x6F74D6A0/0x6F74C4A0/0x6F74B520` copy/dtor chain；record offset stores |
| C-009 | `CWorldObjects` model array stride `0x188`，类大小不是 `0x188` | base dtor loop/calls、array header `+0x0C..+0x18` |
| C-010 | CWorldObjects/CDoodads/Blight 三表各精确 103 槽 | raw pointer table 与后继 COL/ASCII 边界 |
| C-011 | Blight 相对 base 只覆盖 slots `{0,8,9,10,18,47,67,86}` | raw 103-pointer exact comparison |
| C-012 | RenderScene 是 WorldFrame primary slot 12，无栈参数 | vtable pointer + `0x6F3681C0` ASM |
| C-013 | Dispatcher ABI 为 ECX+四栈参、`retn 0x10` | `0x6F363020` prologue/stack reads/epilogues |
| C-014 | 22-stage routing、两次 flush 与条件 stage 序列 | `0x6F3681C0` 与 jump table `0x6F36309E` 全 ASM |
| C-015 | group 0/1/2 分别读 `+16C/+170/+174`，list record stride `0x18` | `0x6F368E30` ASM |
| C-016 | `0x6F184EE0` 的 ECX 是 `CSprite*`；非空 `sprite+0x20` 时调用自身 vslot5，再以该字段尾跳 AddBatch | 原始 25 bytes；两个 direct xref 都先从 record `+0` load；CSprite 五张 raw vtable/RTTI |
| C-017 | stage 11 先 TerrainShadow selector12，再 group0；flush 在 dispatcher 返回后 | `0x6F3631C0..1CE`、`0x6F368310..31E` raw ASM |
| C-018 | `+0x31C` 是 RenderScene 传递的 activeQueue；`+660/+664` 是 mode/category cache | RenderScene/Dispatcher direct reads/writes |
| C-019 | CPU kernel 唯一 caller window是 Lock -> kernel -> normal/SEH join -> Unlock | RVA `0x0EEB76..0x0EEBB6` raw ASM |
| C-020 | actual DIP 允许 0/1/N fan-out，不能按“下一 DIP”关联 | index immediate flush、common tail flush、special/multipass ASM |
| C-021 | normal-return ack、resource identity generation 和 reset owner generation 是独立门 | caller-owned SEH + DXVK resource lifecycle xrefs/contract |
| C-022 | `CDoodads+0xF4/+0xF8` 分别为 borrowed `CDoodadDB*` / `CDestructableDB*` | lazy-global alloc sizes、ctors 写精确 vtable、CDoodads ctor store/dtor不释放 |
| C-023 | `CDoodads+0x118` 为 `TSHashTable<CDoodads::ModelColorHash,HASHKEY_STRI>` embedded subobject，span `0x28` | RTTI/COL/vtable、ctor `this+118`、下字段 `+140`、对称 dtor |
| C-024 | `CWorldObjects+0x94` 为 borrowed `CTerrain*` | ctor 全部两个 direct caller 闭合；CTerrain alloc/ctor/source string 与 method `this` 来源 |
| C-025 | `CBlightPuffs+0xF4` 为 `int32_t puffDurationMs` | `Blight/PuffDuration` 整型配置；创建写 entry `+0x14C`；slot86 累加 `trunc(deltaSeconds*1000)` 到 `+0x148`；slot10 以 elapsed>duration 触发 death |
| C-026 | `CDoodads+0x10C` 是 `Destructibles use height map` bool-like cached toggle，而非 owner pointer | `GetDebugToggleFlag(6)`；descriptor raw string；ctor store；slot37 compare/update/model refresh；dtor不释放 |
| C-027 | `AUWOModel`/doodad runtime entry 精确 stride 为 `0x188`，已闭合一组局部字段 | 析构循环、slots 4/8/9/10/14/15/67/86 的 raw `imul 188h` 与直接偏移访问 |
| C-028 | embedded `ModelColorHash` 四槽 ABI 与 node `0x1C` header 生命周期已闭合 | vtable raw pointers；slots 0..3 全 ASM；allocator type string `AUModelColorHash`；node dtor/unlink/free |
| C-029 | WorldFrame primary slots15..24 是十个 event-record 路由槽，不是 layout/render pass | `0x6F09B490` event-ID dispatcher；各 entry raw bytes；vslot4 adapter `0x6F0562E0` 读取 record `+8` 并最终 `retn 4` |
| C-030 | slots25/26 是 effective-active/inactive 边沿的 `AUKeyboardFocus` attach/detach | raw type descriptor `AUKeyboardFocus`；`0x40` allocation；owner `+0x3C`；`+0x74/+0x80` registrations；`+0xB0` bit `0x10` |
| C-031 | slot27 是 base destruction 中先行调用的 WorldFrame 派生资源 teardown | `0x6F098F50` 经 `[vt+0x6C]` 调 slot27 后才清 base `+0x30/+0x2C`；slot27 全 release loops/offsets |
| C-032 | slots31..33 分别传播 `+0x30` pointer、`+0x34` float、`+0x2C` pointer inherited properties | 三个 setter、child 链 `+0x1C`、virtual fan-out `[vt+0x7C/+0x80/+0x84]` 的真实 ASM |
| C-033 | WorldFrame secondary `CLayoutFrame@+0xB4` 九槽 ABI 与核心 layout 语义已逐槽闭合 | 次表 raw pointers；slot2 raw `sub ecx,0xB4; jmp deleting-dtor`；其余函数全部 CFG/尾部和 owner adjustment |
| C-034 | slots34/35 是 hover enter/leave override；slots52/53 是 hidden/visible 状态传播 | 函数体、`+0xB0` bit0 读写、slot25/26 边沿调用与 child propagation |
| C-035 | WorldFrame primary slots0..14 的逐槽 ABI、cleanup 与 base-owner 已闭合 | raw 15 dwords；`TRefCnt/CObserver/CFrame` base tables；全部 normal exits；direct/virtual callers |
| C-036 | slot2 是 ECX-consuming `CObserver` 三参 event-recipient registration | ctor `mov ecx,this` + 三次 push；callee 首次以 incoming ECX 调 registry helper；raw `ObserverRegistry/ObserverEventReg` strings；`retn 0xC` |
| C-037 | slots10/14 分别为 `CMouseEvent` hit-test 与 world-click router | caller 构造 raw `CMouseEvent`；click router 内仅三种 raw event vtable/ctor：sprite、ghost-sprite、terrain |
| C-038 | slot9 是 recursive frame pass，精确调用 slots11/12/13 ABI | `0x6F099433..454` 的 push/call 序列；slot11 `retn 0xC`，slots12/13 零 push |
| C-039 | `CLayoutFrame@+0xB4` footprint 为 `0x68`，provider grid/solver bits/cached RectF/valid/width/height/scale/latch 字段已闭合 | `CLayoutFrame_Ctor @ 0x6F0BC420`；secondary slots0..8 全 ASM；独立 setters；adjustor dtor 全字段 teardown |
| C-040 | cached RectF 必须由 primary `+0x108` valid gate 授权，valid=0 时矩形字节可保留旧值 | validate/commit、try-get、scale invalidation 与 ctor/dtor 的全部读写点 |
| C-041 | WorldObjects family slots78..102 已从纯机械 cleanup 升级为逐槽 raw behavior ledger | 三表 raw target compare；全部 normal exits；nontrivial implementations 的真实 ASM、字段和 helper calls |
| C-042 | slots78..102 的 39 个唯一 target 已有 conservative IDA 名、精确机械 ABI 与双份注释 | 写前 readback；函数 raw head/tail bytes；vtable data/code xrefs；39/39 `parse_decl/apply_tinfo` 与 name/type/comment readback；IDB save |
| C-043 | WorldObjects family slots28..52 已由 caller/string/DB dataflow 从机械表升级为逐槽保守行为；slots43..46 仍不填业务名 | 三张 vtable raw pointers；39 个唯一 target 的完整 normal exits；entry lifecycle callers；`stand`、DB/config 字符串与真实 x86 ASM |
| C-044 | entry `+0x88/+0x8C` 分别是 static-shadow/selection-circle terrain-image handle，selection 注册函数边界已在 IDA 恢复 | `0x6F74DB30` 对 `[vt+C0/C4/C8/CC]` 的调用与 `+88` store；`0x6F74DA00` 的 type1 register、`[vt+D0]`、`+8C` store、精确字符串及 `retn 4` raw bytes |
| C-045 | `AUWOModel+0x148` 是 manager-specific raw union role，不能全族定名为 elapsed time | Puffs slot86 的毫秒累加；Doodads slot59 写0、slot58 写 terrain-query 导出的 1/2 并据变化刷新纹理；三者真实 ASM |
| C-046 | `AUWOModel+0x88/+0x8C/+0x90/+0x94` 是四个连续 CTerrain-side handle/index，均以 `-1` 为无效值并有复制/更新/注销合同 | `0x6F7586AA..725` + raw `0x6F9601D0` 16-byte all-FF；`AUWOModel_CopyFrom`；四套 register/update/unregister ASM |
| C-047 | `0x6F74DA00` selection-circle register body 语义已证，但当前 binary 静态可达性未证 | 完整 body/strings/slot52/handle store Confirmed；IDA code/data xref=0；原始 `.text` direct-call 与全 PE raw RVA/VA pointer scan 均 0 |
| C-048 | stage16 是四个精确 `CAgentPtr<CUnit>` bucket 加可选 pathing/debug overlay；bucket taxonomy 仍 Unknown | dispatcher feature-bit ASM；`CUnit` TD/vtable/ctor；四 callback 的 ECX/EDX ABI 与 `+0x16C/+0x170` reads；final helper 的 `NIpse::CAcceleratorMap/CLrPathingAcc` owner chain |
| C-049 | WorldFrame `+0x250` 是 borrowed active `CBuildFrame*`，stage18 调 `CPlacementBox+0x190` 与 `CConstructUI+0x18C` | `CBuildFrame` RTTI、双 vtable、`push 0x19C`；CBuildMode publish/clear；`0x6F3C4330` raw ASM；两个成员的 RTTI/alloc/destroy paths |
| C-050 | stage21 顺序是 static-root TextTag、可选 TerrainImage、CGameState embedded TextTag | case13 jump；两个 `CTextTagManager_RenderPass` exact xrefs；`TerrainImage` TD/0xA0 stride；`CGameWar3/CGameState` RTTI 与 `+0x2C8` tail thunk |
| C-051 | `0x6F76F060` 是 `ECX=selector` 的 global fastcall switch，不是 World/TerrainShadow thiscall | function head `cmp ecx,0x10`、17-case jump table、各 case tail；case13 `jmp 0x6F766C70` |
| C-052 | `CWorldObjects+0x9C/+0xA4/+0xA8/+0xAC/+0xC4/+0xF0` 与 `CDoodads+0x110/+0x114` 已有精确同步窗口/serial/config/callback角色 | ctor/dtor、全部 writer/reader xrefs；load/save、entry sweep、preference descriptor raw strings；两个 callback 的寄存器/栈 ABI |
| C-053 | WorldObjects family slots53..77 的 45 个独立 target 已闭合 target/cleanup/caller/DB source，并完成第16批 IDA name/type/comment 写回 | 三张 vtable raw dword；全部正常出口；change-mask/bulk/terrain registration/model-create callers；`occH/numVar/selSize/maxScale/minScale/defScale/maxPitch/maxRoll/radius/visRadius` DB dataflow；45/45 readback |
| C-054 | `CClippable` ABI size `0x4`，且其与 `CWorldObjectsClippable` 两张 10 槽表已逐槽闭合/写回 | ctor/dtor raw bytes、派生首字段 `+4`；20 个 vtable target 的真实正常出口；Clone/centroid/footprint ASM；第17批 readback |
| C-055 | `CBlightPuffs+0xF4`、entry `+0x148/+0x14C/+0x150` 构成毫秒 birth/stand/death 生命周期，manager 由 `CTerrain+0x1F0` owns | create/reuse helper、slot10/86 raw ASM、config keys；CTerrain publish/vslot0 destroy/clear chain |
| C-056 | `CSprite -> {CSpriteMini_,CSpriteUber_}` 与两个 pooled leaf 是 offset-0 单继承族；29 槽表和大小 `0x9C/0x1D4` 已证 | 五组 COL/TD/CHD/BCA/BCD raw RTTI；五张 29-dword vtable；ctors；pool init element size/capacity |
| C-057 | 三个 WorldFrame group owner 各为无 vptr `0x1C` owner，保存 `0x18` record；record `+0` 是 strong `CSprite*` | owner/record ctor、add/acquire、render stride、clear/release、final dtor、WorldFrame ctor/dtor field stores |
| C-058 | release-to-zero 可立即经 pooled leaf vslot1 析构并回池；record buffer 可复用且 add 无 lock-free publish 合同 | `0x6F04F200/0x6F04F1A0` refcount ASM；leaf `0x6F1836B0/D0`；clear/add 指令顺序；第18批 readback/save |
| C-059 | WorldFrame `+0x3B0..+0x65F` 由 Rally/Int32 small vectors、Waypoint ring、六个 raw-pointer vectors 与六个 `CAgentPtr<T>` vectors 组成 | ctor/dtor/resize/reset 完整正常出口；raw allocator type descriptors；inline pointer compare；capacity/count/data/quantum stores；第19批 IDA structs/readback/save |
| C-060 | `+0x178..+0x230/+0x32C` 的权威 writer 是 `CWorldFrameWar3_RefreshGameContextBindings`，其尾调用按 `+0x198` ordinal 更新 Rally/singleton/Waypoint runtime objects | `0x6F3618F0` 51 条真实 ASM；`0x6F36A840` 42 条真实 ASM；全部 caller xref、field stores 与无锁尾调用；第20批 readback |
| C-061 | WorldFrame `+0x254..+0x2F7` 是 exact `0xA4` embedded `CCinematicFilter`；该类只有一槽 vtable，slot0 为 scalar deleting dtor | ctor/dtor call sites；COL `0x6FA874AC`、TD `0x6FB8E0FC`、self-only BCD；raw `vt[-1]/vt[0]/next COL`；`0x6F38A130` ASM；第20/21批 readback/save |
| C-062 | CPU-MT Phase B 的 source/palette freeze 与 output Lock identity 必须分段；staging copy/join 必须在 kernel detour 内、`0x6F0EEB8A` 前完成 | `0x6F13A5BD..C5`、`0x6F138F55`、`0x6F0EEB76..BB6` 真实 ASM；EH4 try `[0x6F0EEB7B,0x6F0EEB8A)`；readback/save |
| C-063 | Game.dll 创建 `MainLoop_6F05F710` 的 EvtSched `_beginthreadex` workers；普通 upload 链本身同步且无 thread hop | `0x6F05E61B/20 -> OsThread_BeginThreadEx -> 0x6F157540 _beginthreadex`；MainLoop registered callback ASM；精确 owner thread 仍 Unknown |
| C-064 | `CEnvEffect -> {CFog, CLight -> COmniLight}` 是 offset-0 RTTI 单继承族，四表各 5 槽；大小分别为 base extent `0x20`、`0xD4/0xDC/0x104` | 四组 raw COL/TD/CHD/BCA/BCD 与 PMD；vtable `0x6F964AC4/4ADC/4B08/4B20`；factory alloc 与 ctor vptr ASM |
| C-065 | WorldFrame `+0x334/+0x340` 是 owned `CFog*/CLight*`；`+0x338/+0x33C/+0x354` 是 strong `CSprite*` 且动态类为 pooled Uber leaf | ctor stores；`CFog_Create/CLight_Create`；DNC/stage0 factory `ECX=1`；vptr `0x6F96485C`；exact DNC strings；slot27 release/clear |
| C-066 | `+0x360..+0x36C` 是 strong `TRefCntPtr<CPathingMapIndicator>` vector；`+0x370..+0x378` 是 stride-`0x18` TargetIndicator vector；`+0x37C` 以 `(cursor+1)&7` 推进 | raw allocator descriptors；ctor/grow/reallocate/destroy ASM；`0x6F36353B..54B` 与 `0x6F3636CF..6DF` cursor stores |
| C-067 | TargetIndicator partial record 的 `-1/strong-ref/float/state/payload/result` 六字段边界与强引用生命周期已闭合 | `0x6F35FF00/0x6F36A040/0x6F36CA00` 构析/reallocate/resize；全部 `0x18` stride producer/reader；acquire/release calls |
| C-068 | `+0x380/+0x384` 是使用 raw `3.0f` threshold 的两个 float accumulators；`+0x388` 是 periodic due latch，`+0x38C` 是 one-shot extra-terrain/deferred-refresh latch | `0x6F368548..55F`、`0x6F3617D0`、`0x6F359470`、`0x6F346440`、`0x6F368E90` 真实 ASM；constant bytes `00 00 40 40`；producer/test/clear xrefs |

## 2. Inferred

| ID | 结论 | 依据与限制 |
|---|---|---|
| I-001 | CShowable `+4/+8` 与 show/enable 状态有关 | slots 1..5 的状态机；精确 public enum/name 未闭合 |
| I-002 | stage 15 与 selection manager 有关 | callee 历史名和行为线索；callee 类/字段未全审 |
| I-007 | WorldFrame `+0x32C` 的 borrowed publication 是 `CGameUI*` | source 精确为 `CGameWar3+0x3C0`，但当前未用该 pointee 的 RTTI/ctor 直接闭合；表内仍保持具体类型 Unknown |

## 3. Unknown

| ID | 未知项 | 需要的下一证据 |
|---|---|---|
| U-001 | WorldFrame 已拆分区间中仍未知的 `+0x588/+0x58C` 等动态类、Target/Rally/Waypoint record 的公开业务字段、各 vector cohort taxonomy、`+0x5D4` 不清零原因，以及 `+0x17C/+0x180/+0x234/+0x240/+0x24C/+0x344` 等孤立槽 | 对应 producer/caller RTTI、业务字符串、枚举或所有 reader/writer；不得把结构 layout 已证误写成业务语义全证 |
| U-002 | CWorldObjects slot57、64..66 的公开语义，slot61..64 尾参数名，以及 slots28..52/78..102 中仍无 caller/type 证据的返回码、flag、DB helper 业务名 | 现有 raw behavior/caller 已闭合；需要新的业务 caller、字符串或公开结构证据，不能仅凭相邻槽命名 |
| U-003 | 三个 WorldFrame group 内真实 gameplay 类别 | record producer/owner RTTI 与增删调用链 |
| U-004 | `CSprite` base 的 standalone allocation size、record `+4/+8/+0C/+10` 业务语义及 owner 的真实非多态 C++ 类名 | base extent/派生完整大小已证；需要 standalone allocator、字段 producer/caller 或非 RTTI 类型来源 |
| U-005 | stage16 四个 `CUnit` bucket 的公开业务 taxonomy，以及 pathing/debug global root 的动态类 | bucket producer/feature naming、global root RTTI；对象类/机械 ABI 已 Confirmed |
| U-006 | stage18 `CBuildFrame+0x194/+0x198`、stage21 static root 的精确类，以及 TerrainImage registry ownership | ctor/store/dtor/xrefs/RTTI；已知 CBuildFrame/TextTag/TerrainImage 类型不得外推剩余字段 |
| U-007 | UI/overlay/particle/effect/terrain/shadow 全部相关类族 | 分域 RTTI inventory + vtable/callers |
| U-008 | Game.dll 已证 EvtSched OS thread 创建，但具体 RenderQueue callback/upload affinity、可能的间接 handoff 与 worker join 仍未知 | worker entry 已闭合；仍需 callback registration/dispatch owner 与所有 indirect caller ASM，不能由 thread 存在推出 render ownership |
| U-009 | CWorldObjects mutations 与 CModel palette producer 的精确先后 | caller graph跨类闭合 |
| U-010 | `CClippable` standalone allocation site | ctor/dtor 与派生首字段已把 base/complete ABI size 闭合为 `0x4`；只缺独立 allocation producer，不缺对象大小 |
| U-011 | `0x6F74DA00` 是否由运行期外部地址、未建模间接机制或已死代码到达 | 当前 binary 无静态 xref/pointer；需要独立 runtime/loader owner 证据，不能仅凭函数体存在升级 |
| U-012 | `CEnvEffect/CFog/CLight/COmniLight` 在已证 base extent 之后的完整成员表、native record 公开字段名，以及 slots2/3/4 的 Blizzard 原始接口名 | 当前已证 size/RTTI/vtable/factory 与 submit/disable 机械行为；仍需逐字段全部 reader/writer、native record type 与业务 caller |

## 4. Contradicted

| ID | 被否定结论 | 反证 |
|---|---|---|
| X-001 | `CWorldFrameWar3 == CWorldObjects` | 独立 RTTI/ctor/vtable/size |
| X-002 | `CWorldObjectsClippable` 是 CDoodads 基类 | 两棵独立 RTTI hierarchy；CDoodads 后邻 COL 属 CClippable |
| X-003 | CDoodads 对象大小 `0x188` | alloc `0x150`；`0x188` 是 model entry stride |
| X-004 | WorldFrame group owner 大小 `0x18` | 三次 `push 0x1C` 且 helper 写 `+0x18` |
| X-005 | stage11 record 全是 WorldObjects group0 | selector12 在 group0之前也可 enqueue |
| X-006 | flush-time ambient stage 可授权 GPU skin | flush 在 dispatcher 返回后，consumer ABI无stage/tag |
| X-007 | 一次 upload 对应下一次 DIP | immediate/tail/special/multipass 形成0/1/N |
| X-008 | Lock/Unlock/DISCARD 本身证明 CPU 覆写或 resource retirement | normal-return exact-range ack与destructor generation另有门 |
| X-009 | IDA `_DllMain@12_*`/PPL lock 自动名是 family slot 语义 | 真实 stubs/ASM 与库签名不符 |
| X-010 | `CDoodads+0x10C` 是需由本类析构的指针字段 | ctor 来源和 slot37 都按 scalar dword 使用；CDoodads dtor 不解引用也不释放 |
| X-011 | WorldFrame slots15..26 是 begin/end layout pass | slots15..24 的 event-ID dispatcher/vslot4 raw adapters，以及 slots25/26 的 `AUKeyboardFocus` type descriptor/lifecycle |
| X-012 | dynamic tail `[vtable+0x10]` 表示 zero-based slot16 | `0x10` 是字节偏移，即 zero-based vslot4；base target `0x6F0562E0` 的 event adapter ASM |
| X-013 | WorldFrame slot47 或 slot6 存在混合 callee cleanup | slot47 全部 CFG 出口均 `retn 0xC`；slot6 唯一正常出口为 `retn 4` |
| X-014 | primary slot0 是 WorldFrame scalar deleting destructor | slot0 与 `TRefCnt` 表共享，只在非空时以 flags=1 调 vslot1；真正 WorldFrame scalar deleting dtor 是 slot1 |
| X-015 | primary slot2 是 `__stdcall` | ctor 显式设置 ECX=this，callee 消费 ECX 并最终 `retn 0xC` |
| X-016 | primary slot10 是 frame pre-update | raw `CMouseEvent` caller、layout/world pick 与 bool hit-test CFG |
| X-017 | primary slot14 同时路由 click 和 track events | 全部构造点只出现 `CSpriteClickEvent/CGhostSpriteClickEvent/CTerrainClickEvent`，无 TrackEvent ctor |
| X-018 | `AUWOModel_Dtor` 或 `CDoodads_Dtor` 本身会结算 entry 全部四个 terrain handle | 完整 dtor chain 只经 `0x6F751770 -> 0x6F75AB60` 注销 `+0x8C`；`+0x88/+0x90/+0x94` 仅见 explicit disable/destroy 结算 |
| X-019 | `0x6F76F060` 是 `CWorld_TerrainShadow_Dispatch(void* this)`，因此 case13 也是 TerrainShadow | ECX 直接与 16 比较并索引 jump table；case13 跳 static TextTag pass；修正为 `RenderGlobalPass_DispatchBySelector(int)` |
| X-020 | `0x6F7518A0/0x6F75D080` 分别是 slots68/69 virtual caller | 两处均先取 manager object field `+0x110/+0x114` 再 direct call；真正 slots68/69 caller 在 entry build `0x6F7584BB/0x6F7584A7` |
| X-021 | `CWorldObjectsClippable::Clone` 证明整个 `0xB4` record 无共享引用的完全深拷贝 | outer array 独立且 nested owner `+0x24` copy-construct；但 `+0x00..+0x20/+0xA8..+0xB0` 是 raw copy，其他 raw/borrowed pointer-like 字段可保留 alias |
| X-022 | `0x6F184EE0` 是 Unknown `WorldObjectEntry_Render`，其 ECX 是 record/AUWOModel | 两个 caller 从 `WorldGroupRecord+0` 取 `CSprite*`；五组 CSprite RTTI/vtable 与 ctor 闭合 |
| X-023 | `CSprite::vslot5` 是通用 PreRender/pose update | base/Mini target是 no-op；Uber 只 flush pending attached state；visibility wrapper `0x6F184F00` 调 slot3 |
| X-024 | record 地址或 activeCount 可作为稳定/lock-free identity | clear release 后复用 record；add 先增 count 后 acquire/store；release-to-zero 可立即析构回池 |
| X-025 | 旧 slot16 对 `+0x24C` 的读取可证明 `CWorldFrameWar3+0x24C` 语义 | 该指令的 receiver 是 `CGameUI*`，访问的是 `CGameUI+0x24C`；WorldFrame 本字段仍 Unknown |
| X-026 | WorldFrame `+0x588` 是 TargetPointConfirm | ctor/init/dataflow 证明 `+0x588` 是 RallyIndicator source runtime object；TargetPointConfirm 位于 `+0x58C` |
| X-027 | `+0x254..+0x2F7` 只能记为 Unknown embedded owner | `CCinematicFilter` TD/COL/vtable、ctor/dtor 和相邻字段共同闭合 exact type 与 size |
| X-028 | 现有 successful D3D9 Lock sidecar/ABI 9 已足以提交并授权 CPU-MT staging job | payload 明文 diagnostics-only且缺 source/palette/candidate token；ABI 无 submit/acquire/commit/abort，`onUpload` 晚于当前 kernel |
| X-029 | worker 可在 `0x6F0EEBA6` 合流点或 Unlock 后再 join/补写 mapped VB | 合流点已混合 normal/fault；`0x6F0EEBB6` Unlock 是 mapped pointer 物理不可逆边界 |
| X-030 | WorldFrame `+0x340` 可因同族存在 `COmniLight` 而命名为 omni light | 字段调用 `CLight_Create`，分配 `0xDC` 并安装 `CLight` vptr；Omni factory 独立分配 `0x104` |
| X-031 | `+0x378` 是固定 inline 8-handle owner | 它是 `TargetIndicatorVector.data`；`+0x370/+0x374` 是 capacity/count，且有 reallocate |
| X-032 | `+0x35C..+0x38F` 没有可拆分字段证据 | stage0 result、两张 vector、ring cursor、两个 accumulator 与两个 latch 均有独立 x86 read/write/clear points |

## 5. 来源层级

1. `Game.dll` 原始 PE 字节、MSVC RTTI records、真实 x86 ASM 和 xref。
2. 已读回的 IDA name/type/comment，仅作为数据库当前状态；语义必须由第 1 层支撑。
3. `native_asm_contract.md` 中附 raw bytes/RVA 的已复核结论。
4. 17/19/24/overnight 等历史文档，只作为线索或冲突来源。
5. Hex-Rays 伪代码只辅助理解，不单独升级状态。
