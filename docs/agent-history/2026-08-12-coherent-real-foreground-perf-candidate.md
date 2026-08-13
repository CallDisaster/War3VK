# Coherent REAL 前台性能候选

本候选是独立的开发构建，不改变默认 Release。它只默认启用此前完成隔离
A/B 的 coherent REAL 地形范围裁剪、generation-backed exact-domain cache 与
D3D9 声明访问域；这三项仍要求同一 draw、rigid/opaque terrain、当前可读
position/index span、完整 map/device/identity/allocation/content generation 和
最终 replay range 验证。

以下已知危险或未准入路线继续关闭：Stage11 direct static source、current-UP
replay、联合消费者/terrain bounds Consume、Persistent Package Consume、RTS
shadow candidate 与 ReBAR。候选支持通过环境变量把 trim mode、domain cache
或 declared-domain 单独退回 Off，便于使用同一 DLL 做前台 A/B。

隔离证据曾测得 declared-domain 将 `ResourceResolve` 从约 0.69 ms/frame 降到
约 0.085 ms/frame，并维持 producer-incomplete、device lost 和新 GPU 事件为
零；这些数字不能替代本轮玩家前台 FPS 与画面验收。

## 前台否决结果

玩家在相同地图连续采集了三份不同 DLL 的前台报告。自动开启本路线的候选
`F5374A10...EFD6A` 仅为 `74.131 FPS`，默认关闭路线的同源 DLL
`ADC7616E...17DCF` 为 `82.323 FPS`，此前部署候选 `EA3D6E63...203D8`
为 `92.202 FPS`。实验候选的 caster 与 geometry work 反而更低，但
`ShadowCapture/PostGate` 从默认 DLL 的 `0.218 ms` 增加到 `1.997 ms`，
`Stage01` 从 `0.424 ms` 增加到 `1.959 ms`。它把平均 Arena 从约
`36.1 MiB` 降到 `13.2 MiB`，代价却是约 `1.5--1.8 ms/frame` 的额外
CPU 证明与冻结准备。

因此该路线不再作为 FPS 候选，专用开发构建也恢复运行时默认 Off。源码仍保留
显式环境变量 opt-in，供未来 Arena 超预算/TDR 压力场景验证；在新的前台
A/B 证明净收益前不得转为 Release 默认或自动开启。
