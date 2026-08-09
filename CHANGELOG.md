# 更新日志

## v1.2003 Hotfix 3 - 2026-08-09

本版本开始采用 `1.2MHH` 编号：`1.2` 是大版本系列，`M` 是功能小版本，末两位
`HH` 是热修复序号。因此本轮正确性修复为 **1.2003**；完成剔除并形成正式性能更新时，
版本将进入 **1.2100**，而不是继续增加热修复号。

### 阴影正确性修复

- 修复不死族与暗夜精灵建筑建造过程中的动画附件阴影缺失或周期闪烁。Stage11 的
  Transparent Type0 现在保留父建筑的语义所有者身份，同时使用子 runtime model/scene node
  的真实 draw identity；固定功能蒙皮附件从同一绘制帧冻结精确 VB、IB、UV 与完整矩阵调色板，
  不再因为父子 `renderablePart` 不同而被误判为陈旧或静态对象。
- 该兼容路线不是恢复全局旧式 VB/IB 跨帧缓存。几何只在当前 Type0 绘制边界内捕获，仍须通过
  map/device epoch、资源代际、范围、索引域、owner 和最终 replay 验证；证明不足时继续
  fail-closed。
- 修复点阴影地面和单位表面的摩尔纹/条带。可信 receiver plane 的全部 PCF tap 统一使用同一
  精确径向深度域；单个 tap 不再在 exact plane 与中心深度之间混算。没有可靠平面的表面使用
  有界、单调的斜率回退。

### 发布冻结

- Hotfix3 强制关闭尚未通过发布门的 Compact WorkTable、Producer Claim Ledger、联合/Bounds
  剔除、Persistent GPU Package、GPU-skin 实验模式、persistent point-shadow worker、资源
  census、deep hook timing、旧 VB cache 和 source-fingerprint reuse。即使启动器遗留旧
  `DXVK_WAR3_*` 环境变量也不能重新打开这些路径。
- 保留 Type0 当帧捕获、点阴影修复、Arena fence、epoch 隔离和最终 replay 验证等正确性合同。
  普通 `Ctrl + F1` 性能报告仍可使用。

### 验证与已知边界

- 用户前台物理验收确认：不死族/暗夜精灵建造动画阴影连续，点阴影摩尔纹不再复现。
- Hotfix3 冻结合同、相关阴影/点阴影静态测试、Win32 runnable 18/18、Win32 DLL 构建、
  `ninja -n` no-work 与 `git diff --check` 通过。
- 高压低视角下的 CSM 安全预算与提前剔除问题仍由 [#5](https://github.com/CallDisaster/War3VK/issues/5)
  跟踪；超过预算时仍可能出现阴影闪烁或暂时消失，以避免提交可能触发 TDR 的超量工作。
- 同进程跨地图仍不宣称修复，继续由 [#6](https://github.com/CallDisaster/War3VK/issues/6) 跟踪。
  1.2003 仍建议一次启动只游玩一张地图，换图前完整重启游戏。

## v1.2.0 Hotfix 2 - 2026-08-07

这是面向高单位密度地图的第二个稳定性热修。建议 `v1.2.0` 和
`v1.2.0-hotfix.1` 用户更新。DLL 内显示版本仍为兼容的 **1.2.0 Release**，
JAPI、YDWE Catalog 和 `warvk:v1` 协议没有变化。

### 稳定性与正确性修复

- 默认关闭高压测试中被证明不安全的 exact-index compact trim 实验路线。该路线只能证明 CPU
  可读 IB 的索引代际，却无法同时证明稍后复制的动态 REAL position backing 属于同一份不可变内容；
  正式路径继续使用完整 exact source 合同，避免把不配套的 VB/IB 证明提交给 GPU。
- Direct packet geoset 缓存命中现在必须同时匹配当前 map epoch 和权威
  immutable-model generation，旧地图资源或同地址替换不能命中陈旧快照。
- DirectGrouped、exact producer 和 replay 统一携带实际索引域、full-domain fallback 与
  BaseVertex 合同；最终验证失败仍整份 fail-closed，不放宽 VB/IB 范围与代际检查。
- receiver 的终态 CSM publication 改为独立终态记录。后续 pre-receiver 或 command-tail
  快照不能再把本帧已经完成的阴影状态覆盖成零，地图/设备会话重置时会同时清除该终态。
- 修复 [#4](https://github.com/CallDisaster/War3VK/issues/4) 中单位选择圈不显示的问题：原生
  ListB 的 type 1/2 UI decal 与 type 4 UberSplat 现在会被保留，而旧单位 blob 阴影和建筑静态
  阴影仍由精确 producer gate 拒绝，不再用整层屏蔽误伤选择反馈。用户实机复测确认选择圈恢复，
  本轮也未再观察到报告中的树木闪烁。
- 扩充 CSM 各 cascade、点阴影各 light/face、receiver、volume、TAA、submit/present 的
  GPU flight breadcrumb，以及最后一个 replay offender 的对象、索引域和 buffer 范围信息。

### 自动化与性能计量

- AutoTest 的高压测试现在只绑定自身启动的默认可见桌面 War3 进程和窗口；isolated desktop
  路径继续拒绝。加入相机快照、低视角、全图视野、世界边界和 5×5 蛇形巡航控制。
- GPU profiler 使用 root interval union 统计嵌套区间，避免把同一段 GPU 时间重复累计。
- generation-backed index-slice cache、Compact WorkTable、Persistent Package 与联合剔除的
  Consume 均未达到正式收益门，继续默认关闭；HotFix 2 不用实验性性能路线换取表面帧率。

### 验证

- 静态合同 508/508、Win32 runnable 16/16、Win32 DLL 构建、`ninja -n` no-work 和
  `git diff --check` 全部通过。
- 在默认可见桌面、4096 CSM 下完成“生与死”DirectInline 三轮各 10 分钟，以及 TAA v2
  一轮 10 分钟；四轮均未记录 AV、device lost、Event 153/4101、Arena/replay 异常或 GPU incident。
- 上述结果证明当前候选满足本机单地图 HotFix 发布门，但不宣称所有硬件、极端镜头或跨地图路径
  已经根治 TDR。

### 已知问题与下一阶段

- 高密度区域压低镜头时，CSM 候选、蒙皮和四级联工作量仍可能超过安全预算。为避免把超量工作
  提交给 GPU 并触发 TDR，不完整 candidate 会被拒绝发布，因此可能表现为阴影闪烁或暂时消失，
  同时仍产生准备成本。该问题将尽量在下一个小版本中通过保守剔除和工作复用解决，跟踪见
  [#5](https://github.com/CallDisaster/War3VK/issues/5)。
- 同进程跨地图生命周期问题继续跟踪于
  [#6](https://github.com/CallDisaster/War3VK/issues/6)；点阴影残留摩尔纹跟踪于
  [#7](https://github.com/CallDisaster/War3VK/issues/7)。
- HotFix 2 发布后进入性能优化阶段：优先补齐可信 terrain/model/skinned-geoset bounds，采用
  receiver 驱动的保守 per-cascade caster volume，在蒙皮、freeze、Arena reservation 和
  packet build 之前剔除无贡献工作，并让多个阴影消费者共享同一份姿态和持久几何。

## v1.2.0 Release - 2026-08-05

这是自 GitHub `v1.1.0` 以来的第一个正式大版本。此前文档中的
`1.2.0-math-curves`、`1.3.0-polyline-curves`、`1.4.x-typed-transport/lighting-clock`
只是尚未公开发布的内部能力里程碑；本次统一归入产品版本 **WarVK 1.2.0 Release**。
`warvk:v1` 线协议和公开 JASS 函数名保持兼容。

### 阴影正确性

- 将太阳阴影默认分辨率固定为 4096；只有显存/主机资源无法安全分配时才锁存回退到 2048，
  不再因场景负载在 4096/2048 间反复重建。
- 收紧普通 PCF 与稳定墙面过滤半径，改善旧版本阴影边缘过度模糊的问题。
- 重建 Stage11/12 当帧 exact source 所有权：树木 alpha-test、普通刚体、蒙皮单位与建筑
  的最终 caster 必须匹配当帧资源、代际、材质和姿态，禁止其他 producer 回填陈旧表示。
- 修复树木方块阴影、蒙皮/未蒙皮部件交替闪烁、单位阴影周期性缺失、世界原点巨型三角/矩形
  阴影和低频单帧裂口。
- 彻底阻止路径/LOS 阻断器及 Warcraft 原生建筑静态阴影、旧单位 blob 阴影进入 WarVK CSM。
- 新增 final-caster、alpha、blocker、source-generation 与 backing/content 追踪，以便在低频错误
  发生时保留有限、可回溯的证据。

### 点光源、点阴影与体积效果

- 恢复点阴影专用径向深度写入与 cube face 渲染，避免把方向光深度合同误用于点光。
- 点阴影加入 receiver-plane/slope bias、texel footprint、texel-center 采样和深度写后同步，
  显著缓解地面与单位表面的严重摩尔纹、条带和伪影，但未消除所有表面与观察角度的残留。
- 点阴影 CPU plan 与准备任务增加 frame/light/settings/history/caster seal，陈旧结果当帧拒绝。
- 接通体积太阳光、体积点光与全局高度雾；高度雾可独立开关，不会连带关闭光源散射。
- 修复体积 pass 覆盖主 CSM 发布统计、深度插值与 coverage/blocker 组合不一致等问题。

### TAA v2 与后处理

- 新增可选 Temporal/TAA v2：3×3 亮度方差裁剪、reactive feedback、双线性历史采样和
  current-only 历史重建。
- ImGui 选择拥有运行时最终控制权；环境变量只作为启动初值，资源重建不再重置用户选择。
- 模式切换只失效一次历史，并公开 requested/effective/shader mode、history generation 与
  最后失效原因。正式版默认仍为 DirectInline。
- 保留并整理 FXAA、SMAA、Bloom、曝光、色彩与单位描边等既有后处理控制。

### GPU 生命周期与跨地图稳定性

- Shadow Arena 改为 64 MiB 页、逐 caster 事务式 bundle reservation；position、blend、UV、
  IB 任一失败都会整体回滚，不会提交半份 caster。
- Arena 代际使用专用 GPU completion fence 证明可回收，移除只按 `frameIndex % 3` 覆写在途
  数据的危险路线；总预算保持有界，溢出时 fail-closed。
- 引入进程单调 map/device epoch，跨地图统一隔离 Manifest、current-draw、freeze、persistent
  geometry、GPU skin、point worker、TAA 与 receiver 状态。
- 地图退出只提出 reset 请求；GPU 资源切换在 Present 安全点合并执行。旧 session 资源进入
  fence 驱动的延迟退役队列，不再由 JASS/退出线程直接清空。
- 新增统一 replay 验证器，覆盖 VB/IB 范围、index type/domain、BaseVertex、blend/UV、有限矩阵、
  GPU-skin palette/source generation 和 map/device epoch。
- CSM 改为完整 candidate 原子发布。不完整帧不会覆盖 last-good，也不会出现“全灭后逐个 caster
  补回再全灭”的可见循环；新地图没有完整图时安全显示无阴影。
- 修复 full-vertex-domain fallback 被重复施加 BaseVertex 所造成的整屏/半屏错误阴影。

### CPU 性能与受控优化基础

- 对已通过 allocation/generation 验证的 write-combined IB 采用有界顺序 bulk read，消除逐索引
  uncached 读取带来的高压 CPU 回归；同 DLL ABBA 中 ShadowCapture/PostGate 开销显著下降。
- 优化语义缓存查找、去重、临时分配、诊断原子计数与重复 replay 构建等热路径。
- 新增 generation-sealed Compact WorkTable、联合消费者剔除、Persistent GPU Package、持久
  点阴影 worker 与 CPU-MT 蒙皮合同。
- 未完成消费者 last-use/收益证明的 Package Consume、联合剔除 Consume 与 CPU-MT 默认仍关闭，
  不以实验路线冒充正式性能收益。

### WarVK JAPI 与 YDWE 作者工具

- 将 clean-room WarVK JAPI 正式内置到 DXVK `d3d9.dll`。通过 Warcraft stock native carrier
  转发 `warvk:v1`，不声明额外 Native，也不依赖 `war3map.dll` 提供接口。
- 发布修订移除了作者包中的 AI/Lua Loader、`WarVK.dll` 与伪装 `.blp` DLL carrier；正式版只支持
  在游戏启动前安装代理 `d3d9.dll`，地图侧初始化仅检测 bridge，不再请求运行时加载。
- 新增可选强类型 Hashtable 数据面：验证 `SaveInteger/SaveReal/LoadInteger/LoadReal` 的
  Game.dll 签名后，以私有表、序号、操作码和 commit/query 事务传递高频数值；其他 Hashtable
  调用无条件转发原函数。
- 新增点光创建/移动/颜色/强度/半径/点阴影控制，体积光与全局高度雾控制，以及独立光照时钟、
  天体轨迹和时间色温曲线。
- 新增闪电模板、连续 Ribbon、分支、贴图、脉冲/滚动/辉光、公式曲线与 2..1024 点 polyline。
- 新增有界 MathProgram/Curve 运行时，支持 scalar/vec2/vec3 表达式、参数、坐标模式、端点锁、
  实数/整数求值、有限差分导数与弧长查询。
- YDWE Catalog 按功能分类；CSM 级联、点阴影分辨率、时钟、闪电渲染、曲线坐标/分量和整数舍入
  均提供 Trigger Type/Params 下拉选项。加入 WarVK 分类图标与简要参数说明。

### 诊断、自动化与发布工程

- `runtime_status.json` 增加 CSM/TAA、Arena、queue、map epoch、replay validator、point worker、
  package 和资源驻留诊断。
- 加入 240 帧 GPU flight recorder、GPU incident JSON、崩溃转储、低磁盘 final-caster trace 和
  attach-only 跨地图记录器。
- AutoTest 增加阴影、点阴影、TAA、Arena、跨地图、JAPI、数学曲线、YDWE Catalog 与性能合同；
  后台性能测试可只关闭 Warcraft 自身 idle-sleep，不再依赖全局 `Sleep` Hook。
- 统一 Meson、DLL 资源、外部 Shader API、JAPI 显示与文档版本为 `1.2.0 Release`。

### 兼容性与已知边界

- 当前构建请求 Vulkan 1.3。请更新到最新官方显卡驱动；仅暴露 Vulkan 1.2 的驱动不满足要求。
- 目标游戏版本为 Warcraft III 1.27a x86；未知 `Game.dll` 签名会 fail-closed。
- TAA v2 仍为可选项，DirectInline 是发布默认值。
- 点光源开启点阴影后，部分地面与观察角度仍可能出现摩尔纹或带状伪影；1.2.0 尚未完全修复。
- 同一 Warcraft III 进程中退出地图后再进入另一张地图，仍可能发生持续性能下降、阴影异常及
  其他资源生命周期问题。1.2.0 的推荐方式是一次启动只游玩一张地图，切换地图前完整重启游戏。
- 跨地图连续游玩与剩余点阴影摩尔纹均延期到后续版本处理，不将隔离桌面结果表述为这些问题已解决。

## WarVK 静态阴影解决版 - 2026-07-08

### 重点更新

- 解决 Warcraft III 原生建筑静态阴影残留问题：默认在 `CUnitUIManager_RecordSetStructureShadow` 写入 `buildingShadow(+0x50)` 时阻断阴影文件名进入 UnitUI 类型记录。
- 保留并默认移除 `TerrainShadow_RenderListB` 旧版单位黑色圆影，让画面不再叠加原版 blob 阴影。
- 退役 `WriteMaskRegion / StaticStampPath / RegisterImage / DoodadStamp` 等历史静态阴影实验默认路径，保留为证伪资料和专项诊断入口。
- 整理静态阴影研究文档，明确 `CUnit+0x50` 不是阴影字符串，真正生产点是 UnitUI type record `+0x50 = buildingShadow`。

### 验证结果

- 实机日志确认 `DXVK War3Hook: CUnitUI buildingShadow BLOCK calls=768 blocked=768 mode=0 ... name=ShadowTreeofLife`。
- 用户实机确认建筑阴影完全不可见。
- `ninja -C build32` 通过。

## v1.1.0 - 2026-04-05

### 重点更新

- 修复了路径阻断器被错误渲染进魔兽争霸3场景的问题，避免非目标几何污染主渲染与阴影链路。
- 内置整合 `StormBreaker`，为项目提供更稳定的内存管理与运行时基础能力。
- 更新 `GPU Arena`，继续推进阴影捕获、几何预算与运行时资源管理。
- 接入第一阶段缓存机制，目前只对静态模型启用；动态单位、飞行单位、蒙皮多边形仍保持非缓存路径，避免阴影静止或姿态错误。

### 当前状态

- 静态模型缓存可用。
- 动态姿态接管仍在推进中，目标是后续改为“静态模型资源 + 每帧 Pose/Palette 更新”。
- 当前版本重点仍是稳定性与链路铺设，而不是完全接管动态模型顶点计算。
