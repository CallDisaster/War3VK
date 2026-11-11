# 2026-10-10 暴风雪 v1 最终独立源码审查

审查快照：台北 2026-10-10 03:20 左右；工作树为 `dxvk` 的现有 dirty 主树。
审查 lane：`/root/weather_api`。主线程拥有构建、实机、部署和最终验收；本 lane 未执行这些操作。
本记录是有限范围的源码/专项静态审查，不是稳定发布批准，也不覆盖其它未提交改动。

## 结论，用人话说

作者设置天气、天气时间线、进入渲染队列的这一条链已落到生产代码。
雪片和雪雾使用同一份帧天气值，不会在 GPU 记录过程中临时读取另一套 JASS 配置。
换图备用入口、天气资源创建失败、创建中途退出等真实边界在审查中暴露过缺口，
现在相关源码已修正，并有针对性的反例/合同测试；不是只新增文档或一层 Manager。

截至这次复审，在下面限定的调用链中没有再找到必须立即阻止构建的新源码缺陷。
这句话不等于“并发/GPU/异常/跨地图已经全部安全”。仍需主线程的组合构建、
实际游戏画面、相机切换、资源压力、换图和退出门；尚未完成的门不能用静态绿灯替代。

首版实际范围仍是可编排的室外雪片、雪雾和低空吹雪，不含自动屋檐避雪、建筑积雪、
地形高度精确贴合或音乐播放位置自动读取。天气不进入 Arena、Stage11 或 caster 收集，
不改变原有 caster 完整性、准入或预算。

## 数据链与责任边界

| 边界 | 当前生产证据 | 能证明什么 / 不能证明什么 |
| --- | --- | --- |
| JASS/Catalog → 严格命令表 | `war3_japi_v1.cpp:262` 起的 12 个 `weather.*` 表项；`DispatchWeather` 位于 `:1042`，JASS/Catalog 同侧专项测试 | Carrier、参数顺序和作者控制入口已接线；静态测试本身不证明游戏 VM 实际调用 |
| 公开句柄 → 活实例 | `war3_japi_v1.cpp:938` 的 ResolveObject 检查类型和实际 backend 存活；`:961` 的 RegisterObject 使用不回绕公开 id；`:1050` 创建失败回滚内部实例 | 旧/无效/异类型句柄不能直接授权天气修改；CPU 协议宏不替代混合 native 对象实机证明 |
| 作者状态 → 有界时间线 | `war3_weather_runtime.cpp:51/58/71` 的有限性/范围检查；`:288/301/330/348/364/376` 的实际变更函数；生产头最多 64 关键帧、一个实例 | 非法输入先拒绝；暂停、关闭、Seek、手动过渡有明确状态，不要求 GPU 就绪 |
| 时间线 → 帧输入 | `d3d9_war3_pipeline.cpp:835`，按 frame/map/device 三元组缓存，`input.weather` 按值复制 | 同一帧的两个消费者拿同一份冻结天气；冻结值本身不证明摄像机、深度或 GPU 有效 |
| 帧输入 → CS 队列 | `d3d9_device.cpp:17988/31098` 捕获，`:17993/31106` move 到队列 lambda | 后来的 API 改动不会回写已排队输入；disable/destroy 不是追溯取消已经排队的旧帧 |
| 雪片消费者 | `d3d9_war3_weather.cpp:300` 起，实际 frame/epoch 与 exact-frame camera 检查；只读 `input.weather` | 消费域被检查且不重新读 live runtime；cmdDraw 已记录不等于 GPU 执行/像素正确 |
| 雪雾消费者 | `d3d9_war3_volumetric_light.cpp:2782/2887` 的独立 weather child 与 weatherMedium 判据 | 借用同一帧值、普通作者 fog 不被天气改写；摄像机兼容范围见下方未覆盖门 |
| 关闭后的最后一帧 | `d3d9_war3_pipeline.cpp:1034` 起保留 `m_weatherSnapshot.enabled` 的一次 world-frame drain | 当其它后处理都关闭时，仍有机会执行实际 off 输入和清理/诊断；无 world frame 时不声称即时 GPU 回收 |

### 时间行为专项复核

- `advanceLocked()` 先更新墙钟再检查 enabled/paused，防止恢复时把关闭/暂停期间补算成演出时间。
- setEnabled/setPaused 在同一把 CPU 状态锁内先推进旧状态再切换；恢复不跳过积压时间。
- setTarget 从当前求值切入手动过渡，不隐式开启一个已关闭的实例；原时间线停止。
- 第一关键帧必须 0，随后严格递增；最多 64 项，播放状态下不允许随意编辑。
- setTime/playTimeline 增加独立 historyRevision；Seek 明确重建位移历史，不伪装为连续运动。
- 演出上限 86400 秒，到界后保持；不是无限时长天气计时器。
- 风位移按变化的风速/最短风角实际积分，不采用“当前风速乘历史总时间”的会传送雪片的公式。
- CPU runtime 锁只保护作者状态；它不能被写成 GPU completion 或跨地图资源退休证明。

## 审查发现过的真实缺口及当前修正

### F1：只在 native-table reset 清天气，备用 MainRunner 会带旧天气进新地图

反例：A 地图天气活跃 → 进入 B 地图但没有 native-table 信号 → 新 mapEpoch →
CaptureWeatherSnapshot 对仍活跃的 A 作者状态赋 B 的 mapEpoch。单独增加 epoch 字段不能解决这个问题。

主线程已补 `ObserveWar3MainRunnerEnter()` 的 scoped `ResetWeatherAuthorState()`，
在 transition gate 获准后、原 MainRunner/Alt trampoline 运行前执行。
实际顺序位于 `d3d9_war3_hook.cpp:443` 和 `war3_hook_lifecycle.cpp:1889/1916`。
`war3_japi_v1.cpp:2402` 只移除 Weather registry 并 reset CPU weather，不清其它点光/局部雾。
公开 id 与内部 id 都不因 reset 回绕复用。

生产协议测试源码现包含：旧天气失活 → 创建新天气 → 旧句柄命令被拒 → 新天气仍可开启。
本 lane 实跑的 API 静态门增加到 8 例；协议可执行结果和跨地图物理结果由主线程记录。
不把直接调用 scoped reset 的 CPU 测例写成“真实 Hook 时序已完成实机认证”。

### F2：独立雪雾 owner 原本仍可因分配异常永久停掉普通作者雾

反例：天气 owner 构造/图像/descriptor 创建抛异常 → 外层 War3Pipeline catch 把整项
`VolumetricLight.enabled=false` → 普通作者 fog 一并停止。128 MiB nominal gate 不排除驱动/host OOM。

现 weather owner 创建和 weather child 资源准备分别捕获 DxvkError/bad_alloc，
同资源 key 至少等待 120 业务帧再试，失败和等待有独立具名诊断。
准备区先创建图像、fallback、pipeline 与懒 descriptor，之后才 copy/UBO update/record。
已锁存 DEVICE_LOST 重抛；记录阶段没有新增吞异常逻辑。

构造中途失败会释放尚未被命令使用的 raw compute pipelines；pipeline map 插入抛异常会释放新 raw handle；
三张 Froxel image 按完整候选成套发布。源锚点：`:660/672/816/834/2822/3443`。
这是资源失败的可恢复源码边界，不是实际 OOM/设备丢失注入已经验收。

### F3：雪片创建失败原本也会越过可恢复边界

雪片最新准备区位于 `d3d9_war3_weather.cpp:461` 起，第一条 depth copy/barrier 在 catch 区之后。
pipeline、depth sample view、bounded copy、懒 depth/color descriptor 都在此准备区完成。
同 key 失败通过共同的 WeatherResourceRetry 退避，未知异常和记录阶段异常继续外传。

新增 owner adoption 的调用侧 cleanup：AdoptWar3TrackedVkPipeline 返回 owner 前抛异常时，
仅释放尚未被命令引用的新 raw pipeline，旧 tracked pipeline 不动（`:165`）。
共享 adoption helper 和其它 pass 未在这个修正中改写。

enabled→off 诊断现在保留传入的实际 WeatherFrameSnapshot，而非伪造空快照。
持续 off 不新增每帧诊断锁；copy 引用只在实际 off 边沿清理，queued 命令继续持有其资源。

## 生命周期复核

- 雪片 raw pipeline 由 War3TrackedVkPipeline 包装，命令列表实际 track pipeline/sampler/image。
  替换当前缓存不等于释放在途 GPU 使用；其最后引用交给既有 DXVK completion 流程。
- DxvkImage 实际缓存 view（`dxvk_image.cpp:257`），view 引用计数转给 image（`dxvk_image.h:895`），
  Vulkan descriptor/view backing 来自 image storage。没有因雪片未单独 track view 就直接判为悬空 view。
- 雪片 retained depth-copy 当前 backing 有 32 MiB 检查；颜色/深度布局精确还原，重复 copy 带 prior-read→write→read 依赖。
- 雪雾 child 在父 pass 内持续保留，不在 disable/seek/map reset 直接 delete；与普通 High 的资源 cache 分开。
- 正常最终析构源码顺序为 Flush → SynchronizeCsThread → waitForIdle → DestroyWar3RenderPipeline
  （`d3d9_device.cpp:9027/9028/9034/9048`），之后才释放父/child raw pipeline。
  这只证明正常路径源码顺序，不保证真实 device-loss、异常终止或进程卸载已验收。
- retry key 只有地图/设备/尺寸/格式，普通天气强度/id/revision 不绕过失败退避；
  单槽方案允许不同 key 交替重新尝试，不声称全局创建频率上限或资源峰值硬限制。

## 仍然明确保留的验收门与债务

| 项目 | 当前边界与实际可能效果 | 后续验收条件 |
| --- | --- | --- |
| 相机时效差异 | 雪片要求 exact-frame；雪雾沿既有普通 volume 兼容相机。上游 last-good fallback 最多允许前一帧（scene.h:54），因此可出现雪片跳过而雪雾仍画的一帧 | 相机旋转/俯仰/世界→正交 overlay 切换的帧诊断和视觉检查，不能声称两者所有路径 exact-frame |
| 实际峰值内存 | 32 MiB 是雪片当前额外 copy；128 MiB 是雪雾 nominal gate。统计不含退休旧资源、driver pipeline/sampler/allocator 额外量；开关后雪雾 owner 保留缓存 | resize/format/换图压力下的 actual current+retired census 与恢复，不把两个数相加当总 VRAM 上限 |
| 热路径真实成本 | 雪雾有额外完整 color/depth copy 和独立 Medium pass；雪片固定 16384 实例仍有像素 overdraw | 单独 GPU 时间、总帧 p95/p99、普通 High+天气 Medium 并用、关闭态与开启态成本 |
| 资源故障 | prepare OOM 现在可具名退避；record 异常仍遵守原外层 fail-stop/disable 行为 | 实际 recoverable OOM/DEVICE_LOST/记录后异常注入和合法天气恢复，不能由源合同推导 |
| 特殊 target | 雪片有 mip/layer/sample/viewport 检查；雪雾继承普通 volume 的 standard world RT 假设 | 非标准 mip/layer/usage/viewport 的显式兼容门，不泛称全部 D3D9 render-target 均可用 |
| 画面与艺术 | 固定冷白雪、室外盒子、相机附近有界格子；没有屋顶避雪、建筑积雪或地形高度查询 | 近远镜头覆盖、透明物件/战争迷雾/UI 顺序、强暴雪可读性和作者视觉验收 |
| 已有通用 reset 的 host-OOM | 全量 ResetAuthorState 仍有 vector reserve 的旧 noexcept 路径；本批 scoped Weather fallback 是 erase+CPU reset，不新增那种临时 vector | 后续独立给通用受管对象 reset 做 allocation-failure 门，不借天气批扩大到全 registry 重写 |

## 本 lane 实际执行的验证

这次独立复跑（均 EXIT 0）：

- `python AutoTest/test_war3_weather_shader_contract.py`：26/26。
- `python AutoTest/test_war3_weather_volume_static.py`：13/13。
- `python AutoTest/test_warvk_weather_api_static.py`：8/8。
- `python AutoTest/test_warvk_japi_safety_static.py`：5/5。
- 相关 API/雪片/雪雾生产与专项测试文件 `git diff --check`：EXIT 0；LF/CRLF 提示未算成失败。

这些是 source contract/reference math，不是 GPU 像素或异常注入。
此前 compiled SPIR-V reflection、CPU runtime/protocol/retry 及完整回归回执由主线程保留其具体身份与时间。
首轮 282 静态脚本的 275 EXIT0/7 failures 仍是首轮真实记录，不能被后续修正追认成首轮全绿；
当时还有等价性脚本因缺 native binary 主动跳过，EXIT0 不能被算成完整动态回归通过。

本 lane 没有编译 Shader/DLL、启动游戏、部署玩家目录、提交、推送或发布。
生产代码已冻结交回主线程；本文件是最终只读复审的交付物。
