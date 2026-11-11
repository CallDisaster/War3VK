# DXSO 编译模块的单一所有者与验证边界

状态：2026-10-22 开发候选；CPU、完整 x86 DLL 链接/no-work、实际 M0 cache 观察和三方 GPU 像素对照已通过；完整组合回归和退出可靠性仍待验。
不是游戏迁移完成、内存收益验收或发布许可。所有图形验证继续在非输入 Win32 Desktop；
不覆盖玩家 DLL，不启动 Warcraft，不修改系统配置。

## 问题与本批决策链

M0 r2 的 96 轮同内容资源创建/销毁中，公开资源及 GPU 退休都能归零，
但 x64 四个 drained 采样点从第 1 到第 95 轮的 private 增长 12,804,096 B。
源码确认原 host 每次 CREATE 都直接调用 DXSO，产生新 shader cookie；
DXVK 的设备级 pipeline/library 缓存仍可持有它们。这个机制说明公开对象归零
不是设备内部缓存清空的证明，但尚不能把全部 OS 增量归因于 shader。

本批使用一个生产共用 `DxsoShaderModuleCache`，由实际设备拥有。
原 D3D9 adapter 与 x64 host 都从它获取同一份 shader 和编译后 metadata，
不再在 host 另建平行缓存，也不改变 profile/caps、预算或 GPU 退休条件。

- 编译域包括实际设备身份、全部九项 `DxsoOptions`、VS/PS 的全部 constant layout 字段。
- stage 和 SHA1 只帮助查找；命中还必须比较完整原始 token，摘要不是复用许可。
- 先编译/完成 metadata/执行 adapter 准入校验，再二次查找；只有 canonical 胜者注册。
- 注册异常保留失败表项，拒绝同 key 重试，不假定已排队的副作用可以回滚。
- shader 与输入签名、sampler/RT 掩码、常量、最大索引及 copy 需求一同交接。
- 正常销毁时 cache 先于 device 释放；缓存不得逃逸设备寿命或升级为全局裸地址缓存。

## 已取得的证据

1. `AutoTest/artifacts/x64-shader-cache-cpu-20261022-r1/receipt.json`：
   实际共享组件与实际 DXSO 编译器，restricted Medium、非输入 Desktop，88 条检查通过；
   未创建 Vulkan device。覆盖同/异 key 并发 rendezvous、18 个编译域变化、metadata、
   校验/注册异常及 owner 寿命。source 与受保护 DLL 保全。
2. 独立只读审查检查了两端 adapter、全部 domain 字段、实际 registrar 调用链、
   wrapper 布局与正常析构次序；未发现阻断当前实现 checkpoint 的确定性缺陷。
3. 静态合同测试 9/9。它不替代 adapter/GPU 运行时等价性。
4. `AutoTest/artifacts/x86-shader-cache-build-20261022-r2/receipt.json`：
   专用 clean-start 构建目录 `build32_device_owner_shader_20261022` 完成全量产品链接/no-work，
   DLL 为 34,500,608 B / `6805CC0B07DF1D1F3314ACCC5A18CF5342823D705E02FCA367F16ED3D99DF3DE`。
   r1 在原 1800 秒构建 watchdog 超时，失败回执保留；确认编译后代均结束后以新 r2 回执续编，
   r1/r2 输入清单完全相同。不是取消超时或借用旧 DLL；既有编译 warning 仍保留，不称无警告构建。
   玩家 DLL、天气候选和旧 Presenter reference 均未覆盖。
5. `AutoTest/artifacts/x64-memory-20261022-r3/receipt.json` 及同目录 `cache-analysis.json`：
   restricted-Medium 非输入 Desktop，原 96 轮/256 Query/319488 像素负载全部执行且零错；
   8 个真实 checkpoint 上的进程/nonce/sequence 与 cache 逐项绑定，192 CREATE 请求、
   2 次成功 DXSO 编译/注册、190 hits，canonical cookies `[1,2]`、token size/capacity 都为 400B。
   1487 个实际 M0 读方反例及 579 个 cache 观察反例全部拒绝；不是额外 GPU 运行次数。
   资源/逻辑句柄最终归零，child 自然退出 62ms，原始日志在 launch-return 冻结且结束重验。
6. `AutoTest/artifacts/x64-shader-equivalence-20261022-r1/receipt.json`：
   old-reference / new-reference / remote 三个独立 restricted-Medium 非输入 Desktop 各 72 帧，
   各 239616 个独立选定像素检查零错，共 718848 项。实际公共 COM 对象可不同但同内容画面一致，
   两个 DEF 常量 word 改变后红/绿输出不同，恢复原 token 后原画面恢复；各 88 次 GetFunction 字节往返，
   shader public refs、device/factory refs 均闭合。像素本身不认证命中，命中证据来自上一项。
   该夹具未独立回报 GetModuleFileName；仅绑定绝对 LoadLibrary 参数与实际复制 DLL 的哈希。

## 必须如实保留的成本和限制

- `registrations` 仅表示 registrar 正常返回，不是 GPU pipeline 编译完成。
  `compileAttempts/compilesSucceeded` 是 DXSO CPU 编译调用，不是 GPU 完成计数。
- 每次命中仍复制 token 以完成精确比较；miss 在 adapter 分析后会再分析一次。
  设备寿命缓存保留 token、metadata/constants，因此不是零分配、零 CPU 或零内存的抽取。
- 当前不淘汰独特 shader；相同内容去重不等于任意独特工作集有界。
- `keyBytes/constantBytes` 不包含 SPIR-V、pipeline、allocator、驱动分配或物理显存。
- 旧 wire 的 `shadersCompiled` 维持原“成功 CREATE 数”语义，不能作为真实去重指标。
  新冷路径观测使用独立 `D3D9 shader-cache:` 前缀，不改冻结 wire。
- 当前 host profile 明确拒绝 relative operand；`needsConstantCopies=true` 的分支
  不能冒充已在该 profile 中验收。生产 D3D9 更宽的 SM/caps/SWVP 分支另需测试。

## 下一验收门

1. 新 x86 配置内完整离线回归（构建/no-work 已通过）。
2. 同身份完整 Presenter/R1，再做独立 Vulkan validation。
   原 5 秒退出红门不因缓存优化或偶尔成功而取消。
3. 对固定重复负载分别报告 x86/x64 OS 观察；若不是同配置配对 A/B，不称节省量。
   产品异步背压、完整游戏路径、实际地图/压力/换图与玩家视觉仍为后续硬门。

关联：[M0 账户](2026-10-22-x64-memory-account.md)、
[Presenter 退出调查](2026-10-22-x64-d3d9-presenter-isolation.md)、
[迁移路线](../plan/2026-10-11-x64-render-migration.md)。
