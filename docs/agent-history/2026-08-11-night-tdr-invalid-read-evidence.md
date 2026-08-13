# 2026-08-11 夜间 TDR：invalid-read 证据与生产者诊断闭环

## 范围与结论边界

本记录只保存一次真实高压运行的证据和随后完成的离线诊断增强。它不证明 TDR 已修复，也不授权继续触发设备丢失。

当晚允许的一次自然 TDR 已发生，因此后续停止所有 Warcraft 实机运行，并恢复夜间开始前的稳定 Release DLL。没有更改系统 TDR 配置。

## 可重复运行与证据

- 场景：`life_and_death_tdr`，DirectInline、4096 CSM、5×5 低视角巡航。
- 运行时长：约 481 秒；采集 845 份 runtime 状态、15 张截图和 99 个 waypoint。
- 证据目录：`AutoTest/artifacts/life_and_death_tdr/20260811_034309`。
- Windows 在 03:49:45 与 03:49:47 记录两次 `nvlddmkm` Event 153。
- device-fault enrichment 返回 `VK_INCOMPLETE`，但提供两个有效地址项：
  - `VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT`：`0x200096c00`；
  - `VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT`：`0x1ea04000`，precision 4096。

这证明驱动观察到 GPU 无效读取/页错误；它仍不能单独把地址归属到某个 WarVK buffer。Khronos 的 device-fault 示例建议结合 `VK_EXT_device_address_binding_report` 将 fault address 与绑定/解绑历史关联，因此该扩展是下一项开发构建诊断，而不是 Release 默认功能。

## TDR 前 240 帧

- 点阴影光源数为零，本次故障不应归因于点阴影算法。
- Arena 驻留约 960 MiB，低于 1.125 GiB 总上限；没有 overflow、busy reuse、partial transaction 或 quarantine 违规。
- 多帧方向光 candidate 为 `ProducerIncomplete`：planned 约 783–909，validated/drawn 为零。
- candidate 恢复后，单帧 868、809、684 等 caster 被分别重放到 C0–C3，868 caster 即 3472 个 cascade draw。
- Arena 累计 position/terrain copy 达 TB 级，说明地形与位置源仍在被大量重复冻结/复制；这不是内存上限溢出，但会放大在途资源和生命周期压力。

旧报告已显示观察到的 3779 个 required omission 全部同时计入 `exactBudgetDeferredUniqueCasterCount`。高压运行中又出现约 1253 个连续 `ProducerIncomplete` 帧，因此固定的 Stage11 每帧 32 次分配准入仍是首要生产缺口；但原 incident 没有携带 position/UV/index/real-allocation/fallback/Arena/freeze 的独立原因，不能继续靠总数猜测。

## Terrain Observe 的重要否证

`cc08287` 增加了 generation-backed terrain bounds provenance，但实机中 proof accepted 仍为零。源码复核发现 indexed-domain 计算仍位于历史上被强制关闭的 exact-trim 路径内；该 trim 曾因动态 REAL position 与 CPU-readable IB 代际不一致触发 NVIDIA reset，不能为获得 bounds 而重新开启。

因此这次 TDR 并不是“剔除已生效后仍然失败”：实际 applied cull 为零。下一版安全观察若继续做 bounds，应使用 generation 已验证的保守完整 VB bounds，或先取得可靠的同代 index domain；不能恢复旧 trim。

## 本阶段离线改动

将以下 scene-owned 值贯通到 runtime status、GPU flight frame、device-lost incident 与性能汇总：

- producer seal 的 frame/map/device epoch；
- required omission 与 unique exact-budget-deferred caster；
- position、mandatory UV、index 分配节流；
- real allocation failure、fallback byte budget、Arena admission、freeze failure；
- completeness reason/sealed/counter-overflow；
- Stage11 static cache live/protected/over-cap/eviction；
- indexed draw unknown-range fallback。

该改动只增加有界数值复制和现有低频 JSON 字段，不改变 CSM、Arena、replay、点阴影、剔除、资源生命周期或发布默认值。

## 官方与只读实现参考

- Khronos [VK_EXT_device_fault proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_device_fault.html)
- Khronos [Device fault sample](https://docs.vulkan.org/samples/latest/samples/extensions/device_fault/README.html)
- Microsoft [Cascaded Shadow Maps](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps)
- 官方 Unreal Engine `release` / `71fe36aac5a8`，只读 sparse checkout：`E:/Mycode/Source/References/UnrealEngine`

Unreal Engine 仅作为架构交叉检查：场景拥有的 bounds、Vulkan device-fault 与开发态 address-binding report 的启用边界。没有复制其源码、shader、资源或专有实现。

## 下一步

1. 首先用本阶段新增字段在下一次获授权物理门中区分 32 次 Stage11 准入、真实分配失败、fallback/Arena 和 cache 工作集。
2. 设计默认关闭的开发构建 `VK_EXT_device_address_binding_report` 环形记录器，把 fault 地址关联到 buffer/image bind/unbind；Release 不启用。
3. 以数据审计 unknown-index full-VB replay validation，尤其是正 `BaseVertexIndex` 是否超出已验证 position range；在覆盖率未知前不直接收紧或放宽。
4. Terrain bounds 先补可到达的安全 provenance，再做 Observe；不得把本次 zero-proof 运行描述为剔除验证。
