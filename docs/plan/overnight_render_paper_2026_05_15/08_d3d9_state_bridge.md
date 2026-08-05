# 第 8 章 ★★ — D3D9 State Bridge / GxDevice

> 本章覆盖 War3 的内部图形设备抽象层（GxDevice）和项目如何通过 D3D9 hook
> 接管渲染状态。GxDevice 是 War3 渲染管线和 D3D9 之间的薄桥接层。

## 0. 阅读与证据基线

### 0.1 写作前提

1. 版本基线：`Game.dll @ ImageBase 0x6F000000`（War3 1.27a）。
2. 反编译产物：`AutoTest/artifacts/_overnight_render_research/B_decompile_gx*.txt`。
3. 项目源码：`src/d3d9/d3d9_device.cpp`（D3D9 hook 接管点）。

### 0.2 关键 RVA 锚点速查表

| RVA | 名字 | 角色 |
|---|---|---|
| `0x6F0E34B0` | `gxApplyStateBlock` | 应用渲染状态块 |
| `0x6F0E3550` | `gxDrawCore` | 核心 draw 调用 |
| `0x6F0E3580` | `gxPreparePrimitive` | 准备图元 |
| `0x6F0E3640` | `gxCleanup74` | 清理状态 74 |
| `0x6F0E3670` | `gxCleanup78` | 清理状态 78 |
| `0x6F0E3910` | `gxRenderSceneFlush` | 场景 flush |
| `0x6F0E3950` | `gxUpdateStage` | 更新 texture stage |
| `0x6F0E3A00` | `gxColorSlotWrite` | 写颜色槽 |
| `0x6F0E3580` | `gxBeginColorMix` | 开始颜色混合 |
| `0x6F0E35B0` | `gxEndColorMix` | 结束颜色混合 |
| `0x6FBC5420` | `gx_device` | 全局 GxDevice 单例指针 |

## 1. GxDevice 架构

### 1.1 概念

GxDevice 是 War3 的内部图形设备抽象。它不是 D3D9 的直接封装，而是一个
**状态机驱动的渲染接口**。所有渲染操作都通过 `gx_device_BC5420` 全局单例的
vtable 调用。

### 1.2 GxDevice vtable 结构

```
gx_device_BC5420 vtable:
  +0x00: QueryInterface
  +0x04: AddRef
  +0x08: Release
  ...
  +0x54: SetRenderState        (vtable[21])
  +0x58: SetTextureStageState  (vtable[22])
  +0x5C: SetSamplerState       (vtable[23])
  ...
  +0x6C: DrawPrimitive         (vtable[27])
  +0x70: DrawIndexedPrimitive  (vtable[28])
  ...
  +0x7C: ApplyStateBlock       (vtable[31])
  ...
  +0x98: UpdateStage           (vtable[38])
  ...
  +0xA0: BeginColorMix         (vtable[40])
  +0xA4: EndColorMix           (vtable[41])
  ...
  +0xB0: PreparePrimitive      (vtable[44])
  ...
  +0xBC: Cleanup74             (vtable[47])
  +0xC0: Cleanup78             (vtable[48])
  ...
  +0xCC: RenderSceneFlush      (vtable[51])
```

### 1.3 所有 Gx* 函数都是 vtable wrapper

每个 `gx*` 函数都是对 `gx_device_BC5420` 的 vtable 调用的薄封装：

```c
// gxApplyStateBlock (0x6F0E34B0)
int gxApplyStateBlock(void* stateBlock) {
    return gx_device->vtable[31](gx_device, stateBlock);
}

// gxDrawCore (0x6F0E3550)
int gxDrawCore(int a1, int a2, int a3) {
    ++drawCallCount;  // dword_6FBC5440
    return gx_device->vtable[27](gx_device, a1, a2, a3);
}

// gxUpdateStage (0x6F0E3950)
int gxUpdateStage(int stage, int value) {
    return gx_device->vtable[38](gx_device, stage, value);
}

// gxPreparePrimitive (0x6F0E3580)
int gxPreparePrimitive(int a1, int a2, int a3, int a4, int a5) {
    return gx_device->vtable[44](gx_device, a1, a2, a3, a4, a5);
}
```

## 2. 状态块系统

### 2.1 GxStateBom

War3 使用"状态块"（State Block）来批量设置渲染状态。每个状态块包含：
- Render states（alpha test, depth test, blend mode, etc.）
- Texture stage states（color op, alpha op, texture args）
- Sampler states（filter, address mode）
- Texture bindings

### 2.2 应用流程

```
RenderQueue_Dispatch_Common (0x6F13A5E0)
  ├─ RenderQueue_UpdateItemWorldMatrix
  ├─ 查 dispatch block → 材质/层描述
  ├─ gxApplyStateBlock(stateBlock)  ← 应用状态
  ├─ gxUpdateStage(0, texture0)     ← 绑定纹理
  ├─ gxUpdateStage(1, texture1)     ← 绑定第二层纹理
  ├─ gxPreparePrimitive(...)        ← 准备图元
  └─ gxDrawCore(...)                ← 发出 draw call
```

## 3. 项目的 D3D9 Hook 接管点

### 3.1 接管层级

项目在 D3D9 层（不是 GxDevice 层）做 hook：

```
War3 GxDevice → D3D9 Device → DXVK D3D9 → Vulkan
                 ↑
           项目 hook 在这里
```

### 3.2 关键 hook 点

| D3D9 API | Hook 函数 | 用途 |
|---|---|---|
| `DrawIndexedPrimitive` | `War3TryCaptureShadowCaster` | shadow caster VB capture |
| `DrawPrimitive` | 同上 | 非索引 draw |
| `SetRenderState` | 状态追踪 | alpha test/blend 追踪 |
| `SetTexture` | 纹理追踪 | diffuse texture 追踪 |
| `SetStreamSource` | VB 追踪 | position stream 追踪 |
| `SetIndices` | IB 追踪 | index buffer 追踪 |
| `SetTransform` | 矩阵追踪 | world/view/projection 追踪 |
| `Present` | `BeforePresent` | 帧结束处理 |

### 3.3 状态追踪

项目维护一个 `m_state` 结构来追踪 D3D9 状态：

```cpp
struct D3D9State {
  // Render states
  uint32_t renderStates[256];  // D3DRS_* values

  // Texture stage states
  struct TextureStageState {
    uint32_t colorOp;
    uint32_t colorArg1;
    uint32_t colorArg2;
    uint32_t alphaOp;
    uint32_t alphaArg1;
    uint32_t alphaArg2;
  } textureStageStates[8];

  // Textures
  Com<D3D9Texture2D> textures[8];

  // Vertex/Index buffers
  struct VertexBufferBinding {
    Com<D3D9VertexBuffer> vertexBuffer;
    uint32_t stride;
    uint32_t offset;
  } vertexBuffers[8];
  Com<D3D9IndexBuffer> indexBuffer;

  // Transforms
  Matrix4 worldMatrix;
  Matrix4 viewMatrix;
  Matrix4 projectionMatrix;
};
```

## 4. CGxMat 材质系统

### 4.1 概念

`CGxMat` 是 War3 的材质描述类，包含：
- 纹理引用（diffuse, normal, specular 等）
- 渲染状态（alpha test, blend mode, depth）
- shader 参数

### 4.2 在 RenderQueue 中的角色

每个 `RenderBatchElement` 引用一个 `CGxMat`，在 `Dispatch_Common` 时
通过 `gxApplyStateBlock` 把材质状态应用到 D3D9 device。

## 5. 项目 hook 对 GxDevice 的影响

### 5.1 透明行为

项目的 D3D9 hook 对 War3 的 GxDevice 是**透明的**：
- War3 继续通过 GxDevice vtable 调用 D3D9
- 项目的 hook 在 D3D9 层拦截，不影响 GxDevice 的状态机
- 这是项目架构的关键优势：不需要逆向 GxDevice 的完整状态机

### 5.2 状态覆盖

在某些场景下，项目会覆盖 D3D9 状态：
- Shadow caster pass: 设置自定义 vertex shader + render state
- Receiver pass: 设置自定义 fragment shader + sampler
- Alpha-test promote: 覆盖 `D3DRS_ALPHATESTENABLE` 和 `D3DRS_ALPHAREF`

## 6. IDA rename 清单（本章新增）

| 原名 | 新名 | 地址 |
|---|---|---|
| `sub_6F0E34B0` | `gxApplyStateBlock` | `0x6F0E34B0` |
| `sub_6F0E3550` | `gxDrawCore` | `0x6F0E3550` |
| `sub_6F0E3580` | `gxPreparePrimitive` | `0x6F0E3580` |
| `sub_6F0E3640` | `gxCleanup74` | `0x6F0E3640` |
| `sub_6F0E3670` | `gxCleanup78` | `0x6F0E3670` |
| `sub_6F0E3910` | `gxRenderSceneFlush` | `0x6F0E3910` |
| `sub_6F0E3950` | `gxUpdateStage` | `0x6F0E3950` |
| `sub_6F0E3A00` | `gxColorSlotWrite` | `0x6F0E3A00` |
| `sub_6F0E3580` | `gxBeginColorMix` | `0x6F0E3580` |
| `sub_6F0E35B0` | `gxEndColorMix` | `0x6F0E35B0` |

---

*本章约 300 行。*
