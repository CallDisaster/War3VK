# RTTI、继承树与对象大小

## 1. 证据口径

本卷直接读取 MSVC x86 RTTI：`vftable[-1] -> CompleteObjectLocator -> TypeDescriptor / ClassHierarchyDescriptor -> BaseClassArray -> BaseClassDescriptor`。PMD 记录为 `(mdisp, pdisp, vdisp)`；`pdisp=0xFFFFFFFF` 表示非 virtual base。旧 `RTTI.md` 只作定位索引，最终地址和关系均从 `Game.dll.i64` 原始 dword 读回。

## 2. `CWorldFrameWar3`

### 2.1 两张表与 COL

| 视图 | vftable | `vftable[-1]` COL | COL dword `[signature, offset, cdOffset, type, hierarchy]` |
|---|---:|---:|---|
| primary | `0x6F98DCD0` | `0x6FA86BD8` | `0, 0, 0, 0x6FB8DA98, 0x6FA86BEC` |
| `CLayoutFrame` secondary | `0x6F98DDB8` | `0x6FA86C34` | `0, 0xB4, 0, 0x6FB8DA98, 0x6FA86BEC` |

TypeDescriptor `0x6FB8DA98` 的装饰名为 `.?AVCWorldFrameWar3@@`。两张 COL 共享 CHD `0x6FA86BEC`；CHD 原始 dword 为 `0, 1, 6, 0x6FA86BFC`，即 multiple-inheritance 属性、6 个 BCD。

| BCD | 类型 | `numContainedBases` | PMD | attributes | 结论 |
|---:|---|---:|---|---:|---|
| `0x6FA86C18` | `CWorldFrameWar3` | 5 | `(0, -1, 0)` | `0x40` | complete object |
| `0x6FA6EAA0` | `CFrame` | 4 | `(0, -1, 0)` | `0x40` | primary direct base |
| `0x6FA6E7D4` | `CLayer` | 2 | `(0, -1, 0)` | `0x40` | `CFrame` ancestry |
| `0x6FA6B024` | `CObserver` | 1 | `(0, -1, 0)` | `0x40` | `CLayer` ancestry |
| `0x6FA6AFB8` | `TRefCnt` | 0 | `(0, -1, 0)` | `0x40` | `CObserver` ancestry |
| `0x6FA6EAE4` | `CLayoutFrame` | 0 | `(0xB4, -1, 0)` | `0x40` | secondary direct base |

原始继承关系因此为：

```text
CWorldFrameWar3
├─ CFrame @ +0x00
│  └─ CLayer @ +0x00
│     └─ CObserver @ +0x00
│        └─ TRefCnt @ +0x00
└─ CLayoutFrame @ +0xB4
```

raw base vftable anchors also independently read回：`TRefCnt @ 0x6F95214C`、
`CObserver @ 0x6F95651C`、`CFrame @ 0x6F95A760`。primary slots
`0,2,4..9,13` 与 CFrame 表项地址完全相同；slot3 是 WorldFrame 独立 entry，但尾转到
CFrame slot3。此 equality ledger 用于把共享方法命名回真实 base owner，不能用于推断未读取的
base 字段边界。

### 2.2 大小证据

唯一构造 xref 位于 `CGameUI` 构造路径：

```asm
6F34A32A  push 668h
6F34A32F  call Storm_SMemAlloc_401
...
6F34A348  push edi                  ; CGameUI owner
6F34A349  mov  ecx, eax
6F34A34B  call 6F35EFB0             ; CWorldFrameWar3 ctor
6F34A358  mov  [edi+3BCh], eax
```

所以 `sizeof(CWorldFrameWar3) == 0x668`，并由 `CGameUI+0x3BC` 持有。该结论为 Confirmed，不依赖相邻符号或伪代码。

## 3. `CEnvEffect -> {CFog, CLight -> COmniLight}`

WorldFrame 的 `+0x334/+0x340` 不是 opaque scene pointers；factory、分配大小、ctor vptr 与 raw
RTTI 把它们分别闭合为 `CFog` 与 base `CLight`。同一 RTTI family 还存在独立
`COmniLight` 派生类，但 `+0x340` 的 factory 不创建该派生类。

| 类型 | `vftable[-1]` cell -> COL | TypeDescriptor | CHD / BCA | 继承层级 | complete size |
|---|---|---|---|---|---:|
| `CEnvEffect` | `0x6F964AC0 -> 0x6FA74898`；vtable `0x6F964AC4` | `0x6FB81DB4`, `.?AVCEnvEffect@@` | `0x6FA748AC / 0x6FA748BC`，3 BCD | `CEnvEffect -> CDataMgr -> CHandleObject` | base extent `0x20`；standalone alloc Unknown |
| `CFog` | `0x6F964AD8 -> 0x6FA748E8`；vtable `0x6F964ADC` | `0x6FB81DD0`, `.?AVCFog@@` | `0x6FA748FC / 0x6FA7490C`，4 BCD | `CFog -> CEnvEffect -> CDataMgr -> CHandleObject` | `0xD4` |
| `CLight` | `0x6F964B04 -> 0x6FA7493C`；vtable `0x6F964B08` | `0x6FB81DE4`, `.?AVCLight@@` | `0x6FA74950 / 0x6FA74960`，4 BCD | `CLight -> CEnvEffect -> CDataMgr -> CHandleObject` | `0xDC` |
| `COmniLight` | `0x6F964B1C -> 0x6FA74990`；vtable `0x6F964B20` | `0x6FB81DFC`, `.?AVCOmniLight@@` | `0x6FA749A4 / 0x6FA749B4`，5 BCD | `COmniLight -> CLight -> CEnvEffect -> CDataMgr -> CHandleObject` | `0x104` |

`CEnvEffect` self BCD 为 `0x6FA748CC`；其 ancestor BCD 为 `CDataMgr @ 0x6FA74214`
(TD `0x6FB81864`) 与 `CHandleObject @ 0x6FA70718` (TD `0x6FB7E2B4`)。四条 hierarchy
中的全部 PMD 都是 `(0,-1,0)`，attributes 都是 `0x40`，所以这是 offset-0 单继承族，没有
secondary base。

大小由真实分配点闭合：`CFog_Create @ 0x6F191320` 分配 `0xD4` 后调用
`CFog_Ctor @ 0x6F1904F0`；`CLight_Create @ 0x6F1913B0` 分配 `0xDC` 后调用
`CLight_Ctor @ 0x6F190900`；`COmniLight_Create @ 0x6F191440` 分配 `0x104` 后调用
`COmniLight_Ctor @ 0x6F190B60`。`CEnvEffect` 只由派生首字段界限证明 base extent `0x20`，
当前没有 standalone allocator，不能把 extent 升级成 standalone complete allocation。

```text
CHandleObject
└─ CDataMgr
   └─ CEnvEffect (base extent 0x20)
      ├─ CFog (0xD4)              [WorldFrame+0x334]
      └─ CLight (0xDC)            [WorldFrame+0x340 exact base instance]
         └─ COmniLight (0x104)    [独立 factory；不是 +0x340 的动态类]
```

## 4. `CShowable -> CWorldObjects -> {CDoodads, CBlightPuffs}`

### 4.1 `CShowable`

- vftable `0x6FA59AAC`，6 槽；COL `0x6FACBC04`。
- TypeDescriptor `0x6FBB4448`，装饰名 `.?AVCShowable@@`。
- CHD `0x6FACBC18`：`0, 0, 1, 0x6FACBC28`。
- 唯一 BCD `0x6FACBC30`：PMD `(0,-1,0)`。
- 构造器 `0x6F74A950` 写 vptr、`+4=1`、`+8=0`；`CWorldObjects` 从 `+0x0C` 开始初始化，因此大小 `0x0C`。

### 4.2 `CWorldObjects`

- vftable `0x6FA59AC8`，103 槽；COL `0x6FACBD24`。
- TypeDescriptor `0x6FBB4478`，装饰名 `.?AVCWorldObjects@@`。
- CHD `0x6FACBCB8`：`0, 0, 2, 0x6FACBCC8`。
- BCD：self `0x6FACBC9C`，`CShowable @ +0` 为 `0x6FACBC30`。
- 构造器 `0x6F74A970` 安装该 vtable；两个已证派生构造器都从 `+0xF4` 开始写派生字段，所以精确基类大小 `0xF4`。

### 4.3 `CDoodads`

- vftable `0x6FA59C7C`，103 槽；COL `0x6FACBC4C`。
- TypeDescriptor `0x6FBB4460`，装饰名 `.?AVCDoodads@@`。
- CHD `0x6FACBC60`：`0, 0, 3, 0x6FACBC70`。
- BCD 顺序：self `0x6FACBC80`、`CWorldObjects` `0x6FACBC9C`、`CShowable` `0x6FACBC30`；全部 PMD `(0,-1,0)`。
- `CDoodads_EnsureSingleton @ 0x6F770FE0` 在 `0x6F771015` 分配 `0x150` 后直接调用构造器，故大小 `0x150`。

### 4.4 `CBlightPuffs`

- vftable `0x6FA59E48`，103 槽；COL `0x6FACBCD4`。
- TypeDescriptor `0x6FBB4494`，装饰名 `.?AVCBlightPuffs@@`。
- CHD `0x6FACBCE8`：`0, 0, 3, 0x6FACBCF8`。
- BCD 顺序：self `0x6FACBD08`、`CWorldObjects`、`CShowable`；全部 PMD `(0,-1,0)`。
- owner 初始化函数 `0x6F7296F0` 在 `0x6F729737` 分配 `0xF8`，`0x6F729755` 直接调用 `CBlightPuffs_Ctor`，故大小 `0xF8`。

## 5. 独立的 `CClippable -> CWorldObjectsClippable`

### 5.1 `CClippable`

- vftable `0x6FA59E1C`，10 槽；COL `0x6FACBB74`。
- TypeDescriptor `0x6FBB4210`；CHD `0x6FACBB88`，只有 self BCD `0x6FACBBA0`。
- ctor `0x6F74A850` 与 dtor `0x6F74BEA0` 的 raw body 都只写 vptr 后返回；派生 ctor 紧接
  `+0x04` 写首字段。因此 `CClippable` base/complete layout span 与 ABI `sizeof` 为精确
  `0x04`。尚未发现的是 standalone allocation site，不是对象大小。

### 5.2 `CWorldObjectsClippable`

- vftable `0x6FA5A2EC`，10 槽；COL `0x6FACBD38`。
- TypeDescriptor `0x6FBB44B0`，装饰名 `.?AVCWorldObjectsClippable@@`。
- CHD `0x6FACBD4C`：`0, 0, 2, 0x6FACBD5C`。
- BCD：self `0x6FACBD68`、`CClippable @ +0` `0x6FACBBA0`。
- `CWorldObjectsClippable_Clone @ 0x6F752220` 唯一显式分配该类型，在 `0x6F752230` 请求 `0x1C`，随后调用构造器；精确大小 `0x1C`。

这条 RTTI 链与 `CWorldObjects` 没有共同 BCD。旧文档把相邻的 `CClippable` COL/vtable 误当作 `CDoodads` 次基类，已经在 [冲突审计](conflicts.md) 标为 Contradicted。

## 6. `CDoodads` 已证组合对象

`CDoodads+0x118` 是内嵌
`TSHashTable<CDoodads::ModelColorHash,HASHKEY_STRI>`，不是未知 heap owner：

| 项 | 地址/证据 |
|---|---|
| vftable | `0x6FA59C68`，4 槽：`0x6F755910/0x6F755AD0/0x6F74CC10/0x6F751730` |
| COL | `0x6FACBBBC` |
| TypeDescriptor | `0x6FBB4230`，装饰名 `.?AV?$TSHashTable@UModelColorHash@CDoodads@@VHASHKEY_STRI@@@@` |
| ctor | `CDoodads_ModelColorHashTable_Ctor @ 0x6F74A640` 写 vptr并初始化至 `+0x24`；CDoodads ctor 以 `ECX=this+0x118` 调用 |
| span | 精确 `0x28`；下一 CDoodads 字段从 `+0x140` 开始 |
| dtor | 先 `0x6F755840(arg=0)`，再 `CDoodads_ModelColorHashTable_Dtor @ 0x6F74BC80`，随后 base dtor |

`CDoodads+0xF4/+0xF8` 则是共享 lazy globals 返回的 borrowed `CDoodadDB*` 与
`CDestructableDB*`；它们不是内嵌基类，也不由 CDoodads dtor 释放。

## 7. embedded 与 stage16/18/21 关联类 inventory（不是本类族派生）

下列类型由 stage caller、RTTI TD/vtable、分配尺寸和字段 dataflow 共同闭合。它们是
`CWorldFrameWar3` 的消费者/发布对象或全局 owner，不应画进 `CWorldObjects` 继承树。

| 类型 | RTTI/vtable/大小证据 | 与本任务类族的关系 |
|---|---|---|
| `CCinematicFilter` | COL `0x6FA874AC`；TD `0x6FB8E0FC = .?AVCCinematicFilter@@`；hierarchy 仅 self BCD；exact one-slot vtable `0x6F98ED34`；WorldFrame 相邻字段证明 size `0xA4` | `CWorldFrameWar3+0x254..+0x2F7` 的 embedded composed object，不是 WorldFrame/CWorldObjects 派生类 |
| `CUnit` | TD `0x6FBADC1C = .?AVCUnit@@`；vtable `0x6FA4A704`；ctor `0x6F643580` 初始化 `+0x16C/+0x170=-1` | stage16 四个 `CAgentPtr<CUnit>` bucket 的精确元素类；本批未宣称完整对象大小 |
| `CBuildFrame` | primary/secondary vtable `0x6F9955B0/0x6F995698`；primary COL `0x6FA899E0`；TD `0x6FB8EDA4 = .?AVCBuildFrame@@`；allocation size `0x19C` | `CWorldFrameWar3+0x250` 的 borrowed active publication |
| `CPlacementBox` | vtable `0x6F98E074`；TD `0x6FB8DD20`；allocation size `0xC0` | `CBuildFrame+0x190` owned stage18 visual object |
| `CConstructUI` | vtable `0x6F994E10`；TD `0x6FB8E498`；allocation size `0x50` | `CBuildFrame+0x18C` active construct UI，生命周期由 build/global registry 协调 |
| `CTextTagManager` | vtable `0x6FA67A50`；TD `0x6FBB7868 = .?AVCTextTagManager@@`；allocation/embedded span `0x88` | stage21 有 static-root owned 与 `CGameState+0x2C8` embedded 两个不同实例 |
| `TerrainImage` | TD `0x6FBB3B48 = .?AUTerrainImage@@`；无 vtable；registration/iteration stride `0xA0` | `CWorldFrameWar3+0x300` 仅保存 CTerrain registry index，不证明拥有 record |
| `CGameWar3` | vtable `0x6F965D58`；TD `0x6FB82BA8`；allocation size `0x408` | global `0x6FBE4238` 的精确类，stage21 获取其 `+0x1C CGameState*` |
| `CGameState` | vtable `0x6F974908`；TD `0x6FB88218`；ctor `0x6F262A60` | 内嵌第二个 `CTextTagManager @ +0x2C8`；本批不外推完整大小 |

stage16 final helper 还通过 unknown global root 精确获得 `NIpse::CAcceleratorMap* @ root+0x23C`
和 `NIpse::CLrPathingAcc* @ root+0x250`；两者 allocator strings/vtables 已证，但 global root
自身 RTTI 类未闭合，故不把它填成任何已知 World/CUnit 类型。

## 8. world-group consumer 的 `CSprite` RTTI 族

`CWorldFrameWar3` 三个 group 的 record `+0` 强持有的是下列运行时族；它们不是
`CWorldObjects/AUWOModel` 的派生对象：

| 类型 | COL | TypeDescriptor / raw name | vftable | 完整大小 |
|---|---:|---|---:|---:|
| `CSprite` | `0x6FA74360` | `0x6FB81928`, `.?AVCSprite@@` | `0x6F96467C` | base extent `0x64`；standalone alloc Unknown |
| `CSpriteMini_` | `0x6FA743B0` | `0x6FB817AC`, `.?AVCSpriteMini_@@` | `0x6F9646F4` | `0x9C` |
| `CSpriteUber_` | `0x6FA7445C` | `0x6FB81790`, `.?AVCSpriteUber_@@` | `0x6F9647BC` | `0x1D4` |
| `TAllocatedHandleObjectLeaf<CSpriteMini_...>` | `0x6FA74404` | `0x6FB81940`, `.?AV?$TAllocatedHandleObjectLeaf@VCSpriteMini_@@$0BAA@@@` | `0x6F9648D4` | `0x9C` |
| `TAllocatedHandleObjectLeaf<CSpriteUber_...>` | `0x6FA744B0` | `0x6FB81988`, `.?AV?$TAllocatedHandleObjectLeaf@VCSpriteUber_@@$0IA@@@` | `0x6F96485C` | `0x1D4` |

五条 raw BCD/PMD 都给出 offset-0 单继承：

```text
CSprite -> CDataMgr -> CHandleObject
CSpriteMini_ -> CSprite -> CDataMgr -> CHandleObject
CSpriteUber_ -> CSprite -> CDataMgr -> CHandleObject
pooled leaf -> corresponding Mini/Uber -> CSprite -> CDataMgr -> CHandleObject
```

ctors 为 `0x6F180710/0x6F180770/0x6F180800`；pool wrapper ctors 为
`0x6F180170/0x6F180190`。pool init `0x6F005440/0x6F005460` 分别以 element
`0x9C/0x1D4`、capacity `0x100/0x80` 初始化 globals `0x6FBE3D9C/0x6FBE3D88`。
这些分配尺寸证明派生/leaf complete size；`CSprite` 只证明到下一个派生字段前的 base extent，
不能伪造 standalone complete allocation。

`AUWOModel` TD `0x6FBB42C4` 是非多态 record descriptor；raw dword scan 未发现指向它的
RTTI/COL 引用。它仍可能经未闭合业务链创建或绑定某个 CSprite，但这种 producer 关系为 Unknown，
不能把两种记录布局合并。

## 9. 尚未证明

- 未发现 `CWorldObjects` 除 `CDoodads`、`CBlightPuffs` 外的 RTTI 派生类；这只表示当前 RTTI catalog 中未命中，不能证明运行时不存在无 RTTI/组合包装。
- `CWorldObjectsClippable` 的初始实例来源可能包含内联构造或序列化路径；当前显式构造 xref 只有 Clone，来源链仍 Unknown。
- `CWorldFrameWar3` 的主基类共享 offset 0 不代表每个祖先没有自身未知字段；精确分界仍需各基类构造/字段访问补证。
