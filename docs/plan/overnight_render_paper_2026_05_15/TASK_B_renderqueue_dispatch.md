# 子线程 B 任务卡 — RenderQueue 入队 / 排序 / 分发深挖

## 任务定位
渲染论文第 2 章。把 RenderQueue 从"add batch"到"实际 D3D draw call"全链路打开。

## 已知锚点（IDA 已命名）
| 地址 | 名字 |
|---|---|
| `0x6F139190` | `renderQueueAddBatch` |
| `0x6F1375C0` | `renderBatchSubmit` |
| `0x6F137AF0` | `aucTransparentAddEntry` |
| `0x6F138210` | `rqFlushTransparent` |
| `0x6F1380A0` | `flushSortedItems` |
| `0x6F13A5E0` | `dispatchCommon` |
| `0x6F13A780` | `dispatchSpecial` |
| `0x6F138F70` | `applyDrawStateAndDraw` |
| `0x6F13A9B0` | `rqStageUpdate` |
| `0x6F1378B0` | `rqItemComparator` |
| `0x6F0E34B0` | `gxApplyStateBlock` |
| `0x6F0E3640` | `gxCleanup74` |
| `0x6F0E3670` | `gxCleanup78` |
| `0x6F13A0E0` | `rqTransparentDispatchType0` |
| `0x6F198C00` | `rqTransparentDispatchType1` |
| `0x6F19DFF0` | `rqTransparentDispatchType2` |
| `0x6F19BC20` | `rqTransparentDispatchType3` |
| `0x6F13A0B0` | `rqTransparentDispatchType4` |

## 全局数据
| 地址 | 含义 |
|---|---|
| `0xBC6BAC` | `rqNumOfElements` |
| `0xBC6BB0` | `rqBatchArrayPtr` |
| `0xBC6BBC` | `rqNumOfTransparent` |
| `0xBC6BC0` | `rqTransparentArrayBasePtr` |
| `0xBD0828` | `rqTransparentSortedPtrs` |
| `0xBC6BA0` | `rqSortedBatchCount` |
| `0xBC6BE8` | `rqSortedBatchPtrs` |
| `0xBDA4D0` | `rqStateOptEnabled` |
| `0xBDA4D4` | `rqStateCleanupPending` |

## 已有研究（增补，不重写）
- `docs/research/war3_render_issues/20_renderqueue_dispatch_layer_reverse/README.md`

## 必须搞清楚的问题
1. **batch 数据结构**：`RenderBatch` 结构体的所有字段（已知 `meshData / sceneNode / layerState / layerIndex` 等，需要补全）
2. **sort key 含义**：`rqItemComparator (0x1378B0)` 用什么 key 排序 opaque batches？
3. **MeshLayerDispatchRecord 完整字段**：从 `+0x00` 到 `+0x100+` 全部偏移
4. **MeshData 结构**：`+0x0C/+0x10/+0x48/+0x4C/+0x58` 已知，剩下 `+0x14 ~ +0x47, +0x60+` 是什么？
5. **Dispatch_Common vs Dispatch_Special** 的分流条件 + 各自的 multipass / fallback 链
6. **transparent 5 种 dispatch type**（type0~4）分别处理什么类型的透明对象？
7. **state block apply 机制**：`gxApplyStateBlock` 怎么避免重复设置状态？
8. **RenderQueue 的 takeover 接口**：项目自己的 native renderer 接管 RenderQueue 是怎么工作的（看 `src/d3d9/war3/native/`）

## 输出格式
写到 `docs/plan/overnight_render_paper_2026_05_15/02_renderqueue_dispatch.md`：

```
# 第 2 章 — RenderQueue 完整数据流

## 2.1 入队接口（AddBatch / AucTransparent）
## 2.2 数据结构
### 2.2.1 RenderBatch 字段表
### 2.2.2 MeshData 字段表（完整版）
### 2.2.3 MeshLayerDispatchRecord 字段表
### 2.2.4 SceneNode 关键字段
## 2.3 排序与 flush
## 2.4 Dispatch_Common 实现
## 2.5 Dispatch_Special 实现 + multipass fallback
## 2.6 Transparent 5 种 dispatch type
## 2.7 state block apply 与 stage update
## 2.8 项目 native renderer takeover 接管点
## 2.9 IDA rename / set_comments 建议
```

## 工具
- IDA MCP 同前
- 项目代码：`src/d3d9/war3/reimpl/war3_render_queue.{h,cpp}` —— **只读**，不要修改

## 不能做的事
- 不动源码
- 不启动游戏
- IDA 写回交给主线程

## 完成条件
- 文档至少 1000 行
- MeshData / RenderBatch / DispatchRecord 三个结构字段表必须**完整**
- 至少 40 处新命名建议
