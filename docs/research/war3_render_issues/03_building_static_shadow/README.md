# 研究方向三：建筑静态阴影无法屏蔽

## 2026-07-08 最终稳定结论：UnitUI buildingShadow 写入端治理
本轮 x32dbg + IDA + 实机日志确认，建筑底部那类原生静态阴影的最高价值治理点不是
`CUnit+0x50`、`ListA/ListB`、`RegisterImage`、`StaticStampPath` 或
`WriteMaskRegion`，而是 **UnitUI 类型记录的 `buildingShadow` 字段写入点**：

- 函数：`CUnitUIManager_RecordSetStructureShadow`
- IDA VA：`0x6F335A00`
- RVA：`0x335A00`
- 写入点：`0x6F335A65`，将解析后的 shadow 字符串/资源指针写到 type record `+0x50`
- 调用来源：`CUnitUIManager_LoadSlkRows(0x6F66BA00)`，callsite `0x6F66BF5F`
- SLK 字段描述：`buildingShadow` descriptor offset `1304`，`unitShadow` descriptor offset `1292`
- 类型记录偏移：`+0x4C = unitShadow`，`+0x50 = buildingShadow`，`+0x48 = UberSplat key`

关键纠偏：

- `CUnit+0x50` 不是 `buildingShadow` 字符串，而是运行期节点/列表类字段；
- `CUnit+0x28` 更接近 `CSprite*`，`CSprite+0x20` 可对应 scene node；
- 真正决定建筑默认静态阴影文件名的是 UnitUI/type record `+0x50`；
- `halt` 的默认值可落到 `ShadowAltarofKings`，`hctw` 可落到 `ShadowCannonTower`，
  实机还捕获到 `ShadowTreeofLife`。

生产方案已经落地：

- 默认安装 `CUnitUIManager_RecordSetStructureShadow` hook；
- 默认在 `NativeShadowMode=0` 时也阻断非空 `buildingShadow` 写入；
- hook 调原函数时传入 `shadowNamePtr=0`，保留原 setter 对旧 record 字段的释放/清理行为；
- direct-load/bootstrap 阶段提前安装，避免等 JASS 或插件中途加载时已经错过 SLK/type record 写入；
- 验证日志：`DXVK War3Hook: CUnitUI buildingShadow BLOCK calls=768 blocked=768 mode=0 ... name=ShadowTreeofLife`；
- 用户实机确认：建筑阴影完全不可见。

当前默认策略：

- 建筑静态阴影：以 `CUnitUIManager_RecordSetStructureShadow` producer gate 为唯一生产主路径；
- `TerrainShadow_RenderListB`：保留并默认全阻断，用来移除老版单位脚下黑色 blob/圆影；
- `WriteMaskRegion / StaticStampPath / RegisterImage / DoodadStamp` 等历史静态阴影实验默认退役，
  仅保留为证伪资料和必要时的专项诊断入口；
- `ListA` 末端过滤继续关闭，避免误伤战争迷雾、边界、悬崖/地形 tile。

后续若要支持“JASS 后中途加载插件”，必须注意：已经在 UnitUI/type record 中写入的
`buildingShadow` 不会被这条 producer gate 反向清除；因此稳定方案必须在 Game.dll 早期
bootstrap 或 D3D9 direct-load 阶段安装。

下面 2026-02 至 2026-05 的内容保留为历史证据链，其中多条假设已被本节结论取代。

## 2026-04-04 更正结论
1. 本页 2026-02-22 那段“`TerrainShadow_RegisterImageEntry(0x713250)` 是 ListA 静态建筑阴影核心上游入口”的表述，已经被后续更深一轮逆向推翻。
2. 当前高置信度结论是：
   - `0x713250` 更接近 `0xA0` 的 stamp/image 注册池；
   - `ListA` 建筑静态阴影真正更上游的主写入链是
     `ShadowPath_StaticStamp_Toggle(0x74E420) -> ShadowStamp_WriteByName(0x713B20) -> ShadowStamp_WriteCore(0x713920) -> shared shadow mask -> ListA`。
3. 因此，这页里“以 RegisterImageEntry 为中心治理 ListA 静态阴影”的部分，应视为历史阶段结论，不再作为当前主事实基线。
4. 最新专题请优先看：
   - `../23_blob_shadow_lista_upstream_reverse/README.md`

## 问题定义
魔兽阴影选项关闭后，动态单位贴花会消失，但静态建筑/可破坏物阴影仍保留；与项目阴影叠加导致观感明显变差。

## ASM 关键证据
1. `TerrainShadow_RenderLayer`（`0x6F737620`）：
   - `arg_0(a2) != 0` 执行 `TerrainShadow_RenderListA`
   - `arg_4(a3) != 0` 执行 `TerrainShadow_RenderListB`
2. `CWorld_TerrainShadow_Dispatch`（`0x6F7378D0`）存在 **stage14 直调链路**：
   - 先调 `sub_6F7276D0` / `sub_6F7376B0`
   - 再直接调用 `TerrainShadow_RenderListB(this, 4, 0)`
   - 该链路不会经过 `TerrainShadow_RenderLayer(a3)`，仅改 `a3=0` 无法覆盖。
3. `TerrainShadow_RenderListB`（`0x6F737400`）内部先做 `cmp [entry], argType`，即按 `type` 分组渲染；
   - `argType=4` 与静态建筑阴影高度相关（本轮新增定向拦截点）。
4. `ShadowProjector_Add_FromObject`（`0x6F76D800`）：
   - 内部调用 `sub_6F76A490`，该函数对 `edx` 参数做“字符串哈希 + 字符串比较”查找，说明 `edx` 是 key 字符串。
   - xref 两条主来源：
     - `ShadowPath_ObjectProjector_Runtime`（`0x6F38D7A0`）
     - `ShadowPath_ObjectProjector_JassBridge`（`0x6F1DEEA0`）
5. `sub_6F713CA0`（投影写入）仅被 `Add_Simple / Add_FromObject` 调用（xref 已确认），
   因此 mode>=1 可在两个入口统一上游拦截，不必依赖末端 ListA/ListB 粗过滤。

## 本轮代码推进
代码位置：`src/d3d9/d3d9_war3_hook.cpp`

已新增：
1. `Hook_ShadowProjector_Add_FromObject`
2. `Hook_ShadowProjector_Add_Simple`
3. `Hook_ShadowUpdate_WriteEntry`（`0x73F7A0`，写入链路上游）
4. （可选）`Hook_Terrain_RenderListA`（默认关闭）
5. `Hook_Terrain_RenderListB`（`0x737400`，默认开启）
6. 安装地址解析与 Hook 安装：
   - `0x76D800` / `0x76D790`
   - `0x73F7A0`
   - `0x737400`
   - 调用来源识别地址：`0x38D7A0` / `0x1DEEA0`

## 已尝试方案清单（同步）
1. 末端粗拦截（已回退）：
   - `Projector mode>=1` 全拦截；
   - `ListA` 过滤。
   结果：会误杀战争迷雾/边界或建筑贴花，不可作为默认方案。
2. `ListB type=4` 定向拦截（暂未默认开启）：
   - 理论上命中静态阴影主链；
   - 但在部分场景存在误伤风险，当前默认关闭，先做上游证据采集。
3. `Projector key` 精确拦截：
   - 目前仅配置了 `OLAR`；
   - 实测不足以覆盖全部静态阴影来源。
4. `ShadowUpdate_WriteEntry` 回调拦截框架：
   - 已具备按 callback RVA 精确封堵能力；
   - 目前缺“稳定的目标回调集合”，因此尚未默认启用封堵。

## 2026-02-21 夜间新增诊断
1. `ShadowUpdate_WriteEntry` 增加 callback 频次 Top 统计：
   - 日志：`DXVK War3Hook: ShadowUpdate cbTop ... top1=...`
   - 目的：找到静态阴影主写入回调，避免盲拦截。
2. `ShadowProjector_Add_FromObject` 增加 key 采样：
   - 日志：`Projector key sample[...] = 'xxx'`
   - 目的：补齐需要加入黑名单的 ubersplat key。

## 拦截策略
优先级从高到低：
1. `kNativeShadowBlockAllProjectorEnabled`
2. `kNativeShadowBlockProjectorFromObjectEnabled` / `kNativeShadowBlockProjectorFromAltEnabled`
3. `kNativeShadowBlockBuildingProjectorEnabled` + `mode>=1` + Runtime/Alt 来源（默认开启）
4. `kNativeShadowBlockBuildingProjectorEnabled` + key 命中 `kNativeShadowBlockedUbersplatKeys`
5. `kNativeShadowBlockUpdateByCallbackEnabled` + `kNativeShadowBlockedCallbackRva`（写入链路精确拦截）

默认值调整：
- `kNativeShadowBlockBuildingProjectorEnabled = true`

新增策略（本轮）：
- `TerrainShadow_RenderListA` 默认不再安装 Hook（避免在渲染末端误杀雾/边界）。
- 在 `ShadowUpdate_WriteEntry` 输出 callback RVA/参数低频统计，可直接在 DebugView 定位建筑静态阴影回调并按回调拦截。
- `ProjectorSimple` 增加桥接来源识别（`0x764AC0`），mode=1 下可定向阻断 simple 路径投影。
- `TerrainShadow_RenderListB` 新增 type 级拦截：
  - mode=1 默认拦截 `type=4`（静态建筑阴影主链路）；
  - mode>=2 可拦截全部 ListB，补齐“完全禁用原生阴影”在 stage14 直调链路上的漏网。
- 新增 ListB 分类型统计日志（`t1..t5` 调用/拦截计数），用于验证是否误伤雾/边界。
- `ShadowProjector_Add_FromObject / Add_Simple` 在 mode>=1 直接拦截对象投影写入，
  不再依赖返回地址范围判定，降低漏拦截概率。

修正（2026-02-21 夜间）：
- 已取消 `mode>=1` 对 `Projector` 的默认全拦截，改为“仅精确条件（key/FourCC）拦截”。
- 原因：全拦截会导致建筑贴花被误杀。

## 预期效果
在 `mode=1` 下，Runtime 对象投影路径会被抑制，建筑静态阴影叠加问题应显著缓解；同时保留后续按 key 精细化放行/屏蔽的能力。

## 2026-02-22（留言4）新增结论：静态阴影真正上游写入点
### 结论摘要
这轮扩大范围复核后确认：
1. 建筑/可破坏物静态贴花阴影并不只走 `ShadowProjector_Add_*`；
2. 其核心注册入口是 `TerrainShadow_RegisterImageEntry(0x713250)`；
3. 静态对象主来源来自：
   - `TerrainShadow_ToggleStaticStampFromObject(0x74DB30)`；
   - `TerrainShadow_ToggleEmitterStamp(0x74DF50)`。

因此，仅在 `ListA/ListB` 末端拦截会天然吃到“已混合条目”的问题；更稳妥路线应当在 `RegisterImageEntry` 上游按来源拦截。

### IDA 交叉验证要点
1. `0x713250` xrefs 覆盖：
   - `0x74DBF5`（位于 `ToggleStaticStampFromObject`）；
   - `0x74DF50`（位于 `ToggleEmitterStamp`）；
   - 以及 `0x7291D7 / 0x76D695 / 0x76D714` 等其他注册路径。
2. `CWorld_TerrainShadow_Dispatch(0x7378D0)` 的 stage14 直调 `RenderListB(type=4)` 依然存在，
   说明“渲染末端 type 过滤”只能兜底，不能替代上游注册过滤。
3. `ShadowProjector_Add_FromObject/Add_Simple` 仅覆盖对象投影器路径，
   不是静态贴花阴影的唯一写入来源。

### 本轮代码实现（已落地）
实现策略：新增 `TerrainShadow_RegisterImageEntry` Hook，并按返回地址识别来源函数进行精确拦截。

改动文件：
1. `src/d3d9/war3/hooks/war3_hook_address_book.h/.cpp`
   - 新增地址：
     - `shadowRegisterImageEntry = 0x713250`
     - `shadowToggleStaticStampFromObject = 0x74DB30`
     - `shadowToggleEmitterStamp = 0x74DF50`
2. `src/d3d9/war3/hooks/war3_hook_shadow.h`
   - `ShadowHookAddresses` 新增以上地址字段。
3. `src/d3d9/d3d9_war3_hook.cpp`
   - 安装阶段传递新地址到 `InstallShadowHooks()`。
4. `src/d3d9/war3/hooks/war3_hook_shadow.cpp`
   - 新增 `Hook_TerrainShadow_RegisterImageEntry`：
     - mode>=1 时按来源拦截：
       - 来源 `0x74DB30`（StaticStamp）；
       - 来源 `0x74DF50`（EmitterStamp）。
     - 命中后直接返回 `-1`，阻止写入 ListB 条目。
   - 保留统计/详细日志，用于实机验证命中率与影响面。
5. `src/d3d9/war3/core/war3_internal_test_config.h`
   - 新增开关（默认开启）：
     - `kNativeShadowRegisterImageHookEnabled`
     - `kNativeShadowBlockStaticStampRegisterWhenMode1`
     - `kNativeShadowBlockEmitterStampRegisterWhenMode1`
     - `kNativeShadowRegisterImageStatsLogging`

### 设计取舍
1. 这轮优先“按来源函数”拦截，而不是“按 ListB type”末端拦截；
2. 这样可以在注册阶段阻断静态阴影条目，避免渲染层混表后再做大范围误杀；
3. 保留原有 `ListB/UpdateWrite` Hook 作为回归兜底与观察手段，不作为主路径。

## 下一步
1. 跑实机日志，重点观察：
   - `DXVK War3Hook: RegisterImage stats ...`
   - `static/emitter` 来源计数与 `blocked` 是否持续上升；
   - `t4`（type=4）注册是否显著下降。
2. 对比画面：
   - 建筑/可破坏物静态贴花阴影是否消失；
   - 战争迷雾、边界、选择圈是否仍正常。
3. 若仍有漏网，再补“来源函数 + key + callback RVA”三维白/黑名单组合。

## 验证建议
1. `mode=1` 场景下对比建筑阴影是否消失。
2. 观察 `DXVK War3Hook: ShadowUpdateWrite stats ... lastCbRva=...`，确认回调分布。
3. 观察 `DXVK War3Hook: ListB stats ... t4=blocked/calls`，确认 type=4 命中与拦截是否稳定。
4. 观察雾/边界等非建筑元素是否仍符合预期。
5. 新增观察：`DXVK War3Hook: RegisterImage stats ...`，确认上游注册拦截是否命中 `static/emitter` 来源。

## 交叉索引
- 留言1~4统一归档：`../06_message_1_4_archive/README.md`
- JASS + 局部合并提交专项：`../05_jass_vm_and_partial_batch_submit/README.md`

## 2026-04-04 修正说明：ListA 混层 vs RegisterImage
这轮重新沿 `0x738ED0 -> 0x73DC00 -> 0x73D9F0` 和 `CTerrainUberSplats.cpp` 复核后，需要修正两点：

1. `0x713250(TerrainShadow_RegisterImageEntry)` 不是 `ListAEntry(0x94)` 的初始化函数。
   它更接近 `0xA0` 的 stamp/image 注册池入口。

2. `ListA` 本身是地形阴影/雾/边界/贴花的混合结果层。
   建筑静态阴影真正的高层写入者是：
   - `ShadowPath_StaticStamp_Toggle(0x74E420)`
   - `TerrainShadow_ToggleStaticStampFromObject(0x74DB30)`
   - `TerrainShadow_ToggleEmitterStamp(0x74DE40)`
   - `CTerrainUberSplats` 的 `WithParams` 路径

3. 这几条路径又被对象级总调度层统一控制：
   - `0x74D500`
   - `0x751290`
   - `0x759880`
   - `0x7599F0`
   - `0x75C5F0`

所以当前更准确的治理原则是：
1. 主治理点前移到 `StaticStampPath + RegisterImage 来源分类`；
2. `ListA` 末端继续只做保守兜底；
3. 不再把“从 ListA 里分离建筑阴影”当成主方向。

详见：`../23_blob_shadow_lista_upstream_reverse/README.md`
