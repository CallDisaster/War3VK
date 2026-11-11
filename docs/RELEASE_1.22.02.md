# WarVK 1.22.02 发布范围与证据

发布日期：2026-11-11。用户授权：“你可以打包这个作为新版本，我们发布一次小版本迭代，将我们已经
可以放出去的更新内容也加到这一轮中，64位迁移相关的内容依然保留。在你准备好并且测试之后自行发布
新版本到Github即可。” 本文件替代 2026-09-28 的 RC1 候选范围（历史内容见 git 记录）。

## 范围

收入（玩家 DLL 默认生效）：

- 启动失败对话框与 `WarVK\Log\startup-error.txt`；Vulkan 加载器缺失和 `Direct3DCreate9`/
  `CreateDevice` 异常改为返回错误码而不是让进程崩溃；同进程只提示一次（PID 写入进程环境）。
- 显卡/驱动下限表（`docs/WarVK_GPU_REQUIREMENTS.md`，随玩家包发布为
  `WarVK_GPU_REQUIREMENTS.md`）与显存档位（低档 CSM 上限 2048）。
- Storm `MDLDATAHash` 低地址域（含本版的部分失败降级）、每线程大表冷启动、NetEvent 安装时机、
  换图阴影会话 Reset、常驻阴影几何事务、64 位迁移中发现的 32 位共用修复。
- 单位 Walk/动画 Override、`LocalizedString` 实数无指数格式、模块加载器默认拒绝。
- 玩家包附 `WarVK_LockD3D9.cmd` / `WarVK_UnlockD3D9.cmd`。

收入但默认关闭：天气预览 12 接口（地图调用后才创建资源）；作者包附 `WEATHER_API.md` 与
`warvk_weather_demo.j`。

不收入：64 位渲染宿主（`warvk_render_host_core_dev` 等 4 个新开发选项与既有诊断选项在正式配置中
全部为 false，源码保留）、未完成 Water、原版模型自动点光接管、未验收剔除实验、作者包中的模块接口。

## 本版额外修复（发布前审查发现）

- 地图模块加载器在缺少 `WarVK\modules\allowed.txt` 时曾经放行，等于下载的地图可让 WarVK 加载任意
  DLL。现改为默认拒绝，只有进程环境 `DXVK_WAR3_MODULE_DEV_MODE=1` 时才放行并告警。
- `StormLowDomain_Prepare` 或导入表发布失败时，旧逻辑会让整个 Storm TLSF 钩子安装失败。现在只放弃
  低地址域；导入表无法回滚时仍保持原先的永久直通，不冒险继续。
- 断言开启的 GCC 测试构建中 `warvk-lazy-thread-array` 失败。复现确认：GCC/mingw 的模拟 TLS 在
  线程退出时先由 winpthreads 释放 emutls 存储，之后才运行 `__cxa_thread_atexit` 析构，
  `thread_local LazyThreadArray` 的析构因此在已释放内存上执行（最小程序里对象的指针字段已变）。
  10 月的验证都用原生 TLS 的 LLVM 构建，没有暴露。新增 `ThreadLocalLazyArray`（可平凡析构的
  TLS 指针 + 每线程 FLS 回调释放后备内存），4 处 `thread_local` 改用它；GCC 与 LLVM 下单测通过，
  CurrentDraw 生产抽取 6,489,896 项 0 失败。产品里这些表只在游戏主线程使用，实际风险集中在
  线程退出时；代码中其他既有的 `thread_local` 容器（`std::vector` 等）属于同类模式、也只在主线程
  使用，本版未改动，留作后续统一处理。

## 验证

构建：源码冻结于 `E:/WarVK-Builds/v1.22.02-release-20261111`（初始 06f1117；`source-final.json`
补入 9b8dbab，`source-final-2.json` 补入 ea24009 的 TLS 修复，`source-final-3.json` 补入 b368268 的
静态测试更新；依赖为固定 gitlink，StormBreaker 用户工作树的未提交内容不进入构建）。
GCC 15.2 `build-win32.txt`，BelowNormal/-j2。

- 正式配置（`b_ndebug=if-release`、`strip=true`、全部开发/诊断选项 false）：未剥离 DLL
  26,402,011 B / `25A220AEC3010116386187B839A05FB30665BF4DF5888621A5B4B13F82087A2C`，PE32/i386，
  版本资源 1.22.02、非预发布；精确目标再次构建为 no-work。配置审计 `{"ok":true,"errors":[]}`，
  配置/打包工具单测 20/20、9/9。
- 断言开启（`b_ndebug=false`）CPU 测试：meson 120/120。全量静态脚本 285/285，在冻结镜像执行；
  依赖 Git 历史的 `test_v122_geoset_identity_static.py` 通过 `GIT_DIR` 只读引用主仓库。首轮静态为
  284/285：`test_native_light_transaction_static.py` 仍在 `d3d9_device.cpp` 里找已移入
  `d3d9_fixed_function_keys.h` 的两行赋值，已改为检查设备调用与新位置。

实机（同一 `25A220AE` DLL，AutoTest 专用沙盒实例 + 隔离桌面，无全局输入，单块 RTX 4060 Ti，
驱动 621.01；玩家现场 DLL 每个门前后复核均为 `1FFE0BD1…35CF`）：

| 门 | 结果 |
| --- | --- |
| 启动诊断 4 场景 | 通过。设备过滤模拟“无可用显卡”：1.0 s 内写出报告；强制弹窗场景只出现一次 WarVK 对话框，关闭后出现游戏自己的 DirectX 错误框，随后进程自行退出；游戏重试第二次只记 “repeated” 日志。正常启动 `tier=high csmResolutionCap=4096`；`VRAM_PRESET=low` 时 CSM 4096→2048。 |
| 天气功能（2560×1440） | 通过（runtimeAcceptance=true）：14 个 JASS 阶段、暂停/恢复/禁用/时间线/销毁均在冻结输入下观察到，截图与基线不同。不含像素/美术验收与 GPU 完成认证。 |
| 单位 Walk（`Hamg`） | 通过：请求到达并被接受，自动 Walk 被抑制，开关恢复后重新出现。不含动画像素证明。 |
| 高压路线“生与死” | 通过：184.0 s、346 次采样、39 个路线节点，无新 GPU 事件/incident，无 device lost。6 次采样阴影图不完整（3 次在出生等待阶段首张阴影图之前，3 次为单帧未执行，下一次采样即恢复）。客户区 1902×963，难度对话框未被自动关闭（测试工具限制，与 RC1 相同），因此不是 2560×1440 压力认证。 |
| 同进程换图 A→B→A | 行为通过，严格分析器 exit 2（见下）。 |

换图说明：epoch 1→2→3，每次切换只应用一次 Reset，旧会话回收 0→1→2，三个阶段稳定后的末尾帧
producerReady/完整 receiver 均为 2/2，绘制 caster 145～147，三张截图 epoch 正确，无 GPU 事件。
与 RC1 不同的是 Reset 请求序号每次 +2：2026-10-24 修复 NetEvent 退场事件缺链后，卸图回调
`ResetWar3RuntimeState` 与 JASS Native 表重建各请求一次，由 Present 合并为一次应用。RC1 分析器
写死了“每次 +1”，并按 JASS 阶段标签取末尾帧（会取到加载过渡中的帧），因此判 `resetSerialAdvanced`
与 `runtimeStateGate` 为 false。按稳定 epoch 重新分析的结果保存在该次 artifact 的
`settled-epoch-reanalysis.json`。分析器本身未修改，原始 exit 2 回执保留。

上表均为最终 DLL `25A220AE` 的结果。修复 `ThreadLocalLazyArray` 之前的 `CA4C0263` 已跑过同样 5 个门，
结论一致（高压 184.4 s/345 次采样/7 次不完整），回执保留为 `gate-*-r2-dll-CA4C0263.log`。

首轮断言开启 meson 为 117/120：除上面的 `warvk-lazy-thread-array` 外，`war3_frame_evidence_runtime`
与 `war3_palette_object_wire_roundtrip` 失败是因为测试进程继承了本机持久的开发环境变量
（`DXVK_WAR3_FRAME_EVIDENCE_OUTPUT` 指向不存在的目录）；在清除 `DXVK_WAR3_*` 的进程中重跑通过。
正式 meson 记录使用同样的干净进程环境，回执保留为 `release-1.22.02-meson-r1-host-env-3fail.log`。

首次尝试的失败回执也保留：换图/Walk/高压的旧 RC1 脚本以玩家目录为实例源，被现行实例池策略
（只允许专用沙盒）拒绝，未启动游戏；天气首跑未调整窗口尺寸，只差 2560×1440 一项。

## 未验证与已知边界

- **这一个 DLL 没有玩家实机验收。** 玩家现场仍运行 2026-09-25 的候选
  （`1FFE0BD1…35CF`），本次没有覆盖或部署它。
- 只在一块 NVIDIA RTX 4060 Ti 上做了隔离自动化测试。AMD/Intel 下限来自公开的驱动扩展记录，
  “显卡不支持/驱动太旧”的对话框是在同一 NVIDIA 显卡上用设备过滤模拟出来的。
- 64 位迁移整理过的 S2 接口中，队伍颜色、shaderpack 材质与帧捕获三条路径没有专门的运行时测试
  （换图与高压截图中队伍色目视正常，不等于专项验证）。
- 正常退出路径没有新的自动化回执：本轮 EndGame + WM_CLOSE 的探针在 GCC 和同源 LLVM 构建上都
  停在计分界面、未能自然退出，属于探针流程问题，强制结束后均无 dump。
- OpenGL 模式（`-opengl`，包括某些启动器硬编码的方式）不会加载 `d3d9.dll`，WarVK 不生效。
- 天气预览的画面风格、长时间与多显卡性能未验收。
- 菜单换图、D3D9 Reset、长时高压仍未完成验收；继续建议每张地图重开游戏。
- Shader API 1.2.0、JASS wire `warvk:v1` 不变；没有新的 FPS 提升承诺。
