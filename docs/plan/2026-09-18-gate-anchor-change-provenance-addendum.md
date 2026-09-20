# 门禁锚点更新的 provenance 补强（2026-09-18，主线程补记）

> 目的：`⑤ Registry domain 隔离` 第一块更新了两个既有静态门禁的锚点，但**其 pre-change SHA-256 未被捕获**（任务开始前它们已是 working-tree modified）。本文件把**当前可实测的事实**与**护栏强度不变的证据**补记归档，并如实声明不可恢复的部分。
> 性质：**纯文档补记**。未改动任何源码或门禁（写本文件前后 `ninja -C build32 -n` 均为 no work）。

## 1. 两个文件的可实测现状（主线程实测）

| 文件 | 字节 | SHA-256（当前，最终树） | mtime |
| --- | --- | --- | --- |
| `AutoTest/test_bridge_ramp_shadow_safety_static.py` | 40,466 | `FC7D2A8D9A5B474E71FF590C1D5BDE5E3A15F5F82DF950F6103ED485F97D71F8` | 2026-09-18 05:30:20 |
| `AutoTest/test_shadow_lifecycle_tombstone_static.py` | 5,311 | `9DADC368672F95A64FB9FDA27B92E4853171BFC6FA9209406A11822F91BE6507` | 2026-09-18 05:31:34 |

## 2. 现锚点原文（主线程实测）

### `test_bridge_ramp_shadow_safety_static.py`

```
846: kStage13ReferencedContentTag =
847:       dxvk::war3::shadow::ShadowGeometryDomainTag(
848:           dxvk::war3::shadow::ShadowGeometryDomain::Stage13Exact)
```

**变更性质**：原先直接钉字符串 `kStage13ReferencedContentTag = 0x53314301u`；现改为**经由 domain 头解析**同一取值（`ShadowGeometryDomain::Stage13Exact`）。**引脚强度不降**：取值仍被钉死，且新增的 `test_registry_domain_isolation_static.py` 从另一侧再钉一次（含「裸常数已从设备源消失」断言）。

### `test_shadow_lifecycle_tombstone_static.py`

```
116: # 2026-09-18 T7/U5：Stage13 清理由无条件 clear() 改为 domain 作用域的
118: # Stage13 retention，但不再可能顺手清掉别的 domain 的条目。
120: self.assertNotIn( m_war3Stage13RetainedCasters.clear() , DEVICE_CPP)
127: dxvk::war3::shadow::ShadowGeometryDomain::Stage13Exact, 0u,
130: self.assertIn( m_war3S1TerrainCasterStash.clear() , DEVICE_CPP)
```

**变更性质**：原锚点为 `m_war3Stage13RetainedCasters.clear()` 在源码中**存在**；现为**不存在**（`assertNotIn`）+ 新增钉住 domain 作用域清理调用形态，并**保留**对 S1 stash `clear()` 的正向断言。**这是加严而非放宽**：断言条数增加，且同时约束「旧形态消失」与「新形态存在」。

## 3. 反向替换式（如需回到变更前风格）

- bridge/ramp：把 `ShadowGeometryDomainTag(…::Stage13Exact)` 表达式还原为字面量 `0x53314301u`（取值相同）。
- tombstone：把第 120 行的断言还原为对 `clear()` 的**正向**断言，并移除第 127 行 domain 形态断言。
> 反向替换式**不能**恢复字节级一致（措辞与注释不同），仅用于语义回退。

## 4. 不可恢复的部分（如实声明）

- **pre-change SHA-256 无法取得**：这两个文件在本轮任务开始前**已是 working-tree modified**，`git HEAD` 不是其 pre-change 状态，工作区也没有更早的备份（子代理的 `%TEMP%` 备份目录仅覆盖 5 个实现文件）。**因此这两处锚点变更没有字节级 provenance。**
- 可用的替代证据（语义级）：①变更当时全量静态为 **254/254**、变更后 **255/255 → 256/256** 全绿；②两文件的断言**只增不减**（上节逐条列出）；③取值由 `war3_shadow_geometry_domain.h` 与新域门禁**双重钉死**。

## 5. 结论

- 护栏强度：**不变或更强**（断言增加、取值双钉）。
- provenance 完备性：**仍不完整**（缺 pre-change 字节），已在此归档，供后续审计引用；若日后要完全闭合，需在**改动前**把这类既有门禁纳入快照集合——这是本夜可固化的流程改进项。

## 6. 本轮未改动任何源码门禁的确认

写本文件前后：`ninja -C build32 -n` = no work；B 树 DLL = 36,283,128 B / `519AFA69…`；现场 DLL = `A0A51AF2…`（未替换）。
