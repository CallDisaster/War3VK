# T2 / T3 复审结论（2026-09-18 主线程；**只复审，未实施**）

> 范围：`2026-09-18-t2-slot-ownership-minimal-evidence-plan.md`、`2026-09-18-t3-thread-writer-runtime-proof-plan.md`。
> 纪律：本夜**未改一字节相关源码、未构建、未跑测试、未 git 写**；复审仅为阅读与判断。

## 1. 复审结论（三句话）

1. **两份方案的证据纪律是正确的**：措辞上限（未实证不得写运行时肯定表述；审计结论上限「未发现其他写入路径」）被完整贯彻，且明确拒绝「fail-closed 所以安全」这一被上级驳回的推论（T3 §1.1）。
2. **两份方案都卡在同一处**：需要 **(a) 源码改动批准** 或 **(b) 实机运行时见证** —— 而 (b) 与 ①/② 同一条前置（**能进图**）。⇒ 今夜无法推进，不是因为方案有问题，而是因为**没有实机窗口**。
3. **T2 存在一个结构性事实需要上级注意**：`slotAllocationGeneration` **全树只有 1 处定义（默认 0）+ 2 处只读引用，无任何写入点** ⇒ 第 ③ 层「对象所有权」在**当前结构下不可能闭合**，除非批准**路线 A（新增 native allocator witness 写入）**。这不是文档缺口，是设计选择。

## 2. T2 复审要点

- **名词混淆风险已被方案自己界定**（§0）：palette 槽位所有权（`s_slotBlendedPaletteCache`）≠ render-host SlotLedger/ReadPermit ≠ `slotAllocationGeneration`。复审**确认该界定必要**：三者同名不同物，混用会直接导致证据链错配。
- **批准路线 A 的爆炸半径（供裁定用）**：除写入点外，必须同步更新**钉零的依赖者** —— 实测至少两处：
  · `AutoTest/analyze_skin_palette_selection.py:18`（`require(...slotAllocationGeneration == 0, ...)` 式断言）；
  · `AutoTest/test_skin_palette_contract.cpp:51`（`check(p.slotAllocationGeneration==0, "no fabricated ...")`）。
  ⇒ 路线 A **不是**"加一个赋值"那么小：它会把一个**被门禁钉死的恒零合同**改成**需要真实 witness 的非零值**，必须同时改门禁并给出 witness 的失败模式（何时仍为 0、为 0 时如何 fail-visible）。
- **未发现方案本身的论证错误**；其「A5 通过只证明字节本帧被写、不证明写者身份」的推理与 P0 工单 §5.6 一致。

## 3. T3 复审要点

- **off-thread drain 是真实路径，不是理论风险**：控制面命名管道分离线程 → `DrainSemanticBuildFromControlPlaneIfAllowed` → `drainPendingBuildForControlPlane` → `ensureLatestFrameBuilt()`；且方案给出了**触发面实证**（`AutoTest/war3_autotest_mcp.py` 4 处携带 `allowControlPlaneSemanticDrain: True`）。复审**认可该触发面证据**（比"可能存在"强得多）。
- **C++ 数据竞争的定性正确**：对非原子槽位缓存的普通读与游戏线程写入并发 ⇒ UB；上位驳回"fail-closed 即安全"的理由（可能读到**碰巧满足等式的旧值**）在逻辑上成立。
- **复核提示（行号漂移）**：T3 自称 `war3_control_plane.cpp:5150` 与线程文档 `:5136` 已不一致。**任何后续实施必须以函数名/唯一子串重新锚定**，不得直接引用方案里的行号。

## 4. 我建议的裁定（供上级决策；本文件不实施）

| 项 | 建议 | 理由 |
| --- | --- | --- |
| T2 路线 A | **暂不批准写入**，但**批准把"恒零"从合同改成"有 witness 则为真值、无 witness 仍为 0 且 fail-visible"的设计评审** | 直接写入会让门禁失去意义；先设计"何时可以非零"更稳 |
| T3 | **批准完成剩余只读审计**（把写者审计从 `s_slotBlendedPaletteCache` 扩到其它共享非原子状态），**不批准**任何"运行时已安全"表述 | 审计是纯离线可做的、且是运行时的前置；表述必须守住措辞上限 |
| 两者共同 | **实机窗口一到，T3 优先**（它直接关系 UB 风险），T2 次之（它是证据完备性） | 风险优先级 |

## 5. 与本夜其它产物的关系

- 与 ①/② 共享同一条前置（**能进图**）；
- 与出站交接 §2 并列：T2/T3 属"需批准或需实机"类，**不阻塞** D4/D5 这类纯离线工作 ⇒ 建议下一班**先做 D4/D5**（规格已备），T2/T3 等裁定。

---

## 6. **重要更正（2026-09-18 07:55 追加，主线程读码发现）**

> 本节**更正本文第 3 节对 T3 的判断前提**。请以本节为准；第 3 节保留原文以留痕。

### 6.1 发现

`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp:9568-9579` 的源码注释与代码显示：

```
9571: // 2026-09-17 上级裁定（线程修复方案 B 第一部分）：构建推进只允许发生在所有者线程。
9572-9573: // 控制面 drain（…）在命名管道的分离线程上调用本函数；而 palette 槽位缓存
9574: // 是非原子读、构建进度字段也在锁外更新 ⇒ 非所有者/所有者未建立一律只保留
9575: // requestLatestFrameBuild() 的请求语义，不推进任何分块。该检查位于推进边界，
9576: // 因此同时覆盖 allowControlPlaneSemanticDrain 与 IsHotSemanticBuildWaitPayload 两条门控。
9577: // 所有者身份取自 hook 生命周期观测到的主循环线程，不是第一个请求线程。
9578-9579: if (!consumePermissionGranted(false)) return;
```

### 6.2 对本文第 3 节的影响

| 第 3 节原表述 | 更正后 |
| --- | --- |
| 「off-thread drain 是**真实路径**…C++ 数据竞争 = UB」 | 前半句成立（源码自认 drain 在分离线程调用该函数）；**但**该调用在**推进边界**被所有者许可门拦住，**非所有者不推进任何分块** ⇒ 本文担心的**那条具体机制在代码结构上已被拦断** |
| 隐含「尚未修复，需要批准修复」 | **方案 B 第一部分已于 2026-09-17 落地**（本夜之前），且**不是**"fail-closed 所以安全"，而是**推进边界的所有者许可门** |
| 建议「実机窗口到了 T3 优先」 | 优先级下调：T3 的**结构性修复已在**，剩余的是**运行时证据**与**其它路径枚举** |

### 6.3 T3 的**剩余**未闭合面（修订后的表述）

1. **(i) 其它非所有者路径**：是否存在别的路径非原子读写 palette 槽位缓存（未做全仓路径枚举）；
2. **(ii) 运行时证据**：实机上许可门确实只放行主循环线程（**无**运行时证据）；
3. **(iii) 同类覆盖范围**：T3 点名的"其它共享非原子状态"是否也被同类门覆盖（**未查**）。

⇒ **仍不声称 T3 已闭合**，但**T3 的性质从"待修复的 UB"变为"已有结构性修复、缺运行时印证与范围枚举"** —— 这直接影响你的裁定：**可选项里应新增"只做 (i)(iii) 只读枚举 + 实机 (ii) 印证"，而不是先改代码**。

### 6.4 当时未验证项（不得忽略）

- 本更正基于**静态读码**；`consumePermissionGranted` 的**实际实现**（如何取得所有者、边界条件）**本轮未读**；
- 因此**不能**据此声称"运行时无竞态"；本条只更正**"修复是否存在"**这一事实层面。

### 6.5 §6.4 的未验证项**已补齐**（同一夜稍后读码）

`consumePermissionGranted` 定义于 `war3_shadow_renderer_core.cpp:9533-9543`：

```
9531-9532: // 2026-09-17 上级裁定：**唯一一份**消费权限检查。规则只此一处，两处推进入口共用，
           // 避免复制两套容易分叉的守卫。所有者未建立/非所有者一律拒绝消费（只保留安全请求）。
9533: bool ShadowValidationRuntime::consumePermissionGranted(bool directEntry) {
9534-9538:  counters.offThreadRefused / ownerUnestablishedRefused / directAdvanceRefused  // 三个可计数拒绝
9539-9542:  return m_buildLifecycle.consumeAllowed(
               directEntry,
               GetMainLoopThreadId(),   // 所有者：hook 生命周期观测到的主循环线程
               ::GetCurrentThreadId(),  // 调用者
               counters);
```

**要点**：①**单一实现**（"唯一一份消费权限检查"），由**两处推进入口共用**（`:9578` `directEntry=false`、`:9687` `directEntry=true`），刻意避免双份守卫分叉；②所有者取自 `hooks::GetMainLoopThreadId()`，与 `:9577` 注释一致；③**拒绝被计数**（off-thread / owner-unestablished / direct-advance 三类）；④`publishBuildProgressLocked`（`:9545`）注释声明"唯一写者：构建推进线程"。

⇒ **这同时给出了 (ii) 运行时印证的可观测量**：实机若在 `allowControlPlaneSemanticDrain` 武装下，`g_semanticBuildOffThreadRefusedCount` 等计数即可作为"非所有者调用被拒绝"的运行时证据（**仍需实机才能取到**）。

> 注意：以上仍为**静态读码**结论；`m_buildLifecycle.consumeAllowed` 的内部实现**本轮未读**（其正确性未由本步覆盖）。

### 6.6 证据等级升级：**执行证据**（同一夜，真跑宿主测试）

- `build32/src/d3d9/war3_shadow_build_thread_gate_test.exe` ⇒ **EXIT=0 / SUMMARY 9/9 PASS**，其中 **`C3 pipe-thread-drain owner=0x1A2B current=0x00FF -> NotOwner`** 即 T3 场景本身；另有 `C4`（hot-wait 门控）、`C5`（第二处下层入口）、`C6`（伪造所有者）、`C7`（reset 前后）、`C1`（所有者未建立）；
- `build32/src/d3d9/war3_shadow_build_lifecycle_test.exe` ⇒ **EXIT=0 / SUMMARY 187/187 PASS**（含 `C2.non-owner-entry-off-thread-count actual=1 expected=1`）；
- ⇒ **T3 的"闸门语义"由执行证据支持**；剩余未验仅为**实机运行期**（所有者取值 + 无其它路径）。**仍不声称闭环**。
