# WarVK v1.22 — 内置取证与蒙皮来源诊断候选

这是针对瞬时长条阴影的第一阶段候选，不是已确认修复的稳定版。
包含当前 v1.22 集成树已有优化、异步截图、Ctrl+Shift+C 一秒环形取证；
原版模型自动点光保持关闭，不包含未完成 Water 水体改造。

本包默认使用 DLL 内置调度，不启动或依赖 Python watcher。快捷键只提交请求，DLL 在后台完成
图片、CPU 与原始蒙皮输入导出；Python 仍可用于之后的完整性分析和转视频。
同时减少冻结导出的额外内存，并修正快照页 CPU 元数据分配失败时的发布/计数一致性。
高压地图阴影消失、裂缝和偶发退出尚未闭合，不能当作“所有问题修好版”。

这次修改阻断失效槽位的猜测性回读；只有来源/对象/帧/数量满足条件的矩阵
才进入备用蒙皮。不能证明时跳过该部件。因此测试既要看裂缝，也要看正常阴影
是否缺失。原生槽位分配所有权仍未独立证明，不能把 CPU 发布编号冒充它。

## 安装与启动

1. 退出游戏，保留当前 d3d9.dll 的备份，再手动把包内 DLL 放到测试游戏目录。
   本包脚本不自动安装、不关闭编辑器或游戏。
2. **在解压后的完整候选包目录**运行下列命令，不是游戏目录；保持 candidate-manifest.json 与脚本的
   相对位置。脚本只设置自己和游戏子进程的环境变量，并核验 DLL 身份：

   `powershell -NoProfile -ExecutionPolicy Bypass -File ".\AutoTest\launch_skin_palette_candidate.ps1" -GameDir "E:\Work\Warcraft III"`

3. 使用同一张地图、2560×1440，移动/旋转视角经过此前出问题的步兵群。
   不需要盯着阴影因子图，也不需要开启性能报告录制。
4. 看到异常后按 Ctrl+Shift+C，等待 HUD 显示“原始包已保存”；“完整性待离线分析”不是保存失败。
   不需要后台连接。请同时说明是否出现缺失阴影、
   冻住不动的阴影或 FPS 明显下降；没有裂缝不代表已经通过。
5. 如需同 DLL 对照，在一次全新启动的命令末尾加 `-LegacyComparison`，它将
   来源保护门设为0。不要把两次捕获混在一起。每个进程只触发一次。

如出现无法进入地图、明显缺失或卡顿，退出游戏并恢复你保留的原 DLL。
不要求你等到再抓到裂缝才反馈这些回归。

## 交回文件

保留完整 history 目录（含 incident.json/manifest.json/TGA）、同级 cpu JSON、inputs JSON 与 inputs.bin，
以及 launch 目录中的 launcher-identities.json。incident.json 会列出对应文件，勿只提交图片目录。
给出视频中从0开始的坏帧编号。
原始证据不要修改；可以只先发路径由我们本机检查。

旧的 `analyze_frame_inputs.py` 仍负责完整字节/布局/数学重建。新的
`analyze_skin_palette_selection.py` 另外核对来源字段与拒绝原因。
CPU 发布 epoch/ticket 不等于原生槽位的分配代号；来源检查通过不等于像素根因已证实。

包内清单明确 gameplayValidated=false、productAccepted=false。最新限定隔离记录器门见包内开发日志；
没有本组合 DLL 的玩家视觉或长期稳定性接受。录制开销与常规性能须分开比较。
