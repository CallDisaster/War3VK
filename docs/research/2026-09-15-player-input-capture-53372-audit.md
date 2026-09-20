# 玩家53372帧输入录制：可用性复核

## 结论

可用于既定的有界Stage11输入研究，当前无需因采集缺失重录。仍需玩家给出本轮VIDEO坏帧
和异常位置后，才能做正确的坏/好帧选择并生成针对性研究包。不能以旧录制的69–71/103–106代填。
这不是根因确认或完整GPU replay批准。源数据均保持原样。

证据目录：`E:\Work\Warcraft III\WarVK\Log\FrameEvidence\history-53372-4805636754173-1`。
独立复核输出：`AutoTest/artifacts/player_input_audit_53372_20260915/`。
脚本：`AutoTest/audit_frame_input_capture.py`，使用桌面交付包的冻结reader，而非替换其schema。
已重核该交付包全部45个文件pin。

## 实际结果

- 126张连续2560×1440 TGA，VIDEO0–125 = render/Present2163–2288。
- 触发前122帧/1.0065556秒，触发后4帧；首末QPC间隔1.0407911秒。
- 每图CPU复制/pipeline/shadow/actual-directional-draw映射通过，帧内未配对scope=0。
  CPU producerLosses=0，input captureDrops=0，所有输入batchGPU完成门通过。
- 完整输入保留环576次调用中13138条可复算，focus4830/4830。
  图片窗口内focus2272/2272：stride32 flags4=2166，stride32 flags0=46，stride12 flags3=60。
  126张图的missingInputFrames和missingFocusFrames均空。不能把整个输入环人口冒充图像窗口人口。
- 额外复核原始旁路`.json.inputs.bin`与history内`inputs.bin`完全相同；JSON根原始/副本相同。
  新计算CPU/history/input analyzer结果与原导出报告完整相同，非只比汇总字段。
- 所有TGA头、字节数、连续帧号、尺寸、可解码性与SHA复核，运行前后文件SHA不变。

主要pin：

| 文件 | bytes | SHA-256 |
| --- | ---: | --- |
| manifest.json | 48503 | C4FCAC5C7E3B76786F65C69B8817044549F7220123FBF0F7F1237BED6E4BD4C1 |
| cpu-events.json | 132464389 | 66AC15CAF30BB170E064999B75A1217B3062C37C77ED0F2F2882E6B2FD6949BE |
| inputs.json | 85527340 | 569CE42C6AE1E547D5C32CE31BBBEDBC6BFBC95FCDB40280F7507FEE02BB9066 |
| inputs.bin | 150987704 | 176724ECF2CC4964BFA58DF098C3B61D19434B9CC729CC631F2AEA12D43501E2 |

## 限制和身份

启动前后磁盘DLL为35805448/6D6C7F3C…F594B1，与交付源码包一致；不是loaded-memory hash。
地图未由启动器指定，因此没有地图pin。环境继承`DXVK_WAR3_WATER_RENDER=modern`，
不可单凭该值推断Water代码被部署或运行。非重点span容量拒绝88992（整个输入环口径），
必须作为缺失保留，不可补0再判等。无pixel draw-ID、texture pixels或中间shadow/volume附件。
不依据当前进程状态倒推录制期间GPU事件、后台负载或玩家的所有设置。

按需查看了VIDEO0/62/125的一张缩略拼图，路径`contact-0-62-125.png`；确认正常游戏场景、
镜头/模型内容变化，不是黑帧或HUD挡住的无效录制。没有穷举视觉坏帧，没有认定这三帧无瑕疵。
日志中VIDEO21–29出现stride12重点输入，此项只作为路径人口线索，不是玩家坏帧标签。

桌面研究提示词与本段PR视频放在`WarVK-53372-PR-review-20260915`。提示词明确区分事实、
历史假设、缺失数据与待填坏帧。参考OpenAI Docs的research prompt指导，未指定新的模型或上传。
本轮没有编译、部署、启动/停止游戏、改动C++或删除原证据。
