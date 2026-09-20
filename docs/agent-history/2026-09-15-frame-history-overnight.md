# 2026-09-15 后半夜：环形图像、实际draw状态与发布准备

## 最终状态

已经形成可运行的初版：GPU最终彩色历史 + CPU帧/阶段/CSM/实际方向draw状态 + 游戏窗口
热键/本地控制触发 + 冻结后异步导出 + 严格离线关联。不是全量GPU回放器；#8撕裂根因未定位，
不宣称修复，不推送GitHub。中间depth/vis/CSM图像、完整原始几何/纹理/palette、像素draw-ID
归因仍缺，不能保证一份样本一定解决所有GPU问题。

后半夜阶段最终候选：PE32/i386，35,670,484 bytes /
`8848ECC43D2699D303DA4F6B8CCB2BA4EC2ED49826308F82DE527C12B03025C2`。
上午玩家发现旧热键并未进入实际安装回调，已另行修正；见`2026-09-15-shortcut-and-one-second-fix.md`。
原版模型自动灯producer/consumer按用户决定均改为明确opt-in；作者显式灯保留，Water未移入。
DLL仍为候选版本资源；1.22.00统一升版/commit/tag/push仍留在真正发布门之后。

## 工程闭合与成本

- 图像环默认64帧前窗+4后窗，1440p为1,002,700,800字节图像有效载荷（约956MiB），
  总cap1GiB，初始化一次分配。不是无限时长缓存；本次窗口实际约0.47秒。
- Armed只做device-local copy、更新预分配metadata；没有逐帧CPU读回、编码、文件写入，
  没有GPU idle/LockRect。仍有GPU带宽和首次分配成本，未测玩家前台性能。
- 触发后按序通过既有三槽读回，保留实际timeline target/observed值。完成query失败不读像素；
  不确定enqueue隔离slot；Reset/epoch/尺寸变化取消，旧GPU引用按原命令生命周期退役。
- 热键Ctrl+Shift+F8只在游戏WndProc，无全局键盘Hook/SendInput。AutoTest测试通过控制面触发，
  未伪报真实键盘物理验收。后台watcher以无render锁peek等待，不持续写磁盘、不结束游戏。
- Kind18记录真正调用cmdDraw/Indexed前的MVP、pc.flags/alphaRef、sampler、imageView、
  VB/IB范围、draw索引、rawcode/jHandle、表面CSM/volumeSun路径和目标图像。
  这证明CPU记录的实际参数，不是GPU执行结果或raw geometry bytes证明。
- 高密度draw记录暴露旧全局try-lock会丢事件，R9不通过实际draw数量门。
  最终改为独立cell多生产者队列、全局reservation ticket、seq_cst写者进入/退出与冻结门。
  旧慢写者不可覆盖较新圈；cell仍busy则显式计数丢失，绝不阻塞渲染。Frozen快照要求写者退场。
- Schema5区分reserved/accepted/evicted；跨线程ticket次序不等于QPC或GPU次序，QPC只按线程校验。
  1–4版历史仍能严格读取。导出冻结后64KiB分批写，避免在Win32构建第二份巨型JSON节点树。

## 实际运行与失败记录

证据前缀：`AutoTest/artifacts/frame_evidence_runs/`。所有运行独立fresh process，非交互桌面、
2560×1440、零global input、精确candidate/backup恢复、正常退出，GPU事件与新dump均0。
以下ok仅指限定流程，不代表撕裂/产品或全量采集接受。

| 运行 | 结果 |
| --- | --- |
| spine_canary_r1 | 零进程门因编辑器38544停止，未创建事务/部署 |
| spine_canary_r2 | E21D，单张截图与Present/pipeline/复制/保存成功关联；0丢失 |
| history_canary_r3 | 命令传入player SHA多一个0，身份门拒绝；无部署/游戏 |
| history_canary_r4 | C765，runner漏传history arm session，控制面拒绝；游戏安全结束恢复 |
| history_canary_r5 | C765，6张历史图已保存，但runner重复触发CPU被冻结会话遭拒绝；不算通过 |
| history_canary_r6 | C765，4前+2后全部通过，0丢失 |
| history_full_r7 | 04B5，30秒滚动，68图全部关联约0.4706秒；CPU旧队列1丢失 |
| history_final_r8 | 8E52，180秒滚动，保留68图约0.4836秒；CPU旧队列5丢失；无GPU/退出异常 |
| history_draws_r9 | 33F7，实际draw压力中1158丢失，后续严格draw人口门失败；保留原失败证据 |
| history_draws_r10 | 最终8848，68图+实际draw逐帧数量通过；每帧584次表面CSM draw，其中388次AlphaTest；0丢失 |
| history_watcher_r11 | 同8848，首个canary和第二会话watcher端到端各68图通过；watcher触发/冻结/导出/读取完成 |
| shortcut_motion_r12 | A9FA，实际窗口过程私有路由Ctrl+Shift+C，256前窗按至少1秒保留；相机移动25种矩阵，132图、预触发1.0019秒、0丢失 |

R10窗口0.4682636秒；113k左右保留事件，整轮accepted=reserved=3,746,671、producerLosses=0。
环开始边界保留部分已打开span尾部，因此全局unmatched有边界项；目标68帧的pipeline、
shadow及实际draw单独闭合，不能静默把所有global unmatched清零。
R10包含实际surface/volumeSun draw数据；不把两条路径的统计混在一起。

已经检查R7/R8的5帧联络图，未在这些取样图中确认玩家提供的坏多边形。其余滚动帧已被
正常覆盖，未录像逐帧人工检查。因此不能说“三分钟完全没有裂缝”，也没有复现或因果修复。
原用户四张PNG与像素定位仍原样保留，不用成功运行冲掉原始问题。

## 编辑器与现场保护

用户允许自主后半夜测试，但编辑器PID38544仍开着。未强关它；只读核实它位于玩家目录，
没有d3d9/Game/dxgi模块，只有只读YDWE工具库。新增严格PID+exe+creation-time例外，
只允许这个已核验编辑器存续，任何其它游戏/构建/编辑器仍会阻断；不据此做FPS比较。
启动、恢复均复核。测试在`E:/Work/War3`，玩家`E:/Work/Warcraft III`从未部署/覆盖。
两目录最终均恢复/保持74CC /35,486,415。原地图5,859,935 /
926822FC233C41431397499F6A922AB988C052BA0CA2E7EB14CB2B0581F5F4D8未改。

## 核心证据身份

| 文件 | bytes | SHA-256 |
| --- | ---: | --- |
| spine_canary_r2_20260915/receipt.json | 386 | FAC82676857F9EFDE9667F0ADB1B7BEBAC9577EA697EC5EC6BA6F3307FD6D337 |
| history_canary_r6_20260915/receipt.json | 939 | 7C991066897700E078715E819B1B8F4E4C8A016D7B9EF8AEC0B377C1326EF47B |
| history_final_r8_20260915/receipt.json | 1044 | 0E11AE403B5B8269225EF3A446E4F5A4BB7B2C2A141A91995BED75D5DE544E65 |
| history_draws_r9_20260915/receipt.json | 1037 | BBDAFF0DE541CAD1A2A3018A373595EEDFFFA51BF0E2993F1427FE9590D0DFE2 |
| history_draws_r10_20260915/receipt.json | 1034 | 6AEF10102EFE5972250CDBB10EBADE91C8B037F734BE836CE259B231E54D1555 |
| history_draws_r10_20260915/frame-evidence.json | 61113200 | CFD01AF722CD78E5DD8CBEEF08293E1B395C7354A414E3C1362EA89951CA38E4 |
| history_draws_r10_20260915/history-manifest.json | 25720 | 1FC987943E120EF9ACE57BB8BB9AA84C1DB4D91ADBD7E85422972489F0D06B0A |
| history_draws_r10_20260915/history-analysis.json | 33097 | D39BB6FCEF8C9E9C2B826C7185C191C83481F28BD89EDE0FB9BB44D643EF6F82 |
| history_watcher_r11_20260915/receipt.json | 1067 | C449A6A8DDABD09C7A5338C01E6393CCC720F3AD34BFA9C903CC4BF45EE06BBB |

## 验证/发布准备

相关Python/static、Win32 ring/runtime与history预算顺序测试及exact DLL构建/no-work已执行；
最终226/226定向Python/static通过（不是全库）；6份工具py_compile通过。Win32核心39,984断言、
history预算/顺序816,238断言通过；最新runtime gate-off3、gate-on27项（四线程10000事件全收、0丢失）通过。
这些循环断言不冒充同数量独立测试。构建日志在`frame_evidence_foundation_20260915/history-build-r*.log`。
候选包、工具说明、源码审查快照与发布阻断清单全部CreateNew准备，不自动安装或上传。
根稳定CHANGELOG和正式版本资源不提前改；待发布说明保留同步优化原理、实际验证边界、
Water/自动模型灯排除及未解决项。完整取证仍欠GPU深度/vis/原始输入，根causeReady继续false。
