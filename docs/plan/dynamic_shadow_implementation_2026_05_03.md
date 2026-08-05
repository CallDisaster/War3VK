# 动态阴影实现阶段文档

Date: 2026-05-03

## 阶段概述

本阶段实现了动态阴影的连贯显示，解决了阴影始终是初始姿态的问题。

## 问题分析

### 根本原因

1. **`CModel + 0x60` 只有 2-3 个根骨骼矩阵**，而非完整骨架的 35+ 个矩阵
2. **引擎使用 `CGeosetData_BuildGroupBlendedPalette` (0x6F12E600) 从全局矩阵池构建完整的调色板**
3. **完整的调色板存储在全局缓冲区 `dword_6FBC6BD0 + 48 * slotIndex`**

### 问题链条

1. 我们的代码从 `CModel + 0x60` 读取 pose，但这里只有根骨骼的 2-3 个矩阵
2. 当 `matrixIndex >= pose.matrixCount` 时，触发 `MatrixIndexOutOfRange`
3. fallback 到 `buildUniformPosePalette`，把第一个矩阵广播到所有组
4. 结果：所有顶点都用同一个根骨骼矩阵变换，看起来就是初始姿态

## 解决方案

### 方案1：直接使用引擎的调色板缓冲区

从 `dword_6FBC6BD0 + 48 * slotIndex` 读取调色板，而不是自己构建。

### 方案2：添加调色板槽位索引缓存

解决 `RenderablePart + 0x08` 在某些帧没有被更新的问题。

### 方案3：使用 `DecodeRuntimePoseMatrix48` 解析矩阵

正确处理 3x4 矩阵格式。

## 关键代码改动

### 文件：`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`

1. **添加全局调色板缓冲区基址常量**：
```cpp
constexpr uintptr_t kGlobalPaletteBufferRva = 0xBC6BD0;
```

2. **添加调色板槽位索引缓存**：
```cpp
struct PaletteSlotCacheEntry {
  void* renderablePart = nullptr;
  uint32_t paletteSlotIndex = 0xFFFFFFFF;
  uint64_t lastUpdateFrame = 0;
};
static constexpr size_t kMaxPaletteSlotCacheEntries = 4096;
static thread_local PaletteSlotCacheEntry s_paletteSlotCache[kMaxPaletteSlotCacheEntries];
```

3. **实现 `FindOrUpdatePaletteSlotCache` 函数**：
```cpp
static uint32_t FindOrUpdatePaletteSlotCache(void* renderablePart, uint32_t currentSlotIndex) {
  // 查找缓存
  for (size_t i = 0; i < kMaxPaletteSlotCacheEntries; ++i) {
    if (s_paletteSlotCache[i].renderablePart == renderablePart) {
      if (currentSlotIndex != 0xFFFFFFFF && currentSlotIndex < 0x3A98) {
        s_paletteSlotCache[i].paletteSlotIndex = currentSlotIndex;
        return currentSlotIndex;
      } else {
        return s_paletteSlotCache[i].paletteSlotIndex;
      }
    }
  }
  // 添加新的缓存条目
  if (currentSlotIndex != 0xFFFFFFFF && currentSlotIndex < 0x3A98) {
    const size_t cacheSlot = s_paletteSlotCacheIndex % kMaxPaletteSlotCacheEntries;
    s_paletteSlotCache[cacheSlot].renderablePart = renderablePart;
    s_paletteSlotCache[cacheSlot].paletteSlotIndex = currentSlotIndex;
    s_paletteSlotCacheIndex++;
    return currentSlotIndex;
  }
  return 0xFFFFFFFF;
}
```

4. **修改 `tryEngineDirectPosePalette` 函数**：
```cpp
auto tryEngineDirectPosePalette = [&]() -> bool {
    // 读取调色板槽位索引
    uint32_t paletteSlotIndex = 0xFFFFFFFF;
    if (renderable.renderablePart != nullptr) {
      dxvk::war3::SafeReadU32Fast(
          renderable.renderablePart,
          dxvk::war3::RenderablePartFieldOffsets::PaletteSlotIndex,
          paletteSlotIndex);
    }
    // 使用缓存机制
    paletteSlotIndex = FindOrUpdatePaletteSlotCache(
        renderable.renderablePart, paletteSlotIndex);
    
    if (paletteSlotIndex == 0xFFFFFFFF || paletteSlotIndex >= 0x3A98)
      return false;

    // 读取全局调色板缓冲区
    uintptr_t gameDllBase = reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
    if (gameDllBase == 0) return false;

    void* globalPaletteBufferPtr = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(
            reinterpret_cast<const void*>(gameDllBase + kGlobalPaletteBufferRva),
            0, globalPaletteBufferPtr) || globalPaletteBufferPtr == nullptr) {
      return false;
    }
    const uintptr_t globalPaletteBufferBase = reinterpret_cast<uintptr_t>(globalPaletteBufferPtr);
    
    const uint32_t requiredCount = outMaxVertexGroupSlot + 1u;
    if (requiredCount == 0 || requiredCount > 256) return false;
    
    // 使用 DecodeRuntimePoseMatrix48 解析 3x4 矩阵
    const uint8_t* enginePalette = reinterpret_cast<const uint8_t*>(globalPaletteBufferBase + 48u * paletteSlotIndex);
    
    if (!dxvk::war3::IsReadableRange(enginePalette, requiredCount * 48u))
      return false;
    
    outPalette.resize(requiredCount);
    for (uint32_t i = 0; i < requiredCount; ++i) {
      outPalette[i] = DecodeRuntimePoseMatrix48(enginePalette + i * 48u);
    }
    
    outUsesAveraging = false; 
    return true;
};
```

## IDA 逆向验证

### 关键函数

| 函数 | RVA | 用途 |
|---|---|---|
| `CGeosetData_BuildGroupBlendedPalette` | 0x6F12E600 | 构建混合调色板 |
| `CMatrixGroup_BlendOutputMatrix` | 0x6F12E200 | 执行矩阵混合 |
| `CModel_AllocAndFillGroupPalette` | 0x6F12FED0 | 分配调色板槽位 |
| `sub_6F138FF0` | 0x6F138FF0 | 分配调色板槽位索引 |
| `sub_6F139060` | 0x6F139060 | 获取调色板数据地址 |

### 关键全局变量

| 变量 | 地址 | 用途 |
|---|---|---|
| `dword_6FBC6BD0` | Game.dll + 0xBC6BD0 | 全局调色板缓冲区基址 |
| `dword_6FBDA4C8` | Game.dll + 0xBDA4C8 | 调色板槽位偏移 |
| `dword_6FBDA4CC` | Game.dll + 0xBDA4CC | 帧标识 |

## 当前状态

### 已实现

- ✅ 动态阴影姿态：阴影现在可以随模型动画变化
- ✅ 地面单位阴影：步兵、骑士等地面单位的阴影正确显示
- ✅ 阴影位置跟随：阴影位置跟着 caster 移动

### 待解决

- ❌ 刷新率低：阴影刷新率约 0.5HZ（每2秒刷新一次）
- ❌ 飞行单位阴影：飞行单位没有阴影
- ❌ 建筑阴影：建筑没有阴影
- ❌ 可破坏物阴影：可破坏物没有阴影
- ❌ 装饰物阴影：装饰物没有阴影

## 下一步计划

1. **提高刷新率**：调查为什么阴影刷新率只有 0.5HZ
2. **添加飞行单位阴影**：调查飞行单位为什么没有阴影
3. **添加建筑阴影**：调查建筑为什么没有阴影
4. **添加可破坏物阴影**：实现可破坏物的阴影渲染
5. **添加装饰物阴影**：实现装饰物的阴影渲染
