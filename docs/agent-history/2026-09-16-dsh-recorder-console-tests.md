# 2026-09-16 录制 HUD 移入 ImGui 控制台：GLM 独立静态测试

状态：主线程已落地生产迁移（`war3_imgui.cpp/.h`）；本文件记录 GLM 的独立静态测试矩阵、
命令与结果。全部为静态文本合同检查——不是运行时、GPU、游戏或提交证据；未运行编译器/
Ninja/游戏/部署。旧树只读；仅写 2 个授权文件与本 ignored 输出目录。

## 1. GLM 交付时身份（主线程后续修订不回写历史 SHA）

| 文件 | size (bytes) | SHA-256 |
| --- | --- | --- |
| `AutoTest/test_frame_recorder_console_static.py` | 14450 | `7c05b1806782f9dedf74fc4cef237692f2edf6635583ede3dffaaa08eb8fa797` |
| `docs/agent-history/2026-09-16-dsh-recorder-console-tests.md`（本文件） | 见文件系统 | 惯例不自哈希 |

## 2. 测试矩阵（13 项=8 接线+2 保留+3 反例自测）

接线族（ConsoleWiringTargetState，函数体/屏蔽体限定，无全文件计数）：
- newFrame：`if(!m_initialized||!m_visible)` guard 与 `return;` 成对验证；`QueryFrameHistoryHud().enabled` 旁路消失。
- render：`if(!m_initialized||m_hasRendered||!m_visible)` + `return;` 成对；同旁路消失；
  延后门 = 主线程指示的落地形态 `if (inScene && tools::evidence::Enabled()) return;`
  （控制台像素不进录制的历史帧）。
- 独立 "WarVK recorder status" 窗口完全移除（注释屏蔽后标题不在 + `SetNextWindowBgAlpha` 不在）。
- drawDebugWindow：`Begin("War3VK 调试器", &m_visible, ...)` 关闭按钮绑定（注释屏蔽保留字面量视图），
  且不再 `Begin("War3VK 调试器", nullptr, ...)`。
- 私有 `drawFrameRecorderPanel`：`war3_imgui.h` private 声明、cpp 定义体（括号配对完整）、
  drawDebugWindow 屏蔽体内真实调用（注释/字符串伪调用不计）。
- panel 显示合同锁定：真实调用 `tools::QueryFrameHistoryHud()`；`CollapsingHeader`；`switch(recorder.state)`
  的 case 0..7 与 default；`recorder.bufferedMs/requiredMs/saved/total/packageReady/selfContained/watcherAlive/notice`；
  packageReady 两态消息原文（"原始包已保存，可退出；完整性待离线分析" / "图片已保存，正在整理日志，请勿退出"）；
  禁用说明 `DXVK_WAR3_FRAME_EVIDENCE=0` + `重新启动` + 不停止采集。
- panel 只读：无 `evidence::Control`/cancel/Release/MemoryAdmission/DiskBudget/内存查询/hud 写入。

保留族（ConsoleContractPreserved）：Ctrl+Shift+C 接线（core.h `key==0x43 && ctrl && shift && !repeat`
+ `message==0x100` + `d3d9_window.cpp` 的 `HandleFrameHistoryShortcut(message`）；render 的
`!inScene && WantCaptureMouse` 光标覆盖门原样。

反例自测（StaticCheckSelfTests，合成文本）：注释伪调用不计为真实调用；缺失 panel 定义时
定义检查必须失败；`mask(blank_literals=False)` 消费完整字符串字面量——字符串内的 `//`
不再被误判为注释，其后真实调用仍可见。

## 3. 实际命令与结果（cwd=集成树根；`python`=3.13.11，均 `-B`）

1. `python -B -c "ast.parse(...)"` 新测试文件 → 通过。
2. run1（主线程落地前，单次）：`Ran 12 tests; FAILED (failures=1)`，exit 1——
   唯一失败为 Present 门旧形态断言（`recorder.enabled`），即当时的落地前记录。
3. run2（落地后按主线程指示形态修订断言后，单次）：`Ran 13 tests ... OK`，exit 0。
   输出：`AutoTest/artifacts/dsh_recorder_console_20260916/console_static_run{1,2}.{stdout,stderr}.txt`，
   互不覆盖。

## 4. 说明与限制

- 断言更新说明：run1 中 Present 门断言按旧形态（`recorder.enabled`）编写；主线程随落地
  明确指示延后门键于全局取证开关（`tools::evidence::Enabled()`），run2 前按该指示更新此
  一处断言，非静默跟随实现。
- 静态检查只证明文本合同；控制台像素实际不进历史帧、隐藏不停采、内存不释放等运行时
  行为需实机/运行时证据，不在本批。
- 主线程负责的两处旧测试修订（shortcut 旧常驻浮窗合同、self_contained 的 selfContained
  格式断言改 panel 作用域正则）不在我的范围；我未改任何旧测试/阈值/生产文件/Kimi 文件/
  AGENTS/总日志/Git。

## 5. 主线程接收修订

主线程复核发现，GLM 交付版只检查了声明存在，尚未证明其 private 访问域；packageReady 也只检查
两条消息同时存在，未锁死真假绑定。接收后补齐屏蔽文本中的访问域、三元表达式真假消息、隐藏不释放
提示，以及显示函数不得调用 FrameHistoryControl/Trigger/reset/atomic store 的断言。仍为13项，
不是新增实机证据。最终身份与主线程复验见 `AutoTest/artifacts/recorder_console_parent_20260916/`。
