# Device-owned COM：关闭 x64 对照基线的生命周期红门

日期：2026-10-22。长期收敛 / x64 迁移前置修复，不是远程渲染发布。

## 问题与本批范围

2026-10-11 的真实 x86 D3D9 → x64 DXSO/DXVK 离屏链已经画对限定场景，
但保留的单进程参考 DLL 最后 `deviceFinalRefs=15`，未走到正常 factory/DLL/window 退出。
原失败回执 `AutoTest/artifacts/x64-d3d9-20261011-r4/receipt.json` 保持原样，不追认。

本批修这条设备内部所有权链：PostFX、ImGui backend / cursor，以及其初始化、Reset 和退出入口；
实际 Reset 审查又发现同一设备所有权问题延伸到 InstanceBuffer，故一并闭合其 owner 与资源引用。
不修改通用 COM 引用计数实现，不放宽 caster、不增加内存预算、不删除外部资源的设备保活。
原有天气、原生数据采集和其他 dirty 改动保留，不能把整树 diff 算成本批工作。

## 谁拥有资源

- 外部 Create/GetDevice/GetContainer、临时 stateblock / GetDirect3D 仍为 public COM。
  外部持有资源时必须继续保活设备。
- Device 自有 PostFX 的纹理、surface、shader、声明与 VB 用现有 `Com<Concrete,false>`。
  接公开创建结果时先取得 private 引用，再交还临时 public 引用，防止先释放后接管。
  Surface 的 private 引用沿现有实现转给父纹理，不另造生命周期规则。
- ImGui 仍用同一份 upstream renderer：构建期从固定 SHA 的原文件生成明确前缀适配，
  只改借用 device 及内部 VB/IB/font texture 引用；用户 TextureID 与临时公开对象不改。
  upstream 未改，生成器对未知源码/替换位置/覆盖源文件 fail-closed。
- 设备创建在 `InitialReset` 期间由 public RAII 保活，成功后才发布调用者引用；
  PostFX 部分初始化失败会清空自身资源，UI 初始化异常不会遗留借用 owner/context。
- 正常退出先撤销 UI 回调，再走原 CS/GPU drain，随后仅销毁该 device 的 UI。
  UI registry 不在 CRT/DLL detach 中隐式析构 GPU 资源；正常资源回收仍由设备负责。
  Reset、Present、cursor 与窗口输入按 exact owner 限定，不允许第二设备借全局 UI 旁路。
- InstanceBuffer 原来是绑定首台 device 的进程 singleton；普通 Reset 即使未启用自动 instancing，
  也会调用 Get 并创建 6 MiB DEFAULT VB。现在由各 device 惰性独占，Reset 不创建从未使用的 owner。
  VB、原声明/派生声明与 captured textures 使用现有 private 引用；保留原声明本体避免裸地址键复用。
  删除 queue 的第二层全局裸 owner 缓存；执行入口按当前 device 筛选 active，锁定指针随实例而非 TLS 共用。
  最终退出先撤回当前线程 active，并结束 outstanding Lock（staging 模式可能产生上传），
  然后走原 Flush/CS/GPU drain；idle 以后才销毁 owner，不改变 GPU 最后使用/退役规则。
- 全量构建同时发现此前 DXSO 抽取漏了一处 material 调用方；同步使用 device/options/float 常量布局的
  三参数合同，没有改 shader 算法。首次全量失败保留，不以局部 TU 编译追认其通过。

## 验证方法与边界

1. 独立全量 Win32 产品构建，不使用旧天气 build 的混合对象，不部署玩家目录。
2. 原 x86→x64 GPU/像素/坏输入恢复/idle/COM 测试原样复跑，用新产品 DLL 作单进程参考。
3. 仅测试的额外 probe TU 与同一批产品对象重链为独立诊断 DLL，不给产品增加 export、环境开关或测试后门。
   隐藏窗口、零全局输入，实际调用生产 PostFX / UI；检测 UI atlas/VB/IB/cursor、外部保活、Reset、双设备与重复退出。
4. 回执分别记录源码、DLL、探针、链接输入和原始输出。构建成功、CPU/static、GPU 像素、正常退出分别判定。
   超时强制结束不算正常退出；`countLosableResources` 未启用时不能冒充已验证外部 DEFAULT 阻止 Reset。

## 实际执行结果

- 独立 Win32 产品构建 `device-owned-build-20261022-r4`：完整构建及 no-work 通过。
  产品 DLL 为 **34,476,032 B**，SHA-256
  `AF5F6282882FFE64B502859E4BFDF36031A69E658DC093158067722E4B044CEF`。
  r1 的 Meson 跨 subproject 源路径拒绝、r2 的 material 旧接口编译失败均保留；r3 构建通过后，
  为补 pre-drain Unlock 重建为 r4。编译仍有已有 warning，不声称零警告。
- 原跨位数门 `x64-d3d9-20261022-r1`：remote/reference × normal/invalid 四场景全部退出 0；
  每例 2,548 项像素检查零错误，image FNV 均为 `97937de3dc0c0c6d`；设备/factory 最终引用均 0。
  远端 healthy idle 6500ms、坏输入拒绝后的合法绘制和原公开 COM 保活合同继续通过。
  `remoteGatePassed/referenceImageMatched/referenceLifecyclePassed/ok` 全为 true。
  这关闭的是旧 15-reference 对照红门，不会改写 10-11 的失败证据。
- 生产对象重链探针 `device-owned-lifetime-20261022-r2`：3 周期、6 台设备、60 次 probe、
  147 次实际 GPU draw；21 次 UI draw、9 次 cursor 更新、6 次 PostFX、3 次部分初始化拒绝后重试。
  PostFX 6,144 像素和 cursor 1,251 纹理像素均零错；另外 144 个 captured-texture 像素零错。
  6 次合法 Reset、3 次真实 external DEFAULT Reset 拒绝、双设备隔离和外部资源保活全部通过；
  9 次实例 storage 测试及 6 次带锁 final Release 正常退出，DLL 卸载/窗口关闭完成，无强制终止。
  只重链诊断副本，多出的 test export 未进入产品；原有 named/NONAME 导出均保持。
- 读方/静态：六模块 60 方法通过；额外严格 lifetime 读方 22 方法通过；此前 D3D9/buffer/GPU
  读方反例 122 方法通过。这些与 Meson/原门中的重复测试不能相加成独立覆盖量。
- 链接符号复核：产品只引用七个 `War3ImGui_ImplDX9_*` 前缀入口，无 stock
  `ImGui_ImplDX9_Init`；upstream 原文件 SHA 保持 `D7943379…A69C5`。

### 保留的一次测试隔离失败

`device-owned-staging-20261022-r1` 的 native fixture 和唯一配置项检查通过，
但总门为 **false**：直接从旧 r1 路径运行同一 EXE，crash handler 向 **EXE 相对目录**写状态，
改变了旧 `device-owned-lifetime-20261022-r1/WarVK/Crash/crash_handler_status.json` 并新增文件。
不能仅凭新 cwd 就声称完全隔离。失败 receipt、旧 receipt 和被修改后的状态均保留，不恢复/追认。
最终默认轮因此重新生成独立 r2；补测将原 EXE/DLL 按精确字节复制到新的执行目录，
仍检查旧基线与产品未变，不修改产品路径规则或玩家目录。

补充 `device-owned-staging-20261022-r2` 已通过：对新 r2 baseline 的 EXE/DLL 复制前后逐字节身份相同，
唯一配置为 `d3d9.allowDirectBufferMapping=False`，实际 DXVK effective 日志确认已读入。
同样 3 周期/60 probe/147 draw、各像素零错、最终引用 0、正常退出；原 baseline 所有文件集合与哈希未变。
这结合冻结源码证明配置覆盖了 staging 路径；**没有直接注入 map-mode 或上传 copy 次数计数**。

`device-owned-regressions-20261022-r1`：独立目录全部测试目标已用 BelowNormal / Ninja -j2 编译，
**Meson 116/116 通过，0 fail/skip**，最终全目标 no-work。产品 DLL、原天气 DLL、玩家 DLL
在前后保持精确身份，生产/测试源码未漂移。回执只保留必要环境，不传播服务凭据到测试日志。
这不是全量 Python 静态脚本、完整游戏或全 GPU 组合门。

### 产物索引与复跑

- 产品构建：`AutoTest/artifacts/device-owned-build-20261022-r4/receipt.json`
- 原跨位数门：`AutoTest/artifacts/x64-d3d9-20261022-r1/receipt.json`
- 默认配置生产对象探针：`AutoTest/artifacts/device-owned-lifetime-20261022-r2/receipt.json`
- 同字节 staging 配置探针：`AutoTest/artifacts/device-owned-staging-20261022-r2/receipt.json`
- 本配置全部 Meson：`AutoTest/artifacts/device-owned-regressions-20261022-r1/receipt.json`
- 122 项读方反例：`AutoTest/artifacts/device-owned-readers-20261022-r1/cpu.stderr`

运行器均在 `AutoTest/`，对应 `run_device_owned_build.py`、`run_render_host_d3d9_gate.py`、
`run_device_owned_lifetime_gate.py`、`run_device_owned_staging_gate.py`、`run_device_owned_regressions.py`。
只接受新的证据目录；先完整构建并冻结，再跑 GPU，禁止多编译/GPU owner 互相改输入。
staging 补门依赖其 baseline 的完整文件/构建元数据快照；后续运行全量测试会合法改变 Ninja 元数据，
因此未来复跑 staging 必须先新建 lifetime baseline，不能拿旧 receipt 强行套当前 build 状态。

玩家 DLL SHA 仍 `1FFE0BD1…EE35CF`，旧天气 DLL 仍 `8A156518…4090CE`。
未部署、启动游戏、改系统/内核、修改私有共享核心、提交、推送或发布。

## 不由本批证明的事项

- 仍未接入游戏的 x64 renderer、Present/WSI、产品异步 owner 与热路径背压。
- 游戏画面、换图、长时压力、真实 device-lost、慢 GPU、x86 VA/commit 和帧尾延迟收益。
- 全库 COM 泄漏消除：其他 shader/material/cache 持有者不在这条 15-reference 基线内。
- UI singleton 任意多线程访问安全；本批 exact-owner 门不是跨线程 quiescence 证明。
- InstanceBuffer 的 active 仍为 TLS，不能把同线程退出/双设备测试外推为跨线程最后 Release 安全。
  自动 instancing 当前默认关闭；未构建的旧 `war3_render_queue.cpp` 仍有锁定 replay/缺少配对 Unlock 等债务，
  header 正常批次虽已有 Unlock，异常路径也未统一为 RAII。重新启用前须审计/统一唯一执行路径、
  完成异常与多线程退出门以及真实实例化绘制；本批不借所有权修复启用该实验功能。
- `D3D9DeviceEx` 构造函数内部所有 raw-new 异常路径都被 RAII 覆盖；这次保活从构造成功后开始。

## 下一阶段：接原 Presenter，不另做渲染器

继续以现有 `Presenter + DxvkSwapchainBlitter + DxvkDevice::presentImage` 接实际 D3D9 颜色输出。
首片固定单窗口/64×64，显式窗口 token、对端 PID/线程、window epoch 和 interval；
颜色目标须有真实 sampled usage/view，不能把只可 attachment 的图像直接喂给 blitter。

GPU timeline、present 接纳、队列 present result、Presenter frame signal 与 swapchain 退休分开。
acquire 后中途失败按终态 owner 隔离，不能靠正常析构无限等待伪装清理成功。
现有 fixture 的独立窗口消息泵可复用；它不证明未来游戏 HWND 线程同步 RPC 无死锁。
新门必须严格要求多帧 `D3D_OK`、绘制输入变化、Present 后继续绘制及正常退出；
旧离屏 `--present` 容忍 `E_NOTIMPL` 的测试不得升级为 WSI 证据。
