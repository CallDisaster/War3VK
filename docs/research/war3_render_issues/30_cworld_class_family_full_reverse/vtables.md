# vtable 全量地址台账

## 1. 表格规则

- 槽号从 vftable 首函数指针开始计数；`vftable[-1]` 的 COL 不算槽。
- 地址从当前 `Game.dll.i64` 原始 dword 读取，不沿用旧文档的 `vt+N` 歧义。
- `Same` 表示派生类与 `CWorldObjects` 使用同一目标；`Override` 只表示地址不同，不等于语义已知。
- ABI 未逐条读真实函数尾部前一律写 Unknown。

## 2. `CWorldFrameWar3` primary，`0x6F98DCD0`，57 槽

| slot | target | 当前语义 | ABI | 置信度 |
|---:|---:|---|---|---|
| 0 | `0x6F031FB0` | shared `TRefCnt` delete-self dispatcher；非空时以 flags=1 调 vslot1 | `void __thiscall(TRefCnt*)` | Confirmed |
| 1 | `0x6F361130` | `CWorldFrameWar3` scalar deleting destructor | `CWorldFrameWar3* __thiscall(this, uint32_t flags)`，`retn 4` | Confirmed |
| 2 | `0x6F056AF0` | shared `CObserver` event-recipient registration；确保 `this+8` registry | `void __thiscall(CObserver*, uint32_t key, uint32_t eventTag, void* recipient)`，`retn 0xC` | Confirmed；旧 `__stdcall` Contradicted |
| 3 | `0x6F367970` | observer event 转发到 `this+0x20` link vslot3；空 link 返回 0 | `int __thiscall(this, eventRecord*)`，最终 `retn 4` | Confirmed；link 类型 Unknown |
| 4 | `0x6F0991F0` | shared `CFrame` event adapter；写 sender `event+0xC=this`，转 vslot5 | `int __thiscall(this, eventRecord*)`，最终 `retn 4` | Confirmed |
| 5 | `0x6F056300` | shared `CObserver` dispatch；按 key 遍历 recipients 并 OR callback result | `int __thiscall(CObserver*, uint32_t key, eventRecord*)`，`retn 8` | Confirmed |
| 6 | `0x6F09CA30` | select/conditional-clear global channel `0x6FBB9D88`；旧/新对象调 vslots29/28 | `void __thiscall(CFrame*, int selectNonzero)`，`retn 4` | 机械语义 Confirmed；channel 名 Unknown |
| 7 | `0x6F09D000` | select/conditional-clear global channel `0x6FBB9D90`；旧/新对象调 vslots37/36 | 同 slot6 | 机械语义 Confirmed；channel 名 Unknown |
| 8 | `0x6F09CF10` | select/conditional-clear global channel `0x6FBB9D94`；旧/新对象调 vslots40/39 | 同 slot6 | 机械语义 Confirmed；channel 名 Unknown |
| 9 | `0x6F099210` | recursive frame pass；调用 self slots11/12/13，再递归 child slot9 | `void __thiscall(CFrame*, void* framePassContext)`，`retn 4` | Confirmed；“纯 render”标签未证 |
| 10 | `0x6F3683F0` | mouse-event hit test；组合 layout rect、world pick 与 CFrame fallback | `bool __thiscall(this, CMouseEvent*)`，`retn 4` | Confirmed |
| 11 | `0x6F368480` | world-frame update/pass preparation；time、camera/frustum、visibility、fog/shadow、particle/ribbon；更新 stage0 result 与 `3.0f` periodic/one-shot latches | `bool __thiscall(this, float, float, void* ctx18)`，`retn 0xC`；第一 float 在此 override 未读 | Confirmed；参数业务名部分 Unknown |
| 12 | `0x6F3681C0` | render scene | `__thiscall(this)`, no stack args | Confirmed |
| 13 | `0x6F0A2270` | post-render category/list cleanup；读 `+0x158/+0x14C/+0x124` bit4 | `void __thiscall(CFrame*)` | 行为 Confirmed；category 名 Unknown |
| 14 | `0x6F367C70` | world click router；只构造 sprite/ghost-sprite/terrain click events | `bool __thiscall(this, CMouseEvent*)`，`retn 4` | Confirmed |
| 15 | `0x6F09BA70` | event `0x400500C9` 转发到 receiver vslot4 | `int __thiscall(this, eventRecord*)`，dynamic tail，最终 `retn 4` | Confirmed |
| 16 | `0x6F368160` | event `0x400500CA` adapter；未处理时刷新 GameUI 两子对象与 world indicator anchor；恒返回 1 | `int __thiscall(this, eventRecord*)`，`retn 4` | Confirmed |
| 17 | `0x6F09BA50` | event `0x400500CB`；可选全局 receiver 非空时替代 `this`，再转 vslot4 | `int __thiscall(this, eventRecord*)`，dynamic tail，最终 `retn 4` | Confirmed；global 身份 Unknown |
| 18 | `0x6F09BA30` | event `0x400500CC` 转发到 receiver vslot4 | 同 slot15 | Confirmed |
| 19 | `0x6F09BA80` | event `0x400500CD` 转发到 receiver vslot4 | 同 slot15 | Confirmed |
| 20 | `0x6F09B7E0` | event `0x40060067` 转发到 receiver vslot4 | 同 slot15 | Confirmed |
| 21 | `0x6F09B9B0` | event `0x40060064` 转发到 receiver vslot4 | 同 slot15 | Confirmed |
| 22 | `0x6F09B9C0` | event `0x40060065` 转发到 receiver vslot4 | 同 slot15 | Confirmed |
| 23 | `0x6F09B9D0` | event `0x40060066` 转发到 receiver vslot4 | 同 slot15 | Confirmed |
| 24 | `0x6F09B9A0` | event `0x40060068` 转发到 receiver vslot4 | 同 slot15 | Confirmed |
| 25 | `0x6F09BAF0` | effective-active edge：按 registration count 安装 `AUKeyboardFocus`，置 `+0xB0` bit `0x10` | `void __thiscall(this)` | Confirmed |
| 26 | `0x6F09B960` | effective-inactive edge：清当前 focus、拆除 `AUKeyboardFocus`，清 bit `0x10` | `void __thiscall(this)` | Confirmed |
| 27 | `0x6F367B40` | destructor-time derived resource teardown | `void __thiscall(this)` | Confirmed；`CFog/CLight` 与三项 pooled-Uber refs 已 exact，`+0x588/+0x58C` 等仍 Unknown |
| 28 | `0x6F09B7C0` | global-current transition enter hook，no-op | `void __thiscall(this)` | body/caller context Confirmed；global 身份 Unknown |
| 29 | `0x6F09B890` | global-current transition leave hook，no-op | `void __thiscall(this)` | body/caller context Confirmed；global 身份 Unknown |
| 30 | `0x6F09BAB0` | 首次置 `+0xB0` bit `0x20` 后的通知槽，no-op | `void __thiscall(this)` | caller context Confirmed；业务名 Unknown |
| 31 | `0x6F09B8B0` | `+0x30==0` 时向 child 链传播 pointer property | `void __thiscall(this, void*)`，`retn 4` | Confirmed；property 类型 Unknown |
| 32 | `0x6F09B8E0` | `+0x34<=0.0f` 时向 child 链传播 float property | `void __thiscall(this, float)`，`retn 4` | Confirmed；property 名 Unknown |
| 33 | `0x6F09B7F0` | `+0x2C==0` 时传播 pointer property；特定 global-current 对象另有 side effect | `void __thiscall(this, void*)`，`retn 4` | Confirmed；property/global 身份 Unknown |
| 34 | `0x6F3679A0` | hover-enter override | `void __thiscall(this)` | Confirmed |
| 35 | `0x6F367A90` | hover-leave override | `void __thiscall(this)` | Confirmed |
| 36 | `0x6F09BA00` | no-op | `void __thiscall(this)` | Confirmed |
| 37 | `0x6F09BA10` | no-op | `void __thiscall(this)` | Confirmed |
| 38 | `0x6F09BA90` | no-op | `void __thiscall(this)` | Confirmed |
| 39 | `0x6F09B9E0` | no-op | `void __thiscall(this)` | Confirmed |
| 40 | `0x6F09B9F0` | no-op | `void __thiscall(this)` | Confirmed |
| 41 | `0x6F09B7D0` | no-op | `void __thiscall(this)` | Confirmed |
| 42 | `0x6F09BAE0` | no-op | `void __thiscall(this)` | Confirmed |
| 43 | `0x6F09BAD0` | default true predicate | `int __thiscall(this, uint32_t, uint32_t)`，`retn 8` | Confirmed；参数语义 Unknown |
| 44 | `0x6F09BB30` | no-op | `void __thiscall(this)` | Confirmed |
| 45 | `0x6F09BB50` | no-op | `void __thiscall(this)` | Confirmed |
| 46 | `0x6F0A20F0` | layout rectangle query | `bool __thiscall(this, RectF* out, UnknownLayoutContext*)`，`retn 8` | ABI/role Confirmed；context 类型 Unknown |
| 47 | `0x6F0A1F80` | normalized/delegated rectangle query | `bool __thiscall(this, RectF* out, const RectF* src, uint32_t reserved)`，所有出口 `retn 0xC` | Confirmed |
| 48 | `0x6F09B940` | default no layout offset；返回 false | `bool __thiscall(this, float outXY[2])`，`retn 4` | Confirmed |
| 49 | `0x6F0985E0` | default false predicate | `bool __thiscall(this)` | Confirmed；业务语义 Unknown |
| 50 | `0x6F09AA20` | default false predicate | `bool __thiscall(this)` | Confirmed；业务语义 Unknown |
| 51 | `0x6F099A50` | 向输出写三个 `0xFFFFFFFF` sentinel | `uint32_t* __thiscall(this, uint32_t out[3])`，`retn 4` | Confirmed |
| 52 | `0x6F09A320` | 设置 `+0xB0` hidden bit0、传播 hidden，调用 slot26 | `void __thiscall(this)` | Confirmed |
| 53 | `0x6F09D450` | 清 hidden bit0；若 effective-active 则调用 slot25，再传播 visible | `void __thiscall(this)` | Confirmed |
| 54 | `0x6F0A1870` | 释放一个 layer/aux association | `void __thiscall(this, void*)`，`retn 4` | role/ABI Confirmed；参数类型 Unknown |
| 55 | `0x6F0A1960` | default true predicate | `bool __thiscall(this, uint32_t, uint32_t)`，`retn 8` | Confirmed；参数语义 Unknown |
| 56 | `0x6F0A2040` | 累加本 frame 与 parent layout offset | `bool __thiscall(this, float inoutXY[2])`，`retn 4` | Confirmed |

### 2.1 主表机械清栈 ABI

真实函数尾/CFG 已给出以下 callee-cleanup 集合；`ret 0` 表示普通 `ret`，不代表返回值类型：

- `ret 0`：slots `0,12..13,25..30,34..42,44..45,49..50,52..53`；
- `ret 4`：slots `1,6..10,14..24,31..33,48,51,54,56`；
- `ret 8`：slots `5,43,46,55`；
- `ret 12`：slots `2,11,47`；
- direct tail：slot 3 -> `0x6F09B470`，slot 4 -> `0x6F0562E0`；
- dynamic tail 到 `[vtable+0x10]`（零基 vslot4，最终由 base adapter `retn 4`）：slots `15,17..24`；
- slot 47 全部 CFG 出口均为 `retn 0xC`。slot 6 的唯一正常出口为 `retn 4`，旧“混合清栈”结论已由逐指令 CFG 否定；
- slot 12 RenderScene 由真实 caller/函数体闭合为无栈参数，不依赖线性尾跳扫描；slot2 的
  incoming ECX 由 ctor caller 和 callee 首次 helper call 双重证明，旧 `__stdcall` 自动型已删除。

这覆盖了 57 槽的机械清栈边界；参数业务名和返回类型仍按逐槽表的置信度处理。

### 2.2 基表共享与真实 overrides

raw base vtables：`TRefCnt @ 0x6F95214C`、`CObserver @ 0x6F95651C`、
`CFrame @ 0x6F95A760`。WorldFrame 与 CFrame 完全共用 slots `0,2,4..9,13`；slot3 是独立
entry 但尾部等价转入 CFrame slot3。已逐 dword 比较确认的 WorldFrame-specific overrides 是
`1,10..12,14,34,35`；slots36..56 与 CFrame 对应表项相同。slots15..33 的行为与 ABI 已闭合，
但本批没有把每个地址与完整 CFrame 表重新做 equality ledger，故不在这里擅自归类为 Same 或
Override。共享地址不等于字段归属可以写成 WorldFrame 派生字段；本卷在命名时保留
`TRefCnt/CObserver/CFrame` owner。

## 3. `CWorldFrameWar3` secondary `CLayoutFrame` view，`0x6F98DDB8`，9 槽

| slot | target | 当前语义 | ABI/备注 | 状态 |
|---:|---:|---|---|---|
| 0 | `0x6F0A1F00` | rectangle-changing notification；回到 owner `this-0xB4` 并 invalidate anchors | `void __thiscall(CLayoutFrame*, const RectF*)`，`retn 4` | Confirmed |
| 1 | `0x6F0BD240` | resolve and commit layout | `bool __thiscall(CLayoutFrame*)` | role/ABI Confirmed |
| 2 | `0x6F360B77` | scalar deleting-dtor adjustor thunk | `sub ecx,0xB4; jmp 0x6F361130`；最终 `retn 4` | raw bytes/adjustment Confirmed |
| 3 | `0x6F0A2730` | validate and commit rectangle | `bool __thiscall(CLayoutFrame*, RectF*)`，`retn 4` | Confirmed |
| 4 | `0x6F0BCE10` | try-get rectangle | `bool __thiscall(CLayoutFrame*, RectF*)`，`retn 4` | Confirmed |
| 5 | `0x6F0A26B0` | set scale and propagate | `void __thiscall(CLayoutFrame*, float)`，`retn 4` | Confirmed |
| 6 | `0x6F0BCE40` | get scaled width | `float __thiscall(CLayoutFrame*)`，x87 return | Confirmed |
| 7 | `0x6F0BCDF0` | get scaled height | `float __thiscall(CLayoutFrame*)`，x87 return | Confirmed |
| 8 | `0x6F0A1B70` | query owner `+0x0C` flag bit3 | `bool __thiscall(CLayoutFrame*)` | Confirmed；bit 的业务名 Unknown |

次表 slot2 是可直接核验的原始 7-byte adjustor thunk，不应再写成“可能调整”。其余槽的
`this` 均保持 secondary view；凡需访问 primary owner 的实现会显式减 `0xB4`。

secondary subobject footprint 为 `0x68`。slots6/7 精确返回
`(+0x60 scale)*(+0x58 width/+0x5C height)`；slot3 写 cached RectF `+0x44..+0x50` 并维护
`+0x54` valid gate，slot4 只有 valid 非零才复制输出。slot1 的 `+0x08..+0x28` 是 9-pointer
3x3 provider grid，`+0x2C` 六个 bit 是递归求解进行中位。所有偏移在
[字段卷](fields.md) 同时列出 secondary/primary 换算；矩形 valid=0 时旧字节不可消费。

## 4. `CShowable`，`0x6FA59AAC`，6 槽

| slot | target | ABI | 可证行为 | 状态 |
|---:|---:|---|---|---|
| 0 | `0x6F74CCD0` | `__thiscall(this, flags)`, `retn 4` | scalar deleting destructor | Confirmed |
| 1 | `0x6F75CE90` | `__thiscall(this, u32)`, `retn 4` | 条件写 `+4`，随后虚调 slot4 | 行为 Confirmed，业务名 Unknown |
| 2 | `0x6F754880` | `__thiscall(this)` | 返回 `+4` | Confirmed |
| 3 | `0x6F75CEB0` | `__thiscall(this, u32)`, `retn 4` | 条件写 `+8`，随后虚调 slot4 | 行为 Confirmed，业务名 Unknown |
| 4 | `0x6F7594C0` | `__thiscall(this)` | no-op | Confirmed |
| 5 | `0x6F751940` | `__thiscall(this)` | `slot2()!=0 && +8==0` | Confirmed |

`CWorldObjects` 继承 slots 1/2/3/5，覆盖 slot4 为 `0x6F7594D0`；该 override 会遍历 `0x188` 字节 model entries 并更新其两个 stamp/index 资源，业务名仍待下游函数闭合。

## 5. `CClippable` 与 `CWorldObjectsClippable`

| slot | `CClippable` | `CWorldObjectsClippable` | ABI | 派生可证行为 |
|---:|---:|---:|---|---|
| 0 | `0x6F74CC70` | `0x6F74CD30` | `__thiscall(this,flags)`, `retn 4` | scalar deleting destructor |
| 1 | `0x6F759860` | `0x6F759870` | 2 stack args，`retn 8` | 派生固定返回 0；base 固定 1 |
| 2 | `0x6F74FC00` | `0x6F74FC10` | 2 stack args，`retn 8` | 派生固定返回 0；base 固定 1 |
| 3 | `0x6F74F8B0` | `0x6F74F8C0` | 1 stack arg，`retn 4` | 派生返回 `arg==0`；base 固定 0 |
| 4 | `0x6F75D480` | `0x6F75D490` | 1 stack arg，`retn 4` | 派生固定 0；base 固定 1 |
| 5 | `0x6F75A990` | `0x6F75A9A0` | 1 stack arg，`retn 4` | 参数非零或 `+18==0` 时返回1，否则转调 slot2 |
| 6 | `0x6F753310` | `0x6F753320` | 1 stack arg，`retn 4` | 按 `+4` 为 1/2 和 arg bool 返回 9/13 或 8/12，否则 -1 |
| 7 | `0x6F752210` | `0x6F752220` | `__thiscall(this)` | Clone：独立 outer array + 已证 nested-owner copy-construction；raw 区原样复制 |
| 8 | `0x6F754520` | `0x6F754540` | `__thiscall(this, float out[3])`, `retn 4` | 平均 records `+8/+C/+10`；空 count 无除零保护 |
| 9 | `0x6F757BA0` | `0x6F757BB0` | `__thiscall(this)` | 返回 `0x1C + count*0xB4` |

slots 1..6 的接口业务名仍 Unknown；固定返回值不能单独证明它们是“clip/intersect/serialize”中的哪一种。

## 6. `CWorldObjects` / `CDoodads` / `CBlightPuffs`，103 槽全地址

表中 `D`、`B` 表示对应派生类地址与 base 不同。

| slot | CWorldObjects | CDoodads | CBlightPuffs | override |
|---:|---:|---:|---:|---|
| 0 | `74CD00` | `74CCA0` | `74CC40` | D,B |
| 1 | `75CE90` | `75CE90` | `75CE90` | - |
| 2 | `754880` | `754880` | `754880` | - |
| 3 | `75CEB0` | `75CEB0` | `75CEB0` | - |
| 4 | `7594D0` | `7594D0` | `7594D0` | - |
| 5 | `751940` | `751940` | `751940` | - |
| 6 | `74F230` | `74F220` | `74F230` | D |
| 7 | `75CB60` | `75CB10` | `75CB60` | D |
| 8 | `74E930` | `74E910` | `74E8F0` | D,B |
| 9 | `75AC10` | `75AC00` | `75ABF0` | D,B |
| 10 | `7593B0` | `7592A0` | `759200` | D,B |
| 11 | `753D80` | `753D00` | `753D80` | D |
| 12 | `753DB0` | `753D90` | `753DB0` | D |
| 13 | `754A30` | `754A20` | `754A30` | D |
| 14 | `74E8B0` | `74E860` | `74E8B0` | D |
| 15 | `7504D0` | `7503B0` | `7504D0` | D |
| 16 | `75CFA0` | `75CF90` | `75CFA0` | D |
| 17 | `753BC0` | `753B40` | `753BC0` | D |
| 18 | `7564C0` | `756470` | `756460` | D,B |
| 19 | `7563B0` | `756390` | `7563B0` | D |
| 20 | `7519E0` | `751980` | `7519E0` | D |
| 21 | `758E70` | `758E70` | `758E70` | - |
| 22 | `751970` | `751960` | `751970` | D |
| 23 | `75BB20` | `75BA60` | `75BB20` | D |
| 24 | `757300` | `7571C0` | `757300` | D |
| 25 | `74EB00` | `74EAA0` | `74EB00` | D |
| 26 | `7537C0` | `7537C0` | `7537C0` | - |
| 27 | `752860` | `752600` | `752860` | D |
| 28 | `7597E0` | `7596B0` | `7597E0` | D |
| 29 | `758E90` | `758E80` | `758E90` | D |
| 30 | `759810` | `7597F0` | `759810` | D |
| 31 | `759B30` | `759AE0` | `759B30` | D |
| 32 | `758C60` | `758C60` | `758C60` | - |
| 33 | `75A980` | `75A980` | `75A980` | - |
| 34 | `758EF0` | `758EB0` | `758EF0` | D |
| 35 | `7591E0` | `7591E0` | `7591E0` | - |
| 36 | `758CD0` | `758CD0` | `758CD0` | - |
| 37 | `758B60` | `758B30` | `758B60` | D |
| 38 | `753360` | `753360` | `753360` | - |
| 39 | `75E540` | `75E540` | `75E540` | - |
| 40 | `753E50` | `753DF0` | `753E50` | D |
| 41 | `754510` | `7544B0` | `754510` | D |
| 42 | `75E530` | `75E4B0` | `75E530` | D |
| 43 | `74F2A0` | `74F2A0` | `74F2A0` | - |
| 44 | `753CF0` | `753CF0` | `753CF0` | - |
| 45 | `74F2C0` | `74F2B0` | `74F2C0` | D |
| 46 | `74EB10` | `74EB10` | `74EB10` | - |
| 47 | `7538B0` | `753820` | `753800` | D,B |
| 48 | `754860` | `754810` | `754860` | D |
| 49 | `75E570` | `75E560` | `75E570` | D |
| 50 | `754870` | `754870` | `754870` | - |
| 51 | `75CF50` | `75CF50` | `75CF50` | - |
| 52 | `75BEC0` | `75BEC0` | `75BEC0` | - |
| 53 | `7549A0` | `7549A0` | `7549A0` | - |
| 54 | `754380` | `754350` | `754380` | D |
| 55 | `753CE0` | `753C90` | `753CE0` | D |
| 56 | `756450` | `756420` | `756450` | D |
| 57 | `75E4A0` | `75E4A0` | `75E4A0` | - |
| 58 | `758E60` | `758CE0` | `758E60` | D |
| 59 | `7596A0` | `759680` | `7596A0` | D |
| 60 | `754770` | `754710` | `754770` | D |
| 61 | `758BE0` | `758B90` | `758BE0` | D |
| 62 | `758C00` | `758BF0` | `758C00` | D |
| 63 | `758C50` | `758C20` | `758C50` | D |
| 64 | `758B80` | `758B70` | `758B80` | D |
| 65 | `758C10` | `758C10` | `758C10` | - |
| 66 | `7591F0` | `7591F0` | `7591F0` | - |
| 67 | `756360` | `756330` | `756310` | D,B |
| 68 | `753990` | `753940` | `753990` | D |
| 69 | `7539F0` | `7539A0` | `7539F0` | D |
| 70 | `74E8E0` | `74E8C0` | `74E8E0` | D |
| 71 | `7546E0` | `7546A0` | `7546E0` | D |
| 72 | `7537D0` | `7537D0` | `7537D0` | - |
| 73 | `7534B0` | `7534B0` | `7534B0` | - |
| 74 | `7538F0` | `7538C0` | `7538F0` | D |
| 75 | `753930` | `753900` | `753930` | D |
| 76 | `754490` | `754460` | `754490` | D |
| 77 | `7537B0` | `753780` | `7537B0` | D |
| 78 | `753C10` | `753BE0` | `753C10` | D |
| 79 | `7522C0` | `7522C0` | `7522C0` | - |
| 80 | `753770` | `7536D0` | `753770` | D |
| 81 | `7504E0` | `7504E0` | `7504E0` | - |
| 82 | `753A00` | `753A00` | `753A00` | - |
| 83 | `754690` | `754690` | `754690` | - |
| 84 | `75BB50` | `75BB30` | `75BB50` | D |
| 85 | `759850` | `759820` | `759850` | D |
| 86 | `758B20` | `758B20` | `758AD0` | B |
| 87 | `74F540` | `74F2D0` | `74F540` | D |
| 88 | `74F810` | `74F810` | `74F810` | - |
| 89 | `74F030` | `74EFC0` | `74F030` | D |
| 90 | `74F290` | `74F280` | `74F290` | D |
| 91 | `7522B0` | `7522B0` | `7522B0` | - |
| 92 | `758CC0` | `758CC0` | `758CC0` | - |
| 93 | `758CB0` | `758CB0` | `758CB0` | - |
| 94 | `753560` | `753560` | `753560` | - |
| 95 | `7544A0` | `7544A0` | `7544A0` | - |
| 96 | `75E490` | `75E460` | `75E490` | D |
| 97 | `7552B0` | `755280` | `7552B0` | D |
| 98 | `758CA0` | `758C70` | `758CA0` | D |
| 99 | `758980` | `758970` | `758980` | D |
| 100 | `757DC0` | `757DB0` | `757DC0` | D |
| 101 | `75CF80` | `75CF60` | `75CF80` | D |
| 102 | `74D720` | `74D700` | `74D720` | D |

所有短地址都隐含模块前缀 `0x6F`。例如 slot 48 的 `CDoodads` 目标是 `0x6F754810`。
逐槽行为与机械 ABI 见 [worldobjects_vtable_abi.md](worldobjects_vtable_abi.md)：当前 103 槽均有
真实 ASM 支撑，slots0..77、78..102 也已按可得 caller/DB/string dataflow 写入保守行为；
slot57、64..66 与若干返回码/flag 的公开业务名仍明确保留 Unknown。

### 6.1 `CDoodads` embedded hash-table vtable

`CDoodads+0x118` 的组合对象有独立 4 槽表：

| slot | target | 当前语义/ABI |
|---:|---:|---|
| 0 | `0x6F755910` | `DeleteNode(this,node)`；ECX 未消费；先析构 `AUModelColorHash` 再 Storm free；`retn 4`；Confirmed |
| 1 | `0x6F755AD0` | `AllocateAndLinkNode(this,linkAnchor,payloadBytes,flags)`；ECX 未消费；分配 `payloadBytes+0x1C`，返回 node/null；`retn 0xC`；Confirmed |
| 2 | `0x6F74CC10` | scalar deleting dtor；总是调 exact dtor，flags bit0 决定 free，返回 this；`retn 4`；Confirmed |
| 3 | `0x6F751730` | `Clear(this)`；销毁 nodes、复位 table 状态；普通 `ret`；Confirmed |

vftable `0x6FA59C68`，COL `0x6FACBBBC`，TD `0x6FBB4230`。表边界、组合类型与四槽
机械 ABI/直接行为均已 Confirmed；slot1 的 `linkAnchor` 精确公开类型仍为 Unknown。

### 6.2 当前已闭合的共同槽

- slots 0..5：见 `CShowable` 节；slot0 为各类自己的 scalar deleting destructor，1/2/3/5 继承，slot4 是 `CWorldObjects` override。
- `CBlightPuffs` 只在 slots `0,8,9,10,18,47,67,86` 与 base 地址不同，八个地址 override
  均已审：8/9 是 base ABI thunk，10 为 birth/stand/death state machine，18 返回 `arg==0`，
  47 输出 `Blight/PuffModel`，67 返回 active flag byte，86 累加 elapsed-ms。
- `CDoodads` 覆盖大量槽；旧文档把 `0x754A20/75CF90/753B40/756470` 写成 slots 48..51，实际是 slots 13/16/17/18。真实 slots 48..51 如上表。

## 7. `CSprite` / Mini / Uber / pooled leaf，五张 29 槽表

以下地址由 IDB 原始 dword 重新读取；短地址均隐含 `0x6F` 模块前缀：

| slot | CSprite | Mini | Uber | LeafMini | LeafUber |
|---:|---:|---:|---:|---:|---:|
| 0 | `181DF0` | `181E20` | `181E50` | `181AB0` | `181AE0` |
| 1 | `0E2170` | `0E2170` | `0E2170` | `1836B0` | `1836D0` |
| 2 | `0E1CF0` | `0E1CF0` | `0E1CF0` | `0E1CF0` | `0E1CF0` |
| 3 | `1820B0` | `1820C0` | `182300` | `1820C0` | `182300` |
| 4 | `1825D0` | `1825E0` | `1826C0` | `1825E0` | `1826C0` |
| 5 | `1877C0` | `1877C0` | `1877D0` | `1877C0` | `1877D0` |
| 6 | `184890` | `1848A0` | `1848D0` | `1848A0` | `1848D0` |
| 7 | `184900` | `184910` | `184930` | `184910` | `184930` |
| 8 | `184960` | `184970` | `184980` | `184970` | `184980` |
| 9 | `1849A0` | `1849B0` | `1849D0` | `1849B0` | `1849D0` |
| 10 | `184A00` | `184A10` | `184A40` | `184A10` | `184A40` |
| 11 | `184A70` | `184A80` | `184AB0` | `184A80` | `184AB0` |
| 12 | `184B70` | `184B80` | `184BB0` | `184B80` | `184BB0` |
| 13 | `184AF0` | `184B00` | `184B30` | `184B00` | `184B30` |
| 14 | `1833C0` | `183410` | `183440` | `183410` | `183440` |
| 15 | `183470` | `183490` | `1834B0` | `183490` | `1834B0` |
| 16 | `1834D0` | `1834E0` | `1834F0` | `1834E0` | `1834F0` |
| 17 | `183500` | `183510` | `183520` | `183510` | `183520` |
| 18 | `183530` | `183540` | `183570` | `183540` | `183570` |
| 19 | `183580` | `183590` | `1835A0` | `183590` | `1835A0` |
| 20 | `1835B0` | `1835C0` | `1835F0` | `1835C0` | `1835F0` |
| 21 | `183780` | `183780` | `183790` | `183780` | `183790` |
| 22 | `182800` | `182800` | `182810` | `182800` | `182810` |
| 23 | `184830` | `184830` | `184850` | `184830` | `184850` |
| 24 | `184610` | `184610` | `184630` | `184610` | `184630` |
| 25 | `1829B0` | `1829B0` | `1829C0` | `1829B0` | `1829C0` |
| 26 | `183610` | `183610` | `183620` | `183610` | `183620` |
| 27 | `182970` | `182970` | `182980` | `182970` | `182980` |
| 28 | `1845C0` | `1845C0` | `1845D0` | `1845C0` | `1845D0` |

本批只把真实 ASM 已闭合的槽升级语义；其余地址完整但 ABI/业务仍 Unknown：

| slot | 已证语义 | ABI/置信度 |
|---:|---|---|
| 0 | 各动态类 scalar deleting destructor | `__thiscall(this,uint32 flags)`, `retn 4`；Confirmed |
| 1 | base/Mini/Uber `0x6F0E2170` 是公共 refcount-zero destroy hook；两个 leaf override 先 `slot0(flags=0)`，再回到 Mini/Uber pool | leaf `void __thiscall(this)`；pool globals `0x6FBE3D9C/0x6FBE3D88`；Confirmed；公共 target 的更细 owner 名待审 |
| 3 | visibility/prepare dispatch；`0x6F184F00` wrapper 调该槽 | caller/dispatch role Confirmed；精确参数/业务分支仍 Inferred |
| 5 | base/Mini `0x6F1877C0` 是 no-op；Uber `0x6F1877D0` flush `+139/+148` 与 `+1A1/+1B0` 两组 pending bit8 后清位 | `void __thiscall(this)`；机械行为 Confirmed，pending 业务名 Unknown |
| 25 | base/Mini 返回 null；Uber 返回 `this+0x78`，供 `0x6F1854A0` recursive candidate tree | pointer `__thiscall(this)`；Confirmed；公开返回类型 Unknown |

因此 `CSprite_PrepareAndQueueAttachedRenderObject @ 0x6F184EE0` 中的 slot5 不能再命名为
`PreRenderAndUpdatePosePalette`；该 helper 本身也只在 `sprite+0x20` 非空时做 slot5 + AddBatch，
不直接 draw。slots2/4/6..24/26..28 仍需逐一读取正常出口、caller 与字段 dataflow，在此保持
ABI/语义 Unknown，不能因旧 19/overnight 文档已有自然语言标题就升级。

## 8. `CEnvEffect` / `CFog` / `CLight` / `COmniLight`，四张 5 槽表

四表边界由各自 COL cell、下一张 COL cell 和 raw dword 连续读取闭合；它们都是 offset-0
单继承视图，不含 adjustor thunk：

| slot | `CEnvEffect @ 0x6F964AC4` | `CFog @ 0x6F964ADC` | `CLight @ 0x6F964B08` | `COmniLight @ 0x6F964B20` |
|---:|---:|---:|---:|---:|
| 0 | `0x6F191090` | `0x6F1910C0` | `0x6F1910F0` | `0x6F191120` |
| 1 | `0x6F0E2170` | `0x6F0E2170` | `0x6F0E2170` | `0x6F0E2170` |
| 2 | `0x6F0E1CF0` | `0x6F0E1CF0` | `0x6F0E1CF0` | `0x6F0E1CF0` |
| 3 | `0x6F191190` | `0x6F1911A0` | `0x6F191210` | `0x6F191280` |
| 4 | `0x6F191550` | `0x6F191560` | `0x6F1915B0` | `0x6F1915B0` |

逐槽机械 ABI 与已证行为：

| slot | ABI | base/derived 行为 | 置信度 |
|---:|---|---|---|
| 0 | `T* __thiscall(T*, unsigned flags)`，`retn 4`，返回原 this | 各类 scalar deleting dtor；先走 non-deleting dtor，flags bit0 决定 free | Confirmed |
| 1 | `void __thiscall(CHandleObject*)` | `CHandleObject_CallScalarDeletingDtor`：this 非空时以 flags=1 调其 vslot0 | Confirmed；旧 Concurrency 自动名 Contradicted |
| 2 | `int __thiscall(CHandleObject*)` | `CHandleObject_Vslot2_ReturnZero`：`xor eax,eax; ret` | Confirmed；公开接口名 Unknown |
| 3 | `void __thiscall(this)` | base `0x6F191190` no-op；CFog `0x6F1911A0` 提交动态 fog 参数；CLight `0x6F191210` 填 native light record 并 commit；COmni `0x6F191280` 先走 base Light 提交，再写自身 `+0xF8..+0x100` vector 并 commit | mechanical behavior Confirmed；外部公开 method 名 Unknown |
| 4 | `void __thiscall(this)` | base `0x6F191550` no-op；CFog `0x6F191560` 提交 fast fog params；Light/Omni `0x6F1915B0` 把 native record state 写 0 并 commit | mechanical behavior Confirmed |

`COmniLight` 只 override slots0/3，slots1/2/4 继承公共 target。WorldFrame `+0x340` 由
`CLight_Create` 构造，vptr 是 `0x6F964B08`；不能因为同族有 `COmniLight` 就把该字段升级成
派生实例。

## 9. `CCinematicFilter`，`0x6F98ED34`，1 槽

| slot | 地址 | ABI | 真实行为 | 置信度 |
|---:|---:|---|---|---|
| 0 | `0x6F38A130` | `CCinematicFilter* __thiscall(CCinematicFilter* this, unsigned flags)`；`retn 4` | 调 `CCinematicFilter_Dtor @ 0x6F386650`；flags bit0 且 this 非空时 Storm-free；返回原 this | Confirmed scalar deleting dtor |

边界由三个连续 raw dword 闭合：`vt[-1] @ 0x6F98ED30 = 0x6FA874AC`（本类 COL）、
`vt[0] = 0x6F38A130`、`0x6F98ED38 = 0x6FA87564`（下一类 `CAllianceSlot` COL）。因此不存在
尚未枚举的 slot1；这张表已经地址、ABI、语义全闭合。

## 10. 未完成条件

103 槽的真实栈清理、this 消费和 slots 0..27 的行为审计已移到
[逐槽 ABI 分卷](worldobjects_vtable_abi.md)。

103 槽表“地址完整”不等于“逆向完成”。每个槽还必须补：

1. 函数起止和真实 `retn N`；
2. ECX/EDX/stack 形参来源及返回值；
3. base 与 derived 的 this-field 访问差异；
4. 直接/间接 callees、owner 和生命周期；
5. 最终语义及 Confirmed/Inferred/Unknown。

在这些列闭合前，不能用邻近函数名、旧 `vt+byteOffset` 或 IDA 自动库函数误识别填充语义。
