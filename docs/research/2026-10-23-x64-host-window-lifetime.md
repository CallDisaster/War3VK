# x64 宿主窗口寿命：真实 CPU 门与两轮 Presenter GPU 验证

日期：2026-10-23。仅开发宿主；Game.dll 禁止、玩家 DLL 不替换、不提交或发布。

## 已经证明的边界

同一个 `HostSurfaceTarget` 实现已在真实 x86 父进程 / x64 宿主进程、
Restricted Medium、非输入 Win32 Desktop 上通过八个场景；没有 GPU 调用。
不是另外一份寿命模型，也不把 `claimSurface` 的 CPU 模拟当作 VkSurface 已创建。

| 模式 | 场景 | 实际结果 |
|---|---|---|
| host-top-level | 正常 | 模拟退休前原 HWND/owner thread 存活，退休后各销毁一次 |
| host-top-level | 父窗 resize | Extent 永久故障；原 HWND/thread 保留到退休；仅故障清理 |
| host-top-level | 父窗 close | PeerWindow 永久故障；原 HWND/thread 保留到退休 |
| host-top-level | owner quit | 两次 WM_QUIT 成功投递；故障后保活 102.4432ms / 12 样本 |
| host-child | 正常 | 原 HWND/thread 保留到退休，正常清理 |
| host-child | 父窗 resize | Extent 永久故障；保活到退休，非正常 teardown |
| host-child | 父窗 close | Windows 外部销毁 child；preRetirementDestroyed=1，不能记保活成功 |
| host-child | owner quit | 两次 WM_QUIT 成功投递；故障后保活 101.9532ms / 12 样本 |

OwnerQuit 以原线程 HANDLE、HWND/PID/TID/nonce marker、QPC 持续核对，并在收到
Retire 后、模拟退休调用前再次检查。两次成功投递不是两次已 dispatch 的计数证明。
故障进入 terminal pump，不因 WM_QUIT 直接退出 owner 线程而隐式销毁 HWND。
父窗造成的 child 外部销毁另列，不能通过豁免把 top-level 的提前销毁也放过。

policy64 精确 54 checks；x86→x64 Desktop probe 通过；十次启动均自然退出、没有强杀，
实际输入 Desktop 一直是 Default，所有 owned Job 为空并关闭隔离 Desktop。
GPU 模块不存在为进程 begin/end 采样；不声称全过程模块加载审计。

## 身份与失败保留

- 纯读方由主线程独立跑 19/19；独审另跑 17 个无临时写入的用例并做反例检查。
- 原 r1 被 x86 编译 `-Werror,-Wsometimes-uninitialized` 拦截，未开始 native；
  保留 `host-child-surface-cpu-20261023-r1/receipt.json`，
  SHA `BCD229D4CA3D6A8574F679D78E19DE13C63DA75ECF5571493143DAC405461296`。
- r2 只初始化测试夹具 Scenario 局部值，强制解析仍保留，不关闭警告、不扩大期限。
- r2 回执：`AutoTest/artifacts/host-child-surface-cpu-20261023-r2/receipt.json`，
  1,434,492 B / `E91A6191937B36E95323F835ACDA605B7BD0271C339366494B436647B1E7B571`。
- 独审重新调用严格读方复算八例逐字段一致；58 冻结输入、15 初始 raw 清单、39 个唯一文件保全。
- x86 parent：805,376 B / `A65442325FDA5419FDDB42FB8B09A9EDD1F25439596A800EF88407048ECB4E39`。
- x64 CPU host：822,784 B / `8F0D32E9E4B50C7235077BF81988485A92560C18374BE4E1EA2918E88F20B5E5`。

## 本次生产责任边界收敛

在 `tools/render_host/dxvk_core/` 的开发宿主接入同一 adapter：

1. 原 peer HWND/PID/TID/marker 继续表示认证的父窗，不换成 host HWND 后沿用旧证明。
2. 独立 x64 UI owner 创建实际 surface HWND；实际 `wsi::createSurface` 消费它。
3. surface 可能开始存在前先 claim；健康退休仍复用原 DXVK timeline、资源使用、frame signal、
   submit/finish queue drain、Presenter 析构，不另造 GPU 释放机制。
4. Presenter 析构后、仍持有 device 时再次检查健康状态，再允许关 HWND。
   这针对析构中锁存的 device loss，不宣称覆盖全部驱动失败注入。
5. 普通工作统一经过 `active(Admission::Work)` 的 owned-window 校验；只有 Present 的
   具名 WindowFaultCleanup 入口可先选择原有、严格证明 GPU quiescent 的故障清理。
6. quarantine 时在 backend 线程输出 `owner-quarantine-snapshot`，保留当时已知的 fault、
   preRetirementDestroyed 和 terminalPump；这些独立原子观测不是进程退出后的最终状态。

原正式宿主四文件 preimage 留在
`AutoTest/artifacts/host-surface-integration-preimage-20261023/`。
adapter 三文件与 CPU 验证版本字节相同：

| 文件 | SHA-256 |
|---|---|
| host_surface_target.h | 4DCE60AFAF6DA3CD320D5CBB29D659343D650E44DB79E77AB200925898C6D683 |
| host_surface_target.cpp | 84BF0A4C961A2C021D97A007E1EE9CAE95EE1B0CC926600A444F3337AE2A8D39 |
| host_surface_target_policy.h | A9D096FC3A87BC5A6B7AFDF6910CAE2B439EF2621335E0B92E4C40B87FDD39E6 |

## 正式开发宿主构建与接线回归

- 主线程独立全新 x64 构建、实际 no-work 均 exit 0；BelowNormal、Ninja `-j2`。
  只构建开发宿主，shipping D3D9 target 关闭；既有编译警告保留，不声称 warning-free。
- 构建回执：`AutoTest/artifacts/host-surface-candidate-build-20261023-r1/receipt.json`，
  SHA `B9132B87B9D4FBD76167E3A9CCC81A1F96E85824BEDC66ED2F6F50810F4F1A8F`。
- 实际 x64 host：5,541,888 B / `C58F020DBFEF5EC2487DECAAC610AC8BBFBCB63037965CAA2E818B4BDEE19623`。
  构建读方重新核对 1,110 个源码输入、完整构建命令、raw 和实际 compile boundary；
  不是旧诊断 host，也没有复制 TU 插桩。
- 新增独立静态交接测试 `AutoTest/test_host_surface_integration.py`，主线程复跑
  **21/21、73 个内存变异全部 KILLED**。原始 baseline 必须先过；去注释后的函数体
  检查默认 Work、Present 唯一故障清理例外、健康检查/退休顺序和 peer 身份。
  这是静态源码门，不是设备丢失注入；本次没有注册或宣称全库 Meson 回归。
- GPU 严格读方纯测 **22/22**；冻结前独审发现并修正 probe 字段层级错误及两个退出时序误通过。
  保留真实 stdout/stderr 缓冲次序，不靠不可能的日志排序制造通过或失败。

## 两轮实际 Presenter GPU 六场景

同一个正式 x64 host、同一组冻结源码和 VVL365；每轮重新编译 x86 proxy 与原测试夹具，
创建新的非输入 Win32 Desktop，并逐场景启动新的认证 x86/x64 进程。
主线程执行，两轮各 **6/6 PASS、exit 0**；不改变夹具、工作量或原来的时限。

| 模式/场景 | 每轮实际工作 | GPU 提交/完成/退休 | r1 / r2 primary exit |
|---|---|---|---|
| top-level normal | 8 帧、24 draw、61,152 次像素检查，0 错 | 65 / 65 / 65 | 109 / 265 ms |
| top-level clear-soak | 256 次 Present、1,048,576 次像素检查，0 错 | 513 / 513 / 513 | 94 / 172 ms |
| top-level destroy | 已静止 GPU 后关闭父窗；1 次已退休 Present | 3 / 3 / 3 | 109 / 141 ms |
| child normal | 同上 8 帧/24 draw/61,152 检查，0 错 | 65 / 65 / 65 | 93 / 141 ms |
| child clear-soak | 同上 256 Present/1,048,576 检查，0 错 | 513 / 513 / 513 | 94 / 140 ms |
| child destroy | 已静止 GPU 后关闭父窗；child 外部提前销毁 | 3 / 3 / 3 | 110 / 234 ms |

这里的 normal 像素检查含 Present 前/后的 D3D9 color target 及实际 acquired swapchain image；
swapchain 部分每例为 20,384 次检查。它不是窗口截图、桌面合成或玩家肉眼验收。
clear-soak 也不能替代包含 shader/纹理的长时游戏负载或性能基准。

每例实际只有一次 surface 创建回调；因此**没有证明真正的 surface recreate**。
正常例均在实际 Presenter 退休后关闭宿主窗口、逻辑资源/句柄归零。
两个 destroy 场景均为 `gpuCleanupCompleted=true`、`quarantined=false`、
`normalTeardown=false`，host 自然 exit 2；测试父进程自然 exit 0 表示预期故障处理成立。
child destroy 两轮均 `preRetirementDestroyed=1`、`externalTargetLoss=true`；
top-level 均为 0/false。不得把 child 的 OS 提前销毁包装成窗口保活成功。

共同安全与验证证据：

- 原 transport/primary exit **5 秒**、Connection call **30 秒**、场景外层 **90/45 秒**未改；
  没有 secondary drain 追认、强杀、quarantine 或 timeout 被算作通过。
- 每例独立 VVL **1.4.365** raw 证明 Core/Synchronization/Object lifetime 等实际启用，
  `errors=0`、`nonStatusWarnings=0`；完整 loader/层 DLL 身份一致。
  不声称 runtime 所有设置均已证明，也不声称覆盖所有设备丢失。
- 每轮 1,163 个冻结输入保持；7 次 launcher 启动及两个架构的实际 Desktop probe，
  Restricted Medium、精确 owned Job、输入 Desktop 前后 Default；Job 为空、Desktop 已关闭。
- 每轮 10 份初始 raw 清单、39 个唯一 raw 文件；完整构建、启动、失败信息均保留。
  独立审查对 r1/r2 原始 native/loader/VVL 日志逐字段重放均一致；主线程也再次完成两轮各六例
  离线重放和全部冻结输入/raw 检查。主线程首次直接比较 Python tuple 与 JSON array 导致比较失败，
  核实差异仅为 loader 的两项层列表序列化形态后，按相同 JSON 表示比较通过；没有改门禁或原始产物。
  两轮各自保存回执，不拼成一个不存在的完整 C2 门。

| 产物 | 大小 | SHA-256 |
|---|---:|---|
| `host-surface-gpu-20261023-r1/receipt.json` | 1,490,859 B | CB61662CF7609137813938D5BAC7F5B7DFE176DAFD89216303ECF54413389530 |
| `host-surface-gpu-20261023-r2/receipt.json` | 1,490,857 B | 8862ACFB3AF8B78B6D42B3CE218754BCF185D9547E7637B9753A84FB7A40ECD7 |
| r1 x86 proxy | 984,576 B | 9F695522859CF70F9F99A8379282DEC4885D150CE5B87C27B2DDCE35E188AE33 |
| r2 x86 proxy | 984,576 B | C588010C285D9F079A8A6C224F7365149A73231F1AA7B5F5771C6DA26B109A32 |

上表路径均位于 `AutoTest/artifacts/`。源级测试/执行配方冻结于
`host-surface-gpu-contract-20261023/`，`draft-freeze.json` SHA
`D36F562E01EED727B955CAE6B893DB1A8045AEEA40A75EF7C0963BF70A5F869C`。
玩家 `E:/Work/Warcraft III/d3d9.dll` 保持
`1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`，未部署/启动游戏/提交/推送/发布。

## 下一小批：空闲故障交接，不扩大产品准入

只读代码审查确认两处缺口：宿主的 idle 首字节等待只监听 I/O 和 peer 进程，
adapter 的 fault 仅唤醒 UI 自己；x86 Connection 空队列等待也没有监听确切 host 进程退出。
因此本轮只能证明**有后续请求时**的窗口故障处理，不能声称空闲状态能主动闭合。

下一批按以下顺序收紧，尚未实施：

1. 独立 manual-reset、不可继承且不复位的已锁存 fault 通知；不得复用 UI auto-reset wake。
2. 只有持有 OVERLAPPED 的 owner 取消并实际 drain idle read，之后才释放缓冲/事件；
   用具名 LocalStop 区分 peer-exit/timeout，不生成不存在的 IPC 请求或推进 sequence。
3. 专门的 owner-thread idle 故障入口复用同一 `canCleanWindowFault` 谓词；
   证明静止才沿原退休顺序清理，资源仍在途/状态未知则 quarantine/fail-stop，不能先等待来凑静止。
4. x86 worker 监听确切 OwnedChild 进程退出并关闭准入；不得当成收到远端回包、推进 done/ack。
5. 分别验证 CPU 管道取消竞态、无新请求的窗口故障、健康 idle 恢复，以及真正 GPU 在途故障。
   UI 完全挂死但尚未锁存 fault 仍需独立健康探测门；驱动等待继续依赖外部 watchdog。

## 仍未覆盖

- 已覆盖上述有限正常退休与 GPU 静止时的父窗故障清理；真正的 surface 重建、
  child 在 GPU 在途时的故障收容仍未覆盖，不能用本轮六例替代。
- idle transport 尚不因本地窗口 fault 主动唤醒；后续普通工作入口会拒绝已观察到的故障，
  但产品 owner 的自主取消和非阻塞退出仍是后续门。
- 同一 HWND 的真实 surface 重建、混合 DPI/显示器、外部父窗消失时的真实 GPU 在途矩阵。
- 完整 C2/D3D9/WarVK pass、游戏内存与尾延迟 A/B、玩家前台画面、发布接受。

历史进度与本轮前置 GPU 六例见
[事件唤醒 checkpoint](2026-10-23-x64-notification-waits.md)；总体实施顺序仍是
[64 位迁移路线](../plan/2026-10-11-x64-render-migration.md)。
