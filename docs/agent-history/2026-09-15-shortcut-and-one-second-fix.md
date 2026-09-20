# Ctrl+Shift+C 实际窗口入口、可见状态与至少一秒前窗

## 玩家问题的确定根因

玩家安装的8848 DLL身份正确。09:19启动器watcher已打印arm，历史目录已创建但没有图片。
源码显示旧Ctrl+Shift+F8分支位于`d3d9_swapchain.cpp::War3WndProcHook`；对应安装函数
标记maybe_unused且没有调用，真正窗口/全屏都使用`d3d9_window.cpp::D3D9WindowProc`。
后者没有记录器分支。所以此前控制端trigger通过不能证明实体按键接线通过；这是工具缺陷，
不是玩家没有用PS1启动，也不归因模型艺术或显卡。watcher随后WaitNamedPipe失败2只是
连接消失证据；无法从该行断言发生时间和具体退出原因。

## 修正

- 实际D3D9WindowProc在IME/ImGui键盘过滤之前调用共享Ctrl+Shift+C匹配器；
  只接受WM_KEYDOWN、C、Ctrl+Shift和首次按下，不注册全局快捷键。旧未安装回调不再承载热键。
- 内部测试通过已注册窗口的私有消息重入实际WndProc；thread-local授权只在内部调用期间
  有效，无SendInput/系统键盘状态伪造。这证明窗口分派/共享匹配/触发链，不冒充玩家实体键盘验收。
- 诊断HUD即使调试主窗口隐藏也显示；无鼠标/键盘捕获，且只在Present阶段绘制。
  状态包括未启动、分配缓冲、实际缓存秒数、已触发、导出数量、已保存、GPU/预算失败。
  历史图像仍在HUD前抓取，防止提示污染稀有坏帧；原生手动截图移到HUD后，便于验证提示。
  两次prepare只有前一次推进burst，不让三连拍变成同一Present消费两次。
- watcher记录每个状态转移而非逐帧刷日志。游戏关闭/管道消失能有界结束。
  此机器对watcher的OpenProcess(SYNCHRONIZE)会拒绝（错误5）；保留现有能力时用句柄，
  无句柄时必须用精确processNonce+session回读匹配，不能因PID复用跟踪另一个游戏。

## 时间窗及资源

默认请求256前帧槽、4后帧槽；触发时从最新前帧逆向选出跨度至少1000ms的最短连续前缀，
再追加4后帧。尚未积累足够时间或高速超过容量时保留可得数据并标durationSatisfied=false，
分析器拒绝一秒验收，HUD显示实际秒数；不降分辨率、不限制游戏FPS、不谎称固定帧数等于一秒。
未选旧帧按trimmedPreFrames记账，不与lost/evicted混同。history manifest升至schema2。

2560×1440全分辨率预留260图约3,833,856,000 bytes（3.57GiB），绝对cap4GiB，
还要求所选device-local heap可用预算至少再剩1GiB，否则明确拒绝。预算取已有DXVK
heapBudget/usage接口，分配失败继续fail-closed；驱动预算不是无限内存保证。
CPU事件容量上限同步为262144，schema6保留旧1–5读取；冻结导出仍分批写。
这是显存换反应时间，未宣称低开销/前台FPS提升；初始化可能产生停顿，普通游戏模式默认关闭。

## 验证

最终DLL35,693,151 /5B6D4E4131FED3C940498ABD2DF687CF9C6A2F7317990448B7CDB9DC4D91262F，
PE32/i386，BelowNormal/-j2 exact DLL构建及no-work。玩家目录8848未改，测试目录74CC恢复。

- `shortcut_motion_r12_20260915`（中间A9FA）：132图，触发前1.0018525s、窗口25种相机矩阵，
  实际窗口回调notice=1、每图对应实际方向draw均闭合、0图像/CPU丢失。零GPU事件/dump、正常退出恢复。
- `shortcut_hud_r13_20260915`：首会话窗口回调/6图通过，watcher新句柄检查因访问拒绝失败。
  安全结束恢复，未算watcher通过；PrintWindow在隔离桌面失败，未回退截取用户桌面。
- `shortcut_hud_r14_20260915`（最终5B6D）：新增nonce/session回退，watcher第二会话126图，
  触发前1.0030183s，0CPU丢失；acknowledge完成。通过HUD后原生截图肉眼确认左上角文字
  “帧记录器 Ctrl+Shift+C / 调试包已保存，可退出游戏”。仅查看该HUD局部，没有重复读取旧玩家图。
- `shortcut_final_r15_20260915`（同5B6D）：相机移动+真窗口私有消息路径，132图、
  前窗1.0002342s、全窗1.0302673s、24种相机矩阵；132张图均闭合实际方向draw，0丢失。
  正常退出、restore/player/map unchanged、新GPU事件/dump均通过。不是随机撕裂已修复证据。
- 共享budget/order/shortcut Win32测试14,539,926次循环断言通过，不是同数量独立测试。
  Python新增窗口入口/HUD/time/nonce与不足时长拒绝检查，完整计数见开发日志收口。

工件根`AutoTest/artifacts/frame_evidence_runs/`，上述唯一目录不覆盖旧失败记录。
当前完整GPU取证仍缺中间图像与完整raw资源；新窗口不改变rootCauseReady=false的边界。
无需修改地图；用户需同时换新DLL与配套AutoTest脚本，旧PS1的候选SHA锁会拒绝新DLL。
未提交、推送或发布。新包单独命名，不覆盖旧试玩包或玩家现场。
