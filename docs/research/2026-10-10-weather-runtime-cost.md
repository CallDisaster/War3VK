# 2026-10-10 天气 CPU 运行时离线成本基准

本批白名单仅 `AutoTest/benchmark_war3_weather_runtime.cpp` 与本记录；不修改生产源码、
共享 Meson、配置、玩家目录或渲染准入。编译和运行由主线程的唯一构建 lease 执行。

## 测量的是什么

基准 include 生产 `war3_weather_runtime.h` 并与实际 `war3_weather_runtime.cpp` 链接，
不复制时间线、锁、风积分或 enabled 判断。宿主计时使用 `std::chrono::steady_clock`，
天气状态使用可控的 injected monotonic fake clock，避免测量时长反过来改变演出输入。

| case | 实际生产调用和状态 |
| --- | --- |
| disabled-hasEnabled | 未创建实例，调用实际 acquire 原子探针；volatile checksum 保留每次返回值。 |
| manual-snapshot-same-clock | 已开启、手动稳定参数、重复相同天气时钟；锁和按值快照存在，风不积分。 |
| manual-snapshot-advance-wind | 手动稳定参数，每次天气时钟 +1/60；真实进行基础风积分。 |
| timeline64-snapshot-advance-wind | 配满64关键帧，强度、方向和速度均变化，每次 +1/60；真实寻找区间、插值和风积分。 |
| paused-snapshot-advance-wall | 已开启并暂停，每次fake墙钟 +1/60；演出时间和风保持不动，仍取锁内快照。 |

另输出 checksum-harness-no-runtime-call，仅执行与 disabled probe 相同的volatile递增；
它用于提示极轻探针的测量装置开销，不冒充生产路径、不偷偷扣减成本，也不作为第六种生产性能。

## 固定工作量和自洽检查

每 case 五轮，每轮构造/设置真实 runtime 后预热10000次，再测200000次；
构造、创建、配置、64项关键帧写入、预热和seek全部在计时/分配窗口之外。
每轮预热后seek到0重建风历史，但fake墙钟保持单调，不把墙钟倒拨到0。
测量一轮只推进3333.333…秒，小于86400秒上限；下一轮重新构造状态，不累计越界。

64关键帧间隔53秒，最后一项3339秒。测量一轮覆盖前62个完整间隔并进入最后渐变区间，
不会因为关键帧耗尽而将测量后半段变成静止hold。参数包含跨360度的风向，生产规范化和最短弧仍使用真实实现。

每轮结束，窗口外检查id/seed/enabled/paused、frame/map/device、演出时间和有限风位移。
手动风按独立恒定向量积分结果检查；64帧渐变按独立终点强度/速度/风角检查且要求风积分非零。
同clock和pause case要求时间/风为0。所有case都有有限非零checksum与固定调用次数检查。

## 统计口径

每1000次调用一个steady-clock计时批，五轮共1000批。输出：

- 每个1000调用批的平均ns/call的median、nearest-rank p95、min/max。
- 五轮各自平均ns/call、五轮均值的median、checksum和计数窗口的heap allocation次数。
- 每轮调用数、预热数和总调用数。

**这里p95是批平均成本的分布，不是单次调用的p95尾延迟，更不是帧耗时或FPS。**
计时结果包含循环、checksum消费、天气fake时钟写入和两次批计时的摊销；
不作baseline扣减，不把开启快照成本与极轻的关闭原子探针直接等同。
基准单线程，无JASS并发写者；不证明作者写入争锁、后台任务干扰、实际GPU耗时或游戏内帧率。
若将结果用于跨版本比较，须保存CPU/编译器/优化级别/位数/电源与后台负载等运行条件。

## 分配窗口与证据边界

构造和设置在窗口外，计时整个窗口重载全局operator new/new[]并统计，要求每轮0。
结果数组均固定内联；排序和printf在计时窗口外。实际生产组件采用mutex，但基准不新增锁。
此计数证明被重载C++默认operator new/new[]可见的分配，未重载C++17 aligned-new，
也不声称拦截CRT malloc、驱动/GPU分配或所有OS内部动作；
生产runtime源码没有malloc路径，但未将本基准当作全进程内存分配审计。

初次源码交付时本 lane 没有编译或运行；以下补充来自主线程取得lease后保存的真实运行，
本 lane 只读核验产物、CPU与编译器信息，没有新增编译。

## 本机实测（主线程 runtime-cost-01）

原始stdout：`AutoTest/artifacts/weather-night-20261010/runtime-cost-01.txt`。
主线程构建exe：`AutoTest/artifacts/weather-night-20261010/weather-runtime-cost.exe`，
SHA-256 `A2591A93184F3F370525C659AD57D85CBA6D5588FF2848405D8827F8761DDE7E`。
本机CPU为AMD Ryzen 5 5600X（6核/12逻辑线程）；clang22.1.8、target i686-w64-windows-gnu，
与当前天气32位LLVM候选同工具链。主线程确认实际构建配方：

```text
i686-w64-mingw32-clang++ -std=c++17 -O2 -DNDEBUG -static -pthread
  AutoTest/benchmark_war3_weather_runtime.cpp
  src/d3d9/war3/render/war3_weather_runtime.cpp
  -o AutoTest/artifacts/weather-night-20261010/weather-runtime-cost.exe
```

以上是同一条命令换行展示；无LTO，构建exit0、运行exit0。父PowerShell进程BelowNormal，
仅在该进程PATH加入LLVM/Vulkan/Python Scripts，exe由同父shell直接运行；
主线程明确本次运行时没有并发编译或GPU测试。未从空build.log臆测这些条件。

| 生产场景 | 批平均中位ns/call | 批平均p95 ns/call | 五轮均值中位ns/call | 每轮测量窗口new |
| --- | ---: | ---: | ---: | ---: |
| disabled hasEnabled | 1.700 | 1.700 | 1.672 | 0 |
| manual same-clock snapshot | 18.700 | 23.200 | 19.128 | 0 |
| manual advance wind snapshot | 189.200 | 204.900 | 192.352 | 0 |
| full64-key advance wind snapshot | 996.500 | 1085.200 | 1002.251 | 0 |
| paused advance wall snapshot | 19.300 | 21.500 | 19.578 | 0 |

装置本身checksum-harness中位/p95均0.500ns/call、五轮均值中位0.480；没有扣减。
每生产case测100万次，五case共500万次，所有25个测量窗口的C++ new计数均0。
含装置case后的自洽断言235项/0失败，最后checksumSink744743.904768；
manual advance每轮checksum380504258.715，timeline64每轮340306081.056，五轮一致。

人话结论仅限本机离线CPU：关闭探针很轻；稳定或暂停快照约几十ns，真实基础风推进约0.2us，
满64关键帧的渐变与解析风积分约1us/快照。单位是每次调用，不是每粒子，也不是整个天气Pass。
这里新分配0与有锁并不矛盾：调用取得已有mutex，并没有创建新的堆对象。

运行未记录CPU频率、电源计划或后台干扰控制，单次批max含调度噪声；
不承诺玩家所有CPU一致，不据此计算FPS收益，也不将它替代生产渲染/GPU的实际成本测量。
