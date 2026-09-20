# 玩家175帧内两次裂缝：锁定Stage11表示切换线索，尚非最终因果修复

## 结论

用户标记VIDEO69–71和103–106，与原始TGA像素突变一致，确认为0起始视频编号。
两段都有一组Stage11投影物从32字节快照表示切到12字节、flags0x3的显式索引蒙皮表示，
其存在区间与玩家标记高度一致；删除这些备用投影物的下一帧，异常多边形消失。
这是比“怀疑雾/同步/随机黑帧”更窄的证据，但尚无像素draw-ID或骨骼/顶点原始字节，
不能断言某一个draw已经被GPU证实画出该像素，更不能宣称某个修补已通过。

本次只做源证据分析、只读源码核对和本地已提取MDX结构检查；没有C++/shader修改、构建、
部署、游戏启动/关闭、GitHub回复或发布。当前原始样本继续有效，不要求用户先再随机找一次。

## 身份、时序与图像

源：`E:/Work/Warcraft III/WarVK/Log/FrameEvidence/history-34548-4617011687416-1/`。

- manifest：7A9243F4E7DACB2A7F8A793FCE77B3747ED1062B17A0909C2DA2BF6AE1846B12。
- cpu-events：45B262FE0F9E1678FB88E688E80236927A817D9BDAA9CE879E3D0A37FBCE87E9。
- history-analysis：E9457BFAF1A7F4279480A0390DE4B9A8ECC11A435D6848E4ACA8E0A9E41122BE。
- schema6，producerLosses=0，保留原来的capability=false与全局边界unmatched，不追认完整GPU回放。
- TGA用manifest顺序和既有逐文件SHA核验。VIDEO=render−3927，Present与render在本段相同。

| 事件 | 视频帧 | render/Present | 下一正常帧 | 首坏至首好QPC间隔 |
| --- | --- | --- | --- | --- |
| A | 69–71 | 3996–3998 | 3999 | 17.8597ms |
| B | 103–106 | 4030–4033 | 4034 | 21.4230ms |

60fps检查视频故意放慢；上表实际时间来自原QPC，不能按3/60或4/60秒描述游戏持续时间。

已经检查A/B概览和最终A-local/B-local两个局部。A为地面大片硬边尖角多边形，B为两条
明显长三角条带；在首好帧直接消失。A71→72与B106→107相机矩阵分别逐字相同，
消失不是这两个帧对的相机移动边缘差；开始时相机发生变化，属于重要复现上下文。
有些相邻帧图像几乎完全不变，不因此丢弃真实render帧。

## A：匿名614顶点/1587索引部件

仅用shape signature，不把临时drawIndex84或geometryHash0当唯一对象ID：

- VIDEO0–68：indexCount1587、stride32、flags0x0（不重新蒙皮，不做AlphaTest），
  该候选geometry hash非零。
- VIDEO69–71：stride12、VB7368bytes=614×12，IB3174bytes=1587×2，flags0x3
  （useBlend+indexedBlend）、blendCount0、paletteIndex0/paletteOffset0、geometry hash0。
- VIDEO72起：此614/1587 signature不再出现。71→72 surface draw548→544，volumeSun274→272，
  正好少一个对象在4个surface级联与2个volumeSun层中的提交；AlphaTest提交数不变。
- 本地3320份提取模型中发现两个同形候选：KiljaedenCinema geoset0与Kiljaeden geoset0。
  前者opaque，后者cutout，尺寸/骨骼组亦不同；这是离线候选，不是本进程挂载SHA或对象名称证明。

因此**第一段不是“原来开Alpha，后来关Alpha”**。不能把此前口头泛化的Alpha解释当结论。

## B：有稳定jHandle的hfoo部件

四个hfoo handle：1051099、1051117、1051135、1051063。

- VIDEO0–102：各自105索引部件走stride32、flags0x4、有效alpha image view。
- VIDEO103–106：各自同105索引/52顶点规模变为stride12、VB624bytes、IB210bytes、
  flags0x3、无alpha view，四个handle各自持续区间都精确为103–106。
- VIDEO107：它们与更早进入备用路径的1051081共同消失，surface440→420、volume220→210；
  删除5个对象×4/2层。其余hfoo正常32字节cutout signature继续存在到视频174。
- 1051081在100就进入该路径，用户没有标记100–102为此处异常；说明“任何fallback都会
  产生肉眼裂缝”并未成立，需要结合屏幕投影/对象位置，不能把相关性当充分条件。

本地stock Footman geoset1确为52顶点/105索引，但材质有opaque底层和alphaBlend覆盖层。
geoset0为447顶点/864索引cutout。在102→103四个handle还同时失去864索引正常部件。
因此B的Alpha关闭虽然已观测到，仍不能单凭此证明是罪魁；更优先查重建蒙皮/子部件/层身份。
没有把该本地MDX的SHA追认为玩家实际挂载资源身份。

## 目前可降低优先级的原因

- 所有选中帧均DirectInline：TAA模式0、history有效/advance均0、直接采样CSM。
  不能用“历史TAA残留了三帧”解释这两段。
- map render serial连续递增，4级联均提交全部准备对象，记录的per-cascade culled=0。
  对象总数变化发生在更上游集合/表示选择，不是这轮CSM内部剔除了一个同样的准备列表。
- 记录的shadow image/view身份与格式未在这些边界切换；这仅削弱重建资源猜测，不能证明
  GPU布局/内容/同步绝对正确。CSM矩阵逐帧有小幅变化，符合太阳变化，不是物体骨骼矩阵。

## 源码对应及证明缺口

`d3d9_device.cpp`：

- `War3TryAppendSemanticShadowPacket`约23664创建12字节position语义几何，约23694
  固定geometry中alphaTest=false，约23899绑定其原始位置；有fresh exact draw-time VB命中
  才在约23920后覆盖为已绘制快照并关闭重新蒙皮。
- 约22657的alpha metadata查询以packet预分类cutout/alphaBlend为前提；约24168仅有完整
  当前payload才将Alpha附回。`war3_shadow_renderer_core.cpp:1508`在无layerContract且非透明队列
  时默认Opaque。这是一个值得核查的“未解析→Opaque”授权边界，而非已证明此次实际经过该分支。
- `war3_shadow_caster_vert.vert`的flags0x3路径从`u_worldMatrices[p_paletteOffset+idx]`
  重建位置。当前记录器没有保存这个SSBO内容、blend/index原字节、实际group-slot映射或
  物体world矩阵，因此不能计算异常顶点最终投影，也不能判定双重变换/错骨骼索引是哪一种。
- 当前源码的packet grace常量为1，但运行signature持续3/4帧不允许直接等同于这个常量：
  还有native producer/frame采样节拍和visible/manifest路径，源阶段必须继续精确归因。
- 12字节表示本身也不能唯一证明调用了哪一个producer函数；文件中约47651的兼容生成点
  也构造紧凑position。当前记录器没有producer callsite标签，本报告锁定表示/蒙皮路径族，
  不把静态代码对应关系冒充实际调用栈证明。

最可疑的共同问题是：当前帧32字节已绘制快照不可用/未被选择时，备用语义路径复建了
与原生绘制不等价的几何/蒙皮/材质。要修应收紧并补齐这条**表示切换的证明**，而不是
关雾、加全局GPU等待、调阴影bias、延长陈旧VB或粗暴删掉所有离屏caster。

下一步可基于此样本制定精确合同/有界对照：当前draw缺失时的选择原因、真实模型/部件/层、
palette来源及原始字节、模式切换前后等价性；若做禁用候选路径的A/B，只算定位实验，
必须另核合法caster完整性，不能用少画阴影冒充正确修复。此轮尚未实施该改动。

## 可复核输出与分析修正

`AutoTest/artifacts/issue8_localized_20260915_history34548/`保存analysis、selected-events、
caster-set-changes、route-lifetimes、local-model-geosets、shape-candidates和A/B局部图。
首版analysis的按drawIndex匹配统计仅作线索：其中`pose`实际为lightVP，`geometry`包含
每帧地址/索引状态，不能证明每个物体形变。后续脚本已把字段名纠正为lightVp/drawState，
未重写首版归档；报告只采用counter/handle/形状区间结果，不使用索引作为稳定身份。
本轮本地生成文档/分析脚本，不改原始样本和产品源代码；结果进入开发日志，不进入稳定更新。
8项分析机械单测、5个分析脚本py_compile、git diff --check通过；manifest与cpu-events
收尾重算SHA仍一致。单测只验证编号/分组/字段解释，不是GPU因果验收。
