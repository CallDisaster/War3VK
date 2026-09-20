# 三个「门禁外的红」：**逐个诊断**（何处过时 / 是否回归）+ 一个结构性发现 — 2026-09-18（round 74）

> 接续 `2026-09-18-gate-coverage-audit.md`（round 73：门禁外 40 个文件里 3 个红）。
> 本轮把"3 个红"从**未解释**推进到**已诊断**，并坚持一条纪律：
> **先读源码，再下判断**（round 66 的误诊正是因为跳过了这一步）。

## 诊断 1 · `test_gpu_skin_static_snapshot_share_offline.py`

**具名失败**：`FAIL: test_gpu_queue_and_resource_share_snapshot`（failures=3）

该用例对 `war3_gpu_skin_resources.h/.cpp`、`war3_gpu_skin_manager.cpp` 做**字符串契约**断言。逐条实测：

| 断言 | 实际 | |
| --- | --- | --- |
| `count("model::ShadowGeosetResourceSnapshot record;" in header) >= 2` | **1** | ❌ |
| `"findGeosetSnapshotByData" in manager` | true | ✅ |
| `"m_staticMisses.push_back({ key, std::move(record) })" in source` | **false** | ❌ |
| `"resource->record = miss.record" in source` | **false** | ❌ |
| `"model::ShadowGeosetResourceRecord record;" not in header` | true | ✅ |

**分歧点**：`"m_staticMisses.push_back"` 在 `resources.cpp` 与 `manager.cpp` **都是 0 次**；
`"resource->record"` 在 **`manager.cpp` 出现 13 次、`resources.cpp` 0 次**。

⇒ **该机制已从 `gpu_skin_resources.cpp` 搬迁到 `gpu_skin_manager.cpp`**，而测试的字符串锚点未随之更新。
⇒ 三个源文件与测试文件**都与基线逐字相同** ⇒ 搬迁发生在**基线之前**，与本计划无关。

**判定**：测试**过时**（锚点未跟随职责搬迁）。

## 诊断 2 · `test_d3d9_memory_chunk_tail_offline.py`（曾经看起来最"像回归"）

**具名失败**：

```
ERROR: test_cpp_zero_request_cannot_create_an_empty_chunk
FAIL:  test_cpp_uses_exact_exhaustion_not_sub_4k_tail_swallow
  ValueError:    substring not found
  AssertionError: 'if (range->length == 0)' not found in 'D3D9Memory D3D9MemoryChunk::AllocLocked…'
```

测试要求（`test_...tail_offline.py:142-151`）：

```python
self.assertIn("if (range->length == 0)", body)          # 精确耗尽
self.assertNotIn("range->length < (4 << 10)", body)     # 禁止 sub-4K 尾段判断
self.assertNotIn("size += range->length", body)         # 禁止吞尾段
```

而当前源码 `d3d9_mem.cpp:384` **正好**做了后两件被禁止的事，**并带解释性注释**：

```cpp
if (range->length < (4 << 10)) {
  size += range->length;
  m_freeRanges.erase(range);
}
```

### ⚠️ 我先怀疑这是**真实回归**，于是读了函数体 —— 结论相反

`test_cpp_zero_request_cannot_create_an_empty_chunk` 要求 `Alloc` 里有
`if (unlikely(Size == 0))` + `return {};` 且**先于** `new D3D9MemoryChunk`。实测该守卫**不存在**。
但读完整函数体后：

```cpp
64  D3D9Memory D3D9MemoryAllocator::Alloc(uint32_t Size) {
65    std::lock_guard<dxvk::mutex> lock(m_mutex);
67    uint32_t alignedSize = align(Size, CACHE_LINE_SIZE);   // ★ 零请求在此被归一化
69      D3D9Memory memory = chunk->AllocLocked(alignedSize);
```

⇒ **零请求不再需要专门守卫**：`align(0, CACHE_LINE_SIZE)` 把它归一化为一个 cache line，
  **比原来的"提前返回空"更强**（不会产生空 chunk，也不会浪费一次失败路径）。
⇒ 尾段的 sub-4K 吞并同样**带注释**（allocator 最小 4 KiB / payload 记账）。

**判定**：测试**过时**（源码有意演进：零请求 → 对齐归一化；尾段 → 有理由地吞并）。
**不是回归** —— 这是**先读函数体**才得到的结论；若按字面"守卫不见了"就升级，会是本项目第 3 次误诊。

## 诊断 3 · `test_ydhost_adapter.py`

**具名失败**（errors=3，全是 ERROR 不是 FAIL）：

```
ERROR: test_mapdump_generator_is_temporary_by_default_and_sha_bound_when_applied
ERROR: test_default_is_dry_run_apply_is_idempotent_and_drift_is_rejected
ERROR: test_snapshot_copy_detects_source_change_after_audit
  ValueError: expected=D:\tmp\AppData\ADMINI~1\Local\Temp\…  actual=…\Administrator\…
```

⇒ **环境性**：8.3 短路径（`ADMINI~1`）与长路径（`Administrator`）在临时目录上的不匹配。
**判定**：与源码无关；换一个用户/启用 8.3 语义即可能自愈。

## ★ 结构性发现（比三个红本身更重要）

```
门禁内 263 个文件里，提及 d3d9_mem.cpp / AllocLocked / D3D9MemoryChunk 的 = 0
整个 AutoTest 里读取 d3d9_mem.cpp 的只有 2 个文件 —— **都在门禁外**：
    test_d3d9_memory_chunk_census_offline.py   (绿)
    test_d3d9_memory_chunk_tail_offline.py     (红，即诊断 2)
```

⇒ **`d3d9_mem.cpp` 的分配契约（`Alloc` / `AllocLocked`）完全没有门禁覆盖。**
⇒ 本轮的三个红能被发现，纯粹是因为我**手工跑了门禁外的文件**；
  否则它们会继续隐形。

**这意味着什么（以及不意味着什么）**：

- **意味着**：在这条路径上，**未来**的真实回归同样不会被任何门禁抓住；
- **不意味着**：当前有回归（诊断 2 已证明当前两处差异是有意演进）。

## 处置建议（**建议，不是决定**）

1. 三个红**我都不自行修复**：诊断 1/2 需要"更新契约测试的锚点"，那是**该契约所有者**的判断；
   诊断 3 是环境问题，改测试反而会掩盖它；
2. 若要把 `test_d3d9_memory_chunk_*.py` 纳入门禁，**必须先**由所有者更新其锚点，
   否则只是把红搬进门禁（这会退化成"为通过而放宽判据"，被裁定禁止）；
3. 是否把 `d3d9_mem.cpp` 的分配契约纳入门禁 —— **需裁定**。

## 不声称

- **不**声称这 3 个红"与我无关"：我只能证明**文件与基线逐字相同**（⇒ 非本计划造成）；
- **不**声称源码的演进（零请求归一化 / 尾段吞并）**正确**：我只证明它**存在、有注释、且与旧契约的差异是解释得通的**；
  该设计是否最优**不在本计划范围**；
- **不**声称三个红"只有"这三种成因（我诊断的是**当前**失败点）；
- **不**声称 `d3d9_mem.cpp` 无其它覆盖（我只查了 `AutoTest/`；宿主机测试也可能覆盖它 —— **未核对**）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。