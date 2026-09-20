# 玩家两组短暂阴影异常帧：像素定位完成，根因未闭合

## 本轮决定和证据边界

用户因美术适配成本明确搁置原版模型自动点光/点阴影路线。停止扩展，将该实验从v1.22
计划发布范围移出；源码和旧候选证据保留。此次只做离线图像/源码诊断，不改渲染实现、
不构建、不部署、不启动游戏、不自动替换玩家正在使用的74CC DLL。
用户要求如果无法找到根因，先停止汇报，再讨论全面逐帧采集；不能自动开始另一轮盲测。

四张原始2560×1440 PNG已CreateNew冻结到
`AutoTest/artifacts/issue8_player_pairs_20260914/`，原文件未修改。
已看用户四张图以及唯一派生局部对比图，不重复重载完整图片。

| 冻结名 | 原视频/导出帧 | bytes | SHA-256 |
| --- | --- | ---: | --- |
| color-58.png | 21.57.16.02 /00_00_16_58 | 5,210,850 | D44629A338582C5AE3FA6250F75C74CCC3E680FBCD979A17F05F6453D4AEF27C |
| color-59.png | 21.57.16.02 /00_00_16_59 | 5,223,909 | AA99E8474276A2A9ED3CC262243957DF904421A6D1DBAE2C25FEEF7754572160 |
| factor-58.png | 22.15.27.05 /00_02_50_58 | 2,190,537 | 756AD286C4CDBDE57C2AD757B4F1CE7D9980085293B1182E4FA166DD8045D634 |
| factor-59.png | 22.15.27.05 /00_02_50_59 | 2,204,306 | 2B1E80BCC35A58279AEE59117943883F359367EC9DB7855C414259DF74D4BFF9 |

## 可复核的像素结果

`AutoTest/analyze_player_fissure_pairs_20260914.py`采用原图8-bit灰度差，绝对阈值32、
3×3 opening连接域；先排除大部分UI，再对人工定位ROI单独分析。
不是线性光强估计，也不是自动异常分类器；正常动画/血条/特效/编码误差均会被算作变化。
三个ROI的phase-correlation平移估计不一致（约-2至+3 pixels），故不套用猜测全局配准。
大块内部几十至上百灰度级的变化，不只是这些细小位移的边缘差。

- 彩色组：城堡下方的地面多边形在58存在、59消失。局部ROI中主连通域20,156 pixels，
  bounds `[672,896,863,1075)`（右/下排他；下缘被ROI裁到1075），平均59−58灰度+50.197。
- 阴影因子组：右侧树影旁，**按文件名排序是58没有、59多出暗多边形**，与第一组方向相反。
  主域7,784 pixels，bounds `[1999,417,2132,556)`，平均灰度差−183.894。
- 因此不能统一给两组打“58坏/59好”标签。用户对视频56–58持续、59消失的描述保留为
  彩色组时间上下文；未提供完整视频时，不外推第二组时间方向或GPU实际持续帧数。
- 局部图：`detail/pair-comparison.png`，每行左58、中59、右差分；红=59变暗，青=59变亮。
  主异常呈清楚的多边形/长直边，不像通常整幅横向错位的scanout tearing。
  但DVR导出不是引擎内部精确帧抓取，不能据此排除全部捕获/呈现问题。

## 源码核对：收窄范围，未证明根因

- `war3_shadow_receiver.frag:1612`取得当前方向CSM可见度（或current visibility prepass），
  Temporal选用时可能再混合history；`:1774`的debugMode2将`vis`输出灰度并return。
  点光/双原生灯颜色合成在其后。若视频使用该UI“阴影因子”模式，它不是直接显示点光
  颜色或灯杯遮挡；不能继续把此问题归因原生模型灯。
- `d3d9_war3_volumetric_light.cpp:1526`按frameSerial取得CSM快照，并有独立volumeSun
  快照分支。雾开启可能改变阴影可读性、负载或资源时序；“更常见”不证明雾是错误producer。
- 多边形形态使“Alpha裁剪/纹理错态造成整张卡片投影”值得优先查，但错误VB/IB/姿态、
  阴影资源/级联/receiver深度或跨帧数据也能造成类似外观，当前不能挑其中一个冒充根因。
- 已查CSM prepare (`d3d9_war3_shadow.cpp:4163`)：alphaBlend-only跳过，已知alpha-test
  缺texture/UV不降级成opaque；`CurrentTextureDescriptor`要求diffuseTexture非空，故
  不能把prepare与最终pc.flags的双重检查误判为已找到空指针导致opaque的源码漏洞。
  这不证明坏帧的实际alpha flag、纹理内容、UV或descriptor身份正确。
- 旧issue8初始布局/迁移调查不是本次因果证据；已确认的ShaderPack 1×1 fallback缺陷
  不能转移为这两处裂缝原因。未进行 speculative barrier/全局GPU等待/关闭体积雾“修复”。

当前玩家磁盘DLL为35,486,415 /74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73；
复制时现有日志245,920 /621748D45CB0730879E40DAB658A91B6B4C7262D5FDE69CD9CECE0BFEC0F5137，
保存在`detail/player-current.log`。该当前磁盘身份不是两段历史视频的loaded-module证明。
日志没有PR帧与引擎帧一一映射；现有报告最近仍为09:43，不属于这两段21:57/22:15录像。
不能从最后日志或平均统计重建那一帧GPU实际读取的内容。

## 下一阶段采集合同草案（未实现、未启动）

目标是用户看正常彩色画面即可定位；阴影因子应后台作为同步证据，不要求用户盯灰度找异常。

1. 统一复合身份：SessionId、DLL/Game/map SHA、map/device epoch、renderFrameId、
   Present ordinal、pass instance/调用序号、CPU QPC与GPU提交/完成serial。不要把现有
   worldSerial、receiver frame、Present计数当成同一时钟。视频同帧烧录可读ID和机器标记。
2. 所有真实render invocation记录轻量状态，不只1/120摘要：相机/太阳/CSM全矩阵与split、
   debug/Temporal/Froxel配置、每级联生产/发布/拒绝、dirty/hold/history索引、资源代际、
   descriptor/view与VB/IB offset/range/index/baseVertex、最终draw数与序号。
3. draw级回溯字典：实例/模型/材质、alphaTest/ref/UV/纹理及sampler、world/palette、
   几何版本、快照/Arena页与retirement。不可变内容按精确版本一次保存，动态变化保留必要
   原始字节；仅前4096字节hash或一个总caster计数不足以还原坏三角形。
4. 同一帧分层像素证据：最终彩色、CSM各层/适用volumeSun、scene/receiver depth、当前vis、
   temporal输入输出、雾前后及实际后处理分界。还需caster/draw ID归因能力，把异常像素指向
   实际投影draw，而不只是知道CSM整体有变化。
5. 后台有界前后帧环：异常发生前的数据必须已经留存，触发只冻结窗口而不是开始才抓图。
   GPU副本/ID证据也要与相同执行序列绑定，异步完成后处理；禁止LockRect或强制GPU idle。
   全分辨率多cascade全帧保存成本很高，先明确预算、保留时长和各层覆盖率，不承诺无界全量。
6. 视频帧和游戏帧严格分开：60fps视频不能保留100+FPS的每个引擎图像；录像须存实际frameId
   映射、重复/丢帧、队列溢出及首末连续范围。要声称“每个渲染画面都录到”，必须使用引擎侧
   有界捕获并实测零漏帧；不足时明确标为不完整，不能靠提高录像帧率或QPC猜算。
7. 在再次让用户找样本前，先用可控标记帧验证端到端对齐、异常前后窗口、动态/不可变数据
   回读及满队列失败状态。出现第N帧后，应能离线定位其前后帧和实际draw输入。
   现有三连拍burst与短时/有样本上限的full trace不能直接叫作这种完整记录器。

参考：[Microsoft desktop frame metadata](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_outdupl_frame_info)
明确桌面更新可积累，不是引擎每次渲染的保证；这不是断言用户DVR采用该API。
[Khronos synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
区分执行先后与可见性依赖；诊断副本必须保留真实资源/完成证明，不能仅按两三帧延迟猜测完成。

## 本轮验证和停止点

像素工具5/5 synthetic测量单测、两文件py_compile通过；工具不宣称渲染/根因接受。
根因未闭合，按用户请求停止并汇报；本轮不新增录像/逐帧C++机制或实机轮次。
四图、原日志、测量JSON、可读局部图均已留存，可在后续研究直接复用。
