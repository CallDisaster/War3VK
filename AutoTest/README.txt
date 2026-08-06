War3 AutoTest MCP（自动化测试服务）
===================================

目标
----
1. 自动启动 War3 并直进指定地图（复用 YDWE 的 -loadfile 思路）。
2. 通过 OutputDebugString 订阅 DXVK/War3 运行日志，自动判断“进入游戏”。
3. 自动截图、自动读取 WarVK 性能报告，形成无人值守回归链路。

文件
----
- war3_autotest_mcp.py
  自定义 MCP 服务（FastMCP）。
- run_mcp.ps1
  启动脚本。
- ydwe_launch_notes.txt
  YDWE 启动链源码结论。

默认路径
--------
- War3 目录: E:\Work\War3_AutoTestSandbox
- 测试地图: E:\Work\War3_AutoTestSandbox\Maps\光影测试.w3x

已实现 MCP 工具
---------------
- ydwe_launch_chain_analysis
- prepare_test_map
- deploy_d3d9_to_war3
- ensure_war3_video_baseline
- preflight_instance_pool
- provision_ydhost_assets（默认 dry-run；哈希锁定且目标漂移即拒绝）
- generate_ydhost_map_metadata（默认临时执行；不启动 War3/WE/ydhost）
- launch_war3_instance
- launch_war3_batch
- run_multi_instance_suite（仅 LAN/ydhost；真实启动显式 opt-in，协议证据不足仍 fail-closed）
- list_war3_sessions
- stop_war3_batch
- cleanup_orphan_sessions
- launch_war3_test
- is_war3_running
- read_runtime_status
- wait_for_runtime_status
- get_runtime_events
- wait_for_game_ready
- query_war3_window
- wait_for_war3_window_ready
- control_war3_window
- capture_war3_screenshot
- stop_war3
- find_latest_perf_report
- read_perf_report
- run_quick_autotest
- list_named_scenario_presets
- run_named_scenario
- run_profile_matrix
- run_windowed_resize_crash_test
- start_periodic_perf_test
- get_periodic_perf_test_status
- stop_periodic_perf_test
- run_periodic_perf_test_blocking
- sync_all_debug
- current_state

关键行为
--------
1) 复刻 YDWE 直进地图
   - 启动参数使用: -loadfile
   - 地图复制到: Maps\Test\WorldEditTestMap.w3x
   - 传相对路径给 -loadfile（减少长路径不识别风险）

1.5) 历史 YDWE/JAPI 地图
   - `launch_war3_test` / `run_quick_autotest` 显式传
     `launcher_mode="ydwe"`、`ydwe_root=<YDWE根>`、
     `use_isolated_desktop=true`。
   - 仅允许既有 `E:\Work\War3_AutoTestSandbox`；只读校验 HKCU InstallPath，
     不修改注册表、不复制新沙盒。
   - 启动链为 `YDWE.exe -war3 -loadfile <短路径> -closew2l`，追踪其 child
     war3 PID；DBWIN/ready/stop 均绑定游戏 PID。
   - 候选图部署后必须与短路径目标 SHA-256 一致；LuaEngine.dll 与
     yd_jass_api.dll 必须在 child 中以精确路径和 SHA 加载。
   - 检测到用户 YDWE/WorldEditor 进程时返回
     `USER_YDWE_PROCESS_CONFLICT`，绝不结束用户进程，也不降级成无 JAPI 直启。

2) 自动性能录制
   - 若 DXVK 新版已包含该开关，可通过环境变量:
     DXVK_WAR3_PERF_RECORD_ON_START=1
   - 本仓库已补充该开关实现。
   - 可选周期导出:
     DXVK_WAR3_PERF_AUTO_EXPORT_SEC=8
     （用于不退出游戏也能拿到最新报告）

2.5) 2K 性能基线（新增）
   - `launch_war3_test` / `run_quick_autotest` / `start_periodic_perf_test` 默认 `windowed=false`。
   - 启动前默认写入注册表基线：
     - `reswidth=2560`
     - `resheight=1440`
     - `refreshrate=59`
   - 可单独调用 `ensure_war3_video_baseline` 预设分辨率。
   - `run_quick_autotest` 会返回 `screenshotSize`，用于校验实际截图是否匹配 2K 基线。
   - AutoTest 通过 `launch_war3_test` 写入的注册表基线，会在 `stop_war3` 成功结束后自动恢复到 launch 前快照，避免把用户长期锁在测试分辨率。

3) 进入游戏判定
   - `wait_for_game_ready` 默认优先走 DLL named pipe control plane 的
     `wait_until`。
   - ready 条件固定为:
     - `module.state == Running`
     - `runtime.jassReady == true`
     - `runtime.runtimeReady == true`
     - `runtime.gameStarted == true`
     - `render.inGameRenderReady == true`
   - `WarVK/Temp/runtime_status.json` 仅保留为兼容/离线诊断 fallback；
     只有 pipe 不可用时才会退回文件/DBWIN 路径。
   - 若 DBWIN 被占用导致抓不到 DebugString，则最后才退化为
     “窗口存在 + 进程 CPU 累计时间”判定。

3.5) 窗口化回归工具（新增）
   - `query_war3_window`
     - 读取主窗口 hwnd / 标题 / showCmd / windowRect / clientRect。
   - `wait_for_war3_window_ready`
     - 只等待“窗口可操作”，不要求正式进图。
     - 适合 resize/maximize/restore 崩溃测试。
   - `control_war3_window`
     - 支持 `query / resize_client / maximize / restore / minimize / close`。
   - `run_windowed_resize_crash_test`
     - 一键执行：
       启动(windowed) -> 等窗口可操作 -> resize -> maximize -> restore -> resize -> 检查是否闪退。
     - 默认 `enforce_video_baseline=false`，避免窗口化崩溃测试污染用户当前分辨率设置。

3.6) 内部最终帧截图（新增）
   - `capture_war3_screenshot` 现在优先走 named pipe control plane：
     - MCP 调用 `capture_final_frame`
     - DXVK 在 `Present` 尾部、真正上屏前读取 backbuffer，导出 BMP
   - MCP 收到 BMP 后会自动转成 PNG 返回，便于后续尺寸校验和图片查看。
   - 只有当 control plane 截图超时/失败时，才会回退到旧的窗口抓图路径。

4) 定时性能测试（新增）
   - `start_periodic_perf_test`：后台定时执行多轮回归（不阻塞会话）
   - `get_periodic_perf_test_status`：查看当前进度与聚合指标
   - `stop_periodic_perf_test`：提前停止任务并清理残留 War3 进程

4.5) 前台工作不抢焦点（新增）
   - `run_quick_autotest` 默认 `avoid_focus_on_stop=true`。
   - 结束测试时走“静默结束”分支（跳过 WM_CLOSE，直接后台结束进程），避免强制切回 Warcraft III 窗口。
   - 若你需要“优雅退出”行为，可显式传 `avoid_focus_on_stop=false`。

4.6) 运行时档位矩阵（新增）
   - `launch_war3_test` / `run_quick_autotest` 新增：
     - `profile`
     - `disable_modules`
     - `env_overrides_json`
   - 对应运行时接口：
     - `DXVK_WAR3_PROFILE=<profile>`
     - `DXVK_WAR3_DISABLE=<csv>`
   - 内置档位：
     - `dxvk_only`
     - `hooks_minimal`
     - `hooks_default`
     - `render_base`
     - `shadow_capture_only`
     - `shadow_full`
     - `full_default`
     - `full_analysis`
     - `full_perf_experimental`
   - 可手动禁用模块：
     - 渲染层总开关别名：`render`
       - 等价于关闭 `hook.render,render.queue,shadow.capture,shadow.map,shadow.receiver,shadow.taa,postfx,ssao,aa`
     - 阴影层总开关别名：`shadow`
       - 等价于关闭 `shadow.capture,shadow.map,shadow.receiver,shadow.taa`
     - 上层语义数据链：`semantic.data`
       - 关闭模型/pose/manifest/semantic contract 采集与消费热路径
   - 建议排查顺序：
     - `DXVK_WAR3_PROFILE=dxvk_only`
       - 纯 DXVK 基线，确认地图/JASS 本身是否慢。
     - `DXVK_WAR3_PROFILE=full_default` + `DXVK_WAR3_DISABLE=render,semantic.data`
       - 保留非渲染 hook/control-plane，关闭渲染干涉与上层语义数据。
     - `DXVK_WAR3_PROFILE=full_default` + `DXVK_WAR3_DISABLE=render`
       - 只关闭渲染干涉，单独观察上层语义数据链开销。
     - `DXVK_WAR3_PROFILE=full_default` + `DXVK_WAR3_DISABLE=shadow`
       - 关闭 shadow capture/map/receiver，但保留 postfx/AA/SSAO。
     - `DXVK_WAR3_PROFILE=full_default` + `DXVK_WAR3_DISABLE=semantic.data`
       - 保留渲染层，单独关闭上层语义数据链。
     - `DXVK_WAR3_PROFILE=full_default`
       - 全量默认路径，用于和上述结果对比。
   - `run_profile_matrix`
     - 自动执行“加法矩阵 + 减法矩阵”
     - 输出统一 `profile_matrix.json` / `profile_matrix.html`
     - 汇总结果包含：
       - `caseSummaries`
       - `topOffenders`
       - `overBudgetCases`
       - `invalidCases`
     - 默认目录：
       `AutoTest/artifacts/profile_matrix/<timestamp>/`
     - 当前默认 `use_isolated_desktop=true`，避免矩阵运行干扰当前工作桌面。

4.7) diag 关闭时的 benchmark fallback（新增）
   - 当某个档位关闭 `diag` 或性能报告未正常导出时，
     AutoTest 会自动打开低开销 runtime benchmark：
     - `DXVK_WAR3_RUNTIME_BENCHMARK=1`
     - `DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC`
     - `DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC`
   - `dxvk_only` 同时兼容旧接口：
     - `DXVK_WAR3_FPS_UNLOCK_ONLY_WARMUP_SEC`
     - `DXVK_WAR3_FPS_UNLOCK_ONLY_SAMPLE_SEC`
   - 这类结果会以 `reportType=benchmark_log` 返回，
     仍然携带 `runtimeProfile/moduleMatrix`，便于统一做矩阵归因。
   - 当前所有读取性能报告的路径还会补齐 `shadowRuntimeV2Summary` 占位字段，
     方便后续的模型/姿态/语义追踪指标直接落位，不会因为字段缺失阻塞报表流水。

4.8) 隔离桌面测试（新增）
   - 当前全局安全隔离：实测 Win32 desktop object 会令交互桌面黑屏，同时把 War3
     留在非输入桌面。所有 `use_isolated_desktop=true` 请求会在 CreateProcess 前失败；
     必须改用可见桌面或 attach-only，修复并完成用户桌面验收前不得解除。
   - `launch_war3_test` / `run_quick_autotest` / `run_profile_matrix` 新增：
     - `use_isolated_desktop`
     - `desktop_name`
   - 实现方式不是 Win11 任务视图“虚拟桌面”，而是 Win32 `desktop object`：
     - AutoTest 创建独立 desktop
     - War3 通过 `CreateProcessW + STARTUPINFO.lpDesktop` 启动到该 desktop
   - 目的：
     - 测试窗口不再出现在你当前工作的桌面
     - 适合夜间性能矩阵与长时间稳定性回归
   - 当前限制：
     - 为避免切全局显示模式，隔离桌面模式会强制 `windowed=true`
     - 更适合性能/稳定性测试；隐藏 desktop 下内部最终帧仍可能不是严格 2K 基线，不建议直接拿来做像素级截图回归

4.9) 命名场景预设（新增）
   - `list_named_scenario_presets`
     - 列出当前内置场景预设。
   - `run_named_scenario`
     - 按预设名直接执行对应测试流。
   - `run_quick_autotest` / `run_profile_matrix` / `start_periodic_perf_test`
     - 现已支持 `scenario_name`，会写入 `DXVK_WAR3_SCENARIO` 并保留到报告中。
   - 当前预设：
     - `low_pressure_static_reuse`
     - `dynamic_shadow_pressure`
     - `model_runtime_probe`
     - `semantic_cost_probe`
   - 这些预设会把场景名写入 `DXVK_WAR3_SCENARIO`，并在报告 JSON 中保留 `scenarioName`。

4.10) 内部测试控制层 / City 专项稳定性套件（新增）
   - 外部测试命令现统一走 DLL named pipe control plane：
     - `invoke_test_command`
     - `capture_final_frame`
     - `shutdown_session`
   - 当前高层命令：
     - `visibility.full_map`
     - `camera.snapshot`
     - `camera.apply`
     - `camera.sweep_aoa`
     - `shadow.debug_mode`
     - `runtime.log_marker`
     - `capture.final_frame`
   - AutoTest 对应新增工具：
     - `invoke_internal_test_api`
     - `set_city_test_view`
     - `capture_shadow_factor_sequence`
     - `compare_frame_sequence`
     - `run_city_shadow_stability_suite`
     - `run_city_shadow_pressure_suite`
   - 仓库内仍保留 legacy JSON 路径名用于清理旧工件/离线诊断；
     但主动控制链不再写入 `internal_test_request.json` /
     `internal_test_result.json`。
   - 当前控制层烟测结论：
     - 小测试图已完成 `3` 轮 smoke
     - `launch / ready / camera.snapshot / ShadowFactor 3 帧序列比较 / silent stop` 全通过
     - `flickerSuspect=false`，`missingShadowSuspect=false`
   - 当前已知阻塞：
     - `City.w3x` 在 standalone `war3.exe -loadfile` 下仍会停在主菜单，尚未实际进图；
       因此 City 专项稳定性/压力套件虽已落地，但真正签收前仍需先解决该地图的加载条件。
   - 夜间执行策略（新增）：
     - `City.w3x` 仍作为优先专项图；
     - 若 standalone 自动链路未能进图，套件会自动回退到 `E:\Work\War3_AutoTestSandbox\Maps\光影测试.w3x`，
       不再因为单张地图启动兼容性阻塞整晚稳定性/压力回归；
     - 夜间 runner 不以中途人工汇总作为停点，而是持续执行
       `稳定性 -> 压力 -> 低压回归`，统一把结果落到 `AutoTest/artifacts/`。

5) 全量调试同步（新增）
   - `sync_all_debug`：一次性返回
     - DBWIN 事件流（DXVK/War3/JASS）
     - `WarVK/Temp/runtime_status.json`
     - `war3_d3d9.log` / `dxvk.log` / `war3.log` 尾部
     - 最新性能报告摘要（可配置数量）

MCP 超时建议（Codex）
----------------------
已在 `C:\Users\Administrator\.codex\config.toml` 配置：
- `[mcp_servers.war3-autotest]`
  - `startup_timeout_sec = 30`
  - `tool_timeout_sec = 1800`
- `[mcp_servers.ida-pro-mcp]`
  - `startup_timeout_sec = 20`
  - `tool_timeout_sec = 120`

MCP 接入示例（手动配置）
------------------------
将如下 server 配置加入你的 MCP 客户端配置:

{
  "mcpServers": {
    "war3-autotest": {
      "command": "python",
      "args": [
        "E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/AutoTest/war3_autotest_mcp.py"
      ]
    }
  }
}

一键流程建议
------------
直接调用 run_quick_autotest：
1. 启动游戏
2. 等待进图
3. 自动截图
4. 采样一段时间
5. 关闭游戏并读取最新性能报告
6. 若报告未更新，会在返回里标记 `newReportDetected=false`

函数级明细读取（新增）
----------------------
- `read_perf_report(include_sections=true, section_top_n=30)` 可返回：
  - `sectionBreakdown.topBySelfCpu`
  - `sectionBreakdown.topByInclusiveCpu`
  - `sectionBreakdown.topByGpu`
- `read_perf_report` 现还会带出：
  - `runtimeProfile`
  - `moduleMatrix`
  - `shadowBudgetSummary`
  - `topShadowOffenders`
- `shadowBudgetSummary` 现额外包含：
  - `skippedPriorityBudget`
  - `degradedAlphaBudget`
  - `reusedFreezeHits`
  - `reusedFreezeMb`
  - `actualFreezeReuseHits / actualFreezeReuseMb`
  - `uniqueGeometryCount`
  - `uniqueInstanceableGeometryCount`
  - `duplicateGeometryInstances`
  - `reuseEligibleDuplicates`
  - `uniqueFreezeAcceptedMb / duplicateFreezeBypassMb`
  - `potentialFreezeReuseHits`
  - `potentialFreezeReuseMb`
  - `instancedGeometryGroups / instancedGeometryInstances / instancedGeometryDrawsSaved`
  - `derivedFromLogKeywords`（当底层结构化预算字段过稀疏时，AutoTest 会从日志关键字回填 `framesBudgetExceeded / framesIncomplete / reuse / partial`）
- `run_quick_autotest` 也支持：
  - `include_sections_in_report=true`
  - `section_top_n=30`
  这样一键测试返回里就会直接携带节点级热点列表。
