# `CWorldObjects` / `CDoodads` / `CBlightPuffs` 逐槽 ABI 分卷

## 1. 记号

完整目标地址见 [vtable 地址台账](vtables.md)。本页的 `stack B/D/P` 分别表示
`CWorldObjects/CDoodads/CBlightPuffs` 目标最终由 callee 清理的栈字节；`this B/D/P` 表示真实
ASM 是否消费传入 ECX（wrapper 保留 ECX 转入 base 时也算消费）。`-` 表示不消费。

“Unknown”是明确结论：函数边界和 ABI 已读，但业务接口名仍缺 caller/类型证据。不能用相邻
函数名或 IDA 自动库签名填充。

## 2. slots 0..27：行为已由 ASM 闭合

| slot | stack B/D/P | base 行为 | CDoodads 差异 | CBlightPuffs 差异 | 状态 |
|---:|---|---|---|---|---|
| 0 | `4/4/4` | scalar deleting dtor，flags bit0 控制 free | 派生 dtor | 直接 base dtor，无额外清理 | Confirmed |
| 1 | `4/4/4` | `+4` gated setter；`+8==0` 时写并调 slot4 | Same | Same | 行为 Confirmed，业务名 Unknown |
| 2 | `0/0/0` | 返回 `this+4` | Same | Same | Confirmed |
| 3 | `4/4/4` | `+8` 改变时写入并调 slot4 | Same | Same | 行为 Confirmed，业务名 Unknown |
| 4 | `0/0/0` | slot5 有效时遍历 `+10/+14` 的 `0x188` entries，按 entry `+84/+88/+90` 经 `terrain+94` 下传 | Same | Same | 行为 Confirmed |
| 5 | `0/0/0` | `slot2()!=0 && this+8==0` | Same | Same | Confirmed |
| 6 | `4/4/4` | 固定返回1，不消费this | 独立同值 stub | Same target as base | Confirmed |
| 7 | `8/8/8` | 空操作 | 转换两个参数后调 `0x6F75C5F0`，不消费this | base空操作 | Confirmed，业务名 Unknown |
| 8 | `12/12/12` | 写 `+D8..EC`；按 `+40/+44` 索引模型，更新主/附属 model pose/animation，最后调 slot86 | 保留ECX wrapper到base | 保留ECX wrapper到base | 行为 Confirmed |
| 9 | `4/4/4` | `CWorldObjects_SubmitModelsToRenderQueue`：`+9C==0` 且slot5有效时，按 entry flags 向 RenderQueue 提交 `+58/+164` models | typed tail wrapper | typed tail wrapper | RenderQueue producer/ABI Confirmed |
| 10 | `4/4/4` | per-model update，经slot38填缓冲并调模型动画接口 | 按 model `+150` 做 birth/death 状态机 | 按 `+150/+148/+14C/+84/+58` 做另一状态机 | 行为 Confirmed；状态名部分 Inferred |
| 11 | `4/4/4` | 空 | 遍历 entries，查询 terrain 并向输出 `+4/+8` 写两个计数 | 空 | 行为 Confirmed，领域 Unknown |
| 12 | `8/8/8` | 第二参数指向值写 `-1` | 写第一参数所指 model `+14C` | base行为 | Confirmed |
| 13 | `0/0/0` | 返回0 | 返回1 | 返回0 | Confirmed |
| 14 | `8/8/8` | 空 | 用 terrain 与 model `+4/+0C/+18/+A0/+A4/+A8` 进入注册/更新链 | 空 | 行为 Confirmed，领域 Unknown |
| 15 | `4/4/4` | 空 | `CDoodads_ResolveModelColorRaw`：从 `doodadDb/destructableDb` 与 embedded hash table 解析并写 model `+9C` | 空 | model-color raw role/ABI Confirmed |
| 16 | `8/8/8` | 返回0 | 返回1 | 返回0 | Confirmed |
| 17 | `8/8/8` | 向输出写全局默认值 | 先调slot18，再按 model flags 查两个DB并写输出 | base行为 | Confirmed |
| 18 | `4/4/4` | 返回0 | 查询 doodad DB 后 destructable DB | 返回 `arg==0` | Confirmed |
| 19 | `4/4/4` | 返回0 | `return sub_6F7521F0(arg,1)`，不消费this | 返回0 | Confirmed |
| 20 | `12/12/12` | 返回1 | 用 destructable DB、全局门和三参数做存在性/许可判断 | 返回1 | 行为 Confirmed，领域 Unknown |
| 21 | `4/4/4` | 空操作 | Same | Same | Confirmed |
| 22 | `0/0/0` | 返回0 | 返回1 | 返回0 | Confirmed |
| 23 | `12/12/12` | 返回1 | 按版本 `>=7/8` 序列化 model/custom 尾字段 | 返回1 | 行为 Confirmed，格式名 Unknown |
| 24 | `12/12/12` | 返回1 | slot23 对应的版本化反序列化 | 返回1 | 行为 Confirmed，格式名 Unknown |
| 25 | `8/8/8` | 空 | custom record 写 model `+DC/+E0/+EC`，调 `0x6F75C5F0` 后slot30 | 空 | Confirmed |
| 26 | `0/0/0` | 返回字符串 `".doo"` | Same | Same | Confirmed |
| 27 | `8/8/8` | 返回0 | 遍历两个DB目录、Storm字符串比较并写输出 index/flag | 返回0 | 行为 Confirmed |

slots 20/23/24 的 base/Puffs 目标被 IDA 误识别成 `_DllMain@12_*`；真实 ASM 只是返回1的
`retn 0xC` stubs。slot19 的 Doodads 目标也不是 PPL lock。

## 3. `CBlightPuffs` 八个真实 overrides

| slot | target | stack | 行为 |
|---:|---:|---:|---|
| 0 | `0x6F74CC40` | 4 | scalar deleting dtor；进入 base dtor |
| 8 | `0x6F74E8F0` | 12 | 保留 ECX 的 base-slot8 wrapper |
| 9 | `0x6F75ABF0` | 4 | tail 到 base slot9 |
| 10 | `0x6F759200` | 4 | 不消费this；state0 尝试 `birth`/fallback `stand` 后置1；state1 在 elapsed-ms `+148` 超过 duration-ms `+14C` 时播 `death` 置2，否则 `stand`；state2 清 flag `0x01000000` |
| 18 | `0x6F756460` | 4 | 返回 `arg==0` |
| 47 | `0x6F753800` | 12 | 用 `"Blight"/"PuffModel"` 查询配置；不消费this |
| 67 | `0x6F756310` | 4 | 返回 `byte(model+0x87)&1` |
| 86 | `0x6F758AD0` | 12 | `CBlightPuffs_AccumulatePuffTimeMs`：`deltaSeconds*1000` 后遍历 model indices，对 flag `0x01000000` 累加 model `+0x148` |

其他 95 槽与 base 使用完全相同的目标地址。

## 4. slots 28..102：机械 ABI 与直接 this-field 访问

此表已覆盖每一个余下槽。`offsets` 只列由传入 ECX/其已证明别名直接访问的常量偏移；不列
栈参数所指对象的字段。slots28..77 已由真实 caller、字符串、DB 字段和 terrain-image 注册链
进一步闭合；表中保留 `Unknown` 的槽不能因相邻槽已有自然语言名而外推。

| slot | stack B/D/P | this B/D/P | D/base/Puffs direct offsets或控制流 | 语义 |
|---:|---|---|---|---|
| 28 | `4/4/4` | `-/Y/-` | base/Puffs no-op；D 按 entry `+4` 从 `+F4/+F8` DB 取 flags，精确改写 entry `+84` 的 `0x01000000/4/0x40/0x02000000/0x80/0x8000`，再调 slot14 与 refresh helper | entry DB flags 初始化/dataflow Confirmed；逐 bit 业务名 Unknown |
| 29 | `4/tail/4` | `-/Y/-` | base/Puffs no-op；D 原样 tail-call `this->slot30(entry)` | forwarding contract Confirmed |
| 30 | `4/4/4` | `-/-/-` | base/Puffs no-op；D 调 helper `(this,entry,0)`，移除 entry `+164`、stride `0x5C` 中 key@`+8==0` 的辅助记录 | removal dataflow Confirmed；记录类型 Unknown |
| 31 | `8/8/8` | `-/Y/-` | base/Puffs no-op；D 在 `this+98!=0` 时以 entry `+158` 形成 `gg_dest_*` 名并作 pre-removal probe；第二 context 参数未消费 | generated-name probe Confirmed；`+98` owner/业务目的 Unknown |
| 32 | `4/4/4` | `-/-/-` | 三表共享 `retn 4` no-op | cleanup-begin caller position Confirmed；接口业务名 Inferred |
| 33 | `4/4/4` | `-/-/-` | 三表共享固定返回 true | destroy-entry predicate caller position/return Confirmed |
| 34 | `4/4/4` | `-/Y/-` | base/Puffs no-op；D 的 entry-activated 路径执行可选 `+114` callback 与 time-of-day/state refresh | activation caller/dataflow Confirmed；callback 类型与 TOD 字段名 Unknown |
| 35 | `0/0/0` | `-/-/-` | 三表共享 1-byte `ret` | destroy-complete caller position Confirmed；业务名 Inferred |
| 36 | `0/0/0` | `-/-/-` | 三表共享 1-byte `ret`；caller 先清 manager `+9C` | object-file-load-complete caller position Confirmed；`+9C` 精确状态名 Inferred |
| 37 | `0/0/0` | `-/Y/-` | D: `CDoodads_RefreshDestructibleHeightMapToggle`，同步 debug flag index 6 到 `+10C`，变化时 refresh models | D behavior/ABI Confirmed |
| 38 | `12/12/12` | `-/-/-` | 共享目标把默认 animation name 写为精确字符串 `"stand"` | output/string behavior Confirmed |
| 39 | `4/4/4` | `-/-/-` | 三表共享固定 false；已证 caller 据此清 entry `+84` bit `0x20` | predicate/consumer effect Confirmed；接口名 Unknown |
| 40 | `12/12/12` | `-/Y/-` | base/Puffs false；D 从 `+F8` destructable DB 输出 object display/editor name | D output source Confirmed；公开字段名 Inferred |
| 41 | `12/12/12` | `-/Y/-` | base/Puffs false；D 从 `+F8` destructable DB 输出 pathing-texture path | D output source/string domain Confirmed |
| 42 | `4/4/4` | `-/Y/-` | base/Puffs true；D 由 `+F8` DB 判定 entry pathing 是否可 toggle | predicate/data source Confirmed |
| 43 | `4/4/4` | `-/-/-` | 三表共享固定 true | return/ABI Confirmed；业务名 Unknown |
| 44 | `4/4/4` | `-/-/-` | 三表共享固定 false | return/ABI Confirmed；业务名 Unknown |
| 45 | `4/4/4` | `-/-/-` | base/Puffs false；D true | return/override relation Confirmed；业务名 Unknown |
| 46 | `16/16/16` | `-/-/-` | 三表共享固定 true；唯一已闭合 caller 只在 manager slot13 返回 2 的分支调用 | caller partition/return Confirmed；四参数业务域 Unknown |
| 47 | `12/12/12` | `-/Y/-` | base 输出 empty variation path；D 从 destructable DB 输出 variation model path；Puffs 查询 `Blight/PuffModel` | output domain/strings Confirmed |
| 48 | `12/12/12` | `-/Y/-` | base/Puffs false；D 从 `+F8` DB 输出 destructable static-shadow resource name | D output source/domain Confirmed |
| 49 | `4/4/4` | `-/-/-` | base/Puffs false；D 对 name-based static-shadow route 返回 true | caller route/return Confirmed；predicate 自然语言 Inferred |
| 50 | `12/12/12` | `-/-/-` | 三表共享 false，不提供 static-shadow rectangle outputs | caller/output contract Confirmed |
| 51 | `4/4/4` | `-/-/-` | 三表共享 false：static-shadow image 初始不可见 | caller/return contract Confirmed |
| 52 | `4/4/4` | `-/-/-` | 三表共享 false：selection-circle image 初始不可见 | raw caller `0x6F74DA00`/return contract Confirmed |
| 53 | `12/12/12` | `-/-/-` | shared false stub；caller 传 `(entry,outPath,0x104)` 并测试返回 | emitter-stamp resource/path query role Inferred；公开名 Unknown |
| 54 | `4/4/4` | `-/Y/-` | base/Puffs ST0=0；D 对 destructable 从 `+F8` DB 取 `occH` | `GetOcclusionHeight` source/caller Confirmed |
| 55 | `4/4/4` | `-/Y/-` | base/Puffs 返回1；D 按 entry class 从 DB 取 `numVar` | variation count source/caller Confirmed |
| 56 | `4/4/4` | `-/Y/-` | base/Puffs false；D 从相应 DB 取 `tilesetSpecific`；另有 full-entry refresh caller | source/predicate/caller Confirmed |
| 57 | `4/4/4` | `-/-/-` | 共享 false；model-create caller仅在 texquality cache允许时据 true 增加 flag `0x8` | caller effect Confirmed；接口名 Unknown |
| 58 | `4/4/4` | `-/Y/-` | base/Puffs no-op；D 刷新 destructable texture/lifecycle，按 terrain query 写 entry `+0x148=1/2` | Blight texture/state dataflow Confirmed |
| 59 | `4/4/4` | `-/-/-` | base/Puffs no-op；D 不消费 this，写 entry `+0x148=0` | destructable Blight terrain-state reset Confirmed |
| 60 | `4/4/4` | `Y/Y/Y` | base/Puffs由 model bounds 算 selection size；D 优先 DB `selSize`，否则 fallback | selection size source/callers Confirmed |
| 61 | `12/12/12` | `-/Y/-` | D 对 destructable/model 比较 old/current XY terrain-query ID，变化时调 slot58；change-mask与bulk callers已证 | position-change hook Confirmed；第三参数名 Unknown |
| 62 | `12/12/12` | `-/-/-` | 三类 no-op（D 独立 target）；facing update 与 change-mask bit2 callers | facing-change hook caller role Confirmed；第三参数名 Unknown |
| 63 | `12/12/12` | `-/Y/-` | D 保留 incoming this，并以其调用 `0x6F75DDD0`；caller `0x6F75CA00` 在 change-mask bit `0x10` 时传 `(entry, old entry+8 variation, 0)` | variation/style change hook dataflow Confirmed；业务名 Unknown |
| 64 | `8/8/8` | `-/-/-` | no-op；D 独立 target；唯一 caller 在 feature mask bit3 时传 `(entry,this+0xD4)` | caller partition Confirmed；hook与 `+D4` 语义 Unknown |
| 65 | `8/8/8` | `-/-/-` | shared no-op；除三张 vtable dref 无 code caller | ABI Confirmed；caller/参数 Unknown |
| 66 | `4/4/4` | `-/-/-` | shared no-op；除三张 vtable dref 无 code caller | ABI Confirmed；caller/参数 Unknown |
| 67 | `4/4/4` | `Y/Y/-` | base: flag `0x400` 时返回 `+B0` 否则1；D flag `0x08000000` hard false否则tail base；Puffs返回 entry high-byte bit0 | raw activation gate Confirmed；公开枚举/接口名 Unknown |
| 68 | `4/4/4` | `-/Y/-` | base/Puffs ST0=1；D 从对应 DB 取 `maxScale` | scale upper-bound source/caller Confirmed；direct `+110` callback已排除 |
| 69 | `4/4/4` | `-/Y/-` | base/Puffs ST0=1；D 从对应 DB 取 `minScale` | scale lower-bound source/caller Confirmed；direct `+114` callback已排除 |
| 70 | `4/4/4` | `-/Y/-` | base/Puffs true；D 返回 `!destructableDb.Contains(typeId)` | non-uniform scale gate role Confirmed |
| 71 | `4/4/4` | `-/Y/-` | base/Puffs ST0=1；D destructable=1、doodad DB `defScale`；near-zero强制1 | default model scale source/consumer Confirmed |
| 72 | `4/4/4` | `-/-/-` | shared ST0=0；caller写 entry `+0x5C`，transform 加到 world Z | model Z offset consumer role Confirmed；公开名 Inferred |
| 73 | `4/4/4` | `-/-/-` | shared ST0=0；无 pathing footprint时写 entry `+0x64` | planar fallback radius consumer role Confirmed；公开名 Inferred |
| 74 | `4/4/4` | `-/Y/-` | base/Puffs ST0=0；D 从对应 DB 取 `maxPitch` | DB field/transform consumer Confirmed；单位 Unknown |
| 75 | `4/4/4` | `-/Y/-` | base/Puffs ST0=0；D 从对应 DB 取 `maxRoll` | DB field/transform consumer Confirmed；单位 Unknown |
| 76 | `4/4/4` | `-/Y/-` | base/Puffs ST0=0；D 取 destructable `radius` / doodad `visRadius` | terrain-alignment/cull consumer Confirmed；公开接口名 Unknown |
| 77 | `4/4/4` | `-/Y/-` | 与 slot76 同源但独立接口；sample ring、slot80量化与cull消费 | source/consumer Confirmed；不得与 slot76 合并 |
| 78 | `4/4/4` | `-/-/-` | base/Puffs 固定返回 1；D 若 debug toggle6 关闭且 `modelArg+0x87` bit0=1 返回 3，否则 1 | 返回码合同 Confirmed；码值业务名 Unknown |
| 79 | `4/4/4` | `-/-/-` | 三表共享固定返回 0 | Confirmed stub；业务名 Unknown |
| 80 | `4/4/4` | `-/Y/-` | base/Puffs 固定 `-1`；D 仅 toggle6 开且 model bit0 时调自身 slot77 得 float，再按 `value/128` clamp upper 3，否则 `-1` | 数值算法 Confirmed；业务名 Unknown |
| 81 | `8/8/8` | `Y/Y/Y` | 共享：`(model*, out48)`；调用 maxPitch/maxRoll/radius slots74/75/76、用 `CTerrain*+0x94` 采样并生成 terrain-aligned 48-byte transform，同时更新 model `+0x84` bit `0x10000` | ABI/source/dataflow Confirmed；bit/公开接口名 Unknown |
| 82 | `12/12/12` | `-/-/-` | 共享：不消费 this；由 model position/scale 与 `+0x68..+0x7C` 两组三维边界写两个 vec3 outputs | bounds math Confirmed；字段自然语言名部分 Unknown |
| 83 | `0/0/0` | `-/-/-` | 三表共享固定返回 `200 (0xC8)` | Confirmed constant；单位/业务名 Unknown |
| 84 | `4/4/4` | `-/-/-` | base/Puffs 固定 0；D 返回 `!(modelArg+0x87 bit0)` | predicate Confirmed；bit 名 Unknown |
| 85 | `4/4/4` | `-/Y/-` | base/Puffs 固定 0；D 仅 model bit0=1 时以 `+0xF8` DB 和 `model+4` 调 helper，否则 0 | branch/DB owner Confirmed；helper 语义 Unknown |
| 86 | `12/12/12` | `-/-/Y` | base/D 为 `retn 0xC` no-op；Puffs 把第三参 seconds 转 ms，对 flag `0x01000000` entries 累加 `+0x148` | Confirmed |
| 87 | `16/16/16` | `Y/Y/Y` | base/Puffs 先用 `CTerrain+0xC4..+0xD0` gate 二维点，再执行 query；D 在同 gate 后按 model bit0 从 `+0xF4/+0xF8` DB 补查询/fallback | control/dataflow Confirmed；query 业务名 Unknown |
| 88 | `8/8/8` | `Y/Y/Y` | 共享 helper query：构造临时 `0x180`-class context，以两个显式参数执行 `sub_6F7520C0`，返回其结果取反式 bool | ABI/boolean contract Confirmed；业务名 Unknown |
| 89 | `8/8/8` | `-/Y/-` | base/Puffs 固定 true；D 对 model 临时设 `+0x84` bit `0x4000`，复制状态并以第二参替换首 dword，调用 owner helper，随后清 bit 并恢复 model state | mutation bracket Confirmed；属性名 Unknown |
| 90 | `0/0/0` | `-/-/-` | base/Puffs 固定 false；D 固定 true | Confirmed predicate；业务名 Unknown |
| 91 | `8/8/8` | `-/-/-` | 三表共享 `retn 8` no-op | Confirmed no-op；返回无合同 |
| 92 | `8/8/8` | `-/-/-` | 三表共享 `retn 8` no-op | Confirmed no-op；返回无合同 |
| 93 | `4/4/4` | `-/-/-` | 三表共享 `retn 4` no-op | Confirmed no-op；返回无合同 |
| 94 | `4/4/4` | `-/-/-` | 三表共享固定返回 0 | Confirmed stub；业务名 Unknown |
| 95 | `4/4/4` | `-/-/-` | 三表共享固定返回 0 | Confirmed stub；业务名 Unknown |
| 96 | `4/4/4` | `-/Y/-` | base/Puffs 固定 0；D 按 model bit0 选择 `+0xF8/+0xF4` DB，以 `model+4` 调不同 helper | DB dispatch Confirmed；返回/helper 语义 Unknown |
| 97 | `4/4/4` | `-/Y/-` | base/Puffs 固定 0；D 仅 model bit0=0 时以 `+0xF4` DB、`model+4` 调 helper，否则 0 | DB dispatch Confirmed；业务名 Unknown |
| 98 | `0/0/0` | `-/Y/-` | base/Puffs no-op；D 遍历 `+0x10` count、`+0x14` array 的全部 `0x188` entries，逐项调 `sub_6F75D8F0(this,entry,0)` | full-entry sweep Confirmed；helper 语义 Unknown |
| 99 | `0/0/0` | `-/-/-` | base/Puffs 固定 false；D 固定 true | Confirmed predicate；业务名 Unknown |
| 100 | `8/8/8` | `-/-/-` | 三者虽地址不同但均固定返回 0 | Confirmed behavioral equality；业务名 Unknown |
| 101 | `8/8/8` | `-/Y/-` | base/Puffs 固定 0；D 仅第二参为 0 时返回 `this+0xD0`，否则 0；第一参未读 | query contract Confirmed；`+0xD0` 语义 Unknown |
| 102 | `8/8/8` | `-/-/-` | base/Puffs `retn 8` no-op；D 仅第二参为 0 时把第一参交给 `sub_6F74D2E0`，接口返回值无稳定合同 | conditional side effect Confirmed；业务名 Unknown |

## 5. 下一轮优先级

1. 先审 Doodads 独有且直接读取 `+F4/+F8/+118/+10C` 的 slots；这些最可能闭合 DB/hash 组合
   关系和 model record 字段。
2. 再审共享的大函数 slots 81/87/88，避免把 base 语义误写成 Doodads 特有语义。
3. 对所有短 stub 先查 caller 对返回值的使用，再命名接口；固定 0/1 不足以识别业务名。
