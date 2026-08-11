# Stage11 有界分页快照候选（2026-08-12）

## 现象与边界

开发观察构建在高压低视角场景记录到大量动态 CurrentDraw position
请求。旧路径为每个 position、独立 UV 和 IB 分别创建 device-local
`DxvkBuffer`；每帧第 32 次创建以后，必需 Caster 会被标记为 producer
incomplete，方向光接收器随后正确地拒绝整份候选。这能避免部分阴影图，
但会表现为全体 Caster 周期性消失。

本阶段没有提高 32 次门、Arena 容量或宽限期，也没有恢复跨帧
fingerprint。它只把多个精确逻辑 slice 放入少量 16 MiB device-local 页，
使旧门限制的是实际 Vulkan 页创建而不是逻辑 Caster 数量。

## 生命周期合同

- 每个 cache entry 分别持有 position/UV/index 所在页的 `shared_ptr`、逻辑
  offset 和容量；copy、slice info 与最终 logical replay binding 使用同一 offset。
- 页内只做 256-byte 对齐的单调 bump，不回收洞、不覆盖旧 slice。
- 只有页不再被任何 live/retired cache entry 引用时，页查找表才移除它。
  即使此时旧 command list 仍持有 `Rc<DxvkBuffer>`，后续也会创建全新的页，
  不会复用原物理范围。
- 地图或设备 epoch 迁移时，cache 先进入 fence-owned retired session，随后
  清空当前页查找表；新 epoch 不会继续从旧页分配。
- 活跃页总容量硬限制为 384 MiB；失败继续进入既有 producer completeness
  fail-closed，不扩大 Arena 的 384 MiB/代际和 1.125 GiB 总上限。
- 首轮 4 MiB 候选在“生与死”隔离 smoke 中把 required omission 从 3685
  降到 274，但 3 个压力帧仍因页创建数超过 32 而拒绝 225 个 position
  snapshot。16 MiB 页让完整 384 MiB 代际最多创建 24 个真实 Vulkan
  buffer，在不放宽 32 次门和驻留上限的情况下闭合该峰值。

## 隔离 smoke 证据

2026-08-12 使用同一地图、120 秒、2×2 低视角巡航比较：

| 指标 | 4 MiB 页 | 16 MiB 页 |
| --- | ---: | ---: |
| 观察帧 | 5229 | 5322 |
| producer incomplete 帧 | 3 | 0 |
| required caster omission | 274 | 0 |
| position page-create budget reject | 225 | 0 |
| fallback byte-budget reject | 49 | 0 |
| Vulkan 页创建 | 187 | 60 |
| Arena 峰值 | 383.848 MiB | 375.587 MiB |

两轮均未产生 `VK_ERROR_DEVICE_LOST`、Event 153/4101、Arena overflow 或
ownership violation，AutoTest 结束后部署 DLL 按 SHA-256 精确恢复。该证据只
证明隔离桌面的资源/完整性门；内部 framebuffer 不能代替玩家前台对细影稳定性
和绝对 FPS 的肉眼验收。terrain fallback 仍约 61 万次，Arena 平均仍约
51 MiB，后续减负不能把这次 omission 归零误写成 Persistent Package 已完成。

## 诊断与验证

新增运行时/性能字段报告页驻留、已用、创建、逻辑子分配、回收、容量拒绝
和真实分配失败。纯数值 runnable 覆盖对齐、页容量、越界、单调 offset 与
硬上限；静态合同锁定 copy/slice offset、地图 reset 顺序和“无 cache lease
才移除页”。

定向 Python、producer completeness、replay validation、Arena lifecycle/
budget、logical binding 与 Win32 runnable 已通过，DLL 增量构建及 no-work
通过。本阶段仍是未部署候选；必须通过隔离桌面高压场景证明 producer
omission 归零、页驻留不单调增长且无 TDR，才可成为阶段基线。
