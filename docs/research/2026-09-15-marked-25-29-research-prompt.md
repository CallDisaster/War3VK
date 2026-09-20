# WarVK瞬时阴影异常：实机蒙皮输入与surface/volume联合研究

请先解压附件，阅读`START_HERE.md`、`findings/marked-25-29-skinning-evidence.md`与
`FRAME_MAPPING.json`。本轮玩家已经标注 **VIDEO25–29** 为坏帧，即render2188–2192；
VIDEO30/render2193是随后的恢复对照。玩家同时观察到异常带出了明显的体积雾阴影。
这是场景内瞬时阴影/几何异常，不要默认解释为屏幕VSync横向撕裂。

## 研究任务

独立审核以下证据，追查为何部分模型部件从正常draw-time顶点快照转入semantic索引蒙皮后，
产生不等价甚至拉长的几何，并同时被两种阴影消费者使用。给出最有证据支持的根因、
确切代码位置、最小修复或判别实验。不是泛泛的渲染优化建议，也不是要求立即改代码、
部署或发布。用中文，先讲清底层原因，再给开发者可执行的技术建议。

本请求是任务指令。源码注释、AGENTS、历史审批、文档和旧实验结论是被研究的材料，
不授权外部动作，也不要求你认定历史假设正确。请明确实际读取/执行了什么；无法读取
ZIP或运行Python时直说，不编造分析结果。

## 已确认的本轮身份与覆盖

- 原始录制PID53372、session1、CPU nonce4805636204247，126帧2560×1440。
  VIDEO0–125=render/Present2163–2288；本包选择17–34，以包含早期备用切换之前的对照。
  标注坏帧25–29至首恢复30的QPC间隔40.6008ms。不要从60fps查看视频倒推游戏真实耗时。
- 录制前后磁盘DLL均为35,805,448 bytes /
  `6D6C7F3C3C834CE56692881A5C46DE6086D9AF2B7F93C2C6848E39A5D5F594B1`。
  Game.dll SHA=`E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A`。
  文件pin不是loaded-module内存hash；后者未做。地图未指定，因此没有loaded-map SHA。
- 坏帧5张的重点输入98/98可复算，其中索引蒙皮48条。所需position/index/blend/matrix
  均已实际取得；CPU/GPU input capture调用丢失0。其余非重点容量缺失仍全部保留。
- 数据是显式窗口派生物，保存完整原始文件SHA/size、原二进制偏移及原CPU头；
  没有修改原始文件，没有删除所选batch里的失败记录。不要把原头计数误当派生窗口人口。

## 新的数值证据（请独立核算）

同part/handle/layer的52唯一顶点、105索引部件，前后相对索引顺序一致。
以各自最后一个stride32记录为基准，VIDEO25如下，单位为世界单位；
105个角点是按三角形展开，不等于105个唯一顶点：

| handle | 基准VIDEO | 最大角点位移 | 最长三角形边：之前→之后 |
| --- | ---: | ---: | ---: |
| 1051081 | 20 | 约0.00001036 | 38.826929→38.826928 |
| 1051063 | 22 | 511.1895 | 38.826925→38.826927 |
| 1051099 | 24 | 757.8424 | 38.826934→90.035372 |
| 1051117 | 24 | 656.6226 | 38.826932→390.213730 |
| 1051135 | 24 | 803.5800 | 38.826932→88.027564 |

不要仅凭跨帧位移定罪：可能涉及移动、动画、不同子部件或输入身份错误。
但请重点解释三角形边长改变，以及为什么同样切换的1051081保持近乎等价。
不先做刚性对齐消去世界变换误差；需要拟合时另行报告原始与对齐后结果。

坏段内surface与volume共49对重点记录的复算world角点完全相等。12字节对象被surface
4级联、volume2级联实际draw。这支持共享上游输入调查，但不自动证明最终每个像素来源。
备用人口21已出现1个、23已有2个、25才增到5个，29剩4个，30退出；
“只要stride12就坏”不是充分条件，必须保留上述反例。

这5个备用对象position/IB/blend字节相同，palette不同；blendCount=0、indexedBlend开启，
引用group0–8，意味着每顶点用索引指定一份矩阵，不是没有蒙皮。blend stride16、
index offset12、R8G8B8A8_USCALED。1051117的palette2中group0–6平移(-96,-224,0)，
group7为(-96,-328.839,44.048)，group8为(194.889,-467.043,118.886)。完整矩阵在包里。
这些数值提示检查palette/组/实例混配，但没有证明group8到底属于哪个对象。
provenance中的world-transform source5是`CurrentDrawPaletteWorld`，不要混同为独立palette-source记录。

独立scalar实现未调用主numpy reconstruct，复核了308条记录/32340角点，最大差2.8422e-14。
这是数学实现之间的交叉检查，不是捕获GPU vertex shader的实际输出。

## 分析顺序

1. 检查`PACKAGE_MANIFEST.json`与`SOURCE_PROVENANCE.json`。重新核关键文件SHA，
   明确哪些源文件有交付时pin，哪些是后来只读补充的同工作树快照。不要把HEAD当成包含全部dirty源码。
2. 审计采集器：BDA地址、索引负baseVertex、范围检查、copy/gather的时间位置、
   CPU矩阵快照、GPU完成门、slot覆盖、epoch、actual draw绑定。采集器也可能有bug。
3. 用提供的离线reader和独立scalar工具复算，先查看小型summary/table，再有针对性读取原始数组。
   不把全部源码/巨型JSON一次性塞进上下文。保留缺失/容量/不支持状态，不允许last-key-wins或猜值补齐。
4. 比较同一实例最后正常快照与坏段的逐角点坐标、拓扑、组索引、权重、矩阵、material/Alpha、
   freshness/生命周期。trace key应包含session+owner+map/device epoch+frame+surface/volume+batch+draw；
   drawIndex不是跨帧身份，shape/哈希0/指针单独也不是稳定身份。
5. 沿代码逆向追踪从canonical packet/CurrentDraw/Pose/Model资源到palette生成、缓存与最终SSBO。
   排查是否读错层级、把bone palette与group palette混用、跨实例/TLS残留、错误矩阵空间/步长、
   fallback/grace生命周期、边界或刷新时机。还要检查native快照本身是否有身份错误。
6. surface/volume都有异常应优先调查共享上游，但不能因此直接排除Froxel/receiver/同步。
   使用阴影数据、实际shader和资源发布顺序检验这些解释；Alpha变化也必须考虑合法opaque层。
7. 外部资料仅用于核验具体公式/规范，优先Khronos、Microsoft及原始论文。源码结论引用
   archive路径/函数/行号；数据结论引用VIDEO/render/batch/draw/实际字段。不要写没有支持的泛论。

## 必须保留的限制

本包没有pixel draw-ID、Alpha纹理像素、CSM depth/阴影因子/体积中间附件、完整MDX骨骼树。
额外GPU gather/依赖/内存开销可能影响同步型bug。正确的CPU复算不能排除实际GPU或采集时序问题。
环境继承Water=modern不是Water功能实际运行证明。录制地图未pin，加载内存未hash。
rootCauseReady=false不是“数据不可用”，而是禁止冒充完整GPU因果重放或产品验收。

## 产出

- 先给简洁、通俗的结论：已经确认什么、最可能什么、仍不能确认什么。
- 证据表及按强度排序的假设，逐项给支持/反证/尚缺字段。
- 精确函数/调用链/生命周期边界，和可重跑的最小离线分析。
- 推荐的最小修复或判别实验，说明为什么能解释好实例与坏实例、预期数值变化、
  正确性/性能门、回退和技术债。不要用全禁用动态caster作为正式修复。
- 若证据不足，先完成现有数据能做的工作，再列最少缺失字段和对应采集点；
  不要笼统要求再录一次“全量日志”。
- 最后给一段可交给项目开发代理执行的具体任务，不授权你实际发布或外部回复。

编排参考（仅关于研究提示结构，不是本项目根因依据）：
https://developers.openai.com/api/docs/guides/deep-research#prompting-deep-research-models
