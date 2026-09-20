# 2026-09-13 18:06:51 玩家全屏报告：涨帧观察与 DataCollection 归因

状态：DIAGNOSTIC_ONLY / OBSERVED_FASTER / STRICT_REPORT_SCHEMA_FAILED / NOT_STABLE_ACCEPTED。
后续时钟审计补充：表中avgFrameTimeMs是profiler有效区间，遗漏endFrame→beginFrame内的
Present尾部，FPS是它的倒数。不能等同完整呈现帧率；见
[GameMainLoop与呈现计时口径](../research/2026-09-13-mainloop-and-present-clock-audit.md)。
用户明确说明本报告为全屏，并另测窗口模式仍有相同帧率表现。该交叉观察予以记录，
但未提供第二份窗口报告，不能将用户观察升级成自动化等分辨率/等相机矩阵证明。

## 1. 输入和实际开关

- 当前报告：`E:/Work/Warcraft III/WarVK/Log/war3_perf_report_2026_09_13_18_06_51.html`，
  3,216,499 bytes，SHA-256 `455726BCC46E35171E8D1BC6AABEFEFAAF7EE003AD1AAF8E3BCDC5E269DC9C3C`。
- DLL：34,450,973 bytes / `139BBE8CE6F941A176B9F4582E601E2841FC26956CB0DD7884903295F4505FD2`，
  与交付诊断候选一致。full_default，所有模块mask开启；GPU skin接管仍disabled。
- DataCollection编译/运行均开启，period=64；PERF_RECORD_ON_START=0。
  旧trace detailEnabled=false；旧deep/residual/shadow-phase等环境未记录为开启。
- 数据树会话9037帧；一般图表仅保留3600帧，windowSec=31.401。两个窗口不得混除。
- 对照一：9月12日15:42:28详细报告，A570 DLL，3600帧，SHA
  `25ED941B2295741911BE85D39932A734CC6A06ACF452C17D428C93755A831ACB`。
- 对照二：9月12日15:13:38普通启动报告，同为A570，2644帧，SHA
  `92FD74DBB54805900AD5F7D91B2AEA6AE4C162C2A95147B9E2B7108E904AD1C6`。

## 2. 实际涨帧数字

| 指标 | 旧详细15:42 A570 | 新18:06 139B | 报告间变化 |
| --- | ---: | ---: | ---: |
| 平均FPS | 100.709 | 118.679 | +17.84% |
| 平均帧时ms | 9.930 | 8.426 | -1.504 / -15.15% |
| P95帧时ms | 11.071 | 9.411 | -14.99% |
| P99帧时ms | 12.290 | 10.676 | -13.13% |
| 主线程OS CPU ms | 5.321 | 4.618 | -13.21% |
| worker CPU合计ms | 3.025 | 2.361 | -21.95% |
| GPU union ms | 2.738 | 2.119 | -22.61% |
| >16ms报告帧数 | 3 | 0 | 本保留窗口中更平稳 |

主线程、worker、GPU和帧墙钟不得相加；p95CpuMs/p99CpuMs是此schema的帧时分位数，
不是CPU执行时间分位数。旧普通启动15:13（没有detail）也只有99.255 FPS，故“仅关旧探针”
不足以解释全部提升；此前“主要来自探针开关”的猜测不作为最终因果结论。

## 3. 为什么还不能将提升归因于本轮5字段投影

### 3.1 不是同源码基线的单变量实验

交付139B来自主树88089cdf加工作改动，旧A570来自a72c700集成参考树加诊断修改。
只比较两个HEAD的src/d3d9、src/dxvk和war3fx shader就有204个文件差异；它不是在A570上
仅加DataCollection和identity-only projection的版本。这是交付时必须说明的归因限制。

shader有实际差异：旧reference receiver含compare-first/comparison sampler相关路径，
主树版本的采样/参数校验路径不同；buffer冻结、布局/生命周期及报告producer也不完全一致。
这里只证明不能单变量归因，未证明某一差异贡献了几毫秒，也没有以删除验证/改变滤波为优化方案。

新树中主线程没有显示出可量化的GeosetQuery/GeosetMaterialize分支收益；而被改动的
ResolveGeosetMetadata在若干默认Finalize路径中本来就禁用。因此没有证据把18%归功于它。
观察器新增自身开销，不应凭FPS更高就推断新观察器比旧版没有成本。

### 3.2 实际工作量并非完全一致

| 同一保留窗口逐帧计数均值 | 15:42 | 18:06 |
| --- | ---: | ---: |
| S1捕获尝试/接受 | 44 / 44 | 44 / 44 |
| fallback captured draw | 52.00 | 52.00 |
| draw-time VB capture | 122.49 | 116.48 |
| dynamic skinned output | 44.49 | 40.08 |
| semantic submitted | 96.49 | 91.78 |
| replay caster | 148.49 | 143.78 |
| replay geometry work（既有单位） | 41307.24 | 39094.50 |

两个报告录制内camera/sun delta均为0，只能说明各自录制时稳定，不证明两次绝对相机/分辨率相同。
新报告可见动画/单位工作量较低，但计数定义和源码差异也可能参与，不能反推玩家少放了几个单位。
用户窗口复核降低了“仅fullscreen开销变化”的可能性，不排除版本/场景/分辨率的其它因素。

### 3.3 更快的阶段与更慢的阶段同时存在

| 路径（保留完整上下文） | 15:42 ms | 18:06 ms |
| --- | ---: | ---: |
| Hook_WorldFramePrepare/NativeOriginal | 1.347 | 1.049 |
| WorldRenderScene/.../FlushSortedItems/NativeOriginal | 1.254 | 1.000 |
| ShadowCapture | 0.666 | 0.520 |
| ShadowCapture/Gates | 0.520 | 0.400 |
| ShadowCapture/PostGate | 0.146 | 0.120 |
| 根Hook_FlushAndReset/NativeOriginal/Populate | 0.264 | 0.598 |
| 同父Populate/DirectGrouped | 0.133 | 0.491 |
| 同父DirectGrouped/BuildEligible | 0.028 | 0.369 |
| GPU ShadowReceiver叶 | 1.417 | 1.009 |
| GPU ShadowMap叶 | 0.950 | 0.718 |
| GPU PostFX/AA叶 | 0.264 | 0.265 |

不同探针/版本使这些值只能用于定位变化线索；不能把各行相加给整个FPS做精确拆账。
特别是Populate明显变慢，不支持“收集层整体每个部分都优化了”的说法。

## 4. 新 DataCollection 树的实际价值和剩余空白

main TID46004：43个路径节点、185992个采样根，采样根累计251.9535ms；另有TID43236的4节点树。
本轮用原始整数字符串ticks独立重算：每节点childTicks等于直接孩子ticks总和，inclusive不小于
children，root等于顶层分支总和。两树closureErrors为空，faults=0，inFlight=false，
threadOverflow=0，abandoned=0。说明这一次已完成采样的树没有发现结构性错账。

| main顶层分支 | 采样累计ms | 占采样根 | 内部未细分比例 |
| --- | ---: | ---: | ---: |
| DrawCapture | 109.6469 | 43.52% | 65.28%（Self=71.5818ms） |
| Populate | 86.0977 | 34.17% | 主要在下层DirectGrouped |
| VisiblePublish | 31.0800 | 12.34% | 72.51%（Self=22.5365ms） |

三顶层分支合计90.03%。Populate内的DirectGrouped占采样根28.23%，其Self=47.3151ms，
占该分支66.53%。这是后续细分的首要候选，而不是再在几乎无工作量的lookup上盲目加缓存。

这些是**已采样数据根的组成比例，不是整帧成本比例**。period64是全局外层入口序号的确定性
采样，不能假定各类入口完全无相位偏差；period1或改进抽样验证仍需独立成本门。
root仅覆盖已登记入口，Self包含尚未分类工作和部分探针开销；数学闭合不等于114入口覆盖所有业务。

main publicationOverheadMsLowerBound=189.7832ms（9037帧会话累计）是发布快照成本下界，
与251.9535ms业务采样累计的采样/执行覆盖不同，不可乘64后混加到业务耗时。
旧Hook报告仍有4.515ms/帧墙钟未覆盖，不能把它整个标成收集CPU或等待，也不能加到DataCollection上。

## 5. 报告可靠性和完整性限制

原报告存在8条重复键，全部位于同一JSON对象内部，值虽相同仍不通过严格JSON合同：

- `/shadowBudgetSummary/semanticSceneCanonicalReadyCutoutCount` 两次175。
- `/shadowBudgetSummary/semanticSceneCanonicalReadyAlphaBlendCount` 两次0。
- `/shadowRuntimeV2Summary/stage13ContentPersistent{Eligible,Hit,Miss,Create,Reject,IdentityMismatch}Count`
  六个键各出现两次0。

源头在当前主树war3_perf_monitor.cpp的重复emitter；之前shader/schema等修订在不同集成树里，
不能把旧通过记录继承给当前主树。此前离线UIfixture采用的是旧有效报告JSON，没有覆盖本次
真实producer输出重复键，这是诊断交付的验证缺口。本轮不修改/宽松化旧parser或重写报告。

本轮独立诊断reader保留所有重复值为列表标记，只分析路径唯一的FPS/计时字段。
`strictJsonValid=false`、`productAccepted=false`。这不是产品正确性或GPU故障判定。

可确认报告中：framesIncomplete、framesBudgetExceeded、arenaOverflow、CSM hold/fallback、
zero-strength等已导出值为0；CSM4级、4096分辨率、Direct模式保持。
但旧版的framesProducerIncomplete和producerRequiredCasterOmissionCount本报告没有导出，
不能把缺失当0，也没有同轮外部GPU event/incident和视觉等价证据，所以不能声称完整性门全过。

Arena汇总均值2.457→27.395MiB，峰值4.124→29.025MiB。它来自各自recording范围fallbackArenaBytes
聚合（22779与9037帧不同），不是进程总显存/内存，也不单凭此断言泄漏；但提示资源策略存在
明显差异，不能只看FPS将139B认定为更优稳定版本。

## 6. 最终判定和后续动作

1. 承认用户体验及保留窗口的涨帧/尾延迟改善：约+17.84% FPS、-1.504ms平均帧时。
2. 数据树真实运行并闭合，热点已集中到Capture/Populate/VisiblePublish；继续拆它们的Self。
3. 暂不能证明“几乎全部性能损耗来自收集”，也不能把涨帧标成5字段投影的净收益。
4. 先修producer重复键、补齐缺失完整性观测，选定同一集成源码基线；将only-instrumentation、
   only-projection和control分开，核相机/输出分辨率、shadow/filter、人口，再做stats-off A-B-B-A。
5. 不建议现在反复要求玩家重跑本报告：格式与源码基线问题应先由开发侧解决。

本轮仅诊断和离线记录，未改渲染代码/报告producer、未构建、部署或启动游戏。新比较工具
`AutoTest/analyze_data_collection_perf_report.py` 不继承任何产品接受资格；6项定向测试通过，
覆盖重复值保留、结构闭合、缺树、错父节点、坏tick和NaN拒绝。

收据：`AutoTest/artifacts/data_collection_tree_20260913/report-comparison-20260913-v2.json`。
旧v1草稿保留，v2增加独立整数树核验与缺失字段标记；原HTML始终不变。
