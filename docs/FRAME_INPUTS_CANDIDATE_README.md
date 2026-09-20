# WarVK 帧输入取证候选（不是撕裂修复版 / 不是正式 v1.22）

保留 Ctrl+Shift+C、一秒图像环形缓冲和现有优化；原版模型自动点光继续默认关闭，
不包含未完成 Water 工作。本包不自动覆盖游戏 DLL、不自动上传、发布或提交。

DLL：35,805,448 bytes；SHA-256：
`6D6C7F3C3C834CE56692881A5C46DE6086D9AF2B7F93C2C6848E39A5D5F594B1`。

## 玩家录制

1. 完全退出游戏，保留当前 `d3d9.dll` 的备份，再手动安装本包 DLL。
2. 保持包内目录结构，使用 `AutoTest/launch_frame_history_player.ps1` 启动。
   默认游戏目录 `E:\Work\Warcraft III`。地图、分辨率/画质和摄像机条件保持原测试；
   使用2560×1440。脚本不修改分辨率，不需要开启性能报告，不需要阴影因子视图。
3. 进入地图，等 HUD 显示已缓冲至少1.00秒。移动视角复现，看到异常后按 Ctrl+Shift+C。
   等待图片导出及原始输入检查完成、HUD确认包已准备好；不要刚按键就退出游戏。
4. 一次只保存一个事件，不覆盖旧包、不自动重新武装。GPU/内存或数据不完整会明确提示。

可选：`-MapPath 'E:\Work\Warcraft III\Maps\Test\WorldEditTestMap.w3x'` 可记录指定地图的
文件身份（不冒充已加载地图的独立证明）。`-EvidenceDir 'C:\WarVK-FrameEvidence'` 将输出
放到其它本地磁盘。至少预留4GiB。脚本环境变量只影响子进程，退出后无需清理系统环境。

## 数据内容与边界

新数据：实际索引、按索引提取的原始顶点/骨骼组/UV流、上传后的完整选中矩阵、
模型/部件/层/代际线索、备用路径切换条件、真实 draw 参数和 GPU 完成证明。
图片序号、render/Present、QPC、CPU事件、input batch/draw 均可关联。
CPU和GPU原始数据只保存在有界内存中，触发后才落盘。

重点保证先前105/1587索引部件的调查人口；其余Stage11尽量保留，超预算明确报缺失。
额外原始缓冲上限144MiB；不是无限制抓帧。图像GPU环仍有自己的4GiB上限/余量检查。
采集的GPU工作、依赖和内存压力有开销，不能承诺不干扰同步类bug。无异常的录制不能
证明修复。仍无逐像素draw-ID、纹理Alpha像素和各中间阴影/体积附件，不能声称全GPU重放。

## 定位帧与研究包

保留完整 `history-*` 目录。可用包内视频工具将TGA按一图一帧转换，不能重新采样/补帧：

```powershell
py .\AutoTest\export_frame_history_video.py '完整history目录' --output '新的录像输出目录'
```

在PR中记录 **从0开始的VIDEO帧号**，不是TGA的slot号码。找到坏帧后可交给本任务打包，
或使用以下命令（数字仅示例，请换成本次标注）：

```powershell
py .\AutoTest\package_frame_input_research.py '完整history目录' --bad-frames '69-71,103-106' --output 'E:\新的研究包.zip'
```

研究包包含坏帧及前后4帧无损PNG、对应时间表、这些窗口的CPU/原始输入证据、配套源码和
`RESEARCH_PROMPT.md`。不包含整个MPQ，不自动上传。`package-info.json` 记录本候选源码
快照身份；窗口包显式标记为派生证据，保留完整源文件的size/SHA和原始二进制偏移，
不删除原录制、不隐藏窗口内的缺失。请在本包目录中运行工具，别用后续修改过的工作树源码替代本候选快照。

内部128/256等slot不是永久对象ID；匿名模型路径、未知代际和缺失字段不能靠猜测补齐。
请一并提供当时是否开启体积雾/AA、视角动作、PR帧号及异常区域；无需再盯阴影因子找问题。
