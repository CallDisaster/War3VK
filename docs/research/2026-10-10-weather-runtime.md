# 2026-10-10 天气生产 CPU runtime

本 lane 只修改工作单 runtime 白名单。主线程拥有共享接线、构建与实机 lease。

## 交付

- `src/d3d9/war3/render/war3_weather_runtime.h/.cpp`
- `AutoTest/test_war3_weather_runtime.cpp`

固定单实例、64 项内联关键帧，所有 author state 在同一个 mutex 下修改和求值；
不保存游戏指针，不建立 GPU 所有权。按值快照携带独立 frame/map/device 域、作者 revision、
演出秒数、连续风位移、完整区域及强度参数。主线程在帧输入冻结一次，各 consumer 消费同值。

`hasEnabled()` 是 acquire 原子 scheduling hint，未创建/关闭时主路径无需取得 runtime mutex；
实际参数仍以锁内快照为准。创建默认关闭，区域默认 XY ±4096、Z 0..2048、羽化 0.1，
作者可通过 SetWeatherRegion 改成场地的明确范围。

## 时间与控制

- 可注入 `double(*)() noexcept` 单调秒；生产默认 `steady_clock`。
- create/disable/pause 期间不累加演出时间或风位移；恢复采样新墙钟，不补算冻结期间。
- 时间上限为 86400 秒；到达上限后保持状态，不再增长风位移。
- 非有限或倒退的测试时钟不污染状态、不重复积分；这是防御行为，不是音频同步证明。
- SetTarget 在求出当前状态后切手动过渡，所有参数从同一起点插值。
- 时间线第一个关键帧必须为 0，后续严格递增；播放或暂停时不允许编辑。
  Disable 后允许 Clear/Add，第一次成功编辑会停止旧时间线并保持当前外观；Enable 本身不会
  自动播放改过的时间线，作者需要显式 Play。Disable 后无编辑则原位续播。
  无效参数、时间顺序或容量失败不改变控制模式。末尾保持最后状态。
- Play 自动开启并解除暂停。Play/Seek 重建风历史；Seek 在手动模式保持当前外观并取消旧过渡，
  在时间线模式按新时间求值。
- 方向规范化到 [0,360)，沿最短弧插值。180 度歧义固定取正方向。

## 风与身份

风位移是随时间对速度向量的积分，不是当前风速乘总演出时间。每个时间线段/手动过渡
按线性风速和最短角度求解析积分；跨关键帧分别结算。小角度用级数避免消差。
改变风速/方向保留既有位移。阵风扰动仍由天气 Shader 从同一时间和 seed 求值；
CPU 积分只承载基础风，consumer 不得把历史位移按当前风向重新旋转。

正整数 id 进程内单调分配，destroy/reset 不复用；耗尽 int32 正数域后 create 返回 0。
因此 reset 后迟到的旧 id 命令无法作用于新实例。此规则只证明本 runtime 的身份隔离，
不替代 JAPI pending mailbox 的地图 epoch 检查，也不替代 GPU 退休。

revision 只在作者修改、启停、reset、seek 时增加，帧推进不增加；它不是历史重建标识或 GPU fence。
额外 `historyRevision` 只在 create/play/seek/destroy/reset 增加，普通 SetTarget/Region/启停不增加，
用于 Froxel history 的不连续失效。historyRevision 不会因地图 reset 归零或回绕；耗尽后 create/play/seek
返回失败，destroy/reset 仍清空实例，之后无法新建或 Seek，避免复用历史身份。
render 不应因普通配置 revision 改变而整体重置雪花。GPU 帧可通过 id/map/device 域拒绝旧输入。

## 测例及证据边界

生产组件测试使用实际 `.cpp`，覆盖空/过期 id、单实例容量、关键帧 64/65、NaN/Inf、范围、
手动和时间线暂停/停用/恢复、Seek、末尾保持、最短角度、连续风、采样频率差异、reset 旧命令、
按值队列保留、零堆分配窗口，以及 2 万写/2 万读的相关字段并发自洽。
第三批新增 Disable 编辑与无编辑恢复、Pause 拒绝、无效请求不切 mode，以及 historyRevision 生命周期。

解析积分还使用独立圆弧积分结果对照，不仅测试实现自身两个分支。
构建/运行由主线程执行；本 lane 截至初次交付未编译、未运行测试、未启动游戏。
`git diff --check` 返回 0（文件为新增 untracked，该命令不是编译或测试证明）。

historyRevision 的 UINT64_MAX 分支由源码审查说明，当前测例未迭代到 2^64 次或提供测试专用 setter。
待主线程填入实际编译/测试结果，之后再考虑 GPU、音频同步、视觉与性能验收。
