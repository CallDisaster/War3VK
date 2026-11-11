# x64 评审后的 T1/T2 首批测量与范围调整

2026-10-29。作者转交 Claude 只读评审后实施。StateBlock 已用 r5/r6 GPU 和 r1
生命周期回执收口，旧红门保留。本批暂停 RT/DS、StretchRect、Reset 等兼容扩展。
**T1 的净迁移收益仍未知，T2 的 go/no-go 尚未完成；没有共享环性能接受或完整游戏迁移结论。**

## 评审中需要更新的事实

- 约 25 ms 的 1,025-draw 数据是较早的实验。10-28 后续提交合并同轮曾为远端
  3.6–3.8 ms / 本地 0.72–0.85 ms；不能跨轮拼成累计优化率。本次新的同轮数字见下表，
  仍明显慢于本地，且与旧轮运行条件/源身份不同。
- 今天实际读取的玩家目录及沙箱 `war3.exe` **均已带 LAA**。本批没有修改 PE、注册表、
  驱动或系统。旧崩溃现场“无 LAA”的结论不代表当前二进制；低 2 GiB 的 MDLDATA 约束
  仍独立存在，不因完整用户 VA 为 4 GiB 而消失。
- `VirtualQueryEx` 不能从 MEM_PRIVATE 自动区分 Storm、DXVK、WarVK 或驱动堆。
  映射文件名也不证明谁创建私有堆。禁止按地址相邻、块大小或显存预算进行归属。
- 实际 census 包含内部调用。其 shader 创建/常量调用不能直接归因给 Game.dll，
  也不能把“原版固定功能为主”解释成当前 WarVK 只有固定功能工作。

## 本批实现与所有权

`native_game_memory_review.py` 使用现有 NativeObserver 的保留只读句柄；验证 PID、
创建时间、映像、WOW64，按受保护磁盘 PE 的 LAA 标志限定 2/4 GiB 区间。最多
32,768 次查询、默认 2 秒合作式截止，输出区域、allocation base、保护、映射文件、
state/type 两轴与 64 KiB 对齐空闲块。只有完整扫描且前后身份一致才标 complete。
不重新开进程、不暂停线程、不读目标载荷、不修改目标；扫描不是原子快照。

调查器新增明确的 memory-review 模式：地图 ready 后等待 12 秒，再重新确认帧推进，
保存 VA 表，截图，原生 EndGame、两次退图见证及独占窗口关闭/自然退出。
CPU recorder 子模式只允许固定 262,144 容量的 arm 和精确 session 的 freeze/discard；
图像历史、raw inputs、draw/palette 子门保持关。缺少 census 的普通构建只可签发单独
的 `LOCAL_GAME_MEMORY_METADATA_AND_EXIT_PASSED`，不伪造 census 覆盖。

宿主新增默认关闭 `WARVK_D3D9_WAIT_PROFILE=1`：同一 GPU owner 内记录 flight
复用的 CPU 提交等待、vkWaitSemaphores 调用时间、DXVK use retirement 等待及总时间。
最多 128 个 EVENT Issue 区间，累计值在正常 teardown 冷输出；满额显式 dropped。
关闭时无计时调用，新增固定元数据只在 x64；没有逐 draw 日志/分配/IPC、新锁或退休规则。
三个子时间包含于 total，不能重复相加；Vulkan wait 的 CPU 调用耗时不是纯 GPU 执行时长。
读方把前 44 个区间逐个对应测试帧，排除 4 帧预热，拒绝丢样、错序、布尔冒充计数、
负数、draw 人口不符及分段总和大于包含时间。没有把最终 EVENT wait 计入复用时间。

## T1：实际游戏 VA 与 CPU 录制分量

三轮同一 DLL `26B411E99AAEEB2A53F064E1D221202915CE995C95742C1B9891659FD4C55A69`，
同一 Lost Temple 内容 SHA、同一 1920×972 输出、同一显式配置。地图初始相机不发送输入；
不是确定性 replay，没有相机矩阵逐值同一证明。OFF r1 与 CPU-ON r1 截图已目视检查：
相同起始基地、五名农民、金矿、树木、UI 和可见阴影；动画/光照时间可不同。
这些画面只证明本地进图，不是 x64 视觉验收。第二 OFF 样本用于显示启动波动。

| 指标（MiB） | 录制 OFF r1 | CPU 事件环 ON r1 | 录制 OFF r2 |
|---|---:|---:|---:|
| GetProcessMemoryInfo Private Bytes | 652.785 | 731.922 | 633.062 |
| Working Set | 690.594 | 763.910 | 664.020 |
| 低于 2 GiB 的总空闲 VA | 697.941 | 617.875 | 718.391 |
| 低于 2 GiB 的最大对齐空闲块 | 160.625 | 131.312 | 178.438 |
| 尚无分配器归属的已占 VA（含 reserve） | 1018.117 | 1098.184 | 997.668 |

ON 相对两个 OFF 的 Private Bytes 增量为 **79.137–98.859 MiB**；OFF 自身差
19.723 MiB，不能宣称精确 100 MiB 的实测净收益。ON 实际 arm 成功、7558 条已接受
CPU 事件、producerLosses=0，freeze/discard 成功；不是只有配置字符串。
该对照没有开启或测量 144 MiB 输入读回/图像历史，不能声称“全部诊断成本已测”。

OFF r2 的未归类 VA 中：MEM_PRIVATE 905.184 MiB，未取得文件名的 MEM_MAPPED
92.484 MiB。这与进程 Private Bytes 633.062 MiB 是不同账轴，不作相加。
完整 4 GiB 的最大空闲块约 2 GiB，不能替代低地址域的 178.438 MiB 连续块。

映像区域可按实际文件名归属：`nvgpucomp32.dll` 64.480 MiB、`nvoglv32.dll`
25.578 MiB、`nvapi.dll` 4.930 MiB、`vulkan-1.dll` 1.480 MiB，合计约 **96.469 MiB**。
这是 VA 映像范围，不是物理常驻量；只有将全部相关渲染依赖移出、x86 不再需要它们时，
才是可移出的候选。`d3d9.dll` 映像 77.352 MiB 混有 hook/采集/渲染/BSS，不能全计迁出。
`Game.dll` 12.844 MiB、`Storm.dll` 0.379 MiB 的映像更不等于它们的私有堆大小。

**尚不能给 T1 净收益数字**：DXVK host-visible chunk/staging/pipeline/shader、
WarVK/TLS/ImGui/PostFX、Storm/Game 私有堆缺少分配区间所有者证据。下一步应由既有
allocator/owner 在冷安全点发布 bounded address spans 与寿命，再与同轮 VA 区域求交，
记录别名/重叠/未覆盖；不装全局 malloc 钩子或把未归类全部算成 renderer。

## T2：8 次等待推断的实际检验

新宿主 `813688CC67648327CCC6542181A8123A81A9BB9B811A7FC68AE84BDC9F6C87A4`，
5,869,568 B，fresh BelowNormal/-j2 build/no-work、sources/protected 保持。
在原 1,025 draw、64×64 合成工作量上，原本地 DLL与 x64 两种模式同轮对照；
只作瓶颈归因，不是实际游戏 FPS。每端 4 预热 +40 测量帧，CPU 总数包含预热/读回。

| 计时 ON，本轮两次 | 完整帧中位 ms | 复用次数/帧 | 复用总时间中位 ms | Vulkan wait 调用中位 ms | 复用总量/完整总量 |
|---|---:|---:|---:|---:|---:|
| 本地前/后 | 1.698 / 1.880 | 不适用 | 不适用 | 不适用 | 不适用 |
| 四 flight，逐批提交 E1/E2 | 15.196 / 14.975 | 29 /29 | 7.332 /7.086 | 6.879 /6.940 | 44.3% /46.7% |
| 四 flight，四批合并 G1/G2 | 14.726 /14.799 | **8 /8** | 8.321 /8.048 | 8.083 /7.919 | **54.7% /51.2%** |

因此“每帧 8 次”只对四批合并模式成立；逐批提交为29。等待确为重要成本，但约一半
耗时尚在其它路径，不能从次数把全部性能差距归到这里。CPU 提交等待中位不足0.003 ms，
退休等待约0.11–0.14 ms；这不等于可以删除任一完成/退休见证。

同候选关闭计时的独立对照：本地1.761/1.736 ms；合并远端14.548/14.609 ms，
p99 16.213/16.707 ms；双进程 CPU 359.375/328.125 ms（44帧含读回），
本地187.500/109.375 ms。远端仍约本地8.3倍。开关轮中位相近，但不是随机交错长门，
系统竞争负载未排除，不能据此宣布计时零成本或跨日期性能回退原因。
40样本的p99就是最大样本，不作为长期尾延迟认证。

### 真实负载采样的发现与缺项

`t2-real-census-20261029-r1` 使用已有 census 开发 DLL `F6589D0F…68F9C4F`，
与T1当前普通构建分列，不拿不同 DLL 比内存。实际 Lost Temple 进图/稳态/退出通过，
5份严格 v2 快照、recordBusy/snapshotBusy/counterSaturated=0。进程共1606个Present、
173378次draw尝试（168206 indexed、5172 nonindexed）、2117次纹理创建请求、
432次StretchRect、5604次SetRenderTarget、431次CreateStateBlock等。
这些包括载图、游戏和退出、内部调用，不能称稳态逐帧数或成功绘制数。

现有 census **没有 Lock/Unlock/GetData，只有稀疏累计快照**，无法给每帧分位数、
上传字节和同步频率。最后累计区间1024→1606的draw平均约255.55/Present，但混有
载图/退图，不能用它签发真实负载压测规模。下一实现先补这些按帧有界、来源明确的
调用/字节计数和冷导出，关门时不启用设备全局多线程锁；然后才以该分布驱动共享环原型。

共享环、差量状态/常量、原DXVK常量storage轮换、异步资源创建/延迟错误及动态VB/IB
共享暂存尚未实施。异步资源失败必须维护 pending/live/failed 和代际，后续依赖失败
可见，不可把“乐观返回”当作GPU资源已成功；背压/断连/Reset仍须有界，不做无限队列。

## T4 与 T1 放在同一口径下比较

| 方案/分量 | 当前证据 | 可作的结论 | 尚缺/限制 |
|---|---|---|---|
| 外置全部图形驱动依赖 | 映像VA约96.469 MiB | 条件性的迁出候选 | x86完全不再需要这些库；私有驱动堆未归属 |
| 外置DXVK/WarVK渲染堆 | 大量private/mapped未知 | 净收益未知 | 原owner区间/寿命、代理/上传/managed副本扣账 |
| 关闭CPU事件录制 | 本轮Private差79–99 MiB | 调试时有显著成本 | 当前正式默认已关，不能再计一遍额外收益 |
| 缩小/按需输入读回与图像历史 | 本轮关闭；已有profile/按需路径 | 需要另测其激活窗口 | 144 MiB等预算不是常驻实测，损失取证时窗/覆盖须显式 |
| 缩小Arena上限 | 源码页为DEVICE_LOCAL、按需创建、1152 MiB硬上限 | 不能算为等量x86 VA节省 | 实际host-visible/驱动映射未知；不得减caster换数字 |
| 现在开启LAA | 今天玩家/沙箱均已开启 | 对当前二进制没有再增加2GiB的收益 | 不等于所有Game指针都高地址安全 |

LAA剩余清单：精确Game/Storm版本门；MDLDATA源名/heap/line的低地址域、native
reserve/commit/realloc/failure语义已局部加固；仍须其它Storm bucket/原生数组及
指针→有符号整数、sentinel/complement链、跨DLL/JASS carrier的分域审查和高地址压力
验证；多地图A→B→A、Reset/长时与其它硬件未覆盖。不可泛改Game的jg/not或把一次
Lost Temple正常退出当作完整LAA认证。本批没有扩大任何预算。

## 回执与未做事项

- `AutoTest/artifacts/t1-va-off-20261029-r1`、`t1-va-cpu-recorder-20261029-r1`、
  `t1-va-off-20261029-r2`：各保存完整VA区域、原始IPC、真实截图、模块与保护身份，
  正常退出通过；可用`analyze_x64_review_baseline.py`复算。
- `AutoTest/artifacts/t1-t2-review-baseline-20261029-r1.json`：明确
  T1Complete=false、netMigrationEstimateBytes=null、T2GoNoGoPassed=false。
- `t2-wait-build-20261029-r1`、`t2-wait-stress-20261029-r1`、
  `t2-wait-stress-off-20261029-r1`：fresh构建、两组真实CPU/GPU/像素/故障恢复通过。
- `t2-wait-vvl-20261029-r1`：三场景，包括原始非法事务/合法恢复，VVL365检查通过，
  不用该轮作性能结论。现有submitted/completed/retired与资源清零门保持。
- 纯测试：full VA 6例、调查器39例、等待读方3例、原批次读方9例通过。
  原低VA读方15例亦复跑通过。
  它们不替代上述实际GPU或游戏证据；未重跑产品全量119项，不继承为本批结果。

T0备份提交仍待作者确认，未写index/HEAD/commit。1.5×/2×建议值尚非作者批准门槛。
T3增强层依赖清单与T2共享环仍待完成；不进入增强层代码搬迁。T5只在本次新测量代码中
采用清楚的所有者、行为拒绝测试和常规格式，没有全库重构。
玩家DLL保持`1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`；
无玩家部署、提交、推送、发布或系统修改。
