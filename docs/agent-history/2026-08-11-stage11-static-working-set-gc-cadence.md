# Stage11 静态工作集与 GC 周期闭合候选

2026-08-11 的第二轮十分钟“生与死”低视角压力记录没有出现 device lost、
Arena overflow、busy reuse 或 allocation failure，但 14,243 个观察帧中有 511 帧
producer incomplete。记录到 15,859 次 position allocation budget defer；同一轮
draw-time static cache 共驱逐 13,070 个条目、约 6.39 GiB。九个连续采样点出现
`planned=590, validated=0, drawn=0`，拒绝原因为 `ProducerIncomplete`，随后恢复。

源码中 GC 每 60 帧运行一次，而静态工作集只保护最近 16 帧。仍在场景中的树木和
静态 caster 可能跨一次原生 world-producer 间隔被归为 inactive 并被 LRU 淘汰，随后
重建又受每帧 32 次分配安全门限制。该组合可解释整批 fail-closed 与逐步恢复，而不
需要假设显存不足。

本候选只做一个策略修正：把 GC 周期定义为共享常量 60 帧，静态工作集保护窗固定为
两个 GC 周期。64 MiB 仍是非活跃静态缓存目标；每帧 32 次分配门、Arena 上限、epoch、
fence 和 replay fail-closed 均不改变。保护集合超过目标时仍使用既有 over-cap 诊断，
非保护条目仍按确定性 LRU 回收。

离线合同只能证明 GC 不会在保护边界内淘汰条目。候选仍需使用相同地图、相同 300 秒
出生区停留与低视角巡航重新测量 producer incomplete、position defer、驱逐字节、整批
零 draw 窗口和 device-lost 事件，不能据此宣称视觉闪烁或 TDR 已解决。
