# 2026-09-20 S2 热路径成本与接口收口（b03 最终合同）

> 状态：本 lane 源码已按 b03 冻结，纯 Python/static 门禁通过；等待联合编译。
> 未编译、未部署、未实机、未 GPU 验证。b01/b02 仅作为判退历史保留在文末。

## 1. 现行 primary API

```cpp
bool TryBuildRuntimeGroupPaletteKernel(
    const RuntimeGroupPaletteInput& in,
    RuntimeGroupPaletteFallbackSet fallbackSet,
    std::vector<Matrix4>& outPalette,
    uint32_t& outMaxVertexGroupSlot,
    bool& outUsesAveraging,
    RuntimeGroupPaletteKernelDetail* outDetail = nullptr);
```

- 直接借用调用方 outPalette；不再有局部 RuntimeGroupPaletteOutput + move。
- 内核固定 256 项 seen/输出数组单趟求 maxSlot + unique；不接受 caller-provided trusted max。
- core 来源链仍需先 FindRuntimeGroupPaletteMaxSlot 一趟，core 总计 2 趟；upper 内核单趟。
- 旧 RuntimeGroupPaletteOutput& 包装保留，只做字段转发，计算仍唯一实现。

## 2. 扫描/分配表

| 路径 | 扫描趟数 | scan 堆分配 | palette 分配 |
| --- | ---: | --- | --- |
| core 来源链 max | 1 | 0 | 0 |
| core 内核计算 | 1（固定扫描） | 0 | 0（容量足够时复用 caller） |
| core 合计 | 2 | 0 | 0 额外分配 |
| upper 内核 | 1（固定扫描） | 0 | 0（容量足够时复用 caller） |
| core logFailure 诊断 | 仅 FallbacksFailed 且达到日志门时 1 趟 | 1 个诊断 vector | 0 |

注：public ScanRuntimeGroupPaletteSlots 测试包装为了返回 vector 会转换/分配，不计入生产内核；
输出容量足够时连续调用应保持 data()/capacity() 不变。

## 3. 别名合同

- 生产非别名：core 的 outPalette 是 resolveRecord 内的局部 runtimeGroupPalette，
  posePalette 来自 ShadowPoseRecord.matrixPalette（不同对象）。
- 生产非别名：upper 的 outPalette 是 UpperLayerShadowResolvedItem::runtimeGroupPalette，
  posePalette 来自 UpperLayerShadowResolvedItem::pose.matrixPalette（同一结构体的两个不同 vector）。
- 共享接口防御：ClassifyRuntimeGroupPaletteOutputPoseAlias 在 clear/resize 前，
  把 output capacity 与 posePalette 视图转成 byte 半开区间 [begin, begin+len)，
  用 ClassifyRuntimeGroupPaletteByteRangeOverlap 分类。
- 关系严格分 NonOverlapping / Overlapping / Invalid：
  * 零长度 -> NonOverlapping；
  * 首尾相接 end==begin -> NonOverlapping；
  * 真正有交集 -> Overlapping，fail-closed，detail.aliasedInputOutput=true；
  * 起点+长度溢出、元素数*元素大小溢出 -> Invalid，同样 fail-closed，detail.aliasRelation=Invalid。
- 不构造 out.data()+capacity、不构造 poseEnd 指针；只转 uintptr_t 做整数半开区间判断，不读取矩阵。

## 4. 源锚点

- core: src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp
  - resolveRecord 内 local std::vector<Matrix4> runtimeGroupPalette（约 :8386）
  - kernelInput.posePalette = pose.matrixPalette.data()（约 :7184）
  - 调用 TryBuildRuntimeGroupPalette(..., runtimeGroupPalette, ...) 的 6 处语句使用同一局部 out 与各自 pose 记录
- upper: src/d3d9/war3/render/war3_upper_layer_shadow.h
  - struct UpperLayerShadowResolvedItem（:14）
  - model::PoseRecord pose（:17）
  - std::vector<Matrix4> runtimeGroupPalette（:23）
- upper: src/d3d9/war3/render/war3_upper_layer_shadow.cpp 的 TryBuildRuntimeGroupPalette
  直接把 out.runtimeGroupPalette 与 out.pose 交给内核，两个字段不别名。

## 5. 测试口径与反例

- 内核测试：T1-T17 行为合同；T18 保留 input posePalette/output storage 别名拒绝，
  并增加 capacity>size/clear 后仍有 capacity 的 output storage 反例；T19 纯整数半开区间边界。
- T19 覆盖：双方相邻（左/右）、内部子区间、同起点、零长度、首尾相接、
  极限地址可表示端、起点+长度溢出、元素数乘法溢出。
- 差分/成本测试：核心 300010 + upper 300005 = 600015 输入，固定种子；
  alias 硬化与旧有效输入等价分开报告；成本断言要求连续调用 data()/capacity() 不变。
- 本 lane 只做静态，不宣称 C++ PASS；差分与内核测试交 validation 编译执行。

## 6. 当前验证状态

- py AutoTest/test_runtime_group_palette_kernel_single_source_static.py -> EXIT 0
- git diff --check -- <白名单文件> -> EXIT 0
- 未执行：编译、Meson、ninja、部署、实机、GPU。

## 7. b01/b02 判退历史摘要

- b01 的 caller-provided precomputedMaxVertexGroupSlot 裸指针已判退；不要重新引入。
- b02 的 std::less 指针区间比较已判退：end==begin 误判重叠，且 data()+capacity 端点指针不合法。
- 现行合同只有本文第 1-5 节；b01 的旧 API 与 b02 的 bool alias helper 不再有效。

## 8. 待联合编译目标

- meson test -C build32 war3_runtime_group_palette_kernel（T1-T19）
- meson test -C build32 war3_runtime_group_palette_kernel_diff（600015 输入）
- ninja -C build32 src/d3d9/d3d9.dll（BelowNormal + -j2）与 ninja -n
