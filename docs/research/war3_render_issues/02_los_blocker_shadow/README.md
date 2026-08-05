# 研究方向二：LOSBlocker 阴影误渲染

## 问题定义
`Doodads\\Terrain\\LOSBlocker\\LOSBlocker` 在玩家视角不可见（单面朝下），但被 ShadowCapture 采集，造成不应出现的阴影污染。

## 现网修复策略
代码位置：`src/d3d9/d3d9_device.cpp`

## 魔兽争霸3原版所有阻断器四字码
'YTab','YTac','YTpb','YTpc','YTfb','YTfc','YTlb','Ytlc'

已实现双通道过滤：
1. `rawcode` 白名单过滤（`YTab/YTac/YTpb/YTpc/YTfb/YTfc/YTlb/YTlc`）。
2. `rawcode` 不可用时，回退到 `Sprite->Model` 路径标记过滤。
3. 新增 `batchHandle -> RenderObjectRegistry::findByHandle` 回查，缓解 `currentObj` 丢失导致的过滤漏判。
4. 新增 `LastRenderHandle` 回退（仅世界对象相关 tag），缓解 TLS 句柄偶发丢失。
5. 即便 `unitPtr` 缺失，也允许按 `rawcode` 直接判定路径阻断器。
6. 修复句柄污染回归：将 `batchHandle` 拆分为
   - `strictBatchHandle`（仅用于描边/高亮/ShadowReceiver 匹配）
   - `lookupHandle`（仅用于 LOSBlocker 回查，可用 LastRenderHandle 兜底）
   避免 `LastRenderHandle` 反向污染导致“全体目标被描边”。
7. `rawcode` 判定增加第二字符大小写归一化，兼容 `YTlc / Ytlc` 变体。
8. 阴影投影器侧 FourCC 黑名单补齐：`YTab/YTac/YTpb/YTpc/YTfb/YTfc/YTlb/YTlc`。

配套能力：
- `src/d3d9/war3/model/war3_model_hook.cpp` 新增 `IsPathBlockerSprite(void*)`。
- 在 `Hook_SpriteSetModel` 记录 sprite 的路径阻断器标记。
- `kPathBlockerHideEnabled` 已改为默认 `true`，确保默认生效。
- `kPathBlockerForceBridgeTrackingEnabled` 默认开启：强制保留对象桥接。

## 2026-02-22 收敛结论（方向二可结项）
1. 阻断器阴影屏蔽已达成：原版 8 个四字码
   `YTab/YTac/YTpb/YTpc/YTfb/YTfc/YTlb/Ytlc`
   已在 ShadowCapture 前稳定拒绝进入阴影采集。
2. FourCC 判定已统一到编辑器顺序，并兼容运行时字节序差异与
   `YTlc / Ytlc` 大小写变体。
3. 默认策略已启用：`kPathBlockerHideEnabled=true`、
   `kPathBlockerForceBridgeTrackingEnabled=true`。
4. 实测回归通过：日志可稳定观测 `YTxx`，且阻断器不再出现在阴影结果中。

## 2026-07-08 复核修正：FourCC 不是唯一入口

用户提供 `LOSBlocker.mdl` 与专用图 `E:\Work\War3\Maps\ShadowTest\光影测试.w3x`
后重新验证，确认阻断器在正常渲染中不可见，但仍可能作为 CSM caster 进入 Stage 11 世界对象路径。
这一轮关键发现是：**不能只依赖 FourCC**。

实机日志同时出现两类命中：

- 有对象身份的阻断器：`YTfb/YTpb/YTab/...`，可走 rawcode/FourCC/模型路径过滤。
- 丢失对象身份的阻断器：`rawcode=0 / jHandle=0 / stage=11 / vtx=4 / idx=6`，
  是 `LOSBlocker.mdl` 那种小型不可见平面进入 CSM 后的匿名形态。

当前生产默认因此收窄成三层：

1. 稳定身份优先：FourCC、模型路径、对象桥接命中直接拒绝。
2. 禁止 broad anonymous rigid gate 默认开启，避免误杀真实单位子网格。
3. 只保留极窄匿名小平面指纹：`vtx<=8 / idx<=12 / stage=11 / rigid / no alpha / no handle`，
   用来处理 `LOSBlocker.mdl` 这类 rawcode 断链后的 CSM 残留。

这解释了此前“把 `hfoo` 放进黑名单可以杀步兵，但阻断器杀不掉”的现象：步兵有稳定 rawcode，
而阻断器在漏网路径里已经退化成 rawcode=0 的匿名小平面。

## 验收口径（结项）
1. 含 LOSBlocker 地图下，阻断器不进入阴影。
2. 非阻断器单位/建筑阴影保持正常。
3. 无“全体描边”回归。

## 2026-02-21 夜间回归结论
实测现象：
- 建筑贴花已恢复；
- 但 LOSBlocker 仍“0 命中拦截”。

根因定位：
- 之前“强制桥接”仅在 `Decorations` 路径生效；
- 但 LOSBlocker 实际可出现在 `WorldObjects/S11` 路径，导致 `pathBlockObj` 为空，阴影采集无法命中黑名单。

已追加修复：
1. `ComputeNeedsObjectTracking()` 在 `PathBlockerHide + ForceBridge` 下强制返回 true。
   - 文件：`src/d3d9/war3/render/war3_render_state.cpp`
2. `ExecBatch` 的 `needsPathBlockerBridge` 扩展到
   `WorldObjects/SelectionOverlay/Decorations/RangeIndicatorTarget`。
   - 文件：`src/d3d9/war3/render/war3_render_exec_batch.cpp`
3. `SceneCollector` 的 `pathBlockerTrackAll` 从“仅 group2”扩展为全组生效。
   - 文件：`src/d3d9/war3/render/war3_scene_collector.cpp`
4. `ShadowCapture` 继续保留前置 PathBlocker 过滤 + 句柄缓存。

新增诊断日志：
- `DXVK: PathBlockerShadow stats ... noObj=... strict=... lookup=...`
- 用于直接观察“是否仍因为对象信息缺失而漏判”。

## ASM 研究结论（模型链）
已确认以下链路可用于继续根治：
- `0x6F6A32F0`：模型路径哈希查找链（含 `ComputeHash` 和哈希桶命中）。
- `0x6F66BA00`：模型资源注册链，出现 `*.mdx` / `*_portrait.mdx` 和 `RegisterShadow/RegisterStructureShadow/RegisterUberSplat` 相关调用。

这条链路后续可用于“在模型注册阶段直接标记不可投影对象”，进一步减少运行时判断开销。

## 维护项（结项后）
1. 对第三方地图自定义阻断器资源做抽样，必要时扩充 FourCC/模型黑名单。
2. 保留低频 PathBlocker 统计日志用于回归抽检。
3. 若后续出现模型替换导致漏判，优先走模型注册期标记缓存补洞。

## 2026-02-22 桥接验证专项（已完成）
目标：在渲染阶段验证“对象桥接是否真的拿到逻辑层 FourCC”，并保证日志开销可控。

实现位置：
1. `src/d3d9/war3/core/war3_internal_test_config.h`
   - `kBridgeRawcodeOneShotLogEnabled=true`
   - `kBridgeRawcodeForceTrackAllEnabled=true`
2. `src/d3d9/war3/render/war3_scene_collector.cpp`
   - 验证期强制 `forceTrackAll`，避免只收 tracked handle 导致样本不全。
3. `src/d3d9/war3/render/war3_render_objects.cpp`
   - 新增 `jHandle -> rawcode` 缓存桥接；
   - 解析优先级：
     - `unitPtr` 直读（最快）
     - `handle` 缓存命中
     - `handle` 首次解析（`HandleResolver -> AgentWrapper -> UnitWrapper`）
   - 对每种 `rawcode` 仅打印一次日志（全进程去重）。

日志格式：
- `DXVK_BRIDGE_RAWCODE: fourcc='XXXX' (0xXXXXXXXX) firstSeen handle=... group=... kind=... source=unitPtr|handleCache|handleResolve`

使用建议：
1. 先在验证图里完整跑一局，收集所有 `DXVK_BRIDGE_RAWCODE` 日志。
2. 对照地图对象编辑器确认 FourCC 覆盖是否完整。
3. 验证结束后建议关闭：
   - `kBridgeRawcodeOneShotLogEnabled=false`
   - `kBridgeRawcodeForceTrackAllEnabled=false`
   以恢复默认性能路径。

## 已知遗留（独立问题，非本方向阻塞）
1. 描边对模型透明区域仍可能产生轮廓；根因是当前 `OutlineMask` 的
   `All Mask` 设计默认忽略 AlphaTest
   （`subprojects/war3fx/shaders/war3_outline_mask.frag` 中 `o_all = 1.0`）。
2. 该问题已转入描边专项，不阻塞 LOSBlocker 方向结项。
3. 后续可选路线：引入 alpha-aware 的 `All Mask` 或“外轮廓优先”混合策略，
   再做性能评估后落地。

## 2026-02-21 夜间第四轮修正（用户回归问题对齐）
1. **PathBlocker 日志不显示修复**：
   - `PathBlockerShadow stats` 从“仅 `kPathBlockerDebugEnabled`”改为
     “`kPathBlockerStatsLogging` 或 `kPathBlockerDebugEnabled` 任一开启即可打印”。
   - 统计打印频率从 `8000` 降到 `2000`，并在 debug 模式下提供前 64 次高频打印。
   - 文件：`src/d3d9/d3d9_device.cpp`。
2. **描边回归修复（目标不命中）**：
   - `SceneCollector` 的过滤模式恢复“直接 handle 值”路径，但要求必须命中 tracked handle 才接收。
   - 这样既避免“全体描边”误判，又修复 `TAG=1` 场景下目标无法描边的问题。
   - 文件：`src/d3d9/war3/render/war3_scene_collector.cpp`。
3. **静态阴影上游诊断可见性增强**：
   - `ShadowUpdateWrite stats` 打印频率从 `10000` 降到 `3000`，短窗口压测也能看到回调统计。
   - 文件：`src/d3d9/war3/hooks/war3_hook_shadow.cpp`。

## 2026-02-21 夜间第五轮修正（针对 noObj/raw=0）
根据实测日志：
- `PathBlockerShadow stats` 中 `blocked=0`、`raw=0x00000000`；
- `noObj` 持续增长，说明大量 draw 没拿到可判定对象数据。

本轮修复：
1. **解耦 PathBlocker 全量追踪与描边过滤模式**
   - 文件：`src/d3d9/war3/render/war3_scene_collector.cpp`
   - 变更：
     - `filtered` 不再被 `pathBlockerTrackAll` 关闭；
     - 增加 `keepForPathBlocker`，允许 PathBlocker 需求下保留非 tracked 对象；
     - 过滤模式下保持 tracked 指针反查（避免描边 handle 错位），仅在 PathBlocker 全量追踪时补充句柄推断用于桥接。
2. **ShadowCapture 增加 handle 直解兜底**
   - 文件：`src/d3d9/d3d9_device.cpp`
   - 变更：
     - 当 `currentObj/findByHandle` 无法提供 `unit/rawcode` 时，新增
       `lookupHandle -> HandleResolver::resolveHandle -> AgentWrapper::GetUnitPtr -> UnitWrapper` 链路；
     - 兼容“handle 表直接存 CUnit*”场景。
3. **PathBlocker 统计日志新增兜底命中率**
   - 文件：`src/d3d9/d3d9_device.cpp`
   - 新字段：`fallback=resolved/try`。
4. **修复 PathBlocker 负缓存污染**
   - 文件：`src/d3d9/d3d9_device.cpp`
   - 变更：
     - 仅在“有有效判定输入（rawcode 或 unit 可读）”时才写入 PathBlocker 缓存；
     - 避免 `noObj/raw=0` 时把 `false` 写进缓存，导致后续长期 `cacheHit` 但永不命中 blocker。
5. **HandleResolver 懒初始化补偿**
   - 文件：
     - `src/d3d9/d3d9_device.cpp`
     - `src/d3d9/war3/render/war3_scene_collector.cpp`
   - 变更：
     - 在热路径检测到 `HandleResolver` 未就绪时，使用 `GetModuleHandleA(\"Game.dll\")` 进行一次懒初始化重试；
     - 用于修复“启动时机过早导致 resolver 初始化失败后长期不可用”的问题。
6. **lookupHandle 回退范围扩展**
   - 文件：`src/d3d9/d3d9_device.cpp`
   - 变更：
     - `LastRenderHandle` 回退从仅 `Decorations` 扩展到 `WorldObjects/Unknown`；
     - 目的：减少 `lastTag=-1` 时 `lookup=0` 导致的长期 `noObj`。

验证重点（本轮新增）：
1. 观察 `PathBlockerShadow stats` 中 `fallback=...` 是否增长。
2. 若 `try` 增长但 `resolved` 长期接近 0，说明当前 `lookupHandle` 不是可解析的 JASS handle，需要回到 ASM 确认字段语义。
3. 若 `resolved` 增长但 `blocked` 仍为 0，需继续采样 `rawcode` 并扩充 blocker 规则。

## 验证建议
1. 开启 `kPathBlockerDebugEnabled` 观察 `pathBlocker/byModel` 命中来源。
2. 关注日志中的 `fromLast=` 与 `pathblocker-skip(no object info)`，确认句柄回退与对象桥接是否命中。
3. 对含大量路径阻断器地图比对前后阴影差异。
4. 观察 `ShadowCapture` 次数是否同步下降。
