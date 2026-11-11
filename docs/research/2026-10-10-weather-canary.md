# 2026-10-10 真实 JASS 暴风雪 canary

## 最新结果（主线程最终补记；以下初版准备表保留为历史）

当前功能套件已扩至 15 个阶段、13 张 2560×1440 图、35 项 JASS 检查，
不是下文初版的 12 阶段/9图。`canary-viewport-after` 在最终 DLL
`8A156518BD029D106EC009DC3C702D0005746E2E97E4589C1C18F8F4684090CE` 下
`runtimeAcceptance=true`，35/0；支持的全部功能判据通过，精确截图天气帧、GPU 完成、
全部美术验收三个字段仍为 false。

隔离桌面初始注册表为1280×720，使用本次拥有的HDESK/HWND安全调整 client 为2560×1440，
真实图片尺寸独立确认；没有写注册表或全局输入。旧 foreign-TID desktop 查询失败的
`canary-d` 仍保留为失败，不归入通过。玩家原图/DLL/Game.dll 前后 SHA 一致。
进程按拥有的 Job 强制清场，不把它称为自然退出门。

长循环的最新成功批次为 `stress-b`：70阶段、1083检查/0错误，32次启停、8次重建、
7张阶段夹持图片。`stress-a` 的70个旧PostFX错误保持失败；新版验证该未实现开关
被明确拒绝，未扩API能力或宣称PostFX总开关可用。见 [长循环实据](2026-10-10-weather-stress-runtime-results.md)
及 [晨间交付](2026-10-10-weather-morning-report.md)。

## 目的与边界

在 AutoTest 拥有的独立进程/目录/桌面中，验证本轮 12 个公开天气 wrapper 真实进入 DLL，
观察雪粒子和天气体积介质的命令记录，保存阶段画面。GPU 命令已记录不等于 GPU 执行或美术验收。
当前只准备脚本与新地图；没有编译 DLL、运行 pjass 或启动游戏。

写入仅发生在新 `AutoTest/artifacts/weather-night-20261010/` 工件和本 lane 三个新源文件。
玩家地图、d3d9.dll、Game.dll 为只读保护输入。不会调用键鼠、切换桌面或写视频注册表。
运行要求已有 2560×1440 设置，否则准确报告配置缺口。仅停止本次 session 拥有的进程。

## 文件与依赖

- `AutoTest/run_weather_canary_20261010.py`：独立 Prepare / Check-Jass / Run 三阶段。
- `AutoTest/weather_canary_20261010.j`：原版 config/main、真实公开 wrapper 的场景动作。
- Python：`D:/Environment/Python3.13.11/python.exe`。
- 现有 AutoTest sandbox：`E:/Work/War3_AutoTestSandbox`。
- 基础地图：`E:/Work/Warcraft III/Maps/Test/WorldEditTestMap.w3x`。
- 当前完整 API：`WarVK/jass/warvk_api.j`。
- 32 位 PowerShell：`C:/Windows/SysWOW64/WindowsPowerShell/v1.0/powershell.exe`。
- StormLib：`E:/Work/War3/YDWE1.32.13 - MemoryHack/bin/StormLib.dll`。
- PJass：同一 YDWE 的 `compiler/pjass/pjass-latest.exe`，搭配 `compiler/jass/24/common.j`
  和 `compiler/jass/24/blizzard.j`。所有工具的精确 size/hash 写入 prepare.json。
- Run 懒加载 `AutoTest/war3_autotest_mcp.py` 及其 `requirements.txt` 中的 MCP 依赖。
- DLL 路径和 SHA 必须由主线程传入，不能把旧 build32 自动当成本轮候选。

新地图保留源 MPQ 的地形、装饰物和资产，但替换整个 war3map.j；原脚本作为工件保存。
不携带原地图玩法触发，避免旧 Walk/实验触发干扰。参照物为两名步兵、主堡与兵营。

## 当前准备回执

- Python AST 解析：PASS（语法结构检查，不等于脚本执行覆盖）。
- 帧判据合成反例3项：PASS；live观察领先5帧但render/frozen同帧可接受、同帧volume可关联、
  volume错帧不得认证。该反例测试不启动游戏，也不充当真实命令执行证明。
- 后续维护测例 `AutoTest/test_weather_canary_classifier.py` 直接导入真实 launcher：首跑20例出现
  8项误通过（EXIT 1），按授权修正判据后24例全部通过（EXIT 0）。
  原8项为：kind伪标、active id=0、未Pause时间静止、仍Enabled时间静止、冻结时钟错id、
  重复截图阶段、错label，以及信任旧alignment bool。所有修正仍使用原时钟/帧域门限。
- 原图：5,861,385 B / `692E92F43BAC99EF8ECED7833E137B22475914F592DA7F78FCC24B54C221E95F`。
- canary-a 新地图：5,874,978 B / `A01F92B127E755229DA3492D54744FEBD4C9E51CCEF2F84997E7BA512546E885`。
- 当前完整 wrapper 数：130，含本轮天气 12 项。
- 原脚本保存、MPQ 替换后逐字 readback、原图 hash 未变：通过。
- JASS 语法检查、真实 VM、GPU、视觉：未执行。

地图路径：`AutoTest/artifacts/weather-night-20261010/canary-a/weather-canary.w3x`。
生成清单：同目录 `prepare.json`。

## 自动演出

地图启动后 25 秒开始，每阶段间隔 8 秒；总计约 113 秒。时间来自地图 Timer 与 runtime
单调时钟，各自证据保留，未声称对音频设备采样时钟同步。本 canary 不包含音乐文件。

| 阶段 | 操作 | 预期 |
|---|---|---|
| 0 | 桥/协议/runtime/feature 查询，Create，Region，Target，Clear/Add；Sun 与旧 Volume 都关 | 天气未开启，基准画面；作者参数被接受 |
| 1 | Play，从 0 秒开始 | 轻雪开始，沿最短方向从350°转到10° |
| 2 | 查询时间（已播放8秒） | 到达暴雪，Sun/旧Volume关闭仍能记录天气介质 |
| 3 | Pause，保存演出时间 | 雪与密度运动冻结，画面保持 |
| 4 | 检查8秒后时间不变；Seek到2秒并保持暂停 | 新天气历史、较轻参数，旧暴雪雾不残留 |
| 5 | Resume | 从2秒继续，不补算暂停墙钟 |
| 6 | Disable，保存时间 | 关闭效果，演出时间冻结 |
| 7 | 检查8秒后时间不变；Enable | 原位继续，天气重新可见 |
| 8 | Disable，Clear/Add替代关键帧 | 新配置允许编辑，保持冻结外观 |
| 9 | Play替代时间线 | 中等雪量、90°风向生效 |
| 10 | Destroy，查询旧id无效 | 天气退场，无旧实例复活 |
| 11 | 再查旧id、保存最终receipt、停Timer | JASS checks全部为0错误 |

计划截图9张：0/1/2/3/4/6/7/9/10；检查实际尺寸均为2560×1440。
图审看近中远层次、遮挡、UI、屋外范围、暂停稳定性、Seek残留、Disable/Destroy清理；
不会根据 CPU 开关或颗粒估计直接写“画面通过”。

## 观察与判据

JASS 用 PreloadGenEnd 向本次 instance 的 `WarVK/Temp/weather_canary_stage_N.txt` 写
stage/checks/errors/id/time，保留原文及hash。Launcher 通过 `weather-status` 采样：

- runtime：作者id、enabled/paused、演出时间、revision/historyRevision、强度参数。
- render：雪的 commandsRecorded、粒子估计、资源占用、执行阶段和自身 id/time/帧/地图/设备身份。
- frozenInput：实际渲染按值快照，含 enabled/paused、id/time、三帧域、revision/historyRevision及参数。
- volumetric：天气介质的 compositeSubmitted、执行阶段、帧/地图/设备身份与预算占用。

runtime.observationKind 为 owner-control-live-state-not-render-input，frozenInput.observationKind 为
render-consumed-value-snapshot。render/volume 来自 queued frame，允许相对于 live runtime 的自然滞后。
每条记录写 liveQueryLagFrames，不以 live frame 和渲染帧不同直接判失败。
joint-frame 判据要求 render 与 frozenInput 的 frame/map/device/id/time 完整对应，volume与同一
frozenInput 的 frame/map/device 对应；且实际天气时间落在 JASS stage 的观察时间窗。
不同帧读数明确为未匹配，不能伪装成同一冻结输入。volume 不含 historyRevision，由同一 frame
的冻结快照关联，不借用 live runtime 的 historyRevision。

认证时重新从原始 observation 计算帧关系，保存的 alignment 布尔值仅是说明字段。
活动绘制必须有正 id 和正确 frozenInput kind；Pause/Disable 时间稳定要求相应状态、同id、
且时间与阶段receipt相符。跨阶段边界的瞬时样本不纳入该阶段证明。
截图必须精确覆盖9个 stage/label组合，阶段唯一；截图完成后复核stage，跨8秒边界则明确未覆盖。

CPU/runtime 准入：12阶段完整、最终JASS错误0、暂停和停用时间稳定、Seek/Destroy结果正确。
绘制观察：记录对应演出时间范围的雪命令与天气体积合成；缺失或身份无法关联则未覆盖。
视觉/GPU完成：始终另列，只有真实图审/相应完成证据后才能更新。

## 主线程执行命令

```powershell
& 'D:/Environment/Python3.13.11/python.exe' AutoTest/run_weather_canary_20261010.py --prepare --name canary-a
& 'D:/Environment/Python3.13.11/python.exe' AutoTest/run_weather_canary_20261010.py --check-jass --name canary-a
& 'D:/Environment/Python3.13.11/python.exe' AutoTest/run_weather_canary_20261010.py --run --name canary-a --dll <本轮DLL绝对路径> --expected-dll-sha <本轮64位SHA256>
```

Prepare 已执行，不能重复使用同名覆盖。后续若 harness/API 改动，要新建 canary-b 等新名称。
Check-Jass 与 Run 均等待主线程 lease。Run 保存 run.json、完整阶段回执、截图身份和源输入收尾核验。
存在既有run/capture/game工件则启动前拒绝重用；本次stop失败或玩家输入漂移会使准入失败，
保存具体收尾错误供主线程恢复，不掩盖成成功。
保护输入缺失或读取失败逐文件写error，不让hash异常抹掉整份收尾回执。
隔离桌面的时间/命令记录可用于功能诊断，不作为前台 FPS 发布基线。

## 隔离安全复核

Launcher 只调用专属 `War3_AutoTestSandbox/_AutoTestInstances/<run>/<session>/root` 的既有入口，
候选DLL由 AutoTest 复制到新实例；基础exe/dll采用复制，只有大MPQ采用硬链接。
本canary不写MPQ运行文件、玩家目录或注册表，不发送全局输入，不操作其他进程。
创建前核验地图/API/harness/script/DLL与工具身份；启动后再核验实例DLL，finally仅按本次session停机，
保存玩家地图/DLL/Game.dll前后hash。这里的复核属于源码安全审查，真实清理结果由run.json给出。

维护测例命令：

```powershell
& 'D:/Environment/Python3.13.11/python.exe' -B AutoTest/test_weather_canary_classifier.py
```
