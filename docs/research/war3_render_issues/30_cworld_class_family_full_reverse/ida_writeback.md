# IDA 增量写回日志

## 1. 数据库与规则

- 数据库：`E:\Work\War3\Game.dll.i64`
- preferred image base：`0x6F000000`
- 目标：Warcraft III 1.27a `Game.dll`，MD5
  `267861A0DFD416DBAD13E7EE3EC7794A`
- 每个地址先读当前 name/type/comment；已有更精确内容时保留，只追加不冲突信息。
- 不写 binary patch；只写 name、type、comment/type declaration。
- 每批写回后重新读回，并调用 `ida_loader.save_database(..., 0xFFFFFFFF)`。

任务开始时发现已有 IDA 进程和数据库会话，因此复用了现有会话；没有再启动第二实例，也没有
关闭用户原有进程。

## 2. 批次 1：类族构析与 vtable 锚

### 2.1 写回前核验

逐项读取了以下地址原 name、function type、repeatable/non-repeatable comment；对已有有意义名称
执行保留或增量更名，未覆盖更精确类型。先声明 opaque 类名，避免用尚未闭合的字段布局伪装成
完整 struct。

### 2.2 最终读回结果

| VA | 最终名称 | 类型/注释重点 | 状态 |
|---:|---|---|---|
| `0x6F74A950` | `CShowable_Ctor` | `__thiscall(CShowable*)` | verified |
| `0x6F74BF60` | `CShowable_Dtor` | `__thiscall(CShowable*)` | verified |
| `0x6F74CCD0` | `CShowable_ScalarDeletingDtor` | flags 栈参 | verified |
| `0x6F74A970` | `CWorldObjects_Ctor` | 批次 1 写为 `this, void*`；批次 3 已由全部 xref 纠正为 `this, CTerrain*` | name verified / type superseded |
| `0x6F74BF70` | `CWorldObjects_Dtor` | owned arrays/entries | verified |
| `0x6F74CD00` | `CWorldObjects_ScalarDeletingDtor` | vslot0 | verified |
| `0x6F74AB70` | `CWorldObjectsClippable_Ctor` | 批次 1 写为 `this, void*`；后续 ASM 证明参数是 enum-like `uint32_t`，列入批次 2 类型纠正 | name verified / type superseded |
| `0x6F74C0B0` | `CWorldObjectsClippable_Dtor` | `0xB4`-stride array owner | verified |
| `0x6F74CD30` | `CWorldObjectsClippable_ScalarDeletingDtor` | vslot0 | verified |
| `0x6F752220` | `CWorldObjectsClippable_Clone` | alloc `0x1C`, deep copy | verified |
| `0x6F74A860` | `CDoodads_Ctor` | derives CWorldObjects | verified |
| `0x6F74BEB0` | `CDoodads_Dtor` | derived cleanup then base | verified |
| `0x6F74CCA0` | `CDoodads_ScalarDeletingDtor` | vslot0 | verified |
| `0x6F770FE0` | `CDoodads_EnsureSingleton` | alloc `0x150` | verified |
| `0x6F74A7E0` | `CBlightPuffs_Ctor` | `Blight/PuffDuration` | verified |
| `0x6F74CC40` | `CBlightPuffs_ScalarDeletingDtor` | vslot0 | verified |
| `0x6FBEE15C` | `g_CDoodadsSingleton` | `CDoodads*` | verified |

并给 `CWorldObjects @ 0x6FA59AC8`、`CDoodads @ 0x6FA59C7C`、
`CBlightPuffs @ 0x6FA59E48`、`CWorldObjectsClippable @ 0x6FA5A2EC` 的 vtable 锚追加了
RTTI/槽数/继承边界注释。所有名称、类型和注释均逐项读回。

### 2.3 保存结果

`ida_loader.save_database(r"E:\Work\War3\Game.dll.i64", 0xFFFFFFFF)` 返回 `True`；保存后
文件大小 `250,555,602` bytes，mtime 已推进。没有 build/deploy、没有启动 War3、没有运行
AutoTest。

## 3. 批次 2：WorldFrame canonical 名与历史歧义清理

### 3.1 read-before-write 发现

- ctor/dtor/scalar deleting dtor 仍是 `sub_*`，ctor 自动类型把唯一栈参写成无语义 `size_t`。
- RenderScene/Dispatcher/RenderGroup 使用历史 `CWorld_*` 名；RenderGroup 普通注释把三个 group
  猜成 doodad/unit/effect，repeatable GPU 注释则已经明确说该 taxonomy 未由 ASM 证明。
- `CWorldObjectsClippable_Ctor/Clone` 的首批注释把 `+0x04` 写成 `ownerContext`；后续 vslot6
  ASM 证明它只识别值 1/2，是 enum-like kind/mode。

### 3.2 写回并读回

| VA | 最终名称 | 最终 ABI/更正 | 状态 |
|---:|---|---|---|
| `0x6F35EFB0` | `CWorldFrameWar3_Ctor` | `this, void* gameUiOwner`，`retn 4` | verified |
| `0x6F3602F0` | `CWorldFrameWar3_Dtor` | `this`，普通 `retn` | verified |
| `0x6F361130` | `CWorldFrameWar3_ScalarDeletingDtor` | `this, uint32 flags`，`retn 4` | verified |
| `0x6F3681C0` | `CWorldFrameWar3_RenderScene` | `void __thiscall(this)` | verified |
| `0x6F363020` | `CWorldFrameWar3_DispatchStage` | `this + stageId/renderMode/categoryMask/activeQueue`，`retn 0x10` | verified |
| `0x6F368E30` | `CWorldFrameWar3_RenderWorldGroup` | `this + groupIdx`，`retn 4` | verified |
| `0x6F74AB70` | `CWorldObjectsClippable_Ctor` | 第二参纠正为 `unsigned int kindOrMode` | verified |
| `0x6F752220` | `CWorldObjectsClippable_Clone` | 注释纠正为复制 enum-like `+0x04` | verified |

普通与 repeatable comments 都保留了原 GPU producer/flush 精确内容，同时删除了不受支持的 group
taxonomy 和泛化 `CWorld*` 类型。逐项读回 name/type/address comment/function comment 成功。

保存调用返回 `True`；该次文件大小为 `250,555,634` bytes。

## 4. 批次 3：`CTerrain*` xref 闭合

写回前重新读取 `CWorldObjects/CDoodads/CBlightPuffs` 三个 ctor 的名称、类型和两类 comment。
全二进制 direct-call xref 证明 base ctor 唯一参数是 `CTerrain*`：CDoodads singleton 取得 lazy
global CTerrain，BlightPuffs 在 CTerrain method 中传入 `this`。

三函数最终类型分别为：

```text
CWorldObjects* __thiscall(CWorldObjects*, CTerrain*)
CDoodads*      __thiscall(CDoodads*, CTerrain*)
CBlightPuffs*  __thiscall(CBlightPuffs*, CTerrain*)
```

comments 同步记录 `+0x94 CTerrain*`、Doodads 两个 borrowed DB 与 embedded hash table、Blight
整数 `PuffDuration`。三项类型/注释均已读回。最终再次保存返回 `True`，IDB 大小
`250,555,659` bytes。

该时点的 `WorldObjectEntry_Render` 中性名已在第18批由 `CSprite` RTTI、raw vtable 与 caller
load chain 正式 supersede；保留本段只记录历史 read-before-write 决策。整个三批次没有 binary
patch、没有 build/deploy、没有启动 War3、没有运行 AutoTest。

## 5. 批次 4：组合对象与字段存取点

写回前分别读取两个目标函数的 name/type/comment，并逐项读取七条字段指令的两类 comment；
这些地址此前均无注释。真实 ASM 证明 `0x6F74A640` 首先写入专用 vtable `0x6FA59C68` 并初始化
至 `this+0x24`，`0x6F74BC80` 对称恢复同表并释放内部 owner，因此增量写回：

| VA | 最终名称/注释 | 读回状态 |
|---:|---|---|
| `0x6F74A640` | `CDoodads_ModelColorHashTable_Ctor(CDoodads_ModelColorHashTable*)` | name/type verified |
| `0x6F74BC80` | `CDoodads_ModelColorHashTable_Dtor(CDoodads_ModelColorHashTable*)` | name/type verified |
| `0x6F74AADB` | `CWorldObjects+0x94 = borrowed CTerrain*` | comment verified |
| `0x6F74A8C6/0x6F74A8CC` | `CDoodads+0x118` 形成并构造 `0x28`-byte embedded hash | comments verified |
| `0x6F74A902/0x6F74A912` | `+0xF4 CDoodadDB*` / `+0xF8 CDestructableDB*` borrowed stores | comments verified |
| `0x6F74A931` | `+0x10C` 为 dword config/mode，反对 pointer 解释 | comment verified |
| `0x6F74A82F` | `CBlightPuffs+0xF4` signed `PuffDuration`，单位 Unknown | comment verified |

opaque `CDoodads_ModelColorHashTable` 只用于函数 ABI；没有用未闭合的内部字段伪造完整 struct。
所有 name/type/comment 已再次读回，随后 `idc.save_database` 返回 `True`；IDB 大小
`250,555,704` bytes。IDA 的旧 stack-frame member `ownerContext` 无法通过 stack/local rename
API 覆盖，但函数 tinfo 与 Hex-Rays 参数均已读回为 `CTerrain* terrain`，故不删除重建 frame，
避免损伤既有栈分析。

整个四批次没有 binary patch、没有 build/deploy、没有启动 War3、没有运行 AutoTest。

## 6. 批次 5：`AUWOModel` producer/consumer 锚

在声明 opaque `AUWOModel` 前先确认 IDB 中不存在同名 struct；随后逐函数读取原
name/type/comment，并逐条读取 18 个目标字段指令的两类 comment。写回只覆盖由 raw ASM
直接闭合的 ABI/语义：

| VA | 最终名称 | 精确 ABI/证据 |
|---:|---|---|
| `0x6F75AC10` | `CWorldObjects_SubmitModelsToRenderQueue` | `this, uint32 renderClassBit`；`retn 4`；`+0x58/+0x164` 实际进入 RenderQueue |
| `0x6F75AC00` | `CDoodads_SubmitModelsToRenderQueue_Thunk` | 同 ABI，direct tail 到 base target |
| `0x6F75ABF0` | `CBlightPuffs_SubmitModelsToRenderQueue_Thunk` | 同 ABI，direct tail 到 base target |
| `0x6F7503B0` | `CDoodads_ResolveModelColorRaw` | `this, AUWOModel*`；两个 DB/embedded hash/fallback 写 `entry+0x9C` |
| `0x6F758AD0` | `CBlightPuffs_AccumulatePuffTimeMs` | `this, unknown0, unknown1, float deltaSeconds`；`retn 0xC`；仅第三参数参与 `*1000` |

指令 comments 同步记录了 `0x188` stride、`+0x84/+0x87` 重叠 flags 视图、`+0x58` 主提交、
`+0x160/+0x164` secondary records、`+0x148` 整数毫秒累计、`+0x150` birth/death 状态机，
以及注册链读取的 `+0x04/+0x0C/+0x18/+0xA4/+0xA8`。所有 name/type/comment 已读回；
`idc.save_database` 返回 `True`，IDB 大小 `250,555,730` bytes。

整个五批次没有 binary patch、没有 build/deploy、没有启动 War3、没有运行 AutoTest。

## 7. 批次 6：embedded `ModelColorHash` 全 vtable

逐槽读取 `0x6FA59C68` 的四个 raw target、所有函数出口、data xref 和既有 name/type/comment；
四函数原先均为 `sub_*` 且无有效类型/注释。另读 `0x6F74BAD0`，其 allocator type string 与
字段释放证明这是 `AUModelColorHash` node dtor。增量写回并读回：

| VA | 最终名称 | ABI/直接行为 |
|---:|---|---|
| `0x6F74BAD0` | `AUModelColorHash_Dtor` | 释放 node `+0x14` owner，unlink 两组 link；不 free node 本体 |
| `0x6F755910` | `CDoodads_ModelColorHashTable_DeleteNode` | `this,node`；ECX 未消费；`retn 4` |
| `0x6F755AD0` | `CDoodads_ModelColorHashTable_AllocateAndLinkNode` | `this,linkAnchor,payloadBytes,flags`；分配 `+0x1C` header；`retn 0xC` |
| `0x6F74CC10` | `CDoodads_ModelColorHashTable_ScalarDeletingDtor` | flags bit0 free；返回 this；`retn 4` |
| `0x6F751730` | `CDoodads_ModelColorHashTable_Clear` | 无栈参；销毁 nodes 并复位 table |

opaque `AUModelColorHash` 只承载已证 ABI；内部 partial layout 单独记入字段分卷。五个函数的
name/type/comment 均读回成功，`idc.save_database` 返回 `True`，IDB 大小 `250,555,763` bytes。

整个六批次没有 binary patch、没有 build/deploy、没有启动 War3、没有运行 AutoTest。

## 8. 批次 7：`CDoodads+0x10C` 从 Unknown 升级

继续追到 `GetDebugToggleFlag` 的唯一数组读取和 debug description table；index 6 的 raw string
精确为 `"Destructibles use height map"`。`CDoodads` slot37 重新读取 index 6，只有与 `+0x10C`
不同时才更新并用两个零参数触发 model refresh。因此该字段不再只是“config/mode scalar”：

- `0x6F703E20` -> `GetDebugToggleFlag(unsigned index)`，`__fastcall`；
- `0x6FB6A4D0` -> `g_DebugToggleFlags`，保留 IDA 已有 `int[]` type；
- `0x6F758B30` -> `CDoodads_RefreshDestructibleHeightMapToggle(CDoodads*)`；
- 四条 compare/store/call 指令追加 exact comment。

全部 name/type/comment 已先读后写并读回；保存返回 `True`，IDB 大小仍为 `250,555,763`
bytes（数据库压缩后大小未变化）。整个七批次没有 binary patch、没有 build/deploy、没有启动
War3、没有运行 AutoTest。

## 9. 批次 8：WorldFrame slots34..56 与 `CLayoutFrame@+0xB4`

逐函数 CFG 和真实尾部确认了 primary 后 23 槽以及 secondary 9 槽。写前搜索现有 local types，
读取每个目标的 name/type/ordinary comment/repeatable comment；只新增缺失的 opaque
`CFrame/CLayoutFrame/RectF/UnknownLayoutContext`，没有覆盖更精确结构。随后定名并应用精确
ABI：

- `CWorldFrameWar3_OnHoverEnter/OnHoverLeave @ 0x6F3679A0/0x6F367A90`；
- `CFrame_QueryNormalizedLayoutRect @ 0x6F0A1F80`，全部出口 `retn 0xC`；
- `CFrame_DefaultNoLayoutOffset/WriteDefaultTripleSentinel/SetHiddenBit0/ClearHiddenBit0/
  AccumulateLayoutOffset`；
- secondary 的 rect-changing、resolve/commit、validate/commit、try-get rect、set scale、scaled
  width/height、owner flag-bit3 query；
- `0x6F360B77` 定名为 `CWorldFrameWar3_CLayoutFrame_ScalarDeletingDtorThunk`，注释保留原始
  `sub ecx,0xB4; jmp 0x6F361130` adjustor 合同。

全部写回逐项读回一致；保存返回 `True`，IDB 大小 `250,555,874` bytes。

## 10. 批次 9：event forwarding、focus 生命周期与 inherited property propagation

写前读取 19 个目标。旧状态只有 `sub_*`、`nullsub_*` 或错误库签名 `unknown_libname_*`，全部
type/comment 为空；因此不存在更精确的用户内容需要保留。写回包括：

- event IDs `0x400500C9..CD` 与 `0x40060064..68` 的十个机械 forwarder；所有注释明确
  `[vt+0x10]` 是 byte offset、zero-based vslot4，并保持事件自然语言名 Unknown；
- `CWorldFrameWar3_HandleEvent400500CA_WithUiFallback`；
- `Frame_OnEffectiveActive_AttachAUKeyboardFocus` 与
  `Frame_OnEffectiveInactive_DetachAUKeyboardFocus`；
- `CWorldFrameWar3_VirtualTeardownResources`；
- 三个 global-current/first-realization no-op 槽，注释将 global 业务身份保持 Unknown；
- `Frame_PropagateInheritedPtr30/Float34/Ptr2C`。

19 个 name/type/ordinary+repeatable comment 全部写入成功并立即读回一致；
`ida_loader.save_database(r"E:\\Work\\War3\\Game.dll.i64", 0xFFFFFFFF)` 返回 `True`，
保存后大小仍为 `250,555,874` bytes。整个九批次没有 binary patch、没有 build/deploy、没有
启动 War3、没有运行 AutoTest。

## 11. 批次 10：WorldFrame primary slots0..14 与基表 owner 纠正

写前读取全部 15 槽的 name/type/comment。slot1 与 slot12 已有本任务更精确 canonical 内容，
保持不动；其余 13 个目标中，slot11 有一段历史长注释。该注释的 camera/frustum/visibility/
fog-shadow/particle-ribbon 调用入口保留为新注释的可证摘要，但删除未经 ASM 证明的
“group0/1/2=装饰物/单位/飞行” taxonomy。其余旧型均为无类型或错误自动型。

新增/纠正：

- `TRefCnt_DeleteSelf`；
- `CObserver_RegisterEventRecipient`，把旧 `__stdcall` 改成消费 ECX、三栈参的
  `__thiscall`；
- `CWorldFrameWar3_ForwardObserverEventToLink20`、`CFrame_DispatchEventWithSender`、
  `CObserver_DispatchEvent`；
- 三个只按地址标识、业务名保持 Unknown 的 global-channel selector；
- `CFrame_RunRecursiveFramePass`；
- `CWorldFrameWar3_HitTestMouseEvent`、
  `CWorldFrameWar3_UpdateWorldFrameAndPreparePasses`、
  `CFrame_PostRenderCategory4Cleanup`、`CWorldFrameWar3_RouteWorldClickEvent`。

写回前只补了 opaque `TRefCnt/CObserver/CMouseEvent` forward declarations；13 个
name/type/ordinary+repeatable comment 全部成功并读回。保存返回 `True`，IDB 大小
`250,555,952` bytes。整个十批次没有 binary patch、没有 build/deploy、没有启动 War3、
没有运行 AutoTest。

## 12. 批次 11：`CLayoutFrame` 精确次对象字段与失效合同

写前逐项读取五个函数的 name/type/ordinary+repeatable comment。`0x6F0BD960` 与
`0x6F0BD7C0` 仅有历史 `SetFrameWidth/Height` 名和把 float 误写为 int 的自动类型；其余三个
函数仍为 `sub_*` 且无类型/注释。没有发现需要保留的更精确人工内容。

真实 ctor/setter/commit ASM 闭合并写回：

- `CLayoutFrame_Ctor @ 0x6F0BC420`：构造精确 `0x68` 字节次对象，provider 数量为 9，清
  provider grid、solver bits、cached rect/valid、unscaled width/height 与 publication latch，
  scale 初始化为 `1.0f`；
- `CLayoutFrame_SetUnscaledWidth @ 0x6F0BD960` 和
  `CLayoutFrame_SetUnscaledHeight @ 0x6F0BD7C0`：参数纠正为 float，分别写次对象
  `+0x58/+0x5C`，随后通知/relink；
- `CLayoutFrame_SetScaleInvalidateRect @ 0x6F0BD7E0`：scale 实质变化时先清
  `cachedRectValid @ +0x54`，再写 scale 并通知；rect bytes 不会随失效清零；
- `CLayoutFrame_CommitCachedRect @ 0x6F0BD920`：设置 valid、调用 rectangle-changing
  virtual notification、复制 16-byte `RectF` 并按条件发布，返回当前 valid。

五个 name/type/ordinary+repeatable comment 全部写入成功并立即读回一致；保存返回 `True`，
IDB 大小 `250,637,872` bytes。整个十一批次没有 binary patch、没有 build/deploy、没有启动
War3、没有运行 AutoTest。

## 13. 批次 12：WorldObjects family slots78..102

先读取三张 vtable 的 25 槽 target，得到 39 个唯一函数；随后对每个函数读取既有
name/type/ordinary+repeatable comment、完整 normal exit、vtable data xref，并读取原始函数
bytes（大函数记录 head/tail）与真实 x86 ASM。除 `CBlightPuffs` slot86 已在早期批次定名外，
旧内容均为 `sub/nullsub`、无类型或 IDA 自动产生的错误 `__stdcall/__userpurge`，且无人工注释。

写回采用保守命名：有稳定数据流的函数定名为
`CWorldObjects_BuildModelTransform3x4`、`CWorldObjects_ComputeModelBounds`、
`{CWorldObjects,CDoodads}_QueryTerrainBoundedContext`、
`CDoodads_VisitAllModelEntriesVSlot98`；缺 caller/business 证据的常量、flag、DB helper 与返回码
全部保留 `VSlotNN` 和 `Unknown`，没有从相邻地址猜业务名。39 个目标覆盖：

- slots78..80 的 debug-toggle/model-bit 返回码与量化控制流；
- slot81 的 `CTerrain* @ +0x94` 采样、48-byte transform 输出和 model `+0x84` bit `0x10000`；
- slot82 的 position/scale/bounds 两个 vec3 输出；
- slots84/85、96/97 的 model bit0 与 borrowed DB 分派；
- slots87/88 的 bounded/temporary query context；
- slot89 的 model `+0x84` bit `0x4000` mutation bracket；
- slots91..95、98、102 的精确 no-op/constant 边界，以及 slot98 派生类的全 entry sweep；
- slots100..102 的两参数 ABI、`this+0xD0` 条件返回和条件 helper side effect。

第一步 name/ordinary+repeatable comment 全部成功；因无函数名的 C declaration 形式被当前 IDA
parser 拒绝，初次 type apply 全部返回 false，旧类型未被清除。随后为每个 declaration 加入当前
canonical function name，经 `parse_decl` 和 `apply_tinfo(TINFO_DEFINITE)` 修正；39/39 均
parsed/applied/readback 成功。最终再次保存返回 `True`，IDB 大小 `251,088,432` bytes。整个
十二批次没有 binary patch、没有 build/deploy、没有启动 War3、没有运行 AutoTest。

## 14. 批次 13：WorldObjects family slots28..52 与缺失函数边界

先读取三张 vtable 的 slots28..52 raw target，得到 39 个唯一既有函数；逐项读取 name、type、
ordinary/repeatable item comment、函数边界、normal exit、data/code xref 和 raw head/tail bytes。
`CDoodads_RefreshDestructibleHeightMapToggle @ 0x6F758B30` 已有更精确 name/type/ordinary comment，
故完全保留，只补等价 repeatable comment；其余旧内容均为 `sub/nullsub`、缺类型或自动误型。

真实 caller/ASM 本批闭合：

- slots28..37 的 entry DB flags 初始化、slot29→30 tail、辅助记录移除、`gg_dest_*` probe、
  destroy/activation/object-file completion hooks 与既有 height-map refresh；
- slot38 的 `"stand"` 默认 animation output，slots40/41/47 的 display/pathing/variation 路径，
  slots48..52 的 static-shadow/selection-image 配置；
- slots43..46 只写精确 ABI/常量返回/caller partition，未杜撰接口业务名；
- 旧数据库漏识别的 `0x6F74DA00` 已由原始 prologue、security-cookie epilogue、全部 CFG 和
  `0x6F74DB21 retn 4` 建成函数，命名为
  `CWorldObjects_RegisterSelectionCircleImageForEntry`；它注册 type1 terrain image、写
  `AUWOModel+0x8C`、调用 slot52，再绑定 `SelectionCircle/ColorFriend`。

39 个 vtable target 均已完整注释：其中 38 个获得 conservative name/type/两份入口 item
comment，既有 slot37 保留更精确 name/type/ordinary comment 并补 repeatable comment；新增函数
也已写回并读回。IDA 将其规范化为 `[0x6F74DA00,0x6F74DB24)`：`0x6F74DB21` 是最后有效 `retn 4`，
`0x6F74DB24` 前的三字节是 `CC` padding，因此不是函数体证据冲突。保存成功后 IDB 大小仍为
`251,088,432` bytes。整个十三批次没有 binary patch、没有 build/deploy、没有启动 War3、
没有运行 AutoTest。

## 15. 批次 14：`AUWOModel` 构析/复制与 terrain-handle 生命周期

写前逐项读取 10 个函数的 name/type/ordinary+repeatable item comment、函数边界与 raw head
bytes；对关键 helper 又读取全部 xref 和真实 ASM。已有 `0x6F74DA00` 的名称/type/注释与
`0x6F758300` 的自动型保留，未用新推断覆盖；其余目标均为 `sub_*`、无注释，ctor/dtor 的旧
自动型分别把 thiscall 错成单 dword/`__cdecl`。

本批写回并读回：

- `AUWOModel_Ctor @ 0x6F74B1A0`、`AUWOModel_Dtor @ 0x6F74C200` 与
  `AUWOModel_CopyFrom @ 0x6F750BC0` 的精确 `__thiscall` ABI；copy 路径 raw 搬运四个 handle，
  同时对 wrapper/nested owners 做 acquire/deep copy；
- `CDoodads_ToggleEntryTerrainAuxHandle @ 0x6F74D730`，只把 feature/domain 保留 Unknown；
- `CDoodads_Update{SelectionCircle,StaticShadow,Emitter}ImagePosition @
  0x6F757D10/0x6F757D40/0x6F757D80`；
- `CDoodads_UnregisterSelectionCircleImageForEntry @ 0x6F75AB60`；
- `sub_6F758300` 入口注释记录 raw 16-byte all-`FF` 常量对 entry `+0x88..+0x94` 的初始化；
- `0x6F74DA00` 原注释完整保留并追加 reachability hard caveat：IDA code/data xref、原始 `.text`
  direct-call 与全 PE raw RVA/VA pointer scan 均为 0。

8 个新 name/type 与 10 个函数的 ordinary/repeatable item comment 均读回一致。第一次误用
`idc.save_database(int)` 的 Python overload 在保存前抛出 `TypeError`，没有回滚 live IDB 内容；
随后立即重新读回全部目标，并以
`ida_loader.save_database(E:\\Work\\War3\\Game.dll.i64, 0xFFFFFFFF)` 保存成功。IDB 大小仍为
`251,088,432` bytes。整个十四批次没有 binary patch、没有 build/deploy、没有启动 War3、
没有运行 AutoTest。

## 16. 批次 15：stage16/18/21 owner、global selector 与关联 RTTI

写前读取 dispatcher 与 15 个相关入口的现有 name/type/ordinary+repeatable comment、raw head
bytes、函数边界和 xref；再由真实指令复核 stage16 feature branches、stage18 gates、stage21
顺序、两个 TextTag tail 和 TerrainImage stride。旧 `CWorld_TerrainShadow_Dispatch` 名/type 把
ECX selector 误作 this，故属于需要纠正的较低精度内容；其余多为 `sub_*`/自动误型。

本批增量写回：

- `CWorldFrameWar3_RenderStage16UnitBucket` 与四个
  `CWorldFrameWar3_Stage16UnitBucket{0..3}Callback`；callback 按 RTTI 精确为
  `int __fastcall(CUnit*,void*)`；
- `RenderStage16PathingDebugOverlay`，注释限定 global root 类 Unknown，仅把两个已证 NIpse
  owner 写入；
- `CBuildFrame_RenderPlacementAndConstructVisuals`；
- 将 `0x6F76F060` 纠正为 `RenderGlobalPass_DispatchBySelector(int)`，并给 case13/thunk 写入
  static-root TextTag 证据；
- `StaticRoot_GetOrCreateTextTagManager`、`RenderTerrainImageByIndex`、
  `CTerrain_RenderTerrainImageByIndex`、`CGameWar3_GetOrCreateGameState`、
  `CGameState_RenderEmbeddedTextTags`、`CTextTagManager_RenderPass`；
- 只为 IDB 中原先不存在的 `CUnit/CBuildFrame/CTextTagManager/CGameWar3/CGameState` 建立 opaque
  forward declarations，未伪造字段。

首次用不含函数名的 `idc.SetType` 形式没有改变旧 type；立即读回发现失败后，没有清除任何类型，
改用带 canonical function name 的 declaration，14/14 apply/readback 成功。全部 name/comment 也
逐项读回，保存返回 `True`。随后一次并行只读 xref 请求曾短暂让既有 IDA 窗口未响应；未写入、
未终止/重启实例，进程自行恢复后由单一轻量 read 验证 MCP 与数据库正常。

## 17. 批次 16：WorldObjects family slots53..77

写前读取 45 个独立 target 的 name/type/ordinary+repeatable comment；全部仅为 `sub_*`、
`nullsub_*`，无 type/comment，因此不存在更精确人工内容被覆盖。三张 vtable、所有正常出口和
caller 参数来源已先闭合；特别排除了把 `CDoodads+0x110/+0x114` direct callbacks 误认作
slots68/69 的历史 false positive。

45 个 target 全部获得 conservative name、精确 `__thiscall`/ST0/stack declaration 与双份注释：

- slots54/55/56/60/68/69/71/74/75 的 DB 字段分别闭合为
  `occH/numVar/tilesetSpecific/selSize/maxScale/minScale/defScale/maxPitch/maxRoll`；
- slots58/59 的 destructable Blight texture/terrain state 与 entry `+0x148`，slots61..63 的
  position/facing/variation change hooks，slot67 的 path-sensitive entry gate；
- slots72/73 的 entry `+0x5C/+0x64` producer-consumer，slots76/77 的
  `radius/visRadius` 同源但不同接口；
- slot57、64..66 以及 slots76/77 的公开 Blizzard 名仍用 `VSlot`/Unknown 表达，没有从相邻槽
  杜撰业务术语。

`readback_count=45`、缺失 type/comment 或仍为 auto-name 的目标 `bad=[]`；
`ida_loader.save_database(E:\\Work\\War3\\Game.dll.i64, 0xFFFFFFFF)` 返回 `True`。保存后 IDB
大小 `251,358,903` bytes。至此十六批均无 binary patch、无 build/deploy、未启动 War3、
未运行 AutoTest。

## 18. 批次 17：`CClippable/CWorldObjectsClippable` 全槽与 `CBlightPuffs` 毫秒生命周期

写前读取 Clippable 两表 20 个 vtable target、两个 ctor/dtor raw byte islands、Puffs ctor、
create/reuse helper 和 slots10/86 的现有 name/type/comments/函数边界。`CClippable` ctor/dtor
在旧 IDB 中只有 code items、未定义函数；两侧 `CC` padding 和下一函数边界分别精确闭合为
`[0x6F74A850,0x6F74A859)`、`[0x6F74BEA0,0x6F74BEA7)`，随后才增量建函数。

本批完成：

- `CClippable_Ctor/Dtor/ScalarDeletingDtor`，并由只写 vptr 的 raw body和派生首字段 `+4`
  把 ABI size 闭合为 `0x4`；
- Clippable/derived slots1..6 的常量/映射/`+0x18` 精确行为，slot7 Clone、slot8 default/
  record centroid、slot9 footprint；业务接口名不足的函数保留 `VSlot`；
- 将旧 Clone 注释从 blanket “deep copy”收窄为 independent outer array + proven nested-owner
  copy construction + raw-region copy；
- `CBlightPuffs_CreateOrReusePuffEntry`、`CBlightPuffs_UpdatePuffLifecycle` 与既有
  `CBlightPuffs_AccumulatePuffTimeMs`，并把 ctor/store comments 中的 duration unit 从 Unknown
  升级为 milliseconds；
- 三条 field-site comment 精确标注 manager `+0xF4`、entry elapsed `+0x148`、duration
  `+0x14C` 的发布/比较合同。

25 个审计入口均有 function/type/ordinary+repeatable comment，`bad=[]`；新建两函数边界、所有
canonical types 和 field comments 均立即读回。保存返回 `True`，IDB 大小
`251,375,314` bytes。至此十七批均无 binary patch、无 build/deploy、未启动 War3、未运行
AutoTest。

## 19. 批次 18：world-group record/owner、`CSprite` RTTI 与 pool-return

写前读取 `0x6F184EE0/0x6F0CAE40/0x6F0CB110/0x6F1836B0/0x6F1836D0`、owner ctor/clear/
accessors 与三条 CSprite ctor 的 name/type/ordinary+repeatable comment。旧内容把
`0x6F184EE0` 写成 `WorldObjectEntry_Render`、ECX 写成 entry，并把 slot5 解释为通用
PreRender；`0x6F0CB110` 又把 record `+0x14` 无条件命名为 ownerHint。真实 ASM/RTTI 证明这些
都是需要纠正的较低精度内容，其余目标为 generic/auto name/type，未覆盖更精确人工结论。

本批先读取并核验：

- `0x6F184EE0` 全部 25 bytes 及仅有的两个 direct call xref；二者都从 stride-`0x18`
  record `+0` 取 `CSprite*`；
- `CSprite/CSpriteMini_/CSpriteUber_` 与两个 pooled leaf 的 COL/TD/单继承 BCD，以及五张
  29 槽 raw dword vtable；
- owner/record ctor、add/acquire、clear/release、final dtor、WorldFrame ctor/dtor field stores；
- refcount `0x6F04F200/0x6F04F1A0` 与 leaf slot1 `0x6F1836B0/D0` 的同步 pool-return ASM。

随后只在原类型不存在时声明 opaque `CSprite/CSpriteMini_/CSpriteUber_`，并建立精确
`WorldGroupRecord` (`0x18`) 与 `WorldGroupRecordOwner` (`0x1C`)；12 个函数完成 conservative
name、精确 `__thiscall` type 和 ordinary/repeatable comment 写回：

- `CSprite_PrepareAndQueueAttachedRenderObject`，正式 supersede `WorldObjectEntry_Render`；
- `WorldGroupRecord_ResetAndReleaseSprite`、`WorldGroupRecordOwner_AddSpriteRef/Ctor/ClearActive/`
  `GetRecords/GetActiveCount`；
- 两个 `TAllocatedHandleObjectLeaf_*_ReleaseToPool`；
- `CSprite/CSpriteMini_/CSpriteUber_` 三条 ctor。

13 个 WorldFrame ctor/dtor/render/帧尾 clear 指令点另加字段与 stop-use comment。readback 显示
12/12 name/type/两类 comment 均为新值，两个 struct size 精确为 `24/28`，旧 stale function
comment 已被替换。`ida_loader.save_database(E:\Work\War3\Game.dll.i64, 0xFFFFFFFF)` 返回
`True`；保存后大小 `251,547,687` bytes，IDA PID `40588` 仍 `Responding=True`。本批无 binary
patch、无 build/deploy、未启动 War3、未运行 AutoTest。

## 20. 批次 19：WorldFrame Rally/Waypoint 与尾部向量

写回前逐个读取 11 个目标的 name/type/ordinary+repeatable comment；旧内容均为 generic/auto
name/type 或空 comment，没有需要保留的更精确人工语义。真实 ctor/dtor/resize/reset ASM 与 raw
allocator descriptor 闭合后，仅在类型不存在时新增：

- `RallyIndicator` (`0x18`)、`WaypointIndicator` (`0x1C`)；
- `RallyIndicatorSmallVector16` (`0x18C`) 与 `Int32SmallVector16` (`0x4C`)；
- 六种 raw-pointer vector 与六种 `CAgentPtr<T>` vector 的 exact `0x10` header；
- 仅作 ABI 锚的 `CItem/CSelectable/CWidget/CGhostImage` forward declarations。

11 个函数写回 conservative name 与精确 ABI，包括
`CWorldFrameWar3_InitRallyIndicators/InitTargetPointConfirm/InitWaypointIndicators/`
`ResetWaypointIndicators`、两个 small-vector grow helper，以及 `CUnit/CItem/CSelectable/CWidget/`
`CGhostImage` 五个 `CAgentPtr` vector destroy helper。九个 ctor/reset/dtor 指令点另写 field/lifetime
comment；`CAgentPtr<CUnit>` helper 的旧泛化名被已证元素类型 supersede。

readback 为 11/11 name/type/两类 comment 一致；全部新 struct size 分别读回为
`0x18/0x1C/0x18C/0x4C/0x10`。保存返回 `True`，IDB 大小 `251,761,774` bytes。本批无 binary
patch、无 build/deploy、未启动 War3、未运行 AutoTest。

## 21. 批次 20：game-context fields 与 embedded `CCinematicFilter`

写前读取 `0x6F3618F0/0x6F36A840/0x6F37F8F0/0x6F386650` 以及 16 个 field/call site 的既有
name/type/comment。前三个已有自动推断的错误 `cdecl/int` type，`0x6F36A840` 无 type，所有目标
comment 为空；没有覆盖更精确人工内容。随后读取：

- `0x6F3618F0` 全部 51 条指令和 7 个 direct caller；
- 尾调用 `0x6F36A840` 全部 42 条指令，确认 Rally/singleton/Waypoint 三段循环；
- `CCinematicFilter` ctor/dtor 的真实正常出口、WorldFrame 构析 call site；
- COL `0x6FA874AC`、TD `0x6FB8E0FC` 和 self-only RTTI hierarchy。

只在 named type 不存在时声明 `struct CCinematicFilter;`，并写回四个函数：

- `CWorldFrameWar3_RefreshGameContextBindings`；
- `CWorldFrameWar3_UpdateIndicatorRuntimeObjectsForOrdinal`；
- `CCinematicFilter_Ctor`；
- `CCinematicFilter_Dtor`。

四个 `__thiscall` type、20 个 ordinary/repeatable evidence comment 均立即读回；function comments
明确区分 Confirmed structural behavior 与 Inferred semantic label。`ida_loader.save_database(path,0)`
返回 `True`，IDB 大小 `251,786,383` bytes。本批无 binary patch、无 build/deploy、未启动 War3、
未运行 AutoTest。

## 22. 批次 21：`CCinematicFilter` one-slot vtable

按 read-before-write 读取 `0x6F38A130` 的自动名、空 type/comment，以及
`0x6F98ED30/34/38` 的 raw dword/comment。连续值为：

```text
0x6F98ED30 = 0x6FA874AC  CCinematicFilter COL
0x6F98ED34 = 0x6F38A130  vslot0
0x6F98ED38 = 0x6FA87564  next CAllianceSlot COL
```

`0x6F38A130` 的 18 条真实指令先调用 non-deleting dtor，flags bit0 且 this 非空时 Storm-free，
返回原 this 并 `retn 4`。因此写回 `CCinematicFilter_ScalarDeletingDtor` 与精确
`CCinematicFilter* __thiscall(CCinematicFilter*, unsigned flags)` type，并给函数、COL cell、
vtable cell 写 ordinary/repeatable comment。name/type/comment 与三 dword 边界全部读回一致。
保存返回 `True`；页压缩后文件大小仍为 `251,786,383` bytes。本批同样无 binary patch、无
build/deploy、未启动 War3、未运行 AutoTest。保存后 IDA PID `40588` 仍为
`Responding=True`，没有启动第二个数据库实例。

## 23. 批次 22：environment family、indicator vectors 与 periodic latches

### 23.1 read-before-write 与 raw evidence inventory

主任务会话已在写前读取目标函数的 name/type/ordinary+repeatable comment，并读取四张表的
COL/TD/CHD/BCA/BCD、raw dword、factory alloc 和 ctor/dtor ASM。需要保留的既有精确内容是
`CFog_ApplyDynamicParams @ 0x6F1911A0` 与 `CFog_SubmitParamsFast @ 0x6F191560` 的人工注释；
`0x6F0E2170` 的 Concurrency `_RefCounter` 自动名与真实 `CHandleObject` vslot 行为冲突，应由
raw ASM 纠正。`CWorldFrameWar3_VirtualTeardownResources @ 0x6F367B40` 旧注释所称
“concrete resource classes Unknown”也已被本批 factory/vptr 证据部分 supersede。

本批 authoritative evidence：

- `CEnvEffect/CFog/CLight/COmniLight` 四组 raw RTTI 与五槽表；factory size 分别为
  `CFog 0xD4`、`CLight 0xDC`、`COmniLight 0x104`，base `CEnvEffect` 只证明 extent `0x20`；
- WorldFrame ctor 对 `+0x334/+0x340` 的 exact `CFog*/CLight*` stores，以及
  `CWorldFrameWar3_InitTerrainZFog @ 0x6F36C390`、DNC helper `0x6F366F10`；
- `CWorldFrameWar3_SetStage0SpriteResource @ 0x6F36D620` 的 `ECX=1` pooled-Uber 创建，
  `+0x348/+0x354/+0x358/+0x35C` 的 path/ref/gate/result 链；
- `+0x360` strong-ref vector 的 allocator descriptor、grow/reallocate/destroy，以及
  `+0x370` TargetIndicator vector 的 `0x18` stride、resize/reallocate/destroy 和 `+0x37C &7`
  cursor；
- `+0x380/+0x384/+0x388/+0x38C` 的 accumulator/latch producer-consumer-clear xrefs，及
  raw threshold bytes `00 00 40 40 == 3.0f`。

### 23.2 增量写回集合与结算状态

本批拟增量写回的 conservative symbols 包括 `CFog_Create/Ctor/Dtor/ScalarDeletingDtor`、
`CLight_Create/Ctor/Dtor/ScalarDeletingDtor`、`COmniLight_Create/Ctor/ScalarDeletingDtor/`
`SubmitDynamicParams`、`CWorldFrameWar3_InitTerrainZFog/InitDncSpriteResources/`
`SetStage0SpriteResource`、两张 vector 的 Destroy/Reallocate/Resize，以及两个共享
`CHandleObject` vslot target。类型声明只建立 exact header：

```text
TargetIndicator                         sizeof 0x18
TargetIndicatorVector                   sizeof 0x0C
CPathingMapIndicatorRefVector           sizeof 0x10
```

其中 `TargetIndicator` 仅命名六个机械槽（sentinel/strongRef/float/state/payload/result），
业务字段仍 Unknown；`+0x344` 不声明成 padding。四张 5 槽表和关键 field instruction sites
只追加原始地址、ABI 与 Confirmed/Inferred 边界，不覆盖上述两条既有 CFog 精确注释。

本节由文档子任务先记录 evidence/write set；它没有连接或修改 IDA。最终 name/type/comment
readback、`save_database` 返回值、数据库文件大小与 IDA PID 状态必须由执行实际写回的主任务
会话在本段后补，未补前不能宣称批次 22 已完成 IDA settlement。本文档子任务同样没有 binary
patch、build/deploy、启动 War3 或运行 AutoTest。
