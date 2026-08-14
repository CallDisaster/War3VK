# 更新日志

## v1.21.00 - 2026-08-14

本版本开始采用 `1.小版本.修复版本` 编号：`1.x` 是当前大版本架构系列，`1.21` 是功能小版本，
末尾 `00` 表示该小版本的首个稳定发布。后续纯修复依次进入 `1.21.01`、`1.21.02`；下一组成组
功能或正式性能路线进入 `1.22.00`。此前的 `1.2003` 保留旧编号，不重写历史标签。

产品、DLL 资源、日志和 JAPI 显示版本统一为 **1.21.00**。外部 Shader API 数字版本仍为 `1.2.0`，
JASS 线协议仍为 `warvk:v1`，地图作者无需因产品版本号变化迁移已有脚本。

### 阴影正确性与画质

- 方向光 CSM 改为 compare-first PCF：先对每个 texel 执行深度比较，再过滤可见性；移除周期性
  世界坐标 Poisson 旋转，使用固定、零质心的对称核，并为可信 receiver plane 的每个 tap 修正
  比较深度。DirectInline 和 prepass 使用同一套有限性与整核回退合同。
- 全部方向级联默认使用相同 alpha cutoff。旧版逐级联增加阈值会让树叶等 cutout Caster 在级联
  blend/切换时改变轮廓；非零 far bias 现在只作为显式 opt-in。
- Stage11 exact producer 为当前 scene、同 epoch last-complete publication 和 GPU 在途 consumer
  引用建立受保护工作集；64 MiB 只作为非活跃静态缓存的 LRU 目标。可见树木/建筑不再因缓存
  自我淘汰、随后受每帧 32 次分配门补回而周期性整批闪烁。
- 新增 scene-stamped producer completeness：position/UV/index 分配预算、真实 allocation failure、
  fallback byte budget、Arena admission/freeze failure 都会显式标记缺失 Caster。未封印、stamp 不匹配
  或存在 required omission 的 scene 会在任何 clear/draw/异步 prepare 前被整份拒绝。
- Defrag/relocation 后 replay 在命令录制线程重新解析当前 logical buffer binding；validator、排序、
  hash 和最终 bind 使用同一份 resolved draw，不再持有 capture-time 的旧物理 `VkBuffer`。
- Warcraft Transparent Type0 建造附件继续使用子模型当前 draw、精确 VB/IB/UV 与完整 palette。
  用户前台复测确认不死族和暗夜精灵建造过程阴影连续。
- 点阴影统一径向 receiver-depth/bias 域。用户前台复测确认地面与单位上的摩尔纹/连续条带不再复现。

### 体积光与局部雾

- 新增最多 8 个 Sphere/Box/Cylinder 局部体积雾区域，可与全局介质独立组合；参数、句柄、数量、
  数值有限性和射线区间均保持有界。
- 新增 Froxel Medium/High，High 为默认体积后端；使用全视图 `[20,10000]` 对数 Z、64/128 层、
  独立 effect 网格 scene-depth 终止及有界的太阳阴影光学积分。
- 方向体积阴影新增独立 base/refined Guide：Integrate 写入方向遮挡证据，Composite 在全分辨率
  scene depth 上重建；小 Caster 阴影柱不再完全受 `1/4` effect 网格限制，几何断层与体积阴影
  边缘使用不同证据。
- Caster silhouette 使用 2x2 comparison-first gather；体积阴影 Guide 按真实 half-texel footprint
  拆分区间，减少动态 Caster 整格跳变。
- Froxel effect、base guide 和 refined guide 采用事务式创建与独立 layout ownership。资源、格式或
  shader-work admission 失败时整套回退 Legacy。1080p 默认请求可进入；1440p/4K 最坏路径允许
  fail-closed 回退，不提交无界工作。

### Vulkan 终态与 TDR 诊断

- 修复 D3D9 CS phantom sequence：device lost 时未派发 chunk 不再留下永远无法完成的等待序号。
- Submission/finish/presenter/frame worker 在全局 terminal latch 后停止新的 Vulkan/WSI work，仍会
  exactly-once 完成 command object、frame tracker、signal 和 CPU retirement，避免 waitForIdle 或
  worker join 永久卡住。
- D3D9 CreateShader、ResetEx 和 AdditionalSwapChainEx，以及 pipeline compiler 的 enqueue/dequeue
  边界都会在 terminal loss 后 fail-stop，不尝试在不可恢复的旧 VkDevice 上重新创建资源。
- 为 command buffer、pipeline、query、event、waitForIdle、fence/keyed-mutex 等规范允许返回
  `VK_ERROR_DEVICE_LOST` 的真实调用补齐 provenance；synthetic CS loss 不会伪装成 driver fault。
- 支持时一次性采集有界 `VK_EXT_device_fault` description/address/vendor text。查询不在 submission
  drain 上运行，不启用 vendor binary；基础 incident 先落盘，完成后最多追加一份关联 enrichment。

### CPU 性能与内存

- exact-owner publication 在 DirectGrouped 构建 packet 前排除 earlier producer 已完成的记录；
  同场景 99% 以上的重复 CurrentDraw 候选不再走 BuildEligible。
- resolved replay 快照由 directional、volume-sun 和 point-shadow consumer 共享，删除每个 consumer
  对约 160 个大 draw 结构的重复复制与引用计数。
- 同场景 A-B-B-A：主线程 CPU `6.135 -> 5.778 ms`（`-0.357 ms / -5.82%`），Populate
  `-76.27%`、DirectGrouped `-87.42%`、BuildEligible `-96.38%`。这是生产者 CPU 收益，不能
  外推为所有地图的绝对 FPS 增幅。
- 32 位性能历史统一钳制到 4000 帧，防止扩展后的逐帧诊断容器耗尽 Warcraft III 地址空间；累计
  workload、预算和错误计数仍覆盖完整运行。
- 提前联合剔除、Persistent Package、ReBAR、CPU-MT 蒙皮和 Canonical Queue Takeover 继续默认关闭。
  1.21.00 不会把未通过完整正确性/性能门的 Consume 路线作为发布默认。

### 验证

- 当前合并候选通过 216/216 静态脚本、50/50 Win32 runnable、Fresh Win32 Release `-j2` 构建、
  DLL exact-target no-work 和 `git diff --check`。
- 体积光测试图隔离桌面 smoke：3901 帧/62 秒，shadow incomplete、budget exceeded、partial、Arena
  overflow、device lost 和新 GPU Event 153/4101 全为 0。该数据只证明稳定性，不代表前台绝对 FPS。
- “生与死”低视角 5x5 巡航 603 秒，峰值 1196 Caster/4784 级联 draw，Arena 峰值约 328.7 MiB，
  未提高 384 MiB/代际上限；TDR、Arena overflow、partial publication 和新 GPU incident 全为 0。
- 用户前台已确认建造动画、点阴影及当前组合画面基本正常；正式 GitHub Release 仍须在发布前审核
  本更新日志、README、最终 1.21.00 DLL 哈希和两个发行包。

### 已知边界

- 同进程跨地图仍未完成正式发布验收，建议退出地图后完整退出并重启 Warcraft III；继续由
  [#6](https://github.com/CallDisaster/War3VK/issues/6) 跟踪。
- Issue #5 的提前联合剔除尚未成为 Release Consume。当前版本修复了确认的 producer 重复开销、
  活跃缓存颠簸与 partial publication，但不宣称所有 terrain/static/skinned 前端剔除已经完成。
- 极细树叶、草线和远距离 alpha silhouette 仍可能存在少量亚像素变化；本版不以无条件 TAA 历史
  或额外模糊掩盖剩余运动。
- 1440p/4K 最坏 Froxel 请求可能按 admission 回退 Legacy；这是设计内的安全边界。

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

### 运行时安全修复

- 发布构建在编译期关闭旧式 `warvk:cmd` 字符串命令入口，不提供环境变量绕过；专用开发测试构建
  即使开启该入口，也会拒绝未知命令、未发布 feature、非有限数、溢出及尾随字符。公开的强类型
  WarVK JAPI 和 `warvk:v1` 协议不受影响。
- JASS VM 重建只清理地图作者的 CPU 状态，不再由 Hook 线程释放渲染线程拥有的 Lightning 纹理；
  完整 GPU 资源清理继续由 Present 所有者在地图事务安全点执行。
- 异步渲染设置改为互斥 mailbox 与不可变 `shared_ptr` 帧快照，修复设置撕裂、对象析构后访问、
  settings/PointLight 锁序反转及递归自锁风险。
- 删除 active device/pipeline 的裸指针旁路；设备构造发布、调用事务、析构撤销和 settings 生命周期
  现在由同一发布锁闭合。
- `Reset`/`ResetEx` 的 device epoch 迁移只允许 Present 所有者提交：先关闭 producer、quarantine
  旧资源并推进 fence，再按命令流顺序失效 receiver。GPU-skin 重绑失败时保持无阴影，不会错误
  重新开放旧资源。
- 地图 epoch 变化时会在所有早退路径之前清理语义注册表、SceneCollector 的裸指针/handle TLS
  与关联 CPU 身份，避免 Map A 地址别名进入 Map B。该修复增强 fail-closed 边界，但本版本仍不
  宣称完整跨地图游玩已经通过物理验收。

### 发布冻结

- Hotfix3 强制关闭尚未通过发布门的 Compact WorkTable、Producer Claim Ledger、联合/Bounds
  剔除、Persistent GPU Package、GPU-skin 实验模式、persistent point-shadow worker、资源
  census、deep hook timing、旧 VB cache 和 source-fingerprint reuse。即使启动器遗留旧
  `DXVK_WAR3_*` 环境变量也不能重新打开这些路径。
- 保留 Type0 当帧捕获、点阴影修复、Arena fence、epoch 隔离和最终 replay 验证等正确性合同。
  普通 `Ctrl + F1` 性能报告仍可使用。

### 验证与已知边界

- 用户前台物理验收确认：不死族/暗夜精灵建造动画阴影连续，点阴影摩尔纹不再复现。
- Hotfix3 冻结合同、相关阴影/点阴影及运行时安全静态测试 76 个脚本/581 个用例、Win32 runnable
  20/20、Win32 DLL clean build、
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
