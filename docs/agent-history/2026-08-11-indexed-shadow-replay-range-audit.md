# Indexed shadow replay 范围审计

## 结论

2026-08-11 的真实 device fault 给出了 `READ_INVALID 0x1ea04000`（precision 4096），但没有报告
该地址所属的 Vulkan 对象。本阶段没有修改 indexed replay：现有证据不足以在“范围错误”和“资源已退役后
仍被 GPU 使用”之间作确定选择，直接收紧会让大量 CPU-opaque IB 的 required caster 被拒绝。

## 已确认的合同

- D3D9 `DrawIndexedPrimitive` 的真实顶点地址为 index-buffer 值加 `BaseVertexIndex`；
  `MinVertexIndex/NumVertices` 描述调用允许引用的范围。
- Vulkan `vkCmdDrawIndexed` 同样以 `index + vertexOffset` 生成实际顶点编号。
- Exact Stage11 在 IB 可安全 CPU 读取时扫描真实 index domain，检查
  `BaseVertexIndex + [actualMin, actualMax]` 是否落在当前 VB，并复制紧凑范围。
- IB 为 CPU-opaque 时，Stage11 不信任 Warcraft 已知不稳定的 Min/Num hint，而是冻结本次完整 IB
  range、复制完整有界 VB，并保留原 `BaseVertexIndex`。这与主 draw 的地址元组一致。
- 最终 validator 对已知真实 index domain 会验证实际偏移范围；但对
  `fullVertexDomainFallback && !actualIndexDomainKnown` 只验证完整 attribute backing 和 IB byte range，
  不把 `vertexOffset` 应用于 `[0, vertexCount)`。因此该分支是保守 replay，却不是独立的真实 index-domain
  证明。

参考：

- <https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitive>
- <https://registry.khronos.org/VulkanSC/specs/1.0-extensions/man/html/vkCmdDrawIndexed.html>

## 为什么本阶段不直接改 validator

把 D3D9 Min/Num hint 当成真实索引域会恢复此前已经证伪的错误裁剪；把所有 unknown-domain、非零
BaseVertexIndex draw 一律拒绝，则会把合法的 device-local/WRITE_COMBINED IB required caster 标成
producer incomplete。在高压场景现有每帧分配节流已经会造成阴影候选缺失，额外扩大拒绝面会掩盖
TDR 根因并重新制造周期性无阴影。

旧的 indexed VB trim 也不能重新启用：它曾把动态 REAL position generation 与 CPU 侧 IB 证据错误
配对，并出现过 NVIDIA reset。正确修复必须取得与被冻结 IB 同一 source generation 的真实 domain，
或在 GPU 上完成一次有界 index-domain 归约/重写后再发布，不能依靠历史 hint 或裸地址。

## 下一次物理门的判定方式

独立开发 DLL 的 `VK_EXT_device_address_binding_report` 会把 device-fault 地址与最近的 bind/unbind 事件
关联：

1. 命中最近 unbind 或已退役 allocation：修复 last-use fence/retirement；
2. 命中仍绑定的 vertex/index buffer 且地址越出其报告区间：修复 full-domain replay/range proof；
3. 命中 image/device memory：转查 CSM receiver、Arena backing 或资源 publication；
4. 无匹配但 ring 未 drop/wrap：保留未知结论，不能据此改 indexed replay。

当前唯一自然 TDR 配额已经使用，开发 DLL没有部署。本文件是源码审计结论，不是稳定性或 TDR 修复
声明。
