# 子线程 A 任务卡 — 剔除层 → 渲染层过渡

## 任务定位
War3 渲染论文的"入口章节"。覆盖：
- `CWorld::FrameUpdate` / 视锥/quadtree 剔除
- 可见对象列表的产生与遍历
- 进入 `RenderQueue` 之前的 stage 分流

## 已知锚点（IDA 已命名）
| 地址 | 名字 |
|---|---|
| `0x6F368480` | `worldFrameUpdateAndPreparePasses` |
| `0x6F3681C0` | `CWorld_RenderScene` |
| `0x6F363020` | `CWorld_DispatchStage` |
| `0x6F368E30` | `CWorld_WorldObjects_RenderGroup` |
| `0x6F184EE0` | `worldObjectEntryRender` |
| `0x6F0CB110` | `worldObjectListEntryWrite` |
| `0x6F368A90` | `sub_6F368A90`（DispatchStage case16 之一） |
| `0x6F367980` | `sub_6F367980`（stage15） |
| `0x6F369560` | `sub_6F369560`（stage16 子分支） |

## 已有研究（不要重写，只增补）
- `docs/research/war3_render_issues/17_cworldframewar3_full_reverse/README.md`
- `docs/research/war3_render_issues/19_blizzard_native_rendering_engine_full_perspective/README.md`

## 必须搞清楚的问题
1. `worldFrameUpdateAndPreparePasses (0x368480)` 内部到底做了什么？是不是包含视锥剔除？
2. War3 的可见对象是按什么数据结构组织的（quadtree / sector / 线性数组）？
3. 哪些函数把"逻辑层对象（CUnit / CWidget / CSprite）"翻译成"渲染层条目（RenderQueue batch）"？
4. `CWorld::DispatchStage` 的 17 个 case 里，**stage 11/12/13** 三个 `WorldObjects_RenderGroup`（不同 group 0/1/2）分别对应什么对象集合？
5. 渲染队列的 group / category 概念是什么？为什么 RenderGroup 调用要传 `0/1/2`？
6. 进入 RenderQueue 之前，对象的"per-draw transform / world matrix"是从哪里取的？

## 输出格式
写到 `docs/plan/overnight_render_paper_2026_05_15/01_visibility_to_renderqueue.md`：

```markdown
# 第 1 章 — 剔除层 → 渲染层过渡

## 1.1 帧前置（FrameUpdate 阶段）
（描述 worldFrameUpdateAndPreparePasses 做的所有工作）

## 1.2 视锥/可见性剔除
（具体函数 + 数据结构 + 算法）

## 1.3 可见对象集分发
（DispatchStage 各 stage 对应什么对象集）

## 1.4 RenderGroup 概念（group 0/1/2）
（具体含义）

## 1.5 worldObjectEntryRender 入队链路
（从 worldObjectListEntryWrite 到 RenderQueue 的最后一步）

## 1.6 IDA rename / set_comments 建议
| 地址 | 建议名 | 注释 |
|---|---|---|
| ... | ... | ... |
```

## 工具
- IDA MCP（HTTP `127.0.0.1:13337/mcp`），用法见 `AutoTest/_ida_call.py`
- 用法示例：
  - `py AutoTest/_ida_call.py decompile 0x6F368480`
  - `py AutoTest/_ida_call.py callees 0x6F368480`
  - `py AutoTest/_ida_call.py xrefs_to 0x6F368480`
  - `py AutoTest/_ida_list_func.py CWorld`

## 不能做的事
- 不动项目源码
- 不启动 War3 / AutoTest
- 不修改现有研究文档（只能新增 01_visibility_to_renderqueue.md）
- IDA rename/comment 不要自己写回，**交给主线程**统一回写

## 完成条件
- 文档至少 800 行 markdown
- 至少 30 处新的函数命名建议
- 给主线程一份 IDA 写回清单
