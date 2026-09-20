# 2026-09-18 — T7 批次 4（U5）Registry domain 隔离：显式 domain 类型 + owner-check 实施记录

> **状态：本块"可离线验证的部分"已实施；F3（绑定真实帧的数值判据）本轮未执行。**
> 本文**不得**被引用为"Registry domain 隔离已完成"、"Stage13 已就绪"或
> "palette 侧职责已迁完"。一次可声称"Registry domain 隔离完成"必须同时满足
> 勘察文档 §4 的 F1–F4；本轮的达成/未达成逐条见 §7。
>
> 范围：`docs/plan/2026-09-18-t7-registry-stage13-survey.md`「审计批次 4（U5）先行」
> 中的 **domain 隔离部分**（不含批次 5/6，不含 Stage13 content-persistent）。
> 观测基线文档：`docs/plan/2026-09-16-stage13-port-audit.md`。
>
> 执行裁定（主线程，逐条落实）：
> 1. domain 形态取**显式字段（option a）**，取值沿用既有隐式 tag（S1 = 0x53310001、
>    Stage13 = 0x53314301），把隐式表达改为显式表达，**语义不变**；
> 2. **必须加 owner-check**：publish / GC / reset 路径触碰条目前校验 domain 归属，
>    跨 domain 命中一律拒绝（fail-closed）；
> 3. **不改任何既有准入/发布语义**、不加新 env、不改 Stage13 准入默认值。

---

## 0. 结论摘要（每条都可在本树复核）

1. **显式 domain 已落地**：新增轻量头 `src/d3d9/war3/shadow/war3_shadow_geometry_domain.h`
   （`enum class ShadowGeometryDomain`、`ShadowGeometryDomainTag`、
   `DecideShadowGeometryOwner` / `ShadowGeometryOwnerAccepts`），生产与宿主机测试共用
   同一份判定内核；registry 条目 / 常驻条目 / Stage13 常驻条目都带显式 `domain` 字段。
2. **owner-check 有 5 个落点**（lookup / publish / GC 两处 / reset + 2 处 domain 作用域
   clear），全部 fail-closed，且**不触碰**越权槽位、不改变被淘汰条目的字节账与退役语义。
3. **既有 key 材料逐位不变**：S1 tag 与 Stage13 tag 现在由显式 domain 取值给出，
   数值与混入顺序不变；宿主机测试用**生产同一份 FNV-1a** 对两条链做了 golden 相等断言
   （`C3.s1-layout-hash-unchanged` / `C3.stage13-source-hash-unchanged` /
   `C3.stage13-layout-hash-unchanged`）。
4. **测试**：新增宿主机边界测试 41/41（meson target `war3_shadow_geometry_domain`）、
   新增静态门禁 13 tests、新增离线域生命周期模型 5 tests（12000 帧随机等价 +
   每帧跨域探测 + F4.10 反例）。
5. **两条变异真跑**：①取消跨域校验 → 构建通过但 16/41 宿主用例红 + 静态门禁红；
   ②两个 domain 取值写成相同 → **编译器 duplicate case value 直接红**，另跑 2B
   （去掉编译期防线）→ 22/41 红 + 静态门禁红。两次还原后 SHA MATCH + touch 真实重编译 + 复绿。
6. **全门禁在最终树上取数**：ninja -n no work、真实重编译 exit 0、预算门禁
   （冻结 157 / 165→112 / 当前 112 / 已迁出 46 / 站点 567）、七条等价门禁全绿、
   meson **84/84**、全量静态 **255/255**、记录器 23/23、成本 PASS(38/38)、
   生命周期 187/187、解析器 94/94、根读方 55/55、往返 CERTIFIED、DLL 身份见 §4.3。
7. **未达成**：F3（真实帧数值判据，本树无 pre-U5 基线快照 ⇒ 无法执行）、
   domain 维度的**字节/条目分账**（D4/D5 未动）、key 级 domain（形态 (b) 未采纳，D1 未闭合）。

---

## 1. domain 类型与取值（形态 (a)）

新增文件 `src/d3d9/war3/shadow/war3_shadow_geometry_domain.h`
（4,029 B / SHA-256 `215443C92054A97534E6B2FDE20B8791AAB035D4748B141E6FE1BAE7F9C8161E`）：

| 符号 | 内容 |
| --- | --- |
| `enum class ShadowGeometryDomain : uint32_t` | `Generic = 0u` / `S1Terrain = 0x53310001u` / `Stage13Exact = 0x53314301u` |
| `constexpr uint32_t ShadowGeometryDomainTag(domain)` | 隐式 tag 的**唯一**来源；调用点不得再写裸常数 |
| `constexpr uint32_t kShadowGeometryDomainCount = 3u` / `ShadowGeometryDomainIndex` | 分账维度与索引（显式 switch 使"两个取值相同"成为编译错误） |
| `enum class ShadowGeometryOwnerDecision` | `Accept` / `DomainMismatch` / `GeometryMismatch` |
| `DecideShadowGeometryOwner(expectedDomain, expectedGeometryId, entryDomain, entryGeometryId)` | 纯判定；domain 不一致优先于 geometryId 不一致 |
| `ShadowGeometryOwnerAccepts(...)` | 同判定的布尔包装 |

取值语义（**数值逐位不变**，只是从隐式改为显式）：

| domain | 取值 | 既有隐式表达（改动前） | 归属的 registry 调用方 |
| --- | --- | --- | --- |
| `Generic` | `0u` | 无 tag（`key.mode`/hash 输入自身区分） | Semantic append、UpperLayer、主 world capture 的非 S1 分支 |
| `S1Terrain` | `0x53310001u` | `layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(0x53310001u)); // S1 tag` | `s1TerrainPersistentPath` 的主 world capture |
| `Stage13Exact` | `0x53314301u` | `constexpr uint32_t kStage13ReferencedContentTag = 0x53314301u;` | `m_war3Stage13RetainedCasters`（Stage13 常驻表，**独立容器**） |

**容器字段**（形态 (a)：domain 加在条目，不改 key 的 `operator==`/`hash`）：

| 结构 | 位置 | 新增 |
| --- | --- | --- |
| `War3ShadowGeometryRegistryEntry` | `d3d9_device.h` | `ShadowGeometryDomain domain = Generic;` |
| `War3ShadowPersistentGeometryEntry` | `d3d9_device.h` | `ShadowGeometryDomain domain = Generic;`（与槽位同源写入，供 GC / 退役反查） |
| `War3Stage13RetainedCasterEntry` | `d3d9_device.h` | `ShadowGeometryDomain domain = Stage13Exact;` |

**三个 registry 入口都携带 domain**（`d3d9_device.h` 声明 + `d3d9_device.cpp` 定义）：
`War3TryFindShadowPersistentGeometry` / `War3FindOrCreateShadowPersistentGeometry` /
`War3CreateShadowPersistentGeometryAfterMiss` 的第一个形参都是
`dxvk::war3::shadow::ShadowGeometryDomain domain`。**调用点**（静态门禁钉死站点数）：

| 调用点 | domain | 站点数（含定义） |
| --- | --- | --- |
| Semantic append 的 cache probe / miss-create | `Generic` | 各 1 |
| UpperLayer `FindOrCreate` | `Generic` | 1 |
| 主 world capture lookup / miss-create | `s1TerrainPersistentPath ? S1Terrain : Generic`（同一局部 `geometryDomain` 变量） | 各 1 |
| 三个入口的成员定义 | — | 共 3 |

调用点合计 **8 处**（`War3TryFindShadowPersistentGeometry` 4 = 3 个真实调用 + `War3FindOrCreateShadowPersistentGeometry`
内部转发 1；`War3FindOrCreateShadowPersistentGeometry` 1 = UpperLayer；`War3CreateShadowPersistentGeometryAfterMiss`
3 = 2 个真实调用 + `War3FindOrCreateShadowPersistentGeometry` 内部转发 1）。
门禁 `FROZEN_ENTRY_SITES` 冻结的是**含定义在内的出现次数**（5 / 2 / 4 = 11），
并逐个断言实参文本含 `domain` ⇒ 新增/删除调用点会立刻变红。

---

## 2. owner-check 落点逐条

全部为 **fail-closed**：拒绝时**不修改**对方的槽位 / instances / lastSeen / 字节账 / 常驻条目。

| # | 路径 | 落点 | 判定 | 拒绝时的行为 | 计数 |
| --- | --- | --- | --- | --- | --- |
| 1 | **lookup（命中）** | `War3TryFindShadowPersistentGeometry`（slot 找到后） | `DecideShadowGeometryOwner(domain, 0, slot.domain, slot.geometryId)` | 不 `instances++`、不刷新 `lastSeen`、不擦除、不把几何交给调用方，直接 `return false` | `domainLookupRejects` |
| 2 | lookup（槽位域已验，再验常驻条目域） | 同上（`geomIt` 找到后） | `ShadowGeometryOwnerAccepts(domain, slot.geometryId, entry.domain, slot.geometryId)` | 既不暴露几何，也不销毁任何一方 | `domainLookupRejects` |
| 3 | **publish** | `War3CreateShadowPersistentGeometryAfterMiss`（`createAttempts` 记账之后、`War3GcShadowPersistentGeometry()` 与任何 GPU 分配之前） | `DecideShadowGeometryOwner(domain, 0, existingSlot.domain, existingSlot.geometryId)` | 返回 `War3ShadowPersistentCreateFailure::DomainConflict`；不覆盖槽位、不建 buffer、不加字节账、不跑 GC | `domainPublishRejects`（helper 层，覆盖所有调用方）+ `rejectDomainConflict`（ShadowCapture 调用方的失败分桶，与 `rejectCapacity` 同构） |
| 4 | **GC（过期队列）** | `War3GcShadowPersistentGeometry` 的 `eraseOwnedRegistrySlot` lambda，过期循环调用 | `DecideShadowGeometryOwner(entry.domain, token.geometryId, slot.domain, slot.geometryId)` | 只跳过**槽位擦除**；常驻条目、S1 别名、`m_war3ShadowPersistentBytesUsed` 减账、`bytesEvicted` 与隔离前完全一致 | `domainGcEraseRejects` |
| 5 | **GC（预算回收）** | 同上 lambda，`evictGeometry` 调用 | 同上 | 同上 | `domainGcEraseRejects` |
| 6 | **reset（整会话退役）** | `War3ResetShadowSessionState` 的容器 `std::move` **之前** | 每个槽位与它指向的常驻条目做 domain + geometryId 归属校验 | 不一致的槽位是"无主别名"：先丢弃并计数，**不**把它当作有效归属带进 retired 记录；常驻条目与字节账照旧一起退役（不能选择性拒绝退役，否则 GPU 资源失去 fence 所有权） | `domainResetOwnerRejects` |
| 7 | **domain 作用域 clear（不可用相机的地图切换）** | `BeforeUi` 的 Stage13 stale purge | `ShadowGeometryOwnerAccepts(Stage13Exact, 0, entry.domain, 0)` | 只清 Stage13Exact；非本 domain 条目保留 | `domainResetPurgeRejects` |
| 8 | **domain 作用域 clear（Stage13 tombstone）** | `War3DrainShadowCasterTombstones` 的 `producerStage == 13` 分支 | 同上 | 同上；`retiredAnything` 仍无条件置 true（与隔离前相同） | `domainResetPurgeRejects` |

未被 owner-check 覆盖、且**行为与隔离前逐字节相同**的路径（已在记录中列出以免误读为漏改）：
lookup 的 stale-slot 清理（槽位指向的常驻条目已不存在时擦除槽位 + S1 别名；
domain 归属已在同一函数内先验过）、`War3EraseS1TerrainEarlyAliasesForPersistentGeometry`
（按 geometryId 反查，结构上只能命中该 geometry 自己的别名）、Stage13 的 cap 淘汰
（`std::min_element` + `erase` 在容器内自身迭代，结构上不可能跨 domain）。

**计数落点**：`War3ShadowPersistentDiagnosticsFrame` 新增
`rejectDomainConflict` / `domainLookupRejects` / `domainPublishRejects` /
`domainGcEraseRejects` / `domainResetPurgeRejects` / `domainResetOwnerRejects`
（每 Present 区间；稳态应为 0，非零即证明发生过越权触碰）。

---

## 3. provenance

### 3.1 逐文件 pre / post 身份与 %TEMP% 备份

捕获时间：**2026-09-18T05:15:58+08:00**；备份目录 `%TEMP%\registry_domain_pre\`
（= `D:\tmp\AppData\ADMINI~1\Local\Temp\registry_domain_pre\`）。
所有备份在写入后立即回读 SHA-256，与源文件比对 `match=True`。

| 文件 | pre bytes / SHA-256 | post bytes / SHA-256 | %TEMP% 备份（名） |
| --- | --- | --- | --- |
| `src/d3d9/d3d9_device.h` | 134,216 / `E6F97012AFCC3003C572DEC5F63F0EE90264810BE9B92292D602D3DEDE9F0AE3` | 137,213 / `D4E15762E915A8F49E27CA4AB505A50CE74AF1866659514E315C66F3201374F5` | `src__d3d9__d3d9_device.h` |
| `src/d3d9/d3d9_device.cpp` | 2,248,277 / `4E82651458826BBAB4BE3536893AADDFACF253C5694849F9528309133C2165FE` | 2,256,632 / `60CA6BCD170DF4022D58917A018E3594303E1564ADB10BEF93AF39CB9371F547` | `src__d3d9__d3d9_device.cpp` |
| `src/d3d9/meson.build` | 56,994 / `B225BD44E07026A8D0BC236E5AACEEF91FA0518FB3B4D0BF11E9FBE77EF806BF` | 58,040 / `D7996BFA77EA059643334239ED4AE8482CA55D9553DBCED38D4342C963244529` | `src__d3d9__meson.build` |
| `src/d3d9/war3/shadow/war3_shadow_geometry_domain.h` | **不存在**（pre-change 实测 `exists=False`） | 4,029 / `215443C92054A97534E6B2FDE20B8791AAB035D4748B141E6FE1BAE7F9C8161E` | — |
| `src/d3d9/war3/render/tests/war3_shadow_geometry_domain_test.cpp` | **不存在** | 21,323 / `FAF525DDED23C6E00026E6285B740AB5D50E08CCF8F9E55DDF20C8422E41A812` | — |
| `AutoTest/test_registry_domain_isolation_static.py` | **不存在** | 17,064 / `9095B915131835A00B742970BA262DE6985339B2E8E0FE62FDED88EF6C8260B8` | — |
| `AutoTest/test_registry_domain_model.py` | **不存在** | 18,797 / `674E1CDD94020319944CE545692D3322F64A7F5B21F70A77E6F8D4559362DEC9` | — |
| `run_ninja_registry_domain.cmd` | **不存在** | 1,010 / `49515416202E03B3A96DAE8B7242D6F5F24226B06ACD69B3945AB0DFB970B433` | — |
| `docs/agent-history/DEVELOPMENT_CHANGELOG.md` | 320,187 / `5D91EBF9CC43BD6B95EA3D7A49B82FA92CA32613500D483D04A36D35C0BA80B1` | 324,829 / `7E85DE517D525153AE6209E38B9A970BF5710430AEA01BB9465027D3121969F0` | `docs__agent-history__DEVELOPMENT_CHANGELOG.md` |
| `docs/plan/2026-09-18-overnight-progress-log.md` | 59,604 / `ABF0A01C9F8670AB6810D80E047B171612279771537C992BFCC0A1148B20751B` | 63,811 / `CD5A29E878FEB7A9A7E21160BF0FC050EFDA0AE7892B0AF288CADAC56C986954` | `docs__plan__2026-09-18-overnight-progress-log.md` |

### 3.2 ⚠️ provenance 缺口（如实记录，不掩盖）

两个**既有静态门禁**在本轮被更新锚点，但**它们不在初始快照集合内**
（我最初把"要改的文件"限定为生产源 + 新文件，直到门禁变红才发现必须改锚点）：

| 文件 | pre-change SHA-256 | post SHA-256 | 状态 |
| --- | --- | --- | --- |
| `AutoTest/test_bridge_ramp_shadow_safety_static.py` | **未捕获** | 40,466 / `FC7D2A8D9A5B474E71FF590C1D5BDE5E3A15F5F82DF950F6103ED485F97D71F8` | 该文件在本任务开始前已是 working-tree modified（`M`），git HEAD 不是它的 pre-change 状态 |
| `AutoTest/test_shadow_lifecycle_tombstone_static.py` | **未捕获** | 5,311 / `9DADC368672F95A64FB9FDA27B92E4853171BFC6FA9209406A11822F91BE6507` | 同上 |

反向重建：两处改动都是**单点文本替换**，pre-change 文本可由 post-change 反向替换得到：

1. `test_bridge_ramp_shadow_safety_static.py`：把
   `"kStage13ReferencedContentTag =\n      dxvk::war3::shadow::ShadowGeometryDomainTag(\n          dxvk::war3::shadow::ShadowGeometryDomain::Stage13Exact)"`
   换回 `"kStage13ReferencedContentTag = 0x53314301u"`（并删掉新增的三行注释）。
2. `test_shadow_lifecycle_tombstone_static.py`：把新增的 domain 作用域断言块
   换回原两行 `self.assertIn("m_war3Stage13RetainedCasters.clear()", DEVICE_CPP)` +
   `self.assertIn("m_war3S1TerrainCasterStash.clear()", DEVICE_CPP)`。

pre-change 的**语义**证据：`docs/plan/2026-09-18-a9-a10-full-gate-rerun.log` §12/§30 记录
同一工作树在 2026-09-18T04:58 的全量静态为 `STATIC_TOTAL=254 STATIC_FAIL=0`，
两个门禁当时都在通过集合内（本轮的 255 = 254 + 新增门禁）。

**锚点更新是否削弱护栏**：没有。两处都把"裸常数"改成"常数必须来自显式 domain 取值"，
数值本身由 `war3_shadow_geometry_domain.h` 与新增门禁
`AutoTest/test_registry_domain_isolation_static.py` 双重钉死（F1-2 的两种形态之一：
"被显式 domain 取代后断言常数消失"）。

### 3.3 反向重建其它依据

- 生产改动全部由 `edit` 工具的 literal 替换完成，替换对（old → new）逐条落在本记录 §1/§2
  与静态门禁断言里；`device.cpp` 的 domain 相关断言在门禁里逐符号钉死。
- 变更前后 `ninja -C build32 -n` 两次都是 `no work to do`（§4.1/§4.2），
  说明最终树与最终 DLL 是 source-consistent 的。
- **未做任何 git 写**：`git` 只被用于只读查询（`status`/`rev-parse`/`show`）。

### 3.4 记录/日志文件身份（本轮新增）

| 文件 | bytes / SHA-256 |
| --- | --- |
| `docs/plan/2026-09-18-registry-domain-isolation-mutation-raw-output.log` | 10,802 / `FE40309A1D74DBD77F79441C25D801EAEF2F7B586C3B3DE1BA288103BDDC1AE2` |
| `docs/plan/2026-09-18-registry-domain-isolation-full-gate.log` | 28,381 / `564D7F07CF2F8E2555FD9D82F047C34FA7FD2D1F78134BBE2017E99A935C49DA`（含 §29 post-doc 复核段） |
| DEVELOPMENT_CHANGELOG / progress log 的 post 身份 | 见 §3.1 表尾两行（追加后回读）：324,829 / `7E85DE51…` 与 63,811 / `CD5A29E8…` |

---

## 4. 测试与门禁证据（全部在最终树上取数）

原始输出：`docs/plan/2026-09-18-registry-domain-isolation-full-gate.log`（27,539 B）
与 `docs/plan/2026-09-18-registry-domain-isolation-mutation-raw-output.log`（10,802 B）。

### 4.1 新增/扩展的定向测试（spec 覆盖）

| 要求 | 覆盖者 | 结果 |
| --- | --- | --- |
| 同 key 不同 domain 不得互相命中 | 宿主机 `C4.same-domain-still-hits` / `C4.counterexample.isolated-cross-domain-rejected`；离线模型 `test_same_key_different_domain_never_hits` + 随机测试每帧跨域探测 | 绿 |
| owner-check 拒绝（lookup / publish / GC） | 宿主机 `C2.reject.*` / `C5.publish.*` / `C6.gc.*`；离线模型 `test_owner_check_rejects_publish_gc_and_purge` | 绿 |
| GC / reset 只影响本 domain | 宿主机 `C6.gc.other-domain-untouched` / `C7.purge.*`；离线模型 `test_gc_and_reset_only_touch_own_domain` | 绿 |
| 既有 S1/Stage13 行为逐点不变 | 宿主机 `C1.tag.*`（取值 = 历史隐式 tag、互不相同）+ `C3.*-unchanged`（生产同一份 FNV-1a golden 相等） | 绿 |
| F4.10 反例（隔离前会命中、隔离后不会） | 宿主机 `C4.counterexample.legacy-cross-domain-hit`（legacy 形状命中）+ `C4.counterexample.isolated-cross-domain-rejected`；离线模型 `test_legacy_model_exposes_the_counterexample` | 绿 |

原始输出（关键行）：

```
=== 14. registry domain host test ===
SUMMARY: war3_shadow_geometry_domain_test 41/41 case(s) passed
EXIT=0
=== 15. registry domain static gate ===
Ran 13 tests in 0.015s
OK
EXIT=0
=== 16. registry domain offline model ===
Ran 5 tests in 1.103s
OK
EXIT=0
```

### 4.2 现有相关静态门禁

```
=== 17. stage13 retention lazy hash gate ===
Ran 4 tests in 0.001s  OK  EXIT=0
=== 18. bridge/ramp shadow safety gate ===
Ran 22 tests in 0.041s  OK  EXIT=0      （锚点已更新，见 §3.2）
=== 19. shadow lifecycle tombstone gate ===
Ran 8 tests in 0.003s  OK  EXIT=0       （锚点已更新，见 §3.2）
=== 20. persistent expiry queue model ===
Ran 2 tests in 5.469s  OK  EXIT=0
```

### 4.3 全门禁（最终树）

| 门禁 | 结果 |
| --- | --- |
| `ninja -C build32 -n`（前 / 后） | `no work to do` ×2，EXIT=0 |
| 真实重编译（touch `d3d9_device.cpp` + 新测试 + Below Normal -j2） | NINJA_EXIT=0；日志 `[2/4] Linking target war3_shadow_geometry_domain_test.exe` / `[4/4] Linking target src/d3d9/d3d9.dll`；随后 `ninja -n` = no work |
| 预算门禁 | 冻结符号 157；行首站点 迁移前 165 → 上限 112 → 当前 112；已迁出 46；device.cpp 行首定义站点总数 567；EXIT=0 |
| M1 等价门禁 | EXIT=0（26 个已迁出符号；legacy 参考 43452 B / `2EA43F97…`） |
| M2 等价门禁 | EXIT=0（M2-1 8 + M2-2 链 1；`D458C1AE…` / `02FF8AFE…`；M2-3 motion `7DF61688…`） |
| M2-3 等价门禁 | EXIT=0（pre-M2-3 device.cpp 2,278,494 B / `ECC4B828…`；motion-on 差分 40,215,908） |
| M2-5 等价门禁 | EXIT=0（pre-M2-5 device.cpp 2,273,048 B / `D8211864…`；逐字节相同 18,937 B） |
| M2-4 等价门禁 | EXIT=0（pre-M2-4 2,254,826 B / `7391F307…`；电池 7,477,257） |
| M2-5B 等价门禁 | EXIT=0（pre-M2-5B 2,250,786 B / `D0E80399…`；逐字节相同 2,421 B） |
| A9 等价门禁 | EXIT=0（pre-A9 2,248,823 B / `FB4D2FF6…`；逐字节相同 321 B） |
| meson test | `Ok: 84  Fail: 0`，MESON_EXIT=0（83 + 新增 `war3_shadow_geometry_domain`） |
| 全量静态 | `STATIC_TOTAL=255 STATIC_FAIL=0`，STATIC_EXIT=0（254 + 新增门禁） |
| 记录器（palette object evidence） | `SUMMARY: 23 passed, 0 failed`，EXIT=0 |
| 成本 | `COST_VERDICT=PASS checks=38 failures=0`，EXIT=0 |
| 生命周期 | `SUMMARY: war3_shadow_build_lifecycle_test 187/187 case(s) passed`，EXIT=0 |
| 解析器静态 | `Ran 94 tests … OK`，EXIT=0 |
| 根读方 | `Ran 55 tests … OK`，EXIT=0 |
| 往返 | `ROUNDTRIP checks=116 failures=0 PASS` / `CHECKS=1107 FAILURES=0` / `ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`（A–F 六场景生产满足度全 True），EXIT=0 |

**DLL 身份**（`-j2`，Below Normal，最终树）：

```
build32/src/d3d9/d3d9.dll|36,279,032|6D17B0382DEC628A1DF9A672BE0B207C1316F9DE0BB1C86A8C554FF0964DAB20|2026-09-18T05:48:16
build32/src/d3d9/war3_shadow_geometry_domain_test.exe|463,405|F8EC212228ACADB9A901DFD6DF377E86728ECC4E07C0474E402D7E2C9A728ACA|2026-09-18T05:47:30
```

**pre-change DLL 身份**（引用同树自身日志，不是我实测）：
`docs/plan/2026-09-18-a9-a10-full-gate-rerun.log` §31 记录 2026-09-18T04:59:02 的
`build32/src/d3d9/d3d9.dll|36,278,629|824AA63B5C151831F19F7599BF89ABFFE92967564A38236B95056009C01D1887`。
本轮 +403 B（新 domain 字段 + owner-check 代码 + 新计数）。
**注意**：本轮的最终 DLL 之所以 source-consistent，是因为变异 2 还原后做过一次
51 步的 header 依赖全量重编译（变异日志 §变异 2B 的 `[51/51] Linking target src/d3d9/d3d9.dll`），
§4.3 的 touch 重编译只再证明改动的两个 TU（device.cpp / 新测试）可编译可链接。

**日志编码说明**：full-gate log 的 §4–§11 中文摘要因控制台编码写入为乱码；
§4b–§11b 用 UTF-8 控制台**重跑同样门禁**并写入可读文本，退出码与数值一致。

---

### 4.4 post-doc 最终复核（记录/进度/CHANGELOG 写盘之后再取一次）

时间 **2026-09-18T05:52:53**（full-gate log §29），用于证明"最终树"不是取数后又被改动：

```
ninja -C build32 -n        -> ninja: no work to do.                                  EXIT=0
full static suite          -> STATIC_TOTAL=255 STATIC_FAIL=0                          STATIC_EXIT=0
war3_shadow_geometry_domain_test -> 41/41 case(s) passed                              EXIT=0
registry domain static gate      -> Ran 13 tests, OK                                  EXIT=0
registry domain offline model    -> Ran 5 tests, OK                                   EXIT=0
build32/src/d3d9/d3d9.dll  -> 36,279,032 B / 6D17B0382DEC628A1DF9A672BE0B207C1316F9DE0BB1C86A8C554FF0964DAB20
                              （与 §4.3 完全一致：文档写入不改变二进制）
```

本轮写盘的文件身份（回读）：

| 文件 | bytes / SHA-256 |
| --- | --- |
| 本记录 | 31,495 / `CD50308DD3FE4A8BEFDFA6B4ABD03B76B947A22E7096624DA26435461666DFB6`（§3.4 补记后再次变化） |
| mutation raw output log | 10,802 / `FE40309A1D74DBD77F79441C25D801EAEF2F7B586C3B3DE1BA288103BDDC1AE2` |
| full gate log（含 §29） | 28,381 / `564D7F07CF2F8E2555FD9D82F047C34FA7FD2D1F78134BBE2017E99A935C49DA` |
| overnight progress log | 63,811 / `CD5A29E878FEB7A9A7E21160BF0FC050EFDA0AE7892B0AF288CADAC56C986954` |
| DEVELOPMENT_CHANGELOG.md | 324,829 / `7E85DE517D525153AE6209E38B9A970BF5710430AEA01BB9465027D3121969F0` |

## 5. 变异验证（≥2 条真跑）

原始输出：`docs/plan/2026-09-18-registry-domain-isolation-mutation-raw-output.log`。

| 变异 | 内容 | 构建 | 测试/门禁 | 还原 |
| --- | --- | --- | --- | --- |
| 1 | `DecideShadowGeometryOwner` 删除 domain 判定（`(void)expectedDomain;`） | NINJA_EXIT=0（能编译能链接） | 宿主 16/41 FAIL、`HOST_TEST_EXIT=1`；静态门禁 EXIT=1 | SHA MATCH + touch 真实重编译 + 复绿 41/41 + 门禁 13 OK |
| 2A | `Stage13Exact = 0x53310001u`（两值相同） | **NINJA_EXIT=1**：`war3_shadow_geometry_domain.h:48:5: error: duplicate case value` | （未到"跑"：编译期即红） | — |
| 2B | 在 2A 上去掉编译期防线（`ShadowGeometryDomainIndex` 改成不区分取值的映射） | NINJA_EXIT=0 | 宿主 22/41 FAIL、`HOST_TEST_EXIT=1`；静态门禁 EXIT=1 | SHA MATCH + touch 真实重编译 + 复绿 41/41 |

逐字节 SHA-256（还原完整性）：

| 变异 | 变异后 | 还原后 | 逐字节相同 |
| --- | --- | --- | --- |
| 1 | `10A86171C14BDD23397ADD11DA27312240FB625B6731292621077BF733C37F32` (3,960 B) | `215443C92054A97534E6B2FDE20B8791AAB035D4748B141E6FE1BAE7F9C8161E` (4,029 B) | 是 |
| 2A | `888D931AB3F26D956DDBAC150970916B2A36D4C64B0D30DEC86E0BBBEA7BC9BA` (4,029 B) | 同上 | 是 |
| 2B | `349D4A1A479C18FC90EB1EBB2B0F3506AB11013A4CC8C39BA9BEE2013B7CBF11` (3,950 B) | 同上 | 是 |

如实记录：变异 1 下离线模型仍绿——它是**独立派生**的域生命周期模型，不读 C++ 内核文本，
因此对"内核文本被改坏"不敏感；咬住该变异的是宿主机测试与静态门禁。
这是该模型的价值边界，不是它"覆盖了内核回归"。

---

## 6. D1–D8 进展表

| # | 依赖点 | 本轮状态 | 说明 |
| --- | --- | --- | --- |
| D1 | 共享 key 类型（`War3Stage13RetainedCasterMap` 用 registry key） | **未闭合（未采纳形态 (b)）** | 形态 (a) 把 domain 放在条目上，key 的 `operator==`/hash 一个字节没改；Stage13 常驻表仍与 registry 共用 key 类型，但它是独立容器。将来 Stage13Exact 若进入 registry，同 key 跨 domain 会被 owner-check **拒绝**（fail-closed），但不会"天然不共享槽位"。 |
| D2 | 共享 lookup/create（唯一读写口） | **已闭合** | 三个入口全部携带 domain；全部调用点（4 lookup + 1 find-or-create + 3 create）都传 domain；静态门禁钉死站点数（`FROZEN_ENTRY_SITES`）与"每个站点实参含 domain"。 |
| D3 | S1 别名联动擦除 | **未动（未闭合）** | 反向索引仍按 geometryId 操作，结构上只能命中该 geometry 自己的别名，因此本轮未加 domain 维度。勘察 §5.2-4 的"完整读写面未逐点核实"仍然成立。 |
| D4 | 共享 persistent 字节账与 cap | **未动** | `m_war3ShadowPersistentBytesUsed` 仍是**单池**，cap 判定与减账逻辑未改；domain 未分账。Stage13 CPU 常驻本来就不进这个池。 |
| D5 | 共享 expiry 队列 | **未动** | token 仍只有 `{lastSeenFrame, geometryId}`，GC 仍全局按 age；"按 domain 分策略"未做。 |
| D6 | Present 整会话退役 | **部分闭合** | 新增 move **之前**的归属校验（不一致槽位丢弃 + 计数）与两处 domain 作用域 clear；退役本身仍整体 `std::move` + 整体清空，**未按 domain 分账**（不能选择性拒绝退役，否则 GPU 资源失去 fence 所有权）。 |
| D7 | Stage13 自己的两处回收路径 | **未动（结构上不跨 domain）** | age clear 现在经 domain 守卫（#7）；cap 淘汰在 Stage13 容器内自身迭代（`min_element` + `erase`），结构上不可能命中别的 domain，故未加显式判定。两条路径仍**绕过** registry 的 cap/expiry/bytes 账（这是批次 6 的前置问题）。 |
| D8 | 隐式 tag（`0x53310001u` / `0x53314301u`） | **已闭合** | 两个裸常数从设备源代码中消失（只留在 domain 头的枚举定义里）；静态门禁断言 `device.cpp`/`device.h` **代码**（去注释后）不含这两个常数，且取值逐位钉死；宿主机测试用生产 FNV-1a 证明 key 材料不变。 |

---

## 7. F1–F4 进展

| 判据 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| F1-1 显式 domain 标识（符号可查） | **达成** | `ShadowGeometryDomain` + 三个条目 `domain` 字段；门禁 `test_registry_entries_carry_explicit_domain` |
| F1-2 常数被门禁钉死（或取代后断言消失） | **达成** | 门禁 `test_domain_literals_only_live_in_the_domain_header`（去注释后 device 源零命中）+ `test_domain_type_and_historic_tag_values`（取值钉死） |
| F1-3 每个读取点都携带 domain | **达成** | 门禁 `test_every_registry_entry_point_carries_domain`（站点数冻结 + 实参含 domain）+ `test_registry_container_touch_points_are_frozen` |
| F2-4 四条路径按 domain 分账（计数可分别读出） | **部分** | 5+1 个拒绝计数已写入 `War3ShadowPersistentDiagnosticsFrame`；**但尚未接到 `War3PerfMonitor` 的对外 dump**（见 §8-2）。GC/reset 仍**不分账**。 |
| F2-5 分账守恒（GPU 常驻 vs Stage13 CPU 常驻各自账户） | **未做** | 单池未拆；Stage13 CPU 常驻仍只出现在 `retired.cpuOwnedBytes`。 |
| F2-6 离线模型（域内/域间命中、过期、擦除、退役） | **达成** | `AutoTest/test_registry_domain_model.py`：5 tests，含 12,000 帧随机等价（惰性过期堆 vs 全扫描）+ 每帧跨域探测 + F4.10 反例 |
| F3-7 `budgetExceeded`/`framesBudgetExceeded` 不上升 | **未执行** | 本树**没有**已登记的 pre-U5 基线快照；未启动游戏、未部署 |
| F3-8 cap 准入拒绝不激增 | **未执行** | 同上 |
| F3-9 字节账峰值 ≤ cap 且非单调增长、可观察退役 | **未执行** | 同上 |
| F4-10 跨 domain 碰撞反例（隔离前命中 / 隔离后拒绝） | **达成** | 宿主机 `C4.counterexample.*` + 离线模型 `test_legacy_model_exposes_the_counterexample`（legacy_cross_domain_hits=2、isolated 命中 0） |
| F4-11 劣化即如实记录、不宣称改善 | **遵守** | 本轮**没有**任何实机指标，因此不声称任何性能/稳定性改善；只声称隔离与 owner-check 的可离线验证部分 |

---

## 8. 未验证项与已知缺口

1. **F3 全部未执行**：本树没有 pre-U5 基线快照，无法给出"未劣化"的数值判据；
   本轮**未**启动游戏、**未**部署 DLL、**未**触碰 `E:\Work`。
2. **域维度计数未对外可见**：`domain*` 计数只写进 `War3ShadowPersistentDiagnosticsFrame`
   （每 Present 区间），没有接进 `War3PerfMonitor::PersistentGeometryFrameStats` 的
   JSON/日志输出 ⇒ 实机侧暂时读不到。**这是本批最需要补的一块。**
3. **未按 domain 分账字节/条目**：`m_war3ShadowPersistentBytesUsed` 仍是单池；
   reset 退役仍整体 move（D4/D5/D6 的"分账"部分）。
4. **D1（key 级隔离）未做**：形态 (b) 未采纳；同 key 跨 domain 只能靠 owner-check 拒绝，
   而不是"天然不共享槽位"。
5. **变异覆盖有限**：只真跑了"内核判定被删"与"取值重复"两条；"删掉 publish owner-check"、
   "删掉 GC owner-check"、"删掉 reset 归属校验"三种变异**未真跑**（仅由静态门禁文本 + 模型覆盖）。
6. **两个既有门禁的锚点被更新**（bridge/ramp、tombstone），且它们的 pre-change SHA 未捕获（§3.2）。
7. **无实机门**：没有前台视觉/性能/TDR 数据；阴影是否因此变化**未验证**
   （理论上无行为变化：owner-check 只在越权时触发，稳态计数应为 0）。
8. **流程副作用（如实记录）**：第一次全门禁 runner 的 base 路径占位符未被替换，
   导致部分命令以**会话当前目录树**（`...\Graphics\dxvk`）为 CWD 运行：该树里被误写入的
   full-gate log 已删除；该树的 `src/d3d9/d3d9_device.cpp` **mtime** 被 `os.utime` 触碰
   （**内容未变**，无写操作）。B 树的第一次门禁结果已整体作废并用修正后的 runner 重跑。
9. **Stage13 CPU 常驻的 cap/expiry 仍绕过 registry**（D7）：批次 6 的前置问题，本轮未动。

---

## 9. 下一步建议

1. **先把域维度计数接出去**（低成本、解锁 F3）：把 6 个 `domain*` 计数复制进
   `War3PerfMonitor::PersistentGeometryFrameStats` 与其 frame workload/JSON 输出，
   然后做一次**基线采集**并登记（否则 F3 永远无法执行）。
2. **分账（D4/D5）**：为域维度建立独立字节/条目账户；cap 与 age 策略**保持全局不变**
   （避免顺手改准入语义）——先只做"可分别读出"，再考虑分策略。
3. **D1 的决策点**：若批次 6（U4 content-persistent）要让 Stage13Exact 进入 registry，
   必须先在"形态 (b) key 级 domain"与"每个新入口强制 domain 参数 + 门禁钉死"之间做一次
   显式裁定；建议同时把 `War3Stage13RetainedCasterMap` 的 key 也换成 domain-scoped key，
   消除"同 key 不同类型容器"这一隐患。
4. **补变异**：至少再真跑"删掉 publish owner-check"与"删掉 GC owner-check"两条。
5. **D3 核实**：按勘察 §5.2-4 穷举 `m_war3S1TerrainEarlyKeysByPersistentGeometryId`
   的全部读写面，再决定是否需要 domain 维度。
6. 任何"Registry domain 隔离完成"的宣称，必须等 F1–F4 **全部**有实测证据（含 F3 基线）。

---

## 10. 未实施声明（不得引用为已完成）

- 本文**没有**宣称 Registry domain 隔离完成、**没有**宣称 Stage13 已就绪、
  **没有**宣称 palette 侧职责已迁完、**没有**宣称稳定版。
- 本轮**没有**改任何准入/发布语义、**没有**加新 env、**没有**改 Stage13 准入默认值
  （门禁 `test_existing_stage13_admission_defaults_are_untouched` 钉死
  `STATIC_RETENTION/SOURCE_GENERATION_VERIFY/UNIQUE_SEMANTIC_CACHE/LATE_DESCRIPTOR_CACHE = 0u`、
  `STATIC_RETENTION_FRAMES = 240u`、`STATIC_RETENTION_CAP = 64u`）。
- 本轮**没有** git 写、**没有**部署、**没有**启动游戏、**没有**触碰 `E:\Work`。
- 批次 6（U4 Stage13 content-persistent）**未开始**；批次 5（U3 static alias migration）**未开始**。
