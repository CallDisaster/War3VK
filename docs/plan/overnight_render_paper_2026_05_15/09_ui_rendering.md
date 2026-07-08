# 第 9 章 ★ — UI 渲染分支

> 本章覆盖 War3 的 UI 渲染系统：`uiRenderableRender`、`uiDispatch`、
> CSpriteFrame / CModelFrame / CBackdropFrame 等 UI 元素的渲染数据流。
> UI 渲染与世界渲染共享 GxDevice 和 RenderQueue 基础设施，但在不同的 stage 执行。

## 0. 阅读基线

### 0.1 关键 RVA 锚点

| RVA | 名字 | 角色 |
|---|---|---|
| `0x6F184F00` | `uiRenderableRender` | UI 可渲染元素入口 |
| `0x6F0CAA90` | `uiDispatch` | UI 可见性查询调度 |
| `0x6F0CAAE0` | `uiDispatchOpt` | 优化版 UI 调度 |
| `0x6F0CAB40` | `uiChunkSize` | UI 块大小 |
| `0x6F0CAB80` | `uiListClear` | UI 列表清空 |
| `0x6F0CAE80` | `uiListGetData` | UI 列表取数据 |
| `0x6F0CAE90` | `uiListGetCount` | UI 列表取数量 |
| `0x6F0CB080` | `uiListResize` | UI 列表调整大小 |
| `0x6F0CB110` | `worldObjectListEntryWrite` | 世界对象列表写入 |
| `0x6F0CB480` | `uiListRender` | UI 列表渲染 |
| `0x6F0CB4D1` | `uiFilterAndAppend` | UI 过滤并追加 |

### 0.2 与世界渲染的边界

UI 渲染在 `CWorld_DispatchStage` 的 stage 15/18/21（后半段）执行，
与世界渲染（stage 11/12/13）通过 `RenderQueue_FlushAndReset` 分割。

## 1. UI 渲染架构

### 1.1 UI 元素类型

War3 的 UI 由以下元素组成：

| 类型 | 描述 | 渲染方式 |
|---|---|---|
| `CBackdropFrame` | 背景框/图片 | 2D quad + texture |
| `CSpriteFrame` | 精灵帧 | 2D sprite + animation |
| `CModelFrame` | 3D 模型帧 | 3D model（头像/光标） |
| `CTextFrame` | 文字帧 | 字体渲染 |
| `CStatusBar` | 状态条 | 进度条/生命条 |
| `CSimpleFrame` | 简单容器 | 子元素聚合 |

### 1.2 渲染流程

```
UI 渲染阶段 (DispatchStage 15/18/21)
  └─ uiListRender (0x6F0CB480)
      └─ 遍历 UI 元素列表
          ├─ uiRenderableRender (0x6F184F00)
          │   └─ vtable[3] 调用（具体渲染由子类实现）
          └─ 递归子元素
```

### 1.3 uiRenderableRender

`uiRenderableRender (0x6F184F00)` 是一个极简的 vtable dispatch：

```c
int uiRenderableRender(int thisPtr, int a2, int a3, int a4) {
    return (*(vtable + 3))(a3, a2, a4, 0);
}
```

实际渲染逻辑由各 UI 元素子类的 vtable[3] 实现。

## 2. 项目对 UI 的 Hook

### 2.1 Hook_UiRenderableRender

项目在 `Hook_UiRenderableRender` 中：
1. 追踪 UI 渲染状态
2. 应用 UI 层切换（`PushUiLayer/PopLayer`）
3. 收集 UI 性能统计

### 2.2 FPS 覆盖

`Hook_GetD3d9Parameters` 在 UI 域：
- 可选覆盖 `PresentationInterval`（强制 immediate present）
- 可选覆盖 `GAME_OPTION_REFRESH_RATE`

## 3. 关键数据结构

### 3.1 CSpriteFrame

CSpriteFrame 是 UI 精灵帧的核心类：
- 继承自 CFrame
- 包含纹理引用、动画状态、位置/大小
- 通过 `uiRenderableRender` 的 vtable dispatch 渲染

### 3.2 CModelFrame

CModelFrame 用于 3D UI 元素（如英雄头像）：
- 包含 CModel 实例引用
- 独立的相机/光照设置
- 通过 `uiRenderableRender` 渲染到 UI 层

## 4. IDA rename 清单

| 原名 | 新名 | 地址 |
|---|---|---|
| `sub_6F184F00` | `uiRenderableRender` | `0x6F184F00` |
| `sub_6F0CAA90` | `uiDispatch` | `0x6F0CAA90` |
| `sub_6F0CAAE0` | `uiDispatchOpt` | `0x6F0CAAE0` |
| `sub_6F0CAB40` | `uiChunkSize` | `0x6F0CAB40` |
| `sub_6F0CAB80` | `uiListClear` | `0x6F0CAB80` |
| `sub_6F0CAE80` | `uiListGetData` | `0x6F0CAE80` |
| `sub_6F0CAE90` | `uiListGetCount` | `0x6F0CAE90` |
| `sub_6F0CB080` | `uiListResize` | `0x6F0CB080` |
| `sub_6F0CB110` | `worldObjectListEntryWrite` | `0x6F0CB110` |
| `sub_6F0CB480` | `uiListRender` | `0x6F0CB480` |
| `sub_6F0CB4D1` | `uiFilterAndAppend` | `0x6F0CB4D1` |

---

*本章约 150 行。*
