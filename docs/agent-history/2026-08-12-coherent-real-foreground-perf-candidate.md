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
