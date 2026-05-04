# Current Status

Date: 2026-05-02 (19:30)

## 1. Executive Summary

**动态阴影修复已实现**：修复了 `tryEngineDirectPosePalette` 函数，直接使用引擎已经构建好的调色板。

## 2. 问题分析

### 2.1 根本原因

`CModel + 0x60` 只有 2-3 个根骨骼矩阵，而非完整骨架的 35+ 个矩阵。当 `matrixIndex >= pose.matrixCount` 时，触发 `MatrixIndexOutOfRange`，fallback 到 `buildUniformPosePalette`，把第一个矩阵广播到所有组，导致初始姿态。

### 2.2 修复方案

直接使用引擎已经构建好的调色板，而不是自己构建。

引擎在 `CModel_AllocAndFillGroupPalette` (0x6F12FED0) 中为每个 RenderablePart 分配了调色板槽位，并将槽位索引存储在 `renderablePart + 0x08`。调色板数据存储在全局缓冲区 `dword_6FBC6BD0 + 48 * slotIndex`。

## 3. 代码改动

### 3.1 war3_shadow_renderer_core.cpp

1. 新增 `kGlobalPaletteBufferRva = 0xBC6BD0`（全局调色板缓冲区基址的 RVA）
2. 修改 `tryEngineDirectPosePalette` 函数：
   - 修复了读取全局调色板缓冲区的方式
   - 直接使用 `gameDllBase + kGlobalPaletteBufferRva` 作为基址
   - 不再通过 `SafeReadU32Fast` 解引用
   - 增加了边界检查和内存可读性检查

## 4. 编译状态

- ✅ `ninja -C build32` 通过

## 5. 下一步

1. 运行测试验证动态阴影是否正确
2. 检查阴影姿态是否随动画变化
3. 更新 automation_exchange 文档

## 6. 更新日志

- 2026-05-02 19:30: 修复 tryEngineDirectPosePalette 函数，直接使用引擎调色板
