# 模型数据框架与统一收集层调用树

状态：首阶段源码候选；入口计时与第一项 metadata-only 优化已实现。不是完整模型系统完成，
不是实机全覆盖/性能收益接受。主树 `88089cdf`，不等同于最近玩家 A570 参考树产物。

## 1. 目标和证据前提

统一资产定义、实例动态参数、当前 draw 证明、消费者快照四层；灯光、材质、附件、阴影和未来
内容共享同一份数据。新消费者不得新增逐 draw 全场景扫描/文件读取/全量模型解析。

现有对照还没有证明“几乎全部损耗来自数据收集”。增强关闭与开启的帧时差包括消费、上传、
同步和 workload 差异；关闭消费者还会反向关闭生产需求。因此本轮优先完善数据层归因，
不以该假设为由跳过完整性检查或重写全部缓存。

## 2. 四层合同与接入方式

| 层 | 入口/所有者 | 存储与更新 | 不允许 |
| --- | --- | --- | --- |
| AssetDefinition | 实际模型加载 bytes/解析结果，资源装配回调 | 以完整内容身份发布不可变几何/材质/骨骼/Light等定义，记录requested与resolved路径 | 以文件名、裸地址或重新读同名磁盘文件冒充内容证明 |
| RenderInstance | 原生对象创建/克隆/销毁与已求值输出 | typed slot+generation、map/device域、当前pose/material/light revision，按需复制变化字段 | 同模型共享动态姿态；只用“最后一次路径”TLS关联嵌套模型 |
| DrawContract | 当前部件/layer/真实buffer和矩阵来源 | 当前draw/frame/index-domain与透明alpha完整性，既有CurrentDraw优先 | 静态模型定义覆盖动态索引/附件修改；哈希相等替代完整身份 |
| RenderSnapshot | 渲染线程在既有安全发布点 | 消费者自有值/不可变payload租约、显式使用fence，旧map/device域退役 | Hook/JASS线程直接释放GPU资源；异步持有可解引用游戏裸指针 |

架构采用既有 Registry/CurrentDraw/Arena 的渐进适配，不并行保留两套“真相”。第一步扩展准确
的 asset metadata/instance identity；按消费者用途提供固定大小查询，不让只读身份的代码拷贝几何。
Ready 按能力分别判断：geometry-ready 不等于 lights-ready 或 alpha-ready。

### 工作频率和成本预算

- 加载/内容变化：一次取得实际bytes，完整SHA/解析在拥有生命周期的冷路径完成；可异步处理时
  先拷成受预算的自有输入。队列满就保留原生路径，不阻塞渲染等待可选元数据。
- 实例创建/重绑定/销毁：记录增量关系和generation；绝不每帧重建所有asset定义。
- 每帧/变化：消费游戏已求值的矩阵、灯光可见性/颜色/位置；不重做整套原生动画。
- 每draw：仅取固定大小的identity/flags/span/lease，允许必要的完整证明，不允许磁盘I/O、
  名称解析、无界容器增长、全场景扫描或无条件复制所有顶点/矩阵。
- 冷热内存分账：immutable payload按bytes，instance/关系按entries，frame scratch按峰值，
  GPU retirement按真正last-use。未定预算先显式上限/拒绝，不用扩大Arena掩盖收集设计问题。

不是所有缓存都应该跨帧：稳定拓扑可以，当前pose/draw透明状态必须有对应代际和新鲜度。
不要复活已判退的 PreparedKey、missing-only、exact-N 或 negative-memo 路线。

## 3. 本轮数据层复审发现

1. `ResolveGeosetMetadata` 只读5个身份字段，却通过 `findGeosetByData` materialize整个记录，
   隐含复制position/normal/UV/index/group等vector。已对一个明确调用点改用无payload拷贝查询。
2. 其它可见对象/上层阴影的metadata-only读取仍有全量record materialize；不能一口气替换，
   byPtr别名/tombstone合并与byData观察字段规则不同，后续按完整返回语义逐项迁移。
3. `snap.records.reserve(size+1)`/按当前批量恰好reserve在容量增长时可能反复搬迁；暖帧已有
   容量可以避开。列为下一候选，尚未改增长策略、顺序或记录上限。
4. 多个 registry 同步/身份查询、pose快照和canonical发布存在重叠职责。应先用新树量出数据流，
   再由单一有代际的写入者更新；不凭成功率高就加不完整key的查询缓存。
5. 消费需求与生产混在一起，现有capture-only不是等人口消融；必须先定义独立需求mask，
   并保持producer结算和frame/fence生命周期，才能比较“收集开/消费关”。此消融尚未实现。

## 4. 已实现的统一计时树

独立 `DataCollection` 树，随现有 HTML report 导出 `dataCollectionTree`，Call Tree 的线程
下拉框可选择它。它是原 Hook 树的**覆盖视角**，不追加进旧sections/root CPU合计，防止双计。

- 同一线程最外层数据入口启动一次计时；所有嵌套分支继承同一个采样决策。不同外层入口之间
  不连续的空隙不算数据层时间，主线程/worker各自独立，不能加成Frame墙钟。
- 整数QPC tick记录：每节点 inclusive、直接children和calls；`Self/Unclassified=inclusive-children`。
  Export核对每个parent的childTicks确实等于直接子节点总和；倒退时钟、非LIFO、深度/节点/整数
  溢出均fault，不使用max(0,差额)伪造正常闭合。
- 虚拟DataCollection根是多个最外层数据区间的并集合计。Root内的self代表尚未细分的本函数
  工作和部分探针成本；Root外完全漏记的入口无法凭父子等式发现，必须另用覆盖审计和独立OS证据。
- 固定8线程槽、每线程512路径节点、64层栈；parent+tag直接索引，热路径不构造字符串/哈希表。
  路径归类用固定Tag，因此114个入口不是114种不重复的函数标签，部分相同职责按同一Tag聚合。
- 写入者使用TLS，只有完整采样根退出时将自有数字快照发布到互斥保护的槽；export不读别人
  正在写的TLS。sampled-root发布仍有拷贝/锁开销，记录了它的下界，不能宣称observer完全免费。
- Export查询本身使用Pause，避免报告生成的registry读取被当成业务收集。recording关闭阻止
  新scope，resetHistory推进session；跨session未完成scope记录abandoned，不混入新session。
- 当前显示**已采样总毫秒/次数**，非“本页滑窗每帧平均”，小于0.005ms仍显示有效位。
  数据域明确为recording-session，不能把累计数据直接除以HTML保留的最近3600帧。

### 开关与正确使用

- 源码默认 `warvk_data_collection_tree_dev=false`，宏关闭时114处scope编译为空操作。
- 本轮诊断构建将该选项设为true；运行时还必须精确设置：
  `DXVK_WAR3_DATA_COLLECTION_TREE=1`、正常PERF_MONITOR和recording开启。
- `DXVK_WAR3_DATA_COLLECTION_SAMPLE_PERIOD` 默认64，可设1作完整入口计时；只接受1..4096的
  2次幂，无效值拒绝采集。一个root抽中时子链全抽中，不复用旧detail的独立采样。
- 运行时关闭不调用QPC/写容器，但诊断构建保留函数/分支开销。**编译关闭**才是零scope代码。
  最终性能接受必须在同配置stats-off产物上做A/B，不用全采样诊断DLL证明净性能收益。
- 开始目标测试前重置录制历史，并在树中选正确tid。P95等旧滑窗统计不套到这棵独立树。

### 入口覆盖边界

清单 `AutoTest/data_collection_entrypoints.json`，**114/114 处源码入口接线**，含：
SceneCollector、ExecBatch Begin/End、可见对象发布/查询/Finalize、RenderObject、Model/Instance/
Pose/ShadowObject registry、Geoset绑定/采集/查询/materialize、CurrentDraw发布/查询/解码、
Semantic Build/Augment、DrawCapture/MetadataCapture、Populate/DirectGrouped、模型姿态观察。

这不是实机全覆盖证明。未独立包住的内部工作在已包住父函数内归Self；独立的GPU-skin worker
任务、泛D3D9前端上传/Lock等待、所有原生trampoline、尚未接入的未来asset loader等不能按名字
全归给本树。故 `coverageComplete=false`、`unknownOutsideRoots=true` 保持失败关闭。
下一轮按实际profile比对入口人口和OS时间线，逐项扩入口/细分，而不是把此标志改true。

## 5. 第一项实际优化：identity-only projection

新增 `ShadowGeosetIdentityView` / `findGeosetIdentityByData`：在原有shared mutex和相同map查询
条件下复制 geosetPtr/dataPtr/modelResourcePtr/modelKey/geosetIndex；无vector、无额外shared_ptr
引用计数，不授权几何新鲜度/所有权。只有 ResolveGeosetMetadata 的首个直接byData读取改用它。

等价依据：原 `materializeGeosetDataRecordLocked` 对canonical记录只叠加3个观察时间戳，不
改变这5个字段；旧miss/null清零规则、后续fallback顺序、别名/byPtr路径均保留。
纯值测试比较旧完整拷贝加时间戳与新projection，包含大数组与源对象变化，验证独立值输出。
已证明少做这一次payload复制，尚未证明当前场景命中频次、端到端毫秒或FPS收益。

## 6. 验证与构建收据

- C++核心26条断言：嵌套/同名递归、child与self、篡改树、LIFO、倒退、溢出和节点/深度上限。
- 真实双线程collector：period64、period1都整数闭合，子节点采样次数与root一致；Pause不入树；
  period3配置被拒绝。独立identity projection runnable通过。
- 新静态6/6，缓存证据回归4/4；15个实际TU×开关0/1共30次语法检查通过，热TU预处理验证
  关闭时不含scope变量。没有把这些检查称为全量项目回归。
- Playwright对离线synthetic DataCollection UI：选择线程、展开分支、精确小数显示、闭合提示
  均通过；v2控制台0错误/0警告，截图在 `output/playwright/data-tree-v2.png`。
  原玩家报告仅作为页面fixture底板，DataCollection来自合成runnable，绝非游戏计时证据。
- 构建前原build32 DLL原字节保存在ignored `build-before.dll`，SHA BEB35D6A...E0590A8。
  诊断构建改了ignored build32配置，不改现场；exact DLL预检280 edges（273 objects+6静态库+1DLL），
  BelowNormal/-j2，完整结果将以 `AutoTest/artifacts/data_collection_tree_20260913/build/build.json`
  为准，未成功之前不授予新DLL身份。

构建收口：exact DLL预检/实际 **280/280** 且步骤集合一致，完成后no-work。
产物 **34,450,973 bytes / PE32/i386 / SHA-256
`139BBE8CE6F941A176B9F4582E601E2841FC26956CB0DD7884903295F4505FD2`**。
原字节CreateNew冻结到 `AutoTest/artifacts/data_collection_tree_20260913/candidate/d3d9.dll`，
source/candidate SHA一致。仅诊断候选，未部署、未实机、不承诺性能收益或稳定接受。
build32 当前为 release + data_collection_tree_dev=true；native_model_lights_dev=false。
构建包含当前主树既有用户改动，并保留原编译警告；不称为无警告或干净提交构建。

本轮不部署、不自动启动游戏，不修改原始性能报告/地图/旧白名单。不将主树产物冒充玩家A570的
同源码替换包；实机前仍须核对目标集成树、现场备份及完整性/视觉/成本门。

## 8. 用户 2026-09-13 18:06:51 报告复核

新报告为 full_default、3600帧/31.401秒、DLL `139BBE8C...5FD2`，平均 **118.679 FPS / 8.426 ms**；
15:42 A570 对照为 100.709 FPS / 9.930 ms，15:13 对照为 99.255 FPS / 10.075 ms。
这是相对数字，不是稳定性能收益：DLL、构建基线、探针开关、运行窗口和采样路径不同。

新报告 avg GPU 2.119 ms，Shadow Main 1.854 ms、ShadowReceiver 1.009 ms；均低于 15:42。
这说明不只是窗口模式差异，但不能分摊到单一收集优化。新 DataCollection 根约 251.95 ms 是
采样会话累计，不是3600帧平均；`coverageComplete=false`，不能与旧 Hook 树相加。

新 workload draw capture 116.48/帧、dynamic skinned 40.08/帧；15:42 为122.49/44.49。
S1 捕获仍44/帧，4096阴影分辨率不变；已导出的framesIncomplete/预算/arenaOverflow为0，
但framesProducerIncomplete和producerRequiredCasterOmissionCount缺失，不能概括为完整性门全过。
部分阶段反而变慢（Populate 0.264→0.598ms），所以不能将涨帧全部归因于数据层优化。仍需
同源码基线的控制构建、同env/机位/分辨率与stats-off A-B-B-A分离版本、探针及投影改动贡献。

新 HTML 发现8个重复 JSON key（canonical-ready cutout/alpha 及 Stage13 persistent 字段，值相同）。
比较工具保留重复值并标记不适合 strict acceptance，不静默 last-wins。比较收据：
`AutoTest/artifacts/data_collection_tree_20260913/report-comparison-20260913.json`。
最终独立整数闭合检查、工作量与三份报告比较见同目录 `report-comparison-20260913-v2.json`，
完整判定见 [18:06玩家报告复核](../agent-history/2026-09-13-player-data-collection-report-180651.md)。

## 7. 后续顺序

1. 用诊断候选验证入口覆盖与探针自身成本，取得顶层/分支/self和人口闭合证据。
2. 对identity-only改动单独stats-off A/B，必要时与只有计时的基线隔离；不在同一比较里混入
   新灯光/阴影/消融开关或画质调整。
3. 扩更多metadata-only消费者、改善增长和batch发布策略，每次保留完整字段、锁/代际和fallback。
4. 接真实模型bytes→asset→instance的冷路径，建立参数能力状态；之后再接原生灯光及点阴影名单。
