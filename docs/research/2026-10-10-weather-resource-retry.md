# 2026-10-10 天气资源失败重试门

本批仅新增生产组件、实际组件测例和本记录。主线程拥有渲染接线、构建与实机 lease；
本 lane 不编辑正在编译的 renderer/runtime/API、共享 Meson 或玩家目录。

## 接口与职责

`src/d3d9/war3/render/war3_weather_resource_retry.h` 提供：

- `WeatherResourceRetryKey`（同时为 `WeatherResourceRetry::Key`）：完整 64 位 mapEpoch/deviceEpoch，
  32 位 width/height/colorFormat/depthFormat。格式仅是数值，不引入 Vulkan 头依赖。
- `bool ready(const Key&, uint64_t frameSerial) noexcept`。
- `void failed(const Key&, uint64_t frameSerial) noexcept`。
- `void clear() noexcept`。

固定单槽、纯按值、无分配/锁/环境变量。由单一渲染所有者线程使用，不提供跨线程同步、
GPU 所有权、设备恢复或资源退休证明。主线程接线必须继续使用已有 DXVK 资源跟踪。

没有失败时可立即创建。相同 key 失败后，距离最后失败业务帧至少 120 帧才允许再试；
真实失败重启等待，同一帧重复失败不重复延长。成功创建后调用 `clear()`。
key 不含天气强度、作者 id/revision 或当前帧，普通演出参数变化不能绕开重试等待。
只有 map/device/输出大小/格式变化才会使不同资源需求立即获准尝试。

这是单槽而非失败历史表：`ready()` 看到不同 key 时丢弃旧失败记录；切回旧 key 被视为新尝试。
这用于避免每帧重建同一个失败资源，不保证多个资源 key 交替切换时的全局创建频率。

## 回退与整数边界

先要求当前帧不小于失败帧，再以减法判断差值，不计算 `failureFrame + 120`。
因此帧号回退和 uint64 回绕不会通过溢出的期限或下溢的差值绕开等待。
同 key 的 backward `failed()` 保留较大旧失败帧，不缩短已记等待。

靠近 UINT64_MAX 时，若剩余可表示帧不足 120，该 key 会一直拒绝到 clear 或资源 key 改变；
这属于刻意 fail-closed，不把回绕伪装成经过 120 个有效业务帧。
全零 key 和失败帧 0 都是可测试正常值，不能充当“尚未失败”的哨兵。

## 主线程接线顺序

1. 构造资源创建输入 key，在创建前调用 `ready()`；false 时只报告退避，不调用创建 API。
2. 资源创建失败或异常捕获时调用同 key、同业务帧的 `failed()`。
3. 创建完整资源成功后 `clear()`，不能在部分资源成功时提前清空失败。
4. 地图/设备资源失效仍按现有 owner 和 retirement 处理，本组件不额外释放资源。

需要确保依赖资源创建失败也走该门；“组件测例正确”不证明生产 renderer 已正确接线。

## 测例与当前证据

`AutoTest/test_war3_weather_resource_retry.cpp` 直接 include 生产头，不复制算法。
测试覆盖 delta 0/119/120、同帧 repeated failed/ready、六种 key 变化及 64 位 epoch 高位、
新 key 的低帧号、全零 key、rollback、UINT64_MAX 临界及回绕、clear、按值副本独立性。
10000 次实际组件循环使用全局 operator new 计数窗口，要求构造/ready/failed/clear 零堆分配。
编译期还要求 trivially-copyable、standard-layout、noexcept 及组件大小不超过 64 B。

本 lane 截至源码交付没有执行编译、测例或游戏。测试通过与生产接线由主线程实际验证后记录；
本记录不声称资源失败已在实机复现、重试成本已测量或 Vulkan 资源已正确退休。
