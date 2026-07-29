# 字段、所有权与生命周期

## 1. 规则

- `Confirmed`：真实 x86 指令给出精确偏移/宽度，并由构造、析构、读写或分配至少两类证据闭合。
- `Inferred`：偏移/宽度已证，但业务名只由调用行为推定。
- `Unknown`：只知道被初始化、复制或释放，不填写猜测名。
- 数组 header 采用四个 dword 的观察布局 `{capacity, count, data, growth}`；若只闭合其中一部分，逐项注明。

## 2. `CShowable`，大小 `0x0C`

| 偏移 | 宽度 | 当前名称 | 构造/析构 | 读写证据 | 状态 |
|---:|---:|---|---|---|---|
| `+0x00` | 4 | vptr | ctor 写 `0x6FA59AAC`；dtor 恢复同表 | 6 槽 vtable | Confirmed |
| `+0x04` | 4 | `UnknownShowState0` | ctor 写 1 | vslot1 条件写；vslot2 直接返回 | 偏移 Confirmed，语义 Inferred |
| `+0x08` | 4 | `UnknownShowState1` | ctor 写 0 | vslot3 条件写；vslot5 与 `+4` 合并判断 | 偏移 Confirmed，语义 Unknown |

不要把 `+4/+8` 直接命名为 `visible/hidden`：vslot1 在 `+8 != 0` 时拒绝更新 `+4`，而 vslot5 返回的是“vslot2 非零且 +8==0”的组合条件；精确状态机名仍需 caller 语义。

## 3. `CWorldObjects`，大小 `0xF4`

### 3.1 容器与 owner 字段

| 偏移 | 宽度 | 当前名称 | 初始化 | 销毁/读写 | 状态 |
|---:|---:|---|---|---|---|
| `+0x00..0x0B` | `0x0C` | `CShowable` base | `CShowable_Ctor` | `CShowable_Dtor` | Confirmed |
| `+0x0C` | 4 | `modelCapacity` | 0 | growable header | layout Confirmed |
| `+0x10` | 4 | `modelCount` | 0 | dtor 循环上界 | Confirmed |
| `+0x14` | 4 | `modelData` | 0 | 指向 `0x188` 字节 `AUWOModel` 元素；逐元素 `0x6F74C200` 后 Storm free | Confirmed |
| `+0x18` | 4 | `modelGrowth` | 最终写 `0x100` | header 第四项 | layout Confirmed |
| `+0x1C..0x2B` | `0x10` | `UnknownArray1` | 全 0 | dtor free `+0x24`；EH cleanup 从 `+0x1C` 开始 | layout Confirmed，元素 Unknown |
| `+0x2C..0x3B` | `0x10` | `worldObjectPtrArray` | 全 0 | dtor free `+0x34`，allocator type string `PAVCWorldObject...` | 类型族 Confirmed，精确元素类型尾名 Unknown |
| `+0x3C..0x4B` | `0x10` | `UnknownArray3` | 全 0 | dtor free `+0x44` | layout Confirmed |
| `+0x4C..0x73` | `0x28` | `UnknownNestedGrowableOwner` | 全 0 | `0x6F74B810` 递归释放；元素内还有嵌套指针 | ownership Confirmed，语义 Unknown |
| `+0x74..0x83` | `0x10` | `uniqueDataArray` | 全 0 | dtor free `+0x7C`，allocator type string `AUWOUniqueData` | Confirmed |
| `+0x84` | 4 | `stringCapacity` | 0 | header | layout Confirmed |
| `+0x88` | 4 | `stringCount` | 0 | dtor 循环上界 | Confirmed |
| `+0x8C` | 4 | `stringData` | 0 | `0x0C` stride；每项调 vtable+4 后 free；type string `AVRCString` | Confirmed |
| `+0x90` | 4 | `stringGrowth` | 0 | header | layout Confirmed |
| `+0x94` | 4 | `CTerrain* terrain` | ctor 唯一参数 | 全部两个 direct ctor caller 均传 CTerrain：CDoodads singleton 取全局 terrain；BlightPuffs 从 CTerrain method 传 `this` | type/source/borrowed role Confirmed |

### 3.2 标量尾部

构造器 `0x6F74A970` 给出以下确定初值。下表把已闭合 reader/writer 的角色升级；无访问证据的
槽仍保留 Unknown，尤其不能因 ctor 恰好清零就称为 padding。

| 偏移 | 初值 | 当前状态 |
|---:|---:|---|
| `+0x98` | 0 | Unknown scalar probe gate；CDoodads slot31 只测试非零后生成 destructable 名称，从不解引用；不是可证指针，标准初始化中保持 0 |
| `+0x9C` | 0 | object-file-load-in-progress / render-submit suppression flag；`0x6F756C00` 在 load 窗口写 1/0，`CWorldObjects_SubmitModelsToRenderQueue` 非零即停止提交；同步窗口 Confirmed |
| `+0xA0` | 0 | mutation/serialization dirty-like latch；创建、TOD/stamp 更新置 1，完整 load/save 成功清 0；角色 Inferred，自然成员名 Unknown |
| `+0xA4` | 0 | scan/generation serial；每轮 scan 末自增，创建时复制进 `AUWOModel+0x98`，扫描比较并回写；角色 Confirmed |
| `+0xA8` | 1 | age-based stale-entry removal enable；标准初始化的当前两条路径均把它写 0，非零时失败 entry 才按 `+0xA4-entry+0x98` 年龄删除；Confirmed |
| `+0xAC` | 1 | doodad-animation-enabled flag；`0x6F75D1A0` 切换并遍历全部 entry 释放/重建 model，关闭时 model-create flags 增加 `0x80`；Confirmed |
| `+0xB0` | 1 | vslot67 query gate；entry flag `0x400` 时 base 返回本字段，否则返回 1，CDoodads 另对 `0x08000000` hard-reject；调用合同 Confirmed，业务名 Unknown |
| `+0xB4` | 未由 ctor 显式写 | Unknown；禁止假设为 0 |
| `+0xB8` | 0 | Unknown |
| `+0xBC` | 0 | Unknown |
| `+0xC0` | 0 | Unknown |
| `+0xC4` | 2 | `texquality` preference cache；descriptor index 7 的 key 为 `"texquality"`，变化时遍历刷新，model-create flags 读取；Confirmed |
| `+0xC8` | 0 | Unknown |
| `+0xCC` | 1 | Unknown flag |
| `+0xD0` | 1 | CDoodads vslot101 的条件返回字段：第二参数为 0 时返回本值，否则 0；调用合同 Confirmed，业务语义 Unknown |
| `+0xD4` | 未由 ctor 显式写 | slot64 唯一 caller 在 feature mask bit3 时把本值作为第二参数转发；业务含义仍 Unknown，禁止假设为 padding |
| `+0xD8..+0xEC` | 6 个 dword 全 0 | Unknown block |
| `+0xF0` | 0 | non-owning load/save progress-pump callback，ABI `void (__stdcall *)(float delta)`；setter 同时写 `CTerrain+0x820`，安装目标 `0x6F2B4FD0` 为 `retn 4`，teardown 写 0；Confirmed |

`+0xF4` 是两个已证派生类的第一个派生字段，因此 `CWorldObjects` 的尾边界为 Confirmed。
`CWorldObjects_Dtor @ 0x6F74BF80` 只拆到 `+0x90` 的 owner/header 后进入 `CShowable` dtor，
不释放或解引用 `+0x94..+0xF0`；这支持 terrain borrowed、scalar 与 callback 分类，但不能把
无访问字段自动判成 padding。

### 3.3 `AUWOModel` / doodad runtime entry，精确 stride `0x188`

这是 `CWorldObjects+0x14` 指向的数组元素，不是 `CDoodads` 类对象。下表只列真实 ASM 已直接
读写的槽；尚未闭合的间隙保持 Unknown。部分偏移在不同派生管理器中可能复用，故业务类型仍需
对应 producer/constructor 才能升级。

| entry 偏移 | 宽度 | 当前名称 | 真实读写证据 | 状态 |
|---:|---:|---|---|---|
| `+0x00` | 4 | `slotIndexRaw` | 建立/复制路径写入；数组访问另以外部 index 乘 `0x188` 定位 | 偏移/宽度 Confirmed，语义 Inferred |
| `+0x04` | 4 | `typeIdRaw` | Doodads slot14 传给注册链；slot15 作为 doodad/destructable DB 查询 key | 4CC/type-id role Confirmed，公开字段名 Unknown |
| `+0x08` | 4 | `variationOrStyleRaw` | build-info copy head；后续直接 reader | 偏移/宽度 Confirmed，语义 Inferred |
| `+0x0C,+0x10,+0x14` | 12 | `positionRaw[3]` | Doodads slot14 以 `entry+0x0C` 地址传入 terrain 注册链 | 三分量位置 role Confirmed |
| `+0x18` | 4 | `facingRaw` | Doodads slot14 以 `entry+0x18` 地址传入同一注册链 | 角度/朝向 role Confirmed，单位 Unknown |
| `+0x58` | 4 | `primaryModelOrBatch` | slots 9/10 读取；非零时进入 sprite 更新或 `RenderQueue_AddBatch` | role Confirmed，精确指针类 Unknown |
| `+0x5C` | 4 | `modelZOffsetRaw` | slot72 写入；`CWorldObjects_BuildModelTransform3x4` 加到 `positionRaw.z` | storage/consumer Confirmed；公开接口名 Inferred |
| `+0x60` | 4 | `defaultModelScaleRaw` | slot71 从 DB `defScale` 写入；精确 warning `Near-zero model scale` 路径把近零强制为 1 | storage/source/role Confirmed |
| `+0x64` | 4 | `noPathingFootprintRadiusRaw` | 仅 `entry+0xA0<=0` 时由 slot73 写入；平面 overlap/collision/spatial-query 作为回退半径消费 | consumer role Confirmed；公开名 Inferred |
| `+0x68..+0x73` | 12 | `localBoundsMinRaw[3]` | slot82 与 position/scale 组合，写第一个三分量 world-bounds output | offset/math role Confirmed；坐标约定 Unknown |
| `+0x74..+0x7F` | 12 | `localBoundsMaxRaw[3]` | slot82 与 position/scale 组合，写第二个三分量 world-bounds output | offset/math role Confirmed；坐标约定 Unknown |
| `+0x80` | 4 | `resolvedRaw` | `0x6F75DDD0` 先以 `(entry,&entryLocal)` 调 manager slot17，再取返回对象首 dword写入；slot63 D 与 EnableFeatures caller 都进入该 helper | write/source chain Confirmed；精确类型与 ownership Unknown，禁止猜成 model pointer |
| `+0x84` | 4 | `flagsRaw` | slot9 读取 bit10；更新链读取 bit5；Blight slot86 读取 `0x01000000`；slot67 以 bit `0x400` 进入 base gate并由 Doodads bit `0x08000000` hard-reject | flags dword Confirmed；逐 bit 业务名 Unknown |
| `+0x87` | 1 | `flagsHighByte` | Doodads slot15 读取 bit0；Blight slot67 返回 bit0 | 与 `+0x84` 重叠的字节视图 Confirmed |
| `+0x88` | 4 | `staticShadowImageHandleRaw` | build path 以 raw all-`FF` 常量初始化；`TerrainShadow_ToggleStaticStampFromObject @ 0x6F74DB30` 注册 type0 后写入，由 slots48..51 决定资源/route/rect/initial visibility；disable/destroy 经 CTerrain 注销并复位 `-1` | handle/CTerrain registry lifecycle Confirmed；handle 是索引而非 owned pointer |
| `+0x8C` | 4 | `selectionCircleImageHandleRaw` | build path 初始化 `-1`；`0x6F74DA00` 注册 type1、slot52 决定 initial visibility，并绑定 `SelectionCircle/ColorFriend`；`0x6F75AB60` 注销并复位，`0x6F757D10` 更新位置 | body/handle/cleanup lifecycle Confirmed；注册 body 无静态 code/data xref，producer reachability Unknown |
| `+0x90` | 4 | `emitterImageHandleRaw` | build path 初始化 `-1`；`TerrainShadow_ToggleEmitterStamp` 注册 type4 后写入、disable/destroy 注销并复位；`0x6F757D80` 更新位置 | handle/CTerrain registry lifecycle Confirmed；emitter 精确 gameplay owner Unknown |
| `+0x94` | 4 | `terrainAuxHandleRaw` | build path 初始化 `-1`；`CDoodads_ToggleEntryTerrainAuxHandle @ 0x6F74D730` 经 slot54/resource preparation 注册，关闭时以另一 CTerrain helper 注销并复位 | handle lifecycle Confirmed；精确 feature/domain Unknown |
| `+0x9C` | 4 | `resolvedRecordOrColorRaw` | Doodads slot15 从两个 DB 或 `ModelColorHash` 解析后写入；base slot9 以零值作门 | 来源/门控 Confirmed，精确类型 Unknown |
| `+0xA0,+0xA4,+0xA8` | 各4 | Unknown registration inputs | Doodads slot14 连同 terrain、`+4/+0C/+18` 下传 | 偏移/来源 Confirmed，语义 Unknown |
| `+0x148` | 4 | `managerSpecificStateRaw` | `CBlightPuffs` slot86 将 `trunc(deltaSeconds*1000)` 累加（仅 flag `0x01000000`）；`CDoodads` slot59 无条件写 0，slot58 写 `1 + !!TerrainQuery(entry+0x0C)` 即 1/2，并按变化触发纹理刷新 | 偏移/两套派生语义 Confirmed；是 class-specific/union raw role，禁止全族单义命名 |
| `+0x14C` | 4 | `managerSpecificLimitRaw` | `CBlightPuffs` 创建把 owner `puffDurationMs` 写入，slot10 与 elapsed `+0x148` 比较；Doodads 另有不同状态语义 | Puffs duration-ms role Confirmed；全族单义名禁止 |
| `+0x150` | 4 | `lifecycleStateRaw` | Doodads slot10 对 0..7 switch，触发 `"birth"/"death"`；slots58/63 在 destructable 特定路径把 4 写成 3 | 状态机 role/transition Confirmed，公开枚举名 Unknown |
| `+0x158` | 4 | `generatedNameOrdinalRaw` | Doodads slot31 用于格式化并探测精确前缀 `gg_dest_*` 的生成名 | formatter input role Confirmed；是否全局 instance ID 仍 Unknown |
| `+0x160` | 4 | `secondaryCount` | slot9 作为循环上界 | count role Confirmed |
| `+0x164` | 4 | `secondaryData` | slot9 按 `0x5C` stride 遍历并提交其中 batch | owner/element type Unknown，布局访问 Confirmed |

`+0x84` 与 `+0x87` 是同一 dword 的两种访问宽度，不是两个独立字段。四个连续 terrain handle
由 `0x6F7586AA..0x6F758725` 从 `0x6F9601D0` 的原始 16 个 `FF` 字节一次性初始化为 `-1`，并由
`AUWOModel_CopyFrom @ 0x6F750BC0` 原样迁移；这条证据不能把它们误写成四个指针。`+0x58/+0x164` 的
consumer 能证明 RenderQueue role，但尚不足以把它们强行定成某个公开 `CSpriteUber*`/batch
结构类型。

## 4. `CWorldObjectsClippable`，大小 `0x1C`

| 偏移 | 宽度 | 当前名称 | 初始化/复制 | 销毁/使用 | 状态 |
|---:|---:|---|---|---|---|
| `+0x00` | 4 | vptr | ctor 写 `0x6FA5A2EC` | dtor 最终恢复 `CClippable` vptr | Confirmed |
| `+0x04` | 4 | `UnknownKindOrMode` | ctor 唯一参数；Clone 原样复制 | vslot6 只识别值 1/2，并按输入 bool 映射到 9/13 或 8/12 | enum-like discriminator Confirmed，业务含义 Unknown |
| `+0x08` | 4 | `clipCapacity` | 0 | growable array header | Confirmed |
| `+0x0C` | 4 | `clipCount` | 0 | Clone 循环、centroid、size vslot | Confirmed |
| `+0x10` | 4 | `clipData` | 0 | 指向 `0xB4` stride `AUWOClipData`；dtor 递归释放 | Confirmed |
| `+0x14` | 4 | `clipGrowth` | 0 | growable header 第四项 | layout Confirmed |
| `+0x18` | 4 | `UnknownCopiedState` | ctor 0；Clone 原样复制 | vslot5 在参数为 0 时作为 vslot2 的参数 | 偏移 Confirmed，语义 Unknown |

### 4.1 `AUWOClipData` 已证边界

- 精确 stride `0xB4`：Clone 在 `0x6F752286 add ecx, 0B4h`；append helper 也按 `imul ...,0B4h`。
- `+0x00..+0x20` 从源记录复制 0x24 字节。
- `+0x24` 起是一个有独立构造/复制/析构的嵌入 owner；`+0x68` 为 `AUInvSlotInfo`、`+0x78`
  为 `AUAbilityInfo`、`+0x98` 为 `AUObjectChance`；`+0x4C/+0x50` 是嵌入数组，元素 stride
  `0x10` 且每项 `+8` 含 `AUObjectChance`。这些 nested allocations 与 outer data 均在
  `0x6F74B520` 释放。
- `+0xA8/+0xAC/+0xB0` 由 Clone 显式写入。
- centroid vslot8 精确平均 record `+0x08/+0x0C/+0x10`，空 count 没有除零保护。
- 其余内部字段保持 Unknown，不能把旧 doodad entry 的 `0x188` 布局套到该记录上。

Clone 只可称“独立 outer array，并 copy-construct 已证 nested owners”：record `+0x00..+0x20`
和 `+0xA8..+0xB0` 是 raw copy，raw/borrowed pointer-like 字段可能仍共享，不能笼统宣称整条
`0xB4` record 全部 deep-copy。

## 5. `CDoodads`，大小 `0x150`

| 偏移 | 宽度 | 当前名称 | 构造 | 析构/读写 | 状态 |
|---:|---:|---|---|---|---|
| `+0x00..+0xF3` | `0xF4` | `CWorldObjects` base | base ctor | base dtor | Confirmed |
| `+0xF4` | 4 | `CDoodadDB* doodadDb` | `sub_6F702AC0()` 返回 | lazy global `0x6FBED474`；DB ctor 写 vtable `0x6FA527CC`；本类 dtor 不释放 | type/lifetime Confirmed，borrowed |
| `+0xF8` | 4 | `CDestructableDB* destructableDb` | `sub_6F702730()` 返回 | lazy global `0x6FBED478`；DB ctor 写 vtable `0x6FA527F4`；本类 dtor 不释放 | type/lifetime Confirmed，borrowed |
| `+0xFC` | 4 | array-like header scalar 0 | 0 | EH cleanup 以 `this+0xFC` 为 header 起点；capacity-like 仅 Inferred |
| `+0x100` | 4 | array-like header scalar 1 | 0 | count-like 仅 Inferred |
| `+0x104` | 4 | owned backing allocation | 0 | ordinary dtor 与 EH cleanup 均读取后 Storm free；元素类型/pointee ownership Unknown |
| `+0x108` | 4 | array-like header scalar 3 | 0 | growth-like 仅 Inferred |
| `+0x10C` | 4 | `destructiblesUseHeightMapToggle` | `GetDebugToggleFlag(6)`；debug descriptor 精确字符串 `"Destructibles use height map"` | slot37 比较 live flag，变化时更新本字段并以两个零参数触发 model refresh | bool-like integer/source/role Confirmed |
| `+0x110` | 4 | `mapObjectLoadRecordCallbackRaw` | ctor 0；object-load 窗口安装、结束总清 0 | non-owning transient callback；consumer `0x6F7518A0` 组装 `0x24` 字节 record，ABI `ECX=const record*`、无栈参、目标 plain `ret`；不是 vslot68；Confirmed |
| `+0x114` | 4 | `entryActivationBridgeCallbackRaw` | ctor 0；标准初始化写 `0x6F7708C0` | non-owning callback；ABI `ECX=name, EDX=active, stack float x,float y`，目标 `retn 8`；激活传 1、停用/更新传 0；Confirmed |
| `+0x118..+0x13F` | `0x28` | `TSHashTable<CDoodads::ModelColorHash,HASHKEY_STRI>` | `CDoodads_ModelColorHashTable_Ctor(this+0x118)`，写 vtable `0x6FA59C68` | `sub_6F755840(...,0)` 后 `CDoodads_ModelColorHashTable_Dtor`；下字段从 `+0x140` 开始 | type/span/lifetime Confirmed；内部字段名 Unknown |
| `+0x140` | 4 | `customCapacity` | 0 | header | layout Confirmed |
| `+0x144` | 4 | `customCount` | 0 | header | layout Confirmed |
| `+0x148` | 4 | `customData` | 0 | Storm free；type string `AUDoodadCustom` | Confirmed |
| `+0x14C` | 4 | `customGrowth` | 0 | header | layout Confirmed |

注意：旧文档所谓“CDoodads 392 字节布局”实际是 `0x188` 字节 doodad/model entry，由 `CWorldObjects+0x14` 指向；它不是 `CDoodads` 类对象。类对象大小是 `0x150`。

### 5.1 embedded `ModelColorHash` 与 node partial layout

table subobject 的内部业务名仍不强填，但 ctor/clear/dtor 给出精确 raw 布局：

| table 偏移 | 宽度 | 初值/行为 | 状态 |
|---:|---:|---|---|
| `+0x00` | 4 | vptr `0x6FA59C68` | Confirmed |
| `+0x04` | 4 | ctor 最终写 `0x0C`；内部 helper 参与初始化/析构 | raw value/lifetime Confirmed，语义 Unknown |
| `+0x08,+0x0C` | 各4 | 自指/取反形式的 intrusive link root | layout Confirmed，精确容器术语 Unknown |
| `+0x10` | 4 | ctor/clear 写 0 | Unknown scalar/count candidate |
| `+0x14,+0x18,+0x1C,+0x20` | 各4 | ctor 写 0；clear 至少复位前三项 | offsets/lifetime Confirmed，逐项语义 Unknown |
| `+0x24` | 4 | ctor/clear 写 `0xFFFFFFFF` | sentinel Confirmed，语义 Unknown |

`AUModelColorHash` node 至少含 `0x1C` 字节 header；slot1 分配 `payloadBytes+0x1C`，slot0 负责
析构并释放 node。已证 node 字段如下：

| node 偏移 | 宽度 | 行为 | 状态 |
|---:|---:|---|---|
| `+0x00` | 4 | relative/intrusive link 运算使用 | layout Confirmed，语义 Unknown |
| `+0x04,+0x08` | 各4 | 第一组 link；dtor unlink | ownership/link behavior Confirmed |
| `+0x0C,+0x10` | 各4 | 第二组 link；dtor unlink | ownership/link behavior Confirmed |
| `+0x14` | 4 | dtor 非零时 Storm free | owned pointer Confirmed，payload/key role Inferred |
| `+0x18` | 4 | `CDoodads_ResolveModelColorRaw` cache hit 读取；miss 插入后写入 `AUWOModel+0x9C` 的同值 | `modelColorRaw` role Confirmed |

node/table 均是 `CDoodads` 的组合缓存生命周期，不能把 node 指针或 `+0x18` color dword误写成
`CDoodads+0x118` 本身的直接字段。

## 6. `CBlightPuffs`，大小 `0xF8`

| 偏移 | 宽度 | 当前名称 | 构造 | 销毁/使用 | 状态 |
|---:|---:|---|---|---|---|
| `+0x00..+0xF3` | `0xF4` | `CWorldObjects` base | base ctor | scalar dtor 直接调用 base dtor | Confirmed |
| `+0xF4` | 4 | `int32_t puffDurationMs` | 整型配置读取器查询 section `Blight` / key `PuffDuration`，default 0 | 创建 puff 时写 entry `+0x14C`；slot86 累加 `trunc(deltaSeconds*1000)` 到 `+0x148`；slot10 以 elapsed>duration 触发 death | signed integer/source/毫秒单位 Confirmed |

## 7. `CWorldFrameWar3`，大小 `0x668`

该类字段量大，本轮先把构造/析构与渲染直接访问闭合的部分列出；所有空洞显式保留。

| 偏移 | 宽度 | 当前名称 | 证据 | 状态 |
|---:|---:|---|---|---|
| `+0x00` | 4 | primary vptr | ctor/dtor 写 `0x6F98DCD0` | Confirmed |
| `+0x04` | 4 | inherited `CObserver` active/lifetime guard | event dispatch vslot5 增减；归零时调 vslot0 delete-self | role/offset Confirmed；精确计数名 Unknown |
| `+0x08` | 4 | inherited `ObserverRegistry*` | registration vslot2 按需构造；dispatch vslot5 查询 | type/role Confirmed；owned lifetime 细节部分 Unknown |
| `+0x0C` | 4 | inherited `CFrame` flags | secondary slot8 回到 owner 后读取 bit `0x08` | offset/bit Confirmed；业务名 Unknown |
| `+0x1C` | 4 | child-list head | slots31..33 以 node `+8` next、`+0x0C` child 遍历并传播继承属性 | layout/role Confirmed；node 类型 Unknown |
| `+0x20` | 4 | parent/link frame | slot3 向该对象 vslot3 转发 observer event；slot56 沿该 link 累加 layout offset | role Confirmed；具体静态类型/所有权 Unknown |
| `+0x2C` | 4 | inherited pointer property A | setter release/acquire/store；slot33 仅在本字段为 0 时向 children 传播 | storage/lifetime Confirmed；具体类型 Unknown |
| `+0x30` | 4 | inherited pointer property B | setter refcount 替换；slot31 仅在本字段为 0 时传播 | storage/lifetime Confirmed；具体类型 Unknown |
| `+0x34` | 4 | inherited float property | setter 值变化时写入；slot32 在本值 `<=0.0f` 时传播 | storage/type Confirmed；业务名 Unknown |
| `+0x74..+0x7F` | 12 | `AUKeyboardFocus` registration set A | slot25 检查 `+0x7C` signed count，并把 `&+0x74` 交给安装 helper | layout/role partially Confirmed；容器类型 Unknown |
| `+0x80..+0x8F` | 16 | `AUKeyboardFocus` registration set B | slot25 检查 `+0x8C` signed count，并把 `&+0x80` 交给安装 helper | layout/role partially Confirmed；容器类型 Unknown |
| `+0xB0` | 4 | inherited `CFrame` state flags | bit0 hidden；bit1 effective-active propagation gate；bit `0x10` 表示 AUKeyboardFocus attached；bit `0x20` 为一次性 post-render/realization edge | bit读写 Confirmed；bit1/bit20 高层名称仍 Inferred |
| `+0xB4` | 4 | `CLayoutFrame` secondary vptr | ctor/dtor 写 `0x6F98DDB8`；RTTI COL offset `0xB4` | Confirmed |
| `+0xB8` | 4 | layout provider slot count | `CLayoutFrame_Ctor` 写 9；dtor 用它遍历 provider grid | value/role Confirmed |
| `+0xBC..+0xDC` | 36 | `layoutProviders[9]` | ctor 全空；slot1 作为 3x3 constraint/provider grid 求解；dtor 逐项 detach/release/置零 | layout/ownership Confirmed；元素具体类型 Unknown |
| `+0xE0` | 4 | layout-solver recursion bits | 六组求解器 RMW bits `0x01/02/04/08/10/20`；ctor 0 | offset/bit contract Confirmed；各 bit 自然语言名 Unknown |
| `+0xE4..+0xF7` | 20 | layout intrusive/dependency headers | dtor 清理 secondary `+0x30/+0x34`、`+0x3C/+0x40`；slot0 由 `+0xF4` 进入 rectangle-change dependency iteration | lifecycle/entry role Confirmed；精确容器 layout 部分 Unknown |
| `+0xF8` | 4 | `rect.axis0Lower` | secondary `+0x44`；ctor 0；validate/commit 写，try-get 读 | offset/type/role Confirmed |
| `+0xFC` | 4 | `rect.axis1Lower` | secondary `+0x48`；同上 | Confirmed |
| `+0x100` | 4 | `rect.axis0Upper` | secondary `+0x4C`；同上 | Confirmed |
| `+0x104` | 4 | `rect.axis1Upper` | secondary `+0x50`；同上 | Confirmed |
| `+0x108` | 4 | `cachedRectValid` | ctor 0；scale 变化清 0；commit 写 1；try-get 为 0 时不写 output 并返回 false | authoritative validity gate Confirmed |
| `+0x10C` | 4 | unscaled width | ctor 0；独立 setter `0x6F0BD960`；slot6 返回 `scale*width` | Confirmed |
| `+0x110` | 4 | unscaled height | ctor 0；独立 setter `0x6F0BD7C0`；slot7 返回 `scale*height` | Confirmed |
| `+0x114` | 4 | inherited layout scale | ctor `1.0f`；slot5/setter 写并递归 children；变化时 invalidate rect | Confirmed |
| `+0x118` | 4 | screen-rect publication latch | ctor 0；commit path 条件发布/update TLS-associated cache；dtor 撤销并置 0 | lifecycle/offset Confirmed；业务名 Unknown |
| `+0x11C..+0x123` | 8 | Unknown | 未由本批闭合 | Unknown |
| `+0x124` | 4 | inherited coordinate/category flags | bit0 被 layout validation 作为 coordinate-adjust gate；bit `0x10` 被 slot13 用于 post-pass category cleanup | offset/bits Confirmed；统一业务名 Unknown |
| `+0x14C` | 4 | inherited post-pass list/state pointer | slot13 与 `+0x158` 联合判断/cleanup | offset/use Confirmed；类型 Unknown |
| `+0x158` | 4 | inherited post-pass list/state pointer | slot13 与 `+0x14C` 联合判断/cleanup | offset/use Confirmed；类型 Unknown |
| `+0x168` | 4 | Unknown | ctor 0 | Unknown |
| `+0x16C` | 4 | `WorldGroupRecordOwner* worldGroup[0]` | owned `0x1C` non-polymorphic owner；RenderGroup(0) 读取；帧尾 release active refs，dtor release constructed refs/free | type/span/ownership Confirmed；真实 C++ 类名 Unknown |
| `+0x170` | 4 | `WorldGroupRecordOwner* worldGroup[1]` | 同上；RenderGroup(1) | Confirmed |
| `+0x174` | 4 | `WorldGroupRecordOwner* worldGroup[2]` | 同上；RenderGroup(2) | Confirmed |
| `+0x178` | 4 | borrowed `CGameWar3*` | `CWorldFrameWar3_RefreshGameContextBindings @ 0x6F3618F0` 从 lazy global `0x6FBE4238` 发布；不 acquire/release | type/provenance Confirmed |
| `+0x17C,+0x180` | 8 | Unknown | ctor 0；本批未见可升级的独立 reader/writer | Unknown |
| `+0x184` | 4 | borrowed game-ordinal object | `ObjectFieldAccess(CGameWar3, uint16 game+0x28)` 返回值 | provenance Confirmed；具体类/语义 Unknown |
| `+0x188` | 4 | borrowed publication from `CGameWar3+0x34` | refresh 直接复制，不做引用计数 | source/ownership Confirmed；具体类型 Unknown |
| `+0x18C,+0x190,+0x194` | 12 | frame-derived scalar block | ctor 0；primary slot11 更新/消费 | offset/read-write block Confirmed；单位与业务名 Unknown |
| `+0x198` | 4 | zero-extended game ordinal/index | refresh 读取 `uint16 CGameWar3+0x28`；indicator 更新链把它放入 EDX | source/type width Confirmed；公开枚举名 Unknown |
| `+0x19C` | 4 | `UnknownOwnedObject19C` | ctor 0；dtor 若非零执行虚拟释放 | ownership Confirmed，旧名 camera 待复核 |
| `+0x1A0` | 4 | Unknown runtime pointer/handle | ctor 0 | Unknown；没有足够证据判定所有权 |
| `+0x1A4` | 4 | interaction flags | ctor 1；click 路由测试其中若干 bit | storage/bit-test Confirmed；逐 bit 业务名 Unknown |
| `+0x1A8` | 4 | interaction mode/flag | ctor 1；交互路径读写 | storage Confirmed；业务名 Unknown |
| `+0x1AC` | 4 | hover/input gate | ctor 0；hover/input 路径测试 | role Inferred；精确状态枚举 Unknown |
| `+0x1B0,+0x1B4` | 8 | array metadata candidate | ctor 0；与 `+0x1B8` 的分配/释放路径成组 | layout Inferred |
| `+0x1B8` | 4 | owned `W4CursorMode[]` data | raw allocator type descriptor 与 dtor free | element type/ownership Confirmed |
| `+0x1BC,+0x1C0` | 8 | Unknown | ctor 0 | Unknown |
| `+0x1C4` | 4 | hover/interaction-active latch | ctor 0；hover enter 写 1；hit-test/退出路径读取或清除 | role/offset Confirmed；自然名 Unknown |
| `+0x1C8` | 12 | `RCString` | 显式安装 RCString vptr并赋空 | dtor 销毁 | type Confirmed |
| `+0x1D4..+0x1E3` | 16 | `float[4]` storage | ctor 清零；运行期逐分量读写 | width/type Confirmed；坐标/颜色等业务语义 Unknown |
| `+0x1E4..+0x1F3` | 16 | four-dword derived-result block | ctor 清零；交互计算链整体写/读 | block boundary Confirmed；元素类型 Unknown |
| `+0x1F4` | 2 | local game-ordinal bit mask | refresh 写 `uint16(1 << gameOrdinal)` | width/formula Confirmed |
| `+0x1F6..+0x1F7` | 2 | Unknown/padding | 没有独立访问证据 | Unknown |
| `+0x1F8..+0x213` | `0x1C` | embedded metadata/container A | 首 dword ctor 写 `0x0C`；`0x6F36E8E0(this,&field,game+0x2C,0)` 初始化 | footprint/source/mode Confirmed；具体容器类型 Unknown |
| `+0x214..+0x22F` | `0x1C` | embedded metadata/container B | 首 dword ctor 写 `0x0C`；以 `+0x1F4`、mode 1 初始化；click path 读取 `+0x214/+0x218/+0x220` | footprint/use Confirmed；具体容器类型 Unknown |
| `+0x230` | 4 | game-ordinal-derived publication | `sub_6F039860(ObjectFieldAccess(...)+0xF0)` 返回值 | source Confirmed；具体类/所有权 Unknown |
| `+0x234` | 4 | Unknown | ctor 0 | Unknown |
| `+0x238` | 4 | transient frame scalar | primary slot11 写/消费后清零 | consume-and-clear contract Confirmed；单位 Unknown |
| `+0x23C` | 4 | transient frame scalar | primary slot11/相邻更新链消费后清零 | consume-and-clear contract Confirmed；单位 Unknown |
| `+0x240` | 4 | Unknown | ctor 0 | Unknown |
| `+0x244` | 4 | owned refcounted runtime object | ctor 0；dtor release/delete 并清零 | ownership Confirmed；动态类 Unknown |
| `+0x248` | 4 | build-hover / hit-test gate | ctor 0；secondary `CLayoutFrame` hit-test 成功时置位，hover leave/build exit 清零 | stage18 publication/render gate Confirmed；自然成员名 Unknown |
| `+0x24C` | 4 | Unknown | ctor 0；旧 slot16 证据实际访问 `CGameUI+0x24C`，不能迁移到本对象 | Unknown |
| `+0x250` | 4 | `CBuildFrame* activeBuildFrame` | ctor 0；`CBuildMode` 创建/发布，取消时在 active resources 销毁前清除 | stage18 exact type/borrowed publication role Confirmed；不是 WorldFrame-owned allocation |
| `+0x254..+0x2F7` | `0xA4` | embedded `CCinematicFilter` | `CCinematicFilter_Ctor @ 0x6F37F8F0`；vptr `0x6F98ED34` | `CCinematicFilter_Dtor @ 0x6F386650`；type/size/lifetime Confirmed，内部空洞保留 Unknown |
| `+0x2F8` | 4 | strong runtime-object wrapper | ctor 0；`0x6F36D870` replacement/publish；替换与 dtor 都 release | ownership Confirmed；动态类 Unknown |
| `+0x2FC` | 4 | TerrainImage publication/render gate | ctor 0；stage21 与 `+0x300` 联合判断 | gate role Confirmed；producer自然名 Unknown |
| `+0x300` | 4 | signed `TerrainImage` index | `0xFFFFFFFF` | stage21 传给 `RenderTerrainImageByIndex`，经 CTerrain `+0x2CC + index*0xA0` 定位记录；index role Confirmed，WorldFrame ownership 未证 |
| `+0x304` | 4 | float extent/radius-like scalar | ctor 0；与 `+0x308` 平方值成对读写 | formula relationship Confirmed；业务名 Inferred |
| `+0x308` | 4 | square of `+0x304` | writer 明确计算乘积 | formula Confirmed |
| `+0x30C` | 4 | payload argument 3 | ctor 0；调用链原样转发 | width/position Confirmed；类型 Unknown |
| `+0x310..+0x31B` | 12 | frame-derived three-dword block | ctor 0；帧更新链成组写/读 | block boundary Confirmed；业务语义 Unknown |
| `+0x31C` | 4 | `activeQueue` | ctor 0 | `RenderScene @ 0x6F368209` 直接读取并传给全部主 stage；0 时追加 15/18/21 | role/offset Confirmed；精确指针类 Unknown |
| `+0x320` | 4 | pick/interaction comparison state | ctor 0；交互链比较/更新 | role Inferred；精确类型 Unknown |
| `+0x324` | 4 | RenderScene branch gate | ctor 1；主场景分支直接测试 | offset/branch role Confirmed；自然名 Unknown |
| `+0x328` | 4 | Unknown flag | ctor 1；运行期读取 | storage Confirmed；业务名 Unknown |
| `+0x32C` | 4 | borrowed `CGameWar3+0x3C0` publication | refresh 直接复制，不 acquire/release | source/ownership Confirmed；`CGameUI*` 仅 Inferred |
| `+0x330` | 4 | frame-update/environment gate | ctor 1；帧更新路径测试 | role Inferred；自然名 Unknown |
| `+0x334` | 4 | owned strong `CFog* terrainZFog` | `CFog_Create @ 0x6F191320` 分配 `0xD4`；`CWorldFrameWar3_InitTerrainZFog @ 0x6F36C390` 读取 `TerrainZFog` 配置 | teardown release/clear；type/ownership/配置来源 Confirmed |
| `+0x338` | 4 | owned strong `CSprite* terrainDncSprite` | `CWorldFrameWar3_InitDncSpriteResources @ 0x6F366F10` 以 terrain DNC model path 创建 | dynamic vptr `0x6F96485C`，即 pooled `CSpriteUber_` leaf；teardown release/clear Confirmed |
| `+0x33C` | 4 | owned strong `CSprite* unitDncSprite` | 同一 helper 以 unit DNC model path 创建 | dynamic vptr/ownership 同 `+0x338`；Confirmed |
| `+0x340` | 4 | owned strong `CLight*` | `CLight_Create @ 0x6F1913B0` 分配 `0xDC`；ctor 写 vptr `0x6F964B08` | teardown release/clear；exact base `CLight`，不是 `COmniLight` |
| `+0x344` | 4 | Unknown | ctor/当前已审路径没有足够独立语义证据 | Unknown；不得称 padding |
| `+0x348` | 12 | `RCString` stage-0 model path | 显式安装 RCString vptr并赋空；setter 保存 `+0x354` 的 path | dtor 销毁；type/use Confirmed |
| `+0x354` | 4 | owned strong stage-0 `CSprite*` | `CWorldFrameWar3_SetStage0SpriteResource @ 0x6F36D620`；非空 path 以 `useUber=1` 创建 | exact dynamic vptr `0x6F96485C` pooled Uber leaf；teardown release/clear |
| `+0x358` | 4 | stage-0 enable gate | ctor 1；`RenderScene` 在 stage 0 前测试 | width/branch role Confirmed；公开成员名 Unknown |
| `+0x35C` | 4 | stage-0 last prepare result | slot11 update 写 `CWorld_VisibilityOrPreRenderHook(+0x354)` 返回值；`RenderScene` 测试 | producer/consumer role Confirmed；返回枚举/自然名 Unknown |
| `+0x360` | 4 | `CPathingMapIndicatorRefVector.capacity` | ctor 0；grow/reallocate 维护 | layout Confirmed |
| `+0x364` | 4 | `CPathingMapIndicatorRefVector.count` | ctor 0；destroy/reallocate 遍历该数量 | layout Confirmed |
| `+0x368` | 4 | `CPathingMapIndicator** data` | ctor 0；owned backing；每元素为 strong ref | allocator descriptor `TRefCntPtr<CPathingMapIndicator>`；ownership Confirmed |
| `+0x36C` | 4 | `CPathingMapIndicatorRefVector.growthQuantum` | ctor 0；容量低于 `0x40` 时按 2 的幂增长，此后 quantum `0x40` | layout/growth formula Confirmed |
| `+0x370` | 4 | `TargetIndicatorVector.capacity` | ctor resize 到 8；reallocate 维护 | layout Confirmed |
| `+0x374` | 4 | `TargetIndicatorVector.count` | ctor resize 到 8；record stride `0x18` | layout/count Confirmed；8 是初始 count，不是固定 owner 大小 |
| `+0x378` | 4 | `TargetIndicator* data` | owned heap backing；resize/reallocate/destroy 逐 record 处理 strong ref | pointer/ownership/stride Confirmed |
| `+0x37C` | 4 | TargetIndicator ring cursor | 两个 producer 都执行 `(cursor+1)&7` 后写回并以 `data+index*0x18` 定位 | storage/formula Confirmed；公开成员名 Unknown |
| `+0x380` | 4 | periodic maintenance float accumulator | slot11 累加帧 delta；与 raw `3.0f` 比较，超阈值置 `+0x388=1`；slot11 maintenance 尾部清零 | type/formula/lifetime Confirmed；时间单位仍 Unknown |
| `+0x384` | 4 | companion periodic float accumulator | `0x6F3617D0` 同时累加 `+0x384` 与 `+0x23C`；与 `3.0f` 比较并触发 helper；slot11 尾部清零 | mechanical role Confirmed；业务单位/名称 Unknown |
| `+0x388` | 4 | periodic-maintenance-due latch | ctor 1；`0x6F359470` 与 `+0x380` threshold 均可置 1；`0x6F368E90` 测试，slot11 随后清 0 | producer/consumer/clear Confirmed；维护业务集合部分 Unknown |
| `+0x38C` | 4 | one-shot extra-terrain/deferred-refresh latch | ctor 0；`0x6F346440` 条件置 1；slot11 真分支执行 selector 8 + extra terrain pass；`0x6F368E90` 注册 callback 后清 0 | mechanical one-shot role Confirmed；公开业务名 Inferred |
| `+0x390` | 4 | accumulated frame time | slot11 每帧累加一个 float 参数 | offset/type/accumulator role Confirmed；单位与参数名 Unknown |
| `+0x394` | 4 | Unknown float | `+Inf (0x7F800000)` | raw value Confirmed |
| `+0x398` | 4 | `scaledAnimTimeConfigRaw` | `Misc/ScaledAnimTime` | key/offset Confirmed，类型待审 |
| `+0x39C` | 4 | `dayHoursConfigRaw` | `Misc/DayHours` | key/offset Confirmed |
| `+0x3A0,+0x3A4,+0x3A8` | 12 | selection colors | `SelectionCircle/ColorFriend,ColorNeutral,ColorEnemy` | keys/offsets Confirmed |
| `+0x3AC` | 4 | Unknown scalar/pointer | ctor 0；未与后续 small-vector header 合并 | Unknown |
| `+0x3B0,+0x3B4,+0x3B8` | 12 | `RallyIndicatorSmallVector16` capacity/count/data | ctor 令 data 指向 inline `+0x3BC`，count 由 initializer 填充；resize/dtor 维护 | header/layout Confirmed |
| `+0x3BC..+0x53B` | `0x180` | inline `RallyIndicator[16]` | 16 个 `0x18` record；构造/重置/析构逐项处理 `runtimeObject08` | exact inline span/stride Confirmed |
| `+0x53C,+0x540,+0x544` | 12 | `Int32SmallVector16` capacity/count/data | data 指向 inline `+0x548`；count=16 | header/layout Confirmed |
| `+0x548..+0x587` | `0x40` | inline `int32_t[16]` | 初始化为 `15..0` | width/values Confirmed；业务上类似 free-order 仅 Inferred |
| `+0x588` | 4 | owned rally-indicator source runtime object | ctor/init 创建，refresh 使用，dtor release | ownership/role Confirmed；动态类 Unknown |
| `+0x58C` | 4 | owned target-point-confirm runtime object | `CWorldFrameWar3_InitTargetPointConfirm` 创建；dtor release | ownership/role Confirmed；动态类 Unknown |
| `+0x590,+0x594,+0x598` | 12 | WaypointIndicator vector capacity/count/data | capacity/count 初始化 `0x100`，data 为 owned heap buffer，stride `0x1C` | layout/ownership Confirmed |
| `+0x59C` | 4 | waypoint ring cursor | reset 为 0；推进公式 `(cursor+1)&0xFF` | width/formula Confirmed |
| `+0x5A0..+0x5AF` | `0x10` | raw `CUnit*` vector | reserve `0x200`，growth quantum `0x80` | backing owned、elements borrowed Confirmed |
| `+0x5B0..+0x5BF` | `0x10` | raw `CDestructable*` vector | reserve `0x400`，growth quantum `0x100` | backing owned、elements borrowed Confirmed |
| `+0x5C0..+0x5CF` | `0x10` | raw `CItem*` vector | reserve/quantum `0x10` | backing owned、elements borrowed Confirmed |
| `+0x5D0..+0x5DF` | `0x10` | raw `CCaptainAI*` vector | 无初始 reserve；`+0x5D4` count 在 `0x6F368E90` 不清 | layout/ownership Confirmed；保留原因 Unknown |
| `+0x5E0..+0x5EF` | `0x10` | raw `CEffectImagePos*` vector | reserve/quantum `0x10` | backing owned、elements borrowed Confirmed |
| `+0x5F0..+0x5FF` | `0x10` | raw `CGhostImage*` vector | reserve/quantum `0x10` | backing owned、elements borrowed Confirmed |
| `+0x600..+0x60F` | `0x10` | `CAgentPtr<CUnit>` vector A | resize `0x7D0` | backing与元素 refs owned；业务 cohort Unknown |
| `+0x610..+0x61F` | `0x10` | `CAgentPtr<CItem>` vector | ctor 全零 | backing与元素 refs owned Confirmed |
| `+0x620..+0x62F` | `0x10` | `CAgentPtr<CUnit>` vector B | resize `0xC8`；stage16 bucket | backing与元素 refs owned；bucket 公开名 Unknown |
| `+0x630..+0x63F` | `0x10` | `CAgentPtr<CSelectable>` vector | resize `0x14` | backing与元素 refs owned Confirmed |
| `+0x640..+0x64F` | `0x10` | `CAgentPtr<CWidget>` vector | ctor 全零；deferred cohort role Inferred | backing与元素 refs owned Confirmed |
| `+0x650..+0x65F` | `0x10` | `CAgentPtr<CGhostImage>` vector | resize `0x7D0` | backing与元素 refs owned Confirmed |
| `+0x660` | 4 | `currentRenderMode` | RenderScene 入口/出口写 `-1` | Dispatcher 比较、disable/enable 后写入 | role/offset Confirmed |
| `+0x664` | 4 | `currentRenderCategory` | RenderScene 入口/出口写 `-1` | Dispatcher 比较、disable/enable 后写入 | role/offset Confirmed |

slot25 的 helper 会分配精确 `0x40` bytes、带原始 type descriptor 字符串
`AUKeyboardFocus` 的对象，并将 owner 写入该对象 `+0x3C`。slot26 在 effective-inactive
边沿撤销当前全局 focus，再 detach/destroy 该对象；因此上表两个 registration 区是
`CFrame` 基类拥有的输入集合，但其内部容器类型仍保持 Unknown。

派生析构 slot27 还确认了资源边界：`+0x370/+0x374/+0x378` 是
`TargetIndicatorVector {capacity,count,data}`，初始 resize 到 8，`+0x378` 只是 data pointer；
不能再把它描述成“固定 owner 内 8 个 handles”。`+0x360..+0x36C` 是另一张强引用 vector。
`+0x334/+0x338/+0x33C/+0x340/+0x354` 会在 teardown 中 release/clear，其中前四项和
`+0x354` 的动态类已由 factory/vptr 闭合；`+0x588/+0x58C` 的动态类仍 Unknown。

`CLayoutFrame` subobject 的精确 footprint 是 primary `+0xB4..+0x11B`（secondary size
`0x68`）。缓存矩形只有在 `+0x108!=0` 时可消费；valid 为 0 时，`+0xF8..+0x104` 可保留
旧矩形或 validation 中间值。数值条件只确认
`axis0Upper>axis0Lower && axis1Upper>axis1Lower`；ASM 还不足以把两个轴命名成固定屏幕
`top/left/bottom/right`，因此本表刻意保留 axis 命名。

### 7.1 `TargetIndicator` partial record 与两张 vector

`TargetIndicatorVector` 的 backing allocator descriptor 为 `.?AUTargetIndicator@@`，record
精确 stride `0x18`。下表只给出所有 reader/writer/构析路径共同支持的机械字段；除 strong-ref、
cursor 与 fallback sentinel 外，不给业务 payload 猜名：

| record offset | width | 已证字段/行为 | 置信度 |
|---:|---:|---|---|
| `+0x00` | 4 | signed sentinel，初始化为 `-1` | storage/init Confirmed；业务名 Unknown |
| `+0x04` | 4 | strong `TRefCnt*`；替换时 acquire/release，destroy 时 release | ownership/lifetime Confirmed；具体动态类 Unknown |
| `+0x08` | 4 | float；producer/maintenance 有 SSE add/reset | type/read-write Confirmed；单位/语义 Unknown |
| `+0x0C` | 4 | dword state；已审 producer 写 1 | width/write Confirmed；枚举 Unknown |
| `+0x10` | 4 | dword payload，来自 producer stack argument | width/provenance Confirmed；类型/语义 Unknown |
| `+0x14` | 4 | signed result/index；失败路径写 `-1` | width/fallback Confirmed；公开返回含义 Unknown |

`CPathingMapIndicatorRefVector` 则是 exact `0x10` header，元素 allocator descriptor 为
`.?AV?$TRefCntPtr@VCPathingMapIndicator@@@@`。`CPathingMapIndicatorRefVector_Destroy @
0x6F360080` 逐元素 release 后 free backing；`Reallocate @ 0x6F36A4D0` 在新 backing acquire、
旧 backing release。两张 vector 的 backing 都由 WorldFrame owned，元素身份都不能跨
reallocate/destroy 当成稳定 publication。

### 7.2 game-context refresh 与 `+0x178..+0x230`

`CWorldFrameWar3_RefreshGameContextBindings @ 0x6F3618F0` 的 51 条真实指令提供了这一段的
authoritative writer：

```text
lazy global CGameWar3 @ 0x6FBE4238
  -> WorldFrame+0x178
  -> game+0x34                 -> +0x188
  -> uint16(game+0x28)         -> +0x198
  -> uint16(1 << ordinal)      -> +0x1F4
  -> ObjectFieldAccess(...)    -> +0x184
  -> helper(object+0xF0)       -> +0x230
  -> init +0x1F8, mode 0
  -> init +0x214, mode 1
  -> game+0x3C0                -> +0x32C
  -> tail indicator-runtime refresh
```

该函数及尾调用 `0x6F36A840` 都没有锁、generation 或 immutable snapshot；这只证明当前
render/UI lane 的同步写序，不能据此宣布跨线程可读。`+0x32C` 的 source offset 已确认，
但 `CGameUI*` 仍只是旧上下文推断，表中不将它升级为 RTTI-confirmed type。

### 7.3 embedded `CCinematicFilter @ +0x254`，大小 `0xA4`

`CCinematicFilter` 有独立 RTTI：COL `0x6FA874AC`、TD `0x6FB8E0FC`
(`.?AVCCinematicFilter@@`)；hierarchy 只有 self BCD。vftable `0x6F98ED34` 只有一个槽：

| 槽 | 地址 | ABI/行为 | 状态 |
|---:|---:|---|---|
| 0 | `0x6F38A130` | `CCinematicFilter* __thiscall(this, unsigned flags)`；先调 dtor，flags bit0 时 Storm-free，返回原 this，`retn 4` | Confirmed scalar deleting dtor |

下一 dword `0x6F98ED38` 已是 `CAllianceSlot` COL，故一槽边界不是按相邻函数名猜测。成员只记录
已证部分：

| filter 偏移 | 宽度 | 字段/生命周期 | 状态 |
|---:|---:|---|---|
| `+0x00` | 4 | vptr `0x6F98ED34` | Confirmed |
| `+0x04..+0x0F` | 12 | Unknown | Unknown |
| `+0x10..+0x1B` | 12 | embedded `RCString`；ctor 构造，dtor 最后 release | type/lifetime Confirmed |
| `+0x1C..+0x47` | `0x2C` | Unknown | Unknown |
| `+0x48,+0x54,+0x60,+0x6C,+0x78,+0x88,+0x94,+0xA0` | 各 4 | owned container/buffer pointer；dtor 按逆序检查并 free | pointer/ownership Confirmed；元素类型 Unknown |
| 其余至 `+0xA3` | — | container metadata 与空洞 | Unknown；禁止按相邻 free 填满 |

构造器 `0x6F37F8F0` 返回 this，析构器 `0x6F386650` 为 `void __thiscall`。WorldFrame ctor 在
`0x6F35F218` 以 `this+0x254` 调 ctor，dtor 在 `0x6F36060E` 对称销毁；下一字段从 `+0x2F8`
开始，因此完整内嵌 span 精确为 `0xA4`。

### 7.4 Rally/Waypoint 与尾部向量

`RallyIndicatorSmallVector16` 精确大小 `0x18C`：12-byte header 加 16 个 inline `0x18`
record。`Int32SmallVector16` 精确大小 `0x4C`：同样的 header 加 16 个 inline dword。两者在
data 指向 inline storage 时不把 inline storage 当 heap free；resize 才迁移到 owned heap。

| record | 精确布局 |
|---|---|
| `RallyIndicator` (`0x18`) | `+0 vptr`、`+4 Unknown04`、`+8 runtimeObject08`、`+0xC/+0x10/+0x14 Unknown` |
| `WaypointIndicator` (`0x1C`) | 上述前 `0x18` 字节加 `+0x18 Unknown18` |

`CWorldFrameWar3_UpdateIndicatorRuntimeObjectsForOrdinal @ 0x6F36A840` 遍历 Rally count/data、
singleton `+0x588` 与 Waypoint count/data；每个非空 runtime object 都以
`EDX=[WorldFrame+0x198]` 调 `0x6F3393E0`。循环、stride、参数来源 Confirmed；“更新 player
ordinal”是保守语义标签，`0x6F3393E0` 的公开业务名仍需独立闭合。

`+0x5A0..+0x65F` 的每个通用向量 header 都是：

| header 偏移 | 字段 |
|---:|---|
| `+0x00` | capacity |
| `+0x04` | count |
| `+0x08` | data pointer |
| `+0x0C` | growth quantum |

raw-pointer 六组只拥有 backing storage，不 acquire/release 元素；六组 `CAgentPtr<T>` 向量同时
拥有 backing 与元素引用。`CAgentPtr` dtor helper 分别由 raw allocator descriptor
`.PAV?$CAgentPtr@VCUnit@@@...` 等闭合。`CCaptainAI*` vector 的 count 在帧尾 helper
`0x6F368E90` 未清是 Confirmed 事实，但保留原因仍为 Unknown。

### 7.5 world-group owner、record 与 `CSprite` 强引用

三个 owner 都由 `WorldGroupRecordOwner_Ctor @ 0x6F0CA900` 以 `mode=1` 初始化，构造器不写
vptr，因此 `WorldGroupRecordOwner` 是本文的结构性名称，不冒充已证 RTTI 类名：

| owner 偏移 | 宽度 | 字段 | 证据/状态 |
|---:|---:|---|---|
| `+0x00` | 4 | `UnknownMode00` | 三个实例均写 1；业务语义 Unknown |
| `+0x04` | 4 | `capacity` | grow/shrink 读写；Confirmed |
| `+0x08` | 4 | `constructedCount` | 最终 dtor 遍历上界；Confirmed |
| `+0x0C` | 4 | `WorldGroupRecord* records` | accessor `0x6F0CAE80`；owned buffer |
| `+0x10` | 4 | `growthQuantum` | ctor 写 `0x40`；Confirmed |
| `+0x14` | 4 | `activeCount` | add 先增、render/clear 使用；Confirmed |
| `+0x18` | 4 | `UnknownShrinkHistory18` | clear 写 `floor((63*old+activeCount)/64)` 并用于 half-capacity shrink；数学合同 Confirmed，高层名 Unknown |

每项精确 `0x18`：

| record 偏移 | 宽度 | 字段 | 构造/clear/所有权 |
|---:|---:|---|---|
| `+0x00` | 4 | `CSprite* strongSpriteRef` | add 经 `0x6F04F200` acquire；clear/dtor 经 `0x6F04F1A0` release；Confirmed strong ref |
| `+0x04` | 4 | `float Unknown04` | default/clear `+Inf`；业务语义 Unknown |
| `+0x08` | 4 | `float Unknown08` | default `+Inf`；`WorldGroupRecord_ResetAndReleaseSprite` 故意不改；业务语义 Unknown |
| `+0x0C` | 4 | `float Unknown0C` | default/clear `+Inf`；业务语义 Unknown |
| `+0x10` | 4 | `uint32_t Unknown10` | default/clear 1；业务语义 Unknown |
| `+0x14` | 4 | `rootTagOrZero` | 仅 query root 写 context `+0x0C`，descendant 写0；不能无条件命名 owner pointer |

`CWorldFrameWar3_RenderWorldGroup` 只消费 `activeCount`，按 `0x18` stride 从 record `+0` 取
`CSprite*`。帧尾 `0x6F368EA9/EB4/EBF` release active refs 并将 activeCount 清零，constructed
buffer 可跨帧复用；最终 dtor 则遍历全部 constructed records 后释放 buffer。record 地址因此
不是稳定对象身份。release 归零可同步调用 pooled leaf vslot1 并立即回收 `CSpriteMini_`/`Uber_`；
旧指针在 reset helper 返回后不得再读。add 又是先增 activeCount 后填 record，不能把它解释成
lock-free publication。

### 7.6 stage18 关联对象的已证字段边界

| 对象/偏移 | 类型与生命周期 | 证据状态 |
|---|---|---|
| `CBuildFrame+0x18C` | active `CConstructUI*`；slot3 参与 stage18，取消路径协调清理 | type/consumer Confirmed；不能简单概括为独占 owned |
| `CBuildFrame+0x190` | owned `CPlacementBox*`；`0x6F378A40` 执行 GPU draw，取消时 vslot1(flags=1) 销毁并清零 | type/ownership/consumer Confirmed |
| `CBuildFrame+0x194/+0x198` | Unknown | 只有相邻边界，保持 Unknown |

`CBuildFrame` 精确大小 `0x19C`，主/次 vtable 为 `0x6F9955B0/0x6F995698`；上表只记录
stage18 实际消费的尾字段，不据此填满其余布局。

### 7.7 `CTextTagManager` render-pass partial layout，大小 `0x88`

| 偏移 | 当前角色 | 证据状态 |
|---:|---|---|
| `+0x08` | record count | `CTextTagManager_RenderPass` 循环上界 Confirmed |
| `+0x0C` | record array pointer | 以 `0x34` stride 访问 Confirmed |
| `+0x14` | active gate | 为 0 时整 pass 早退 Confirmed |
| record `+0x2C` | priority raw | priority queue consumer Confirmed |
| record `+0x30` | flags raw | render filtering bits消费 Confirmed；逐 bit 自然名 Unknown |
| `+0x84` | pass synchronization/state raw | pass 尾同步读写 Confirmed；公开类型 Unknown |

static root 通过 `+0x30` lazy-own 一个独立 `CTextTagManager`；`CGameState` 则在 `+0x2C8`
内嵌第二个实例。相同 render pass 不表示二者有共同 owner 或可共享生命周期。

这张表刻意没有沿用旧 `war3_native_renderer.h` 中大量推测字段名；只有 reader/writer、构析与 owner 边界闭合后才会升级。
