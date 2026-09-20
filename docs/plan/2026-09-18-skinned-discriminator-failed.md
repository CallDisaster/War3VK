# ④ 判别尝试**失败**：我用错了数组（第五次"先确认在看正确的地方"）— 2026-09-18

## 1. 我想做什么

④ 的最后一个未做动作是判别：**本场景有没有 skinned draw？**
（有 ⇒ 指向产出方问题；无 ⇒ 场景不含蒙皮对象，需换场景。）

## 2. 我做了什么、以及为什么它不成立

我在 `d3d9_device.cpp:21525-21532` 读到 `inputEvidenceProvenance` 的构造，
其第 9 号（0 基）元素是 `uint64_t(skinned)`；于是我去统计 run B（`CONTRACT=0`）导出里
`batches[].draws[].provenance[9]`。

结果：

```
draws=23459   provenance-short=0
pv[0] : {'2': 23459}        <-- 全部是 "2"
pv[9] : {'0': 23459}        <-- 全部是 "0"
sample: ['2','0','65536','2506','0','2506','0','0','0','0','0','0','0','0','0','0']
```

## 3. 为什么**不能**据此说"无 skinned draw"

`draw.inputEvidenceProvenance` 的构造（`d3d9_device.cpp:21525`）**首元素是 `1u`**：

```cpp
draw.inputEvidenceProvenance = {1u, uint64_t(...runtimeModelPtr), 0u, 0u, ...};
```

而导出里 `pv[0]` **全部是 `2`**。

⇒ **`r.provenance`（`war3_frame_inputs.cpp:314` 迭代的那个）与 `inputEvidenceProvenance` 不是同一个数组。**
⇒ 我套用的索引映射**没有根据**。
⇒ `pv[9]=0` **不能**解释为 `skinned=0`。

**因此本轮没有判别出 ④ 的分叉。**

## 4. 我为什么不"先按这个结果记下来"

如果我把 `pv[9]=0` 当成 `skinned=0`，就会得出
「本场景不含蒙皮对象 ⇒ ④ 需换场景」—— **一个看起来合理、但基于错误索引的结论**。

这类结论最危险：它**可以被后续一切观察"证实"**（因为换场景后大概率确实还是看不到），
从而把一个"我没查对"变成一条"项目事实"。

⇒ **宁可记为"未判别"，不记为"判别的结果"。**

## 5. 正确的下一步（明确）

```
找到 war3_frame_inputs.cpp 里 r.provenance 的**构造处**（不是 inputEvidenceProvenance），
读出它与 skinned 的对应关系（或找到另一个**有明确语义**的 skinned 标记字段），
再统计。

另一条更省事的路：draws 里是否已有语义明确的字段能区分蒙皮（例如 meshPayload / words /
或 provenance 里某个已知含义的槽）。先读构造，再统计 —— 不要先猜索引。
```

## 6. 这是同一类教训的第几次

| # | 实例 |
| --- | --- |
| 1 | 在 `label` 里找 `skin-selection`（实际在 inputs 导出的结构化 schema 里） |
| 2 | 文件名截断显示，把 `AutoTest/artifacts/...` 副本当成 `src/` 文件 |
| 3 | 误读 `glob.paths[0]`（那是 artifacts 副本，不是 src） |
| 4 | 在 `war3_palette_object_evidence.h` 与 `.cpp` 之间搞错记录器是头文件 |
| 5 | **本轮：套用 `inputEvidenceProvenance` 的索引去读 `r.provenance`** |

⇒ 共同形态：**在"读到的数据"与"我假设的语义"之间没有验证，就直接下结论。**

防御办法（本轮之后再想清楚的一条）：**任何跨文件的索引/字段映射，必须先在代码里看到
"写入点与读取点是同一个数组"的证据，再用来统计。**

## 7. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读分析；临时脚本已删除）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```