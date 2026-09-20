"""2026-09-17 上级裁定（codex 01a02e0b）：构建推进 / 发布生命周期静态门禁。

本测试只钉死**已落地的事实**，不构成线程性质证明：
- 消费权限只有唯一一份实现：判定 DecideShadowBuildAdvance 在**产品源**中只允许出现在
  war3_shadow_build_lifecycle.h 的 ShadowBuildLifecycle::consumeAllowed 内；运行时唯一入口
  ShadowValidationRuntime::consumePermissionGranted(bool directEntry) 必须构造
  ShadowBuildAdvanceCounters（三个进程级 atomic 指针）并调用 m_buildLifecycle.consumeAllowed(；
  两处推进入口（ShadowValidationRuntime::ensureLatestFrameBuilt 与
  ensureFrameBuiltForContract）都必须调用 consumePermissionGranted(false/true)，且早于本函数内
  首次 std::unique_lock / m_mutex / buildFrameChunk；入口不得自行比较线程 id，也不得复制
  一套自己的拒绝计数规则（避免两套守卫分叉）；
- 摘要载体是**按值**状态机、且**不新增每块堆分配**：运行时不直接持有 ShadowBuildProgressState，
  而是持有按值成员 ShadowBuildLifecycle m_buildLifecycle（组件内按值持有
  ShadowBuildProgressState m_progress）；产品源不得再出现 m_publishedBuildProgress，也不得为
  进度摘要 / 生命周期组件使用 std::make_shared<...Progress|...Lifecycle> /
  std::shared_ptr<...Progress|...Lifecycle>（无可变别名）；
- 生命周期齐全：组件提供 beginBuild(const void* work, …) / isCurrent( / publishIfCurrent( /
  completeIfCurrent( / cancel( / reset()（底层 publish( / complete( 仍保留，但**只供状态机测试**），
  运行时出现 m_buildLifecycle.beginBuild/publishIfCurrent/completeIfCurrent/isCurrent/cancel/reset
  与 m_buildWorkGeneration；m_buildLifecycle.publishIfCurrent( 与 m_publishedBuildStats 的写入必须
  处于**同一临界区**；
- **工作身份成对携带**（2026-09-17 接线变更，上级要求"每块发布传当时的工作身份 + 其固定代际"）：
  运行时**不得**再出现 m_buildLifecycle.publish( / m_buildLifecycle.complete(（必须用
  publishIfCurrent( / completeIfCurrent( / isCurrent(）；beginBuild 必须绑定工作对象指针
  （m_buildWork.get()）；ensureFrameBuiltForContract 必须在 m_buildInProgress 分支内**锁内成对取得**
  buildWork = m_buildWork; 与 buildWorkGeneration = m_buildWorkGeneration;；完成块必须**先**出现
  isCurrent(buildWork.get(), buildWorkGeneration) 判断，**再**调用发布并写入 m_lastStats /
  m_lastFrame（按源码偏移判定，防止"先写结果后核验"）；部分帧写入前同样先核验；
- reader 只读已发布摘要：snapshot() / buildStateSnapshot() 的**代码**（注释除外）不得出现
  m_buildWork，且必须读 m_buildLifecycle.hasValues()/values()；
- 三个见证计数器 static std::atomic<uint64_t> + 三个访问器；三个字段各自走满 **7 处导出链**
  （bridge.h 字段 / bridge.cpp 直拉 atomic getter / hub.h 字段 / hub.cpp 摘要赋值 /
  hub.cpp json 字段 / control_plane.cpp json 字段 / perf_monitor.cpp json << 写手）；
- bridge.h 对新计数必须写明「进程累计」与「不得未经证明相加成总拒绝」（或等价措辞）；
- 保留既有请求首行、既有调用者、既有 FORBIDDEN 肯定式宣称禁令与回归护栏。

"同一临界区"的判定方式（可解释的近似，非形式化证明）：
  1) m_buildLifecycle.publish( 与最近的 m_publishedBuildStats 赋值行距 <= 8 行；
  2) 两者文本之间不出现 '}'（作用域收束）/ 'unlock(' / 新的 std::unique_lock；
  3) 两者的 unique_lock 作用域归属相同（注释与字符串字面量先置空后，用
     "声明之后第一个深度为 0 的 '}' 即所属复合语句结束" 求出每个锁作用域区间）；
  4) 每次 publishBuildProgressLocked(*...) 调用点都位于某个 unique_lock 作用域区间内
     （发布辅助函数自身要求调用者持锁，锁由调用点证明）。
  Complete/Cancel/Reset 与 m_publishedBuildStats = {} 的清理同样按上述 (1)(2)(3) 配对。

措辞上限：本门禁只说明"非所有者不再推进构建 / reader 只读按值发布的摘要"；禁止把并发访问
描述为"已消除竞争 / 线程问题已闭合"式宣称（否定式规则表述除外，见 FORBIDDEN 列表）。

变异验证（可复现记录；本门禁的每一条断言都必须至少被一条变异杀死。"变异验证的可复现性"
由下面的固定步骤保证：把本脚本按相同的 ROOT 结构镜像到临时目录，只改镜像里的文件，
运行镜像中的本脚本，确认非零退出）：
  M1 消费权限不再走组件：把 core.cpp 中 consumePermissionGranted 的
     "return m_buildLifecycle.consumeAllowed(" 改写成 "return true;"（其余不动）
     ⇒ 由 §3b 的 m_buildLifecycle.consumeAllowed( 断言杀死。
  M2 生命周期组件退化成堆分配别名：把 core.h 的
     "ShadowBuildLifecycle m_buildLifecycle;" 改写成
     "std::shared_ptr<ShadowBuildLifecycle> m_buildLifecycle;"
     ⇒ 由 §6 的按值成员正则与 shared_ptr<...Lifecycle 禁令杀死。
  M3 新计数导出只接一半：删除 perf_monitor.cpp 中
     'json << "    \"semanticBuildDirectAdvanceRefusedCount\": "' 的两行
     ⇒ 由 §5 的 7 处链路（PM 必须是 json << 写手）断言杀死。
  M4 判定调用泄漏到运行时：在 consumePermissionGranted 内加入
     "const auto leaked = DecideShadowBuildAdvance(0u, 0u);"
     ⇒ 由 §3a 的"判定只在组件 consumeAllowed 内"断言杀死。
  M5 reader 改读 live 构建工作对象：在 snapshot() 内加入
     "if (m_buildWork != nullptr) { return m_lastStats; }"
     ⇒ 由 §8 的 strip_comments 后 m_buildWork(?!Generation) 禁令杀死。
  M6 直接入口不再单独计数：删除组件 consumeAllowed 中
     "if (directEntry && counters.directAdvanceRefused != nullptr)" 两行
     ⇒ 由 §3b 的 direct 入口计数断言杀死。
  M7（新） 每块发布退回底层代际门：把 core.cpp 的 publishBuildProgressLocked 中
     "if (m_buildLifecycle.publishIfCurrent(&work, generation, values))" 改写为
     "if (m_buildLifecycle.publish(generation, values))"（旧工作可被配上新 token）
     ⇒ 由 §7a 的"运行时不得出现 m_buildLifecycle.publish("与"publishIfCurrent(&work, generation, values)"
     断言杀死。
  M8（新） 完成块先写结果后核验：把 core.cpp 完成块中的
     "if (!m_buildLifecycle.isCurrent(buildWork.get(), buildWorkGeneration)) return;" 整行移动到
     "m_lastFrame = completedFrame;" 之后（其余不动）
     ⇒ 由 §7b 的"完成块按偏移先核验 isCurrent，再写 m_lastStats / m_lastFrame"断言杀死。
  M9（新） beginBuild 不再绑定工作对象身份：把 core.cpp 的
     "m_buildLifecycle.beginBuild(m_buildWork.get(), …" 改写为
     "m_buildLifecycle.beginBuild(nullptr, …"
     ⇒ 由 §7a 的 beginBuild 必须携带 m_buildWork.get() 断言杀死。
  M10（新） reset() 不再清空工作身份：删除组件 reset() 中的 "m_currentWork = nullptr;" 行
     ⇒ 由 §3e 的 reset() 必须清空 m_currentWork 断言杀死。

记录器（对象级 palette 证据）修正 A/B 的变异（2026-09-17 增补；同样只在镜像里改，运行镜像中的本脚本，
并重编镜像中的 war3_palette_object_evidence_test 一并确认被杀死）：
  M11（新，修正 A）Insert 又预写拒绝帧标记：把 Insert 的 "e.firstRejectFrame = frame;" 改写成
     "e.firstRejectFrame = frame; e.lastRejectFrame = frame;"（复现旧缺陷）
     ⇒ 由 §11g 的"Insert 不得写 lastRejectFrame"杀死，并被宿主机 Case1（emitted 必须为 3）
     与 Case8（建条目必须发射第一条 Rejected）同时杀死。
  M12（新，修正 B）NoteReject 强制帧域 unknown：在 NoteReject 的 "record.frames = frames;" 之后加入
     "record.frames.manifestUnknown = true; record.frames.nativeUnknown = true;"
     ⇒ 由 §11g 的禁止式杀死，并被宿主机 Case2（终态保留调用点口径）/Case12（known 保持 known）杀死。
  M13（新，修正 B）PrepareStage 强制帧域 unknown：把 PrepareStage 的 "record.frames = frames;" 改写成
     "record.frames = frames; record.frames.manifestUnknown = true; record.frames.nativeUnknown = true;"
     ⇒ 由 §11g 杀死，并被宿主机 Case12（served/enqueued/drawn 的 SameFrames）杀死。
  M14（新，修正 B）CloseWindow 强制帧域 unknown：在 CloseWindow 写入 record.frames.renderFrame 之后加入
     "record.frames.manifestUnknown = true; record.frames.nativeUnknown = true;"
     ⇒ 由 §11g 杀死，并被宿主机 Case2/Case12 的终态 SameFrames 断言杀死。
  M15（新，修正 B）阶段事件沿用条目首次帧：把 PrepareStage 的 "record.frames = frames;" 改写成
     "record.frames = e->firstFrames;" ⇒ 由 §11g 的 "record.frames = frames;" 断言杀死，
     并被宿主机 Case12（各阶段帧域必须等于各自调用点）杀死。
  M16（新，既有纪律）普通事件不再受 3584 上限：删除 CanEmit 中
     "if (!terminal && m_counters.emitted >= kNormalBudget)" 两行
     ⇒ 由 §11b 正则与宿主机 Case7 杀死。
  M17（新，既有纪律）hitCount 在预算预检之前递增：把 NoteServed 的 "if (!CanEmit(false))" 预检移动到
     "e->hitCount++" 之后 ⇒ 由 §11d（CanEmit 必须早于任何 e-> 状态修改）与宿主机 Case15/Case17 杀死。
  M18（新，既有纪律）chainSequence 不再递增：删除 NoteReject 的
     "record.chainSequence = ++e->chainSequence;" 行 ⇒ 由宿主机 Case2（相对建条目事件逐条 +1）杀死。
  M19（新，既有纪律）同一帧的第二次 Reject 不再去重：删除 NoteReject 中
     "if (e->lastRejectFrame == frames.renderFrame)" 判断 ⇒ 由 §11j 与宿主机 Case8 杀死。
  M20（新，修正 A）Insert 不再记录 firstRejectFrame：删除 Insert 的 "e.firstRejectFrame = frame;"
     ⇒ 由 §11g 的"Insert 仍必须记录 firstRejectFrame"杀死，并被宿主机 Case2/Case12 的时间差
     断言（deltaFrames 500-499 等）杀死。
  M21（新，修正 A）NoteReject 不再记录 lastRejectFrame：删除
     "e->lastRejectFrame = frames.renderFrame;" ⇒ 由 §11g 与宿主机 Case8（同帧第二次必须去重）杀死。
  M22（新，既有纪律）lastRejectFrame 写入早于预算预检：把该行移到 "if (!CanEmit(false))" 之前
     ⇒ 由 §11g 的顺序断言与 §11d（CanEmit 必须早于任何 e-> 状态修改）杀死；实测宿主机用例
     **不**覆盖这一条（事件仍会因预算被拒），所以必须留在静态门禁里。
  M23（新，测试侧）宿主机测试删掉"known 保持 known"断言（修正 B 的见证）⇒ 由 §11h 的文本断言杀死。
  M24（新，测试侧）宿主机测试重新写回"强制 unknown"字样的断言 ⇒ 由 §11h 的旧口径禁令杀死。
  M25（新，wire 同步）发射器不再写 lifecycleIdentity：删除
     "event.data[2] = record.key.lifecycleIdentity;" ⇒ 由 §11i 杀死。
  M26（新，wire 同步）解析器不再声明槽位：把 "LIFECYCLE_IDENTITY_SLOT=('data',2)" 改成 None。
     首版 §11i 用子串匹配时本条**存活**（PROPOSED_LIFECYCLE_IDENTITY_SLOT=('data',2) 也让子串成立），
     现已改为 ^...$ 多行整行匹配 ⇒ 被杀死。这条说明"变异验证"必须真的跑，不能只写文档。

G1..G4 生产修复（2026-09-17 主线程：往返测试暴露后在生产代码落地）的变异（同样只在镜像里改，
运行镜像中的本脚本；涉及宿主机行为的条目还要重编镜像中的 war3_palette_object_evidence_test）：
  M27（新，G3）live Enqueued 重新计算 delta：把 NoteEnqueued 的 "record.deltaFrames = 0u;" 改写成
     "record.deltaFrames = frames.renderFrame >= e->firstRejectFrame ? "
     "frames.renderFrame - e->firstRejectFrame : 0u;"
     ⇒ 由 §11k(a) 杀死，并被宿主机 Case2（live Enqueued 必须 0）与往返测试 C 杀死。
  M28（新，G3）live Drawn 重新计算 delta：NoteDrawn 同上改写
     ⇒ 由 §11k(a) 与宿主机 Case2（live Drawn 必须 0）杀死。
  M29（新，G2）CloseWindow 不再置窗口关闭位：删除 CloseWindow 的 "m_windowClosed = true;" 行
     ⇒ 由 §11k(b) 杀死，并被宿主机 Case14（关闭后五个入口必须早退）杀死。
  M30（新，G2）NoteDrawn 丢掉窗口关闭早退：删除 NoteDrawn 的 "if (m_windowClosed) return;" 两行
     ⇒ 由 §11k(b) 杀死，并被宿主机 Case7(b)/Case14 杀死。
  M31（新，G4）发射器不再对环淘汰 fail-visible：删除 EmitPaletteObjectEvent 的
     "if (Record(session, event) == 0u)" 与 "g_paletteObjectEvidence.NoteRingEviction(1u);" 两行
     ⇒ 由 §11k(c) 杀死，并被往返测试 C（objectLevelEvidenceDropped 必须可见）杀死。
  M32（新，G4）freeze 分支先冻结环再关窗：交换 war3_frame_evidence.cpp freeze 分支里的
     "ClosePaletteObjectWindow();" 与 "if(!s->ring.finish(generation))" 两行
     ⇒ 由 §11k(e) 杀死，并被往返测试 A（终态必须进导出）杀死。
  M33（新，G1）watchCount 又写成字符串：把 PaletteObjectHeaderJson 的
     "result["watchCount"]=header.watchCount;" 改成 "result["watchCount"]=std::to_string(header.watchCount);"
     ⇒ 由 §11k(d) 杀死，并被往返测试（生产解析器必须直接接受真导出）杀死。
  M34（新，G2）disarm 又回到 freeze：在 freeze 分支的 "ClosePaletteObjectWindow();" 之后加一行
     "DisarmPaletteObjectEvidence();" ⇒ 由 §11k(d) 的"只允许出现在 discard 分支"杀死，
     并被往返测试（导出头块必须与记录器一致且非零）杀死。

F1/F2/F3（2026-09-17 主线程修复"单窗口终态上界 / 结算桶 / 终态 delta"）的变异：
  M35（新，F1）终态重新受每帧 64 约束：把 CanEmit 的
     "if (!terminal && m_frameBudgetUsed >= kPerFrameBudget)" 改回
     "if (m_frameBudgetUsed >= kPerFrameBudget)"
     ⇒ 由 §11k(f) 杀死，并被宿主机 Case7(c)（512 预留必须可达）杀死。
  M36（新，F1）预留耗尽的终态改记 per-frame：把 AccountDrop 的
     "if (terminal && m_counters.terminalEmitted >= kTerminalReserve) { droppedTerminalReserve++; return; }"
     改成 "if (false)" ⇒ 由 §11k(f) 杀死，并被宿主机 Case7(c)（droppedTerminalReserve 必须为 512）杀死。
  M37（新，F2）CloseWindow 结算桶回到预算门之前：把三个 closed*++ 移到 "EmitUnchecked(record, true);" 之前
     ⇒ 由 §11k(f) 杀死，并被宿主机 Case7(c)（closedWindowExpired 必须 == 512，而不是 1024）杀死。
  M38（新，F3）ObjectGone 不再显式写 delta：删除 NoteObjectGone 的
     "record.deltaFrames = record.frames.renderFrame >= record.firstRejectFrame ? ... : 0u;" 赋值
     ⇒ 由 §11k(f) 杀死（宿主机 Case6 只钉数值 0，无法区分显式赋值与默认值，故必须留在静态门禁里）。

并发所有者协议 / 预冻结钩子（2026-09-17 上级裁定第 1/2 项）的变异（同样只在镜像里改，
运行镜像中的本脚本；涉及宿主机行为的条目还要重编镜像中的往返测试 exe）：
  M39（新，预冻结钩子）arm 分支不再安装钩子：删除 war3_frame_evidence.cpp 的 arm 分支里
     "s->ring.setPreFreezeHook(&PaletteObjectPreFreezeHook);" 这一行
     ⇒ 由 §11l 的"arm 分支必须安装预冻结钩子"杀死，并被往返测试场景 F 杀死（钩子没跑 ⇒ 终态只能在
     环已 Frozen 之后由 export 分支的 ClosePaletteObjectWindow 结算 ⇒ append 被拒、
     ringEvictedAfterRecord != 0、导出里没有 Recovered 终态）。
  M40（新，并发所有者协议）四个 Note* 不再取 StateLock：删除 NoteReject / NoteServed /
     NoteEnqueued / NoteDrawn 函数体第一行的 "StateLock guard(*this);"
     ⇒ 由 §11l 的"StateLock 必须是函数体第一条语句"杀死，并被往返测试场景 E 杀死
     （4 写者并发下 emitted 必须恰等于导出事件条数 2000；无锁时 m_counters/m_entries 出现
     可见的丢失计数与丢链不一致）。
  M41（新，递归状态锁 / 结构性重入）StateLock 的底层锁改回**不可重入**：把生产头
     war3_palette_object_evidence.h 的 "mutable std::recursive_mutex m_mutex;" 改回
     "mutable std::mutex m_mutex;"
     ⇒ 由 §11l 的 recursive_mutex 断言杀死，并被宿主机案例 20 杀死。本次**实测**（只改生产头、
     重建 war3_palette_object_evidence_test.exe、还原后复跑）：
       变异：19 PASS + "[FAIL] 20 emitter-callback reentry into CloseWindow must not deadlock
             (watchdog timeout 10000 ms)"，进程 exit=2（看门狗 10 s 触发后 std::_Exit(2)）；
       还原：21/21 PASS，exit=0。
     即"改回 std::mutex"是**被杀死的**（非零退出 + 明确打印），而不是把门禁挂死。案例 20/21 都跑在
     独立工作线程上、由主线程按超时做看门狗；案例 21 还用真实生产 Ring 证明容量冻结恰好发生在
     palette 发射回调内时，环收不下的事件全部计入 ringEvictedAfterRecord（fail-visible）。

D6（2026-09-17 上级裁定：同键同帧同阶段**只有在载荷可证等价时**才能压缩成一条；载荷冲突必须计数
并阻止认证）的变异（同样按 M41 的实测口径：只改生产头、重建宿主机 test exe、还原后复跑，并用
SHA-256 证明还原）：
  M42（新，D6 载荷口径）四处同帧去重退回**不看载荷**的旧行为：把 NoteReject / NoteServed /
     NoteEnqueued / NoteDrawn 的
       "if (<载荷比较>) m_counters.droppedDuplicatePerFrame++; else m_counters.droppedPayloadConflict++;"
     改回 "m_counters.droppedDuplicatePerFrame++;"（旧行为：载荷冲突被当成"已证明等价的重复"吞掉）
     ⇒ 由 §11m(a) 的"载荷比较必须先于两个计数、duplicate 只在 if 分支、conflict 只在 else 分支"杀死，
     并被宿主机 Case8（同 reason/different reason 两形态）、Case16（四阶段两向见证）与
     Case22（上级反例：同帧第二次 D 已清空语义 palette ⇒ droppedPayloadConflict >= 1）杀死。
     本次**实测**结果见交付报告（变异 ⇒ 门禁与宿主机测试同时变红；还原后复跑全绿）。
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADOW = ROOT / "src/d3d9/war3/shadow"
GATE_H_PATH = SHADOW / "war3_shadow_build_thread_gate.h"
PROGRESS_H_PATH = SHADOW / "war3_shadow_build_progress.h"
LIFECYCLE_H_PATH = SHADOW / "war3_shadow_build_lifecycle.h"
CORE_PATH = SHADOW / "war3_shadow_renderer_core.cpp"
CORE_H_PATH = SHADOW / "war3_shadow_renderer_core.h"
GATE_H = GATE_H_PATH.read_text(encoding="utf-8")
PROGRESS_H = PROGRESS_H_PATH.read_text(encoding="utf-8")
LIFECYCLE_H = LIFECYCLE_H_PATH.read_text(encoding="utf-8")
CORE = CORE_PATH.read_text(encoding="utf-8")
CORE_H = CORE_H_PATH.read_text(encoding="utf-8")
BRIDGE_H = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.h").read_text(encoding="utf-8")
BRIDGE = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp").read_text(encoding="utf-8")
HUB_H = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h").read_text(encoding="utf-8")
HUB = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp").read_text(encoding="utf-8")
CP = (ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp").read_text(encoding="utf-8")
PM = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(encoding="utf-8")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")
TEST_SRC = ROOT / "src/d3d9/war3/render/tests/war3_shadow_build_thread_gate_test.cpp"
TEST_TEXT = TEST_SRC.read_text(encoding="utf-8")
PROGRESS_TEST_SRC = ROOT / "src/d3d9/war3/render/tests/war3_shadow_build_progress_test.cpp"
LIFECYCLE_TEST_SRC = (ROOT / "src/d3d9/war3/render/tests/"
                             "war3_shadow_build_lifecycle_test.cpp")

GATE_FN = "DecideShadowBuildAdvance"
PERM_FN = "consumePermissionGranted"
OFF_THREAD_COUNTER = "g_semanticBuildOffThreadRefusedCount"
OWNER_UNESTABLISHED_COUNTER = "g_semanticBuildOwnerUnestablishedRefusedCount"
DIRECT_ADVANCE_COUNTER = "g_semanticBuildDirectAdvanceRefusedCount"
COUNTERS = (OFF_THREAD_COUNTER, OWNER_UNESTABLISHED_COUNTER, DIRECT_ADVANCE_COUNTER)
ACCESSORS = ("QuerySemanticBuildOffThreadRefusedCount",
             "QuerySemanticBuildOwnerUnestablishedRefusedCount",
             "QuerySemanticBuildDirectAdvanceRefusedCount")
OFF_THREAD_FIELD = "semanticBuildOffThreadRefusedCount"
OWNER_UNESTABLISHED_FIELD = "semanticBuildOwnerUnestablishedRefusedCount"
DIRECT_ADVANCE_FIELD = "semanticBuildDirectAdvanceRefusedCount"
# 三个导出字段与各自取数访问器（三个字段都必须走满 7 处链路）。
EXPORT_CHAIN = (
    (OFF_THREAD_FIELD, "QuerySemanticBuildOffThreadRefusedCount"),
    (OWNER_UNESTABLISHED_FIELD, "QuerySemanticBuildOwnerUnestablishedRefusedCount"),
    (DIRECT_ADVANCE_FIELD, "QuerySemanticBuildDirectAdvanceRefusedCount"),
)
# 旧"主循环未建立即放行"计数器 / 访问器：不得再存在于任何产品源（用正则，不回写旧名）。
LEGACY_COUNTER = re.compile(r"semanticBuild\w*BeforeMainLoop\w*", re.IGNORECASE)
THREAD_ID_NAMES = ("ownerThreadId", "currentThreadId", "mainLoopThreadId")
# 旧进度摘要载体（shared_ptr<const ...> 别名）与"每块新增堆分配"的进度类型。
LEGACY_PROGRESS_MEMBER = "m_publishedBuildProgress"
PROGRESS_TYPE = "ShadowBuildProgressState"
PROGRESS_MEMBER = "m_buildProgress"
PROGRESS_PUBLISH_CALL = "publishBuildProgressLocked(*"
PROGRESS_STATS_NAME = "m_publishedBuildStats"
# 生产与测试共用的生命周期组件（按值成员，不引入堆分配）。
LIFECYCLE_TYPE = "ShadowBuildLifecycle"
LIFECYCLE_MEMBER = "m_buildLifecycle"
COMPONENT_COUNTER_FIELDS = ("counters.offThreadRefused",
                            "counters.ownerUnestablishedRefused",
                            "counters.directAdvanceRefused")
HEAP_CARRIER_TYPES = ("Progress", "Lifecycle")
MAX_SAME_SECTION_LINES = 8


def assert_no_thread_id_bypass(body, label):
    """线程 id 不得在判定函数之外被直接比较（含 static_cast<...>(id) 包装形态）。"""
    for line in body.splitlines():
        if not any(name in line for name in THREAD_ID_NAMES):
            continue
        if GATE_FN in line:
            continue  # 判定函数调用实参行
        assert "==" not in line and "!=" not in line, (label, line.strip())


def extract_function(text, signature):
    """返回 (函数体含大括号, 大括号在全文中的偏移)。注释 / 字符串中的括号不计。"""
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    i = brace
    n = len(text)
    in_line_comment = False
    in_block_comment = False
    quote = None
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
        elif in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 1
        elif quote is not None:
            if ch == "\\":
                i += 1
            elif ch == quote:
                quote = None
        elif ch == "/" and nxt == "/":
            in_line_comment = True
            i += 1
        elif ch == "/" and nxt == "*":
            in_block_comment = True
            i += 1
        elif ch in "\"'":
            quote = ch
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[brace:i + 1], brace
        i += 1
    raise AssertionError("unterminated function body: " + signature)


def strip_comments(text):
    """去掉 // 行注释与 /* */ 块注释，保留字符串/字符字面量内容。

    用于"函数体内不得出现某标识符"这类**代码级**检查：注释里说明某字段不再被读取，
    不等于代码读取了该字段。
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif ch == "/" and nxt == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                i += 1
            i += 2
        elif ch in "\"'":
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\":
                    i += 1
                    if i < n:
                        out.append(text[i])
                        i += 1
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)




# 2026-09-18 P0-6（Astra 裁定 C1）：S/E/D 的每链操作被抽成 helper，**入口体只剩委派**。
# 门禁若继续只看入口体，就会把「代码搬进 helper」误判成「断言失败」。
# 因此统一提供 effective_body()：入口体 + 它委派的那个 helper 的体。
_DELEGATED_HELPERS = {
    "void NoteServed(": "void AdvanceServedOnChain(",
    "void NoteEnqueued(": "void AdvanceEnqueuedOnChain(",
    "void NoteDrawn(": "void AdvanceDrawnOnChain(",
}


def effective_body(signature):
    body, _ = extract_function(RECORDER_H, signature)
    for prefix, helper in _DELEGATED_HELPERS.items():
        if signature.startswith(prefix) and helper.replace("void ", "") in body:
            helper_body, _ = extract_function(RECORDER_H, helper)
            return body + "\n" + helper_body
    return body
def mask_code(text):
    """把注释与字符串 / 字符字面量替换为等长空白（保留换行与全部偏移）。

    仅用于**结构**判定（作用域配平、行距）。等长替换保证掩码文本的偏移与原文一致。
    """
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif ch == "/" and nxt == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                if i + 1 < n:
                    out[i + 1] = " "
                i += 2
        elif ch in "\"'":
            quote = ch
            out[i] = " "
            i += 1
            while i < n:
                if text[i] == "\\":
                    out[i] = " "
                    if i + 1 < n and text[i + 1] != "\n":
                        out[i + 1] = " "
                    i += 2
                    continue
                if text[i] == quote:
                    out[i] = " "
                    i += 1
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def lock_scopes(masked, lock_token="std::unique_lock"):
    """返回每个 lock_token 声明所辖的 (声明偏移, 作用域结束偏移) 区间。

    作用域 = 该声明所在复合语句：从声明之后向前的括号配平中，第一个深度为 0 的 '}' 即结束。
    掩码文本已把注释与字符串字面量置空，因此括号计数不受说明文字影响。
    """
    scopes = []
    for match in re.finditer(re.escape(lock_token), masked):
        at = match.start()
        depth = 0
        end = len(masked)
        i = masked.find(";", at)
        if i == -1:
            i = at
        while i < len(masked):
            ch = masked[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                if depth == 0:
                    end = i
                    break
                depth -= 1
            i += 1
        scopes.append((at, end))
    return scopes


def in_lock_scope(scopes, offset):
    return any(start <= offset <= end for start, end in scopes)


def brace_scope_at(text, brace_at):
    """返回从给定 '{' 起配对的复合语句区间 (起偏移, 结束偏移)。

    用于"某段逻辑必须在某个分支内"这类**结构**判定（文本需已 mask_code 掩码）。
    """
    depth = 0
    for i in range(brace_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return brace_at, i
    raise AssertionError("unbalanced braces at offset " + str(brace_at))


def assert_same_critical_section(masked, token_a, token_b, label,
                                 max_lines=MAX_SAME_SECTION_LINES):
    """断言 token_a 的每次出现都在 max_lines 行内与某处 token_b 配对，且两者同临界区。

    判定方式见模块 docstring：行距 <= max_lines、之间无 '}' / 'unlock(' / 新
    std::unique_lock、两侧 unique_lock 作用域归属相同。故意不追踪别名或宏展开。
    """
    a_offsets = [m.start() for m in re.finditer(re.escape(token_a), masked)]
    b_offsets = [m.start() for m in re.finditer(re.escape(token_b), masked)]
    assert a_offsets, (label, "缺少 " + token_a)
    assert b_offsets, (label, "缺少 " + token_b)
    for a in a_offsets:
        a_line = masked.count("\n", 0, a)
        b = min(b_offsets, key=lambda s: abs(masked.count("\n", 0, s) - a_line))
        b_line = masked.count("\n", 0, b)
        assert abs(b_line - a_line) <= max_lines, (label, a_line, b_line)
        lo, hi = sorted((a, b))
        between = masked[lo:hi]
        assert "}" not in between, (label, "配对之间出现作用域收束 '}'")
        assert "unlock(" not in between, (label, "配对之间出现 unlock")
        assert "std::unique_lock" not in between, (label, "配对之间出现新的 unique_lock")
        assert in_lock_scope(CORE_LOCK_SCOPES, a) == in_lock_scope(CORE_LOCK_SCOPES, b), \
            (label, "两侧不属于同一 unique_lock 作用域")
    return len(a_offsets)


def assert_component_refusal_counting(body, label):
    """组件拒绝路径：Allow 分支先 return true；三个拒绝计数都在其后按 relaxed 自增，
    各自的空指针必须显式守卫；未知所有者只记 ownerUnestablished（不得记 off-thread）；
    direct 入口计数只在 directEntry 分支内；函数只有一处 return true 与一处 return false。
    """
    masked = mask_code(body)
    assert "const ShadowBuildAdvanceDecision decision =" in masked, label
    assert "if (decision == ShadowBuildAdvanceDecision::Allow)" in masked, label
    allow_return_at = masked.index("return true;")
    for field in COMPONENT_COUNTER_FIELDS:
        assert field + " != nullptr" in masked, (label, field, "空指针必须显式守卫")
        at = masked.find(field + "->fetch_add(")
        assert at != -1, (label, field)
        assert at > allow_return_at, (label, field, "计数必须在 Allow 放行之后")
        assert re.search(
            re.escape(field) +
            r"->fetch_add\(\s*1u,\s*std::memory_order_relaxed\s*\)", masked), \
            (label, field)
    # 未知所有者分支只允许记 ownerUnestablished；off-thread 只允许出现在 else 分支。
    assert re.search(
        r"decision\s*==\s*ShadowBuildAdvanceDecision::OwnerUnestablished\)\s*\{\s*"
        r"if\s*\(\s*counters\.ownerUnestablishedRefused\s*!=\s*nullptr\s*\)\s*"
        r"counters\.ownerUnestablishedRefused->fetch_add", masked), \
        (label, "未知所有者只允许记 ownerUnestablished 计数")
    assert re.search(
        r"\}\s*else if\s*\(\s*counters\.offThreadRefused\s*!=\s*nullptr\s*\)\s*\{\s*"
        r"counters\.offThreadRefused->fetch_add", masked), \
        (label, "非所有者只允许记 off-thread 计数")
    # direct 入口计数只在 directEntry 分支内（两种入口分类不是同一维度）。
    assert re.search(
        r"if\s*\(\s*directEntry\s*&&\s*counters\.directAdvanceRefused\s*!=\s*nullptr\s*\)\s*"
        r"counters\.directAdvanceRefused->fetch_add", masked), \
        (label, "directEntry 计数必须在 directEntry 分支内")
    assert masked.count("return true;") == 1, (label, "Allow 之后不得再有第二个 return true")
    assert masked.count("return false;") == 1, (label, "只允许一处拒绝 return false")
    direct_guard_at = masked.index("if (directEntry &&")
    assert direct_guard_at < masked.rindex("return false;"), \
        (label, "直接入口计数必须在 return false 之前")


def assert_export_chain(field, accessor, label):
    """断言一个见证字段走满 7 处导出链，且 PM 处必须是 json << 写手。"""
    assert "uint64_t " + field + " = 0;" in BRIDGE_H, (label, "bridge.h 字段")
    assert "shadow::" + accessor + "();" in BRIDGE, \
        (label, "bridge.cpp 必须直接拉 atomic getter")
    assert "summary." + field + " =" in BRIDGE, (label, "bridge.cpp 摘要赋值")
    assert "uint64_t " + field + " = 0;" in HUB_H, (label, "hub.h 字段")
    assert "summary." + field + " =" in HUB, (label, "hub.cpp 摘要赋值")
    assert '{"' + field + '"' in HUB, (label, "hub.cpp json 字段")
    assert '{"' + field + '"' in CP, (label, "control_plane.cpp json 字段")
    assert '\\"' + field + '\\"' in PM, (label, "perf_monitor.cpp json 字段")
    assert 'json << "    \\"' + field + '\\": "' in PM, \
        (label, "PM 处必须是 json << 写手（不是仅出现字段名）")
    assert "runtimeSummary." + field in PM, (label, "PM 必须写 runtimeSummary 的值")
    for src_label, text in (("BRIDGE_H", BRIDGE_H), ("BRIDGE", BRIDGE), ("HUB_H", HUB_H),
                            ("HUB", HUB), ("CP", CP), ("PM", PM)):
        hits = [ln for ln in text.splitlines() if field in ln]
        assert len(hits) >= 1, (label, src_label, len(hits))
    # hub.cpp 必须同时提供摘要赋值与 json 字段，才是 7 处链路中的两处。
    hub_hits = [ln for ln in HUB.splitlines() if field in ln]
    assert len(hub_hits) >= 2, (label, "hub.cpp 必须同时有摘要赋值与 json 字段", len(hub_hits))
    assert "uint64_t " + accessor + "();" in CORE_H, (label, "core.h 访问器声明")


def product_sources():
    """产品源（排除测试目录与 *_test.*）：用于"不得再出现/只能出现在"这类全量扫描。"""
    suffixes = (".h", ".hpp", ".inl", ".cpp")
    result = []
    for path in sorted((ROOT / "src").rglob("*")):
        if not path.is_file() or path.suffix not in suffixes:
            continue
        if any(part in ("tests", "test") for part in path.parts):
            continue
        if path.name.endswith(("_test.cpp", "_test.h", "_test.hpp", "_test.inl")):
            continue
        result.append(path)
    return result


PRODUCT_SOURCES = product_sources()
assert len(PRODUCT_SOURCES) > 100, len(PRODUCT_SOURCES)

# ---- 1. 判定单一来源：轻量头定义 inline 纯函数，生产 TU 不再自带判定 ----
assert GATE_H.startswith("#pragma once"), "轻量头必须以 #pragma once 开头"
assert "#include <cstdint>" in GATE_H
assert "namespace dxvk::war3::shadow" in GATE_H
assert "enum class ShadowBuildAdvanceDecision" in GATE_H
assert re.search(r"inline\s+ShadowBuildAdvanceDecision\s+" + GATE_FN + r"\s*\(", GATE_H), \
    "判定必须是轻量头里的 inline 定义"
# 逐字语义：owner==0 ⇒ OwnerUnestablished；current!=owner ⇒ NotOwner；否则 Allow。
assert "if (ownerThreadId == 0u)" in GATE_H
assert "ShadowBuildAdvanceDecision::OwnerUnestablished;" in GATE_H
assert "if (currentThreadId != ownerThreadId)" in GATE_H
assert "ShadowBuildAdvanceDecision::NotOwner;" in GATE_H
assert "return ShadowBuildAdvanceDecision::Allow;" in GATE_H
# core.h 通过 include 保持可见，且不再自带 enum / 声明；core.cpp 不再有 out-of-line 定义。
assert '#include "war3_shadow_build_thread_gate.h"' in CORE_H
assert "enum class ShadowBuildAdvanceDecision" not in CORE_H, "enum 必须只在轻量头"
assert not re.search(r"ShadowBuildAdvanceDecision\s+" + GATE_FN + r"\s*\(", CORE_H), \
    "core.h 不得再声明判定函数"
assert not re.search(r"(?m)^" + GATE_FN + r"\s*\(", CORE), "core.cpp 不得再有 out-of-line 定义"
assert not re.search(r"(?m)^ShadowBuildAdvanceDecision\s+" + GATE_FN, CORE), \
    "core.cpp 不得再有 out-of-line 定义"

# ---- 2. 宿主机边界测试：所有者门 + 进度摘要状态机 + 生产共用生命周期 ----
assert TEST_SRC.is_file(), str(TEST_SRC)
assert "war3_shadow_build_thread_gate.h" in TEST_TEXT
assert GATE_FN + "(" in TEST_TEXT
assert "war3_shadow_build_thread_gate_test.cpp'" in MESON
assert "war3_shadow_build_thread_gate_test = executable(" in MESON
assert "'war3_shadow_build_thread_gate'," in MESON

assert PROGRESS_TEST_SRC.is_file(), str(PROGRESS_TEST_SRC)
PROGRESS_TEST_TEXT = PROGRESS_TEST_SRC.read_text(encoding="utf-8")
# 只允许包含产品轻量头与两个标准头（照抄既有宿主测试的边界口径）。
PROGRESS_TEST_INCLUDES = re.findall(r'#include\s*[<"]([^">]+)[">]',
                                    PROGRESS_TEST_TEXT)
assert PROGRESS_TEST_INCLUDES == [
    "../../shadow/war3_shadow_build_progress.h", "cstdint", "cstdio"
], PROGRESS_TEST_INCLUDES
assert PROGRESS_TYPE in PROGRESS_TEST_TEXT
for method in ("BeginBuild(", "Publish(", "Complete(", "Cancel(", "Reset()"):
    assert method in PROGRESS_TEST_TEXT, ("进度状态机宿主机测试缺少用例", method)
assert "war3_shadow_build_progress_test.cpp'" in MESON
assert "war3_shadow_build_progress_test = executable(" in MESON
assert re.search(r"(?m)^test\(\s*$", MESON)
assert "'war3_shadow_build_progress'," in MESON
assert re.search(r"'war3_shadow_build_progress',\s*\n\s*war3_shadow_build_progress_test,\s*\n\s*timeout\s*:\s*30,\s*\n\)", MESON), \
    "新测试必须以 test('war3_shadow_build_progress', ..., timeout: 30) 登记"

# 2b. 生产共用的推进 / 发布生命周期测试：只包含产品组件头 + 标准头（无 d3d9 / 游戏状态源）。
assert LIFECYCLE_TEST_SRC.is_file(), str(LIFECYCLE_TEST_SRC)
LIFECYCLE_TEST_TEXT = LIFECYCLE_TEST_SRC.read_text(encoding="utf-8")
LIFECYCLE_TEST_INCLUDES = re.findall(r'#include\s*[<"]([^">]+)[">]',
                                     LIFECYCLE_TEST_TEXT)
# <mutex> 是 std::unique_lock 的正式归属头（libstdc++ 的 <shared_mutex> 不提供它），
# 而 C8 的写侧必须与运行时同模式使用 unique_lock，故白名单包含这一个必需标准头。
# 上级 03:58 裁定："接受固定次数测试，不要求凑满一秒；同时允许 <chrono> 用于计时或超时"；
# 因此白名单再加入 <chrono>（运行器超时保护）与 <cstdlib>（超时时 std::_Exit 立即终止）。
assert LIFECYCLE_TEST_INCLUDES == [
    "../../shadow/war3_shadow_build_lifecycle.h",
    "atomic", "cstdint", "cstdio", "mutex", "chrono", "cstdlib",
    "shared_mutex", "thread",
], LIFECYCLE_TEST_INCLUDES
# C8 必须同时具备：共同开始握手、交错证据、以及超时保护（上级 03:58 要求）。
for needle, label in (("startGate", "C8 必须有共同开始握手"),
                      ("intermediateObservations", "C8 必须证明读写实际交错"),
                      ("watchdog", "C8 必须有运行器超时保护"),
                      ("std::_Exit(2)", "超时必须立即失败而不是继续等待")):
    assert needle in LIFECYCLE_TEST_TEXT, label
for needle, label in ((LIFECYCLE_TYPE, "必须直接实例化生产组件"),
                      ("consumeAllowed(", "必须调用组件的消费权限检查"),
                      ("beginBuild(", "必须覆盖正常推进"),
                      # 底层代际门已 private ⇒ 测试只能走公开的 *IfCurrent 入口。
                      ("publishIfCurrent(", "必须覆盖成对发布"),
                      ("completeIfCurrent(", "必须覆盖成对完成"),
                      ("cancelIfCurrent(", "必须覆盖成对取消"),
                      ("reset()", "必须覆盖 Reset"),
                      ("hasValues()", "必须覆盖摘要可见性"),
                      ("values()", "必须覆盖摘要读取"),
                      ("std::shared_mutex", "并发摘要读取必须用 shared_mutex"),
                      ("std::shared_lock", "读侧必须 shared_lock（与运行时同模式）"),
                      ("std::unique_lock", "写侧必须 unique_lock（与运行时同模式）"),
                      ("std::thread", "并发摘要读取必须真起线程")):
    assert needle in LIFECYCLE_TEST_TEXT, ("生命周期宿主机测试缺少覆盖", label, needle)
# 未知所有者 / 两种入口 / 非所有者 / 旧工作不得重新发布 都要有独立用例。
for label in ("unknown-owner", "direct-entry", "non-owner",
              "old-generation", "reset", "cancel", "snapshot-self-consistency"):
    assert label in LIFECYCLE_TEST_TEXT, ("生命周期宿主机测试缺少用例分组", label)
assert "war3_shadow_build_lifecycle_test.cpp'" in MESON
assert "war3_shadow_build_lifecycle_test = executable(" in MESON
assert "'war3_shadow_build_lifecycle'," in MESON
assert re.search(r"'war3_shadow_build_lifecycle',\s*\n\s*war3_shadow_build_lifecycle_test,\s*\n\s*timeout\s*:\s*60,\s*\n\)", MESON), \
    "新测试必须以 test('war3_shadow_build_lifecycle', ..., timeout: 60) 登记"

# 2c. 接线变更后的新接口必须在宿主测试里被**真实调用**：不能只测 Publish(oldGeneration) 返回 false。
for needle, label in (("beginBuild(&", "beginBuild 必须传入测试内工作对象地址（工作身份）"),
                      ("isCurrent(", "必须覆盖工作身份核验"),
                      ("publishIfCurrent(", "必须覆盖成对发布的接线路径"),
                      ("completeIfCurrent(", "必须覆盖成对完成的接线路径"),
                      ("nullptr", "必须覆盖空工作身份")):
    assert needle in LIFECYCLE_TEST_TEXT, ("生命周期宿主机测试缺少新接口覆盖", label, needle)
assert LIFECYCLE_TEST_TEXT.count("beginBuild(&") >= 5, LIFECYCLE_TEST_TEXT.count("beginBuild(&")
# A/B 实际接线回归必须存在，并按上级指定顺序：A 发布 -> Reset -> B 开始 -> B 发布 -> A 尝试。
for fn in ("RunAbWiringResetRegressionCases", "RunAbWiringReplacementRegressionCases"):
    assert fn in LIFECYCLE_TEST_TEXT and fn + "();" in LIFECYCLE_TEST_TEXT, \
        ("生命周期宿主机测试缺少 A/B 接线回归用例", fn)
for label in ("A-publish-after-B-rejected", "A-complete-after-B-rejected",
              "B-still-current-after-A-attempts", "B-values-equal-published-valuesB",
              "same-work-stale-generation-publish-rejected",
              "null-work-is-current-rejected"):
    assert label in LIFECYCLE_TEST_TEXT, ("生命周期宿主机测试缺少 A/B 接线回归断言", label)
AB_ORDER = ("C9.A-publish-accepted", "C9.building-false-after-reset",
            "C9.B-begin-build-generation-differs-from-A", "C9.B-publish-accepted",
            "C9.A-publish-after-B-rejected", "C9.A-complete-after-B-rejected")
AB_OFFSETS = [LIFECYCLE_TEST_TEXT.index(tok) for tok in AB_ORDER]
assert AB_OFFSETS == sorted(AB_OFFSETS), ("A/B 回归的动作顺序不符", AB_ORDER, AB_OFFSETS)

# ---- 3. 唯一一份消费权限检查：组件内判定 + 运行时入口只做转发 ----
LATEST_SIG = "void ShadowValidationRuntime::ensureLatestFrameBuilt() {"
CONTRACT_SIG = "void ShadowValidationRuntime::ensureFrameBuiltForContract("
PERM_SIG = "bool ShadowValidationRuntime::consumePermissionGranted("
LATEST_BODY, LATEST_START = extract_function(CORE, LATEST_SIG)
CONTRACT_BODY, CONTRACT_START = extract_function(CORE, CONTRACT_SIG)
PERM_BODY, PERM_START = extract_function(CORE, PERM_SIG)
PERM_END = PERM_START + len(PERM_BODY)

assert CORE.count(PERM_SIG + "bool directEntry) {") == 1, "消费权限检查必须只有一份定义"
assert "bool " + PERM_FN + "(bool directEntry);" in CORE_H, "core.h 必须声明唯一入口"

CORE_MASK = mask_code(CORE)
CORE_LOCK_SCOPES = lock_scopes(CORE_MASK)
assert CORE_LOCK_SCOPES, "产品源必须存在 std::unique_lock 临界区"

# 3a. DecideShadowBuildAdvance 在产品源中只允许出现在组件 consumeAllowed 内。
assert LIFECYCLE_H.startswith("#pragma once")
assert 'namespace dxvk::war3::shadow' in LIFECYCLE_H
assert '#include "war3_shadow_build_thread_gate.h"' in LIFECYCLE_H, \
    "组件必须复用唯一判定头"
assert '#include "war3_shadow_build_progress.h"' in LIFECYCLE_H, \
    "组件必须复用按值进度状态机"
assert "#include <atomic>" in LIFECYCLE_H
assert "struct ShadowBuildAdvanceCounters" in LIFECYCLE_H
for counter_member in ("offThreadRefused", "ownerUnestablishedRefused",
                       "directAdvanceRefused"):
    assert "std::atomic<uint64_t>* " + counter_member + " = nullptr;" in LIFECYCLE_H, \
        ("计数下沉结构缺少原子指针成员", counter_member)
assert "class " + LIFECYCLE_TYPE in LIFECYCLE_H
CONSUME_SIG = "bool consumeAllowed(bool directEntry,"
CONSUME_BODY, CONSUME_START = extract_function(LIFECYCLE_H, CONSUME_SIG)
CONSUME_END = CONSUME_START + len(CONSUME_BODY)

LIFECYCLE_H_MASK = mask_code(LIFECYCLE_H)
component_gate_calls = [m.start() for m in
                        re.finditer(re.escape(GATE_FN + "("), LIFECYCLE_H_MASK)]
assert len(component_gate_calls) == 1, \
    ("组件内判定只允许有一处调用", len(component_gate_calls))
assert CONSUME_START <= component_gate_calls[0] < CONSUME_END, \
    "判定调用必须位于 consumeAllowed 内"
for path in PRODUCT_SOURCES:
    if path in (CORE_PATH, GATE_H_PATH, LIFECYCLE_H_PATH):
        continue
    text = path.read_text(encoding="utf-8", errors="replace")
    n = mask_code(text).count(GATE_FN + "(")
    assert n == 0, ("判定函数只允许在组件 consumeAllowed 内出现", str(path), n)
assert GATE_FN + "(" not in CORE_MASK, \
    "运行时不得再直接调用判定（必须经 m_buildLifecycle.consumeAllowed）"

# 3e. 组件的工作身份接口（2026-09-17 接线变更）：beginBuild 绑定工作对象身份；isCurrent /
#     publishIfCurrent / completeIfCurrent 是"旧工作不得回写"的唯一规则；reset() 清空身份。
assert "beginBuild(const void* work" in LIFECYCLE_H, \
    "组件必须提供 beginBuild(const void* work, uint64_t, uint64_t)"
assert re.search(r"bool\s+isCurrent\(\s*const\s+void\*\s+work\s*,\s*uint64_t\s+generation\s*\)\s*const",
                 LIFECYCLE_H), "组件必须提供 isCurrent(const void*, uint64_t) const"
assert re.search(r"bool\s+publishIfCurrent\(\s*const\s+void\*\s+work\s*,\s*uint64_t\s+generation\s*,"
                 r"\s*const\s+ShadowBuildProgressValues&\s+values\s*\)", LIFECYCLE_H), \
    "组件必须提供 publishIfCurrent(const void*, uint64_t, const ShadowBuildProgressValues&)"
assert re.search(r"bool\s+completeIfCurrent\(\s*const\s+void\*\s+work\s*,\s*uint64_t\s+generation\s*\)",
                 LIFECYCLE_H), "组件必须提供 completeIfCurrent(const void*, uint64_t)"
assert "m_currentWork = work;" in LIFECYCLE_H, "beginBuild 必须绑定工作对象身份"
# isCurrent 必须同时要求"非空工作身份"与"代际非 0 且等于当前代际"。
assert "work != nullptr && work == m_currentWork" in LIFECYCLE_H, \
    "isCurrent 必须要求非空工作身份且与当前工作相同（空工作不得当作当前）"
assert "generation != 0u && generation == m_progress.generation()" in LIFECYCLE_H, \
    "isCurrent 必须要求代际非 0 且等于当前代际"
# publishIfCurrent / completeIfCurrent 必须以 isCurrent 为唯一前置门（不得复制第二套规则）。
PIF_BODY, _ = extract_function(LIFECYCLE_H, "bool publishIfCurrent(")
CIF_BODY, _ = extract_function(LIFECYCLE_H, "bool completeIfCurrent(")
assert "if (!isCurrent(work, generation))" in mask_code(PIF_BODY), \
    "publishIfCurrent 必须先核验 isCurrent（旧工作不得回写）"
assert "if (!isCurrent(work, generation))" in mask_code(CIF_BODY), \
    "completeIfCurrent 必须先核验 isCurrent（旧工作不得完成）"
# reset() 必须同时清空工作身份与代际状态。
RESET_BODY, _ = extract_function(LIFECYCLE_H, "void reset()")
assert "m_currentWork = nullptr;" in mask_code(RESET_BODY), \
    "reset() 必须清空当前工作身份 m_currentWork"
assert "m_progress.Reset();" in mask_code(RESET_BODY), "reset() 必须重置代际状态机"

# 3b. 运行时的唯一入口：构造计数下沉结构并转发给组件；不得自带判定 / 计数。
PERM_MASK = mask_code(PERM_BODY)
assert "ShadowBuildAdvanceCounters counters;" in PERM_MASK, \
    "消费权限检查必须构造计数下沉结构"
assert "counters.offThreadRefused = &" + OFF_THREAD_COUNTER + ";" in PERM_MASK
assert "counters.ownerUnestablishedRefused =" in PERM_MASK and \
    OWNER_UNESTABLISHED_COUNTER in PERM_MASK
assert "counters.directAdvanceRefused =" in PERM_MASK and \
    DIRECT_ADVANCE_COUNTER in PERM_MASK
assert LIFECYCLE_MEMBER + ".consumeAllowed(" in PERM_MASK, \
    "消费权限检查必须调用 " + LIFECYCLE_MEMBER + ".consumeAllowed("
assert re.search(r"return\s+" + re.escape(LIFECYCLE_MEMBER) +
                 r"\.consumeAllowed\(\s*directEntry,", PERM_MASK), \
    "组件调用必须以 directEntry 为首参（两种入口分类下沉到组件）"
assert "dxvk::war3::hooks::GetMainLoopThreadId()" in PERM_MASK
assert "::GetCurrentThreadId()" in PERM_MASK
assert GATE_FN not in PERM_MASK, "运行时入口不得自带第二份判定"
assert ".fetch_add(" not in PERM_MASK, "运行时入口不得自带第二份拒绝计数"
assert_no_thread_id_bypass(PERM_BODY, PERM_FN)

# 3c. 组件拒绝路径：Allow 先放行；三个拒绝计数按 relaxed 自增；单一 return false。
assert_component_refusal_counting(CONSUME_BODY, "ShadowBuildLifecycle::consumeAllowed")
# directEntry 只允许用于"直接入口另计一份拒绝数"的守卫：不得参与放行/拒绝判定，
# 否则两种入口的判定规则会分叉（分类维度与判定维度必须分开）。
assert mask_code(CONSUME_BODY).count("directEntry") == 1, \
    "directEntry 只允许用于直接入口计数的守卫"

# 3d. 两处推进入口：调用同一份检查，且早于本函数内首次加锁 / 分块推进。
ENTRY_GATE_AT = {}
for label, body in (("ensureLatestFrameBuilt", LATEST_BODY),
                    ("ensureFrameBuiltForContract", CONTRACT_BODY)):
    body_mask = mask_code(body)
    gate_at = body_mask.find(PERM_FN + "(")
    assert gate_at != -1, (label, "推进入口必须调用唯一消费权限检查 " + PERM_FN)
    ENTRY_GATE_AT[label] = gate_at
    for anchor in ("std::unique_lock", "m_mutex", "buildFrameChunk"):
        idx = body_mask.find(anchor)
        assert idx == -1 or idx > gate_at, (label, anchor, idx, gate_at)
    # 入口不得复制判定/计数规则，也不得自行比较线程 id 绕过唯一入口。
    assert "decision" not in body_mask, (label, "入口不得复制判定结果分支")
    # 入口可以用 m_buildLifecycle 维护生命周期（beginBuild/cancel/…），但**不得**直接调用
    # 组件的消费权限检查，否则就会绕开唯一的 consumePermissionGranted 转发点。
    for duplicated in COUNTERS + (LIFECYCLE_MEMBER + ".consumeAllowed(",
                                  "GetMainLoopThreadId", "GetCurrentThreadId"):
        assert duplicated not in body_mask, (label, "入口不得复制消费权限规则", duplicated)
    assert_no_thread_id_bypass(body, label)

assert "if (!" + PERM_FN + "(false))" in mask_code(LATEST_BODY), \
    "ensureLatestFrameBuilt 必须以入口门调用消费权限检查"
assert "if (!" + PERM_FN + "(true))" in mask_code(CONTRACT_BODY), \
    "ensureFrameBuiltForContract 必须以直接底层门调用同一份消费权限检查"

# 门必须先于全文首次 m_core.buildFrameChunk 调用点。
chunk_at = CORE.index("m_core.buildFrameChunk(")
assert LATEST_START + ENTRY_GATE_AT["ensureLatestFrameBuilt"] < chunk_at
assert CONTRACT_START + ENTRY_GATE_AT["ensureFrameBuiltForContract"] < chunk_at

# ---- 4. 计数器本体 + 访问器：必须是 std::atomic，不得是普通可变全局 ----
for name in COUNTERS:
    assert "static std::atomic<uint64_t> " + name + "{0};" in CORE, name
    for bad in ("static uint64_t " + name + " = 0;",
                "static uint64_t " + name + "{0};",
                "uint64_t " + name + " = 0;",
                "static volatile uint64_t " + name):
        assert bad not in CORE, (name, bad)
    # 任何对该名字的非原子声明都不能出现：定义行必须恰好一条且必须是 std::atomic 形态。
    decl_lines = [ln for ln in CORE.splitlines()
                  if re.search(r"\b(static\s+)?(std::atomic<uint64_t>|uint64_t|"
                               r"volatile\s+uint64_t)\s+" + re.escape(name) + r"\b", ln)]
    assert len(decl_lines) == 1, (name, decl_lines)
    assert "std::atomic<uint64_t> " + name in decl_lines[0], (name, decl_lines[0])
    # 计数器只允许出现在三处：自身定义行、自身访问器、唯一入口内（取地址交给组件）。
    # 判定与自增都在组件里，运行时入口不得自己计数。
    for m in re.finditer(re.escape(name), CORE_MASK):
        at = m.start()
        line_start = CORE_MASK.rfind("\n", 0, at) + 1
        line_end = CORE_MASK.find("\n", at)
        if line_end == -1:
            line_end = len(CORE_MASK)
        line = CORE_MASK[line_start:line_end]
        if "std::atomic<uint64_t> " + name in line:
            continue  # 自身定义行
        if "return " + name + ".load(" in line:
            continue  # 自身访问器
        assert PERM_START <= at < PERM_END, \
            (name, "计数器只允许在定义 / 访问器 / 唯一入口内出现", line.strip())

for counter, accessor in zip(COUNTERS, ACCESSORS):
    assert "uint64_t " + accessor + "();" in CORE_H, accessor
    assert "uint64_t " + accessor + "() {" in CORE, accessor
    assert re.search(re.escape(counter) + r"\.load\(\s*std::memory_order_relaxed\s*\)",
                     CORE), (counter, accessor)

# 旧"主循环未建立即放行"计数器 / 访问器不得再存在于任何产品源（注释也算）。
LEGACY_SOURCES = {
    "GATE_H": GATE_H, "PROGRESS_H": PROGRESS_H, "LIFECYCLE_H": LIFECYCLE_H,
    "CORE": CORE, "CORE_H": CORE_H,
    "BRIDGE_H": BRIDGE_H, "BRIDGE": BRIDGE, "HUB_H": HUB_H, "HUB": HUB,
    "CP": CP, "PM": PM,
}
for label, text in LEGACY_SOURCES.items():
    hit = LEGACY_COUNTER.search(text)
    assert hit is None, (label, hit.group(0) if hit else "")

# ---- 5. 导出链：三个见证计数各自必须走满 7 处 ----
for field, accessor in EXPORT_CHAIN:
    assert_export_chain(field, accessor, field)
# 新计数必须写明"进程累计值"与"不得未经证明相加成总拒绝"，且措辞紧邻字段声明。
NEW_COUNTER_DECL_AT = BRIDGE_H.index("uint64_t " + OWNER_UNESTABLISHED_FIELD + " = 0;")
NEW_COUNTER_COMMENT = BRIDGE_H[max(0, NEW_COUNTER_DECL_AT - 400):NEW_COUNTER_DECL_AT]
for phrase in ("进程累计", "不得未经证明相加成", "总拒绝"):
    assert phrase in NEW_COUNTER_COMMENT, \
        ("bridge.h 新计数导出注释缺少措辞", phrase)

# ---- 6. 进度摘要状态机本体：按值、无可变别名、无每块堆分配 ----
assert PROGRESS_H.startswith("#pragma once")
assert "#include <cstdint>" in PROGRESS_H
assert "namespace dxvk::war3::shadow" in PROGRESS_H
PROGRESS_H_MASK = mask_code(PROGRESS_H)
for forbidden_carrier in ("std::shared_ptr", "std::make_shared", "std::unique_ptr",
                          "std::vector", "malloc(", "operator new"):
    assert forbidden_carrier not in PROGRESS_H_MASK, \
        ("按值状态机不得引入堆分配 / 可变别名载体", forbidden_carrier)
assert "struct ShadowBuildProgressValues" in PROGRESS_H
for field in ("workGeneration", "frameSerial", "publishRevision", "nextRecordIndex",
              "recordCount", "chunkCount", "totalBuildDurationUs", "drawCount"):
    assert field in PROGRESS_H, field
assert "uint64_t BeginBuild(uint64_t frameSerial, uint64_t publishRevision)" in PROGRESS_H
assert "bool Publish(uint64_t generation, const ShadowBuildProgressValues& values)" in PROGRESS_H
assert "bool Complete(uint64_t generation)" in PROGRESS_H
assert "bool Cancel(uint64_t generation)" in PROGRESS_H
assert "void Reset()" in PROGRESS_H
# 读取器只暴露 const 引用（值语义载体，不是共享工作对象）。
assert "const ShadowBuildProgressValues& values() const" in PROGRESS_H
# 代际守卫：Publish / Complete 都必须同时要求 building 与当前代际；且代际不得回绕到 0。
assert PROGRESS_H.count("if (!m_building || generation == 0u || generation != m_generation)") >= 2
assert "m_values = values;" in PROGRESS_H, "发布必须是按值拷贝"
assert "++m_generation;" in PROGRESS_H and "m_generation == 0u" in PROGRESS_H, \
    "代际必须 +1 且显式跳过 0"

# 6b. 生命周期组件本体：按值持有状态机、不引入堆分配 / 可变别名载体。
for forbidden_carrier in ("std::shared_ptr", "std::make_shared", "std::unique_ptr",
                          "std::vector", "malloc(", "operator new"):
    assert forbidden_carrier not in LIFECYCLE_H_MASK, \
        ("生命周期组件不得引入堆分配 / 可变别名载体", forbidden_carrier)
assert PROGRESS_TYPE + " m_progress;" in LIFECYCLE_H, \
    "组件必须以按值成员持有进度状态机"
for method in ("beginBuild(", "publish(", "complete(", "cancel(", "reset()",
               "building()", "hasValues()", "generation()", "values()"):
    assert method in LIFECYCLE_H, ("生命周期组件缺少方法", method)

# 产品源：旧 shared_ptr 载体不得再出现，也不得为进度摘要 / 生命周期组件分配堆对象。
for path in PRODUCT_SOURCES:
    text = path.read_text(encoding="utf-8", errors="replace")
    assert LEGACY_PROGRESS_MEMBER not in text, (str(path), LEGACY_PROGRESS_MEMBER)
    masked = mask_code(text)
    for heap_type in HEAP_CARRIER_TYPES:
        assert not re.search(r"std::make_shared<\s*[^>]*" + heap_type, masked), \
            (str(path), "摘要 / 生命周期组件不得使用 make_shared 别名载体", heap_type)
        assert not re.search(r"std::shared_ptr<\s*[^>]*" + heap_type, masked), \
            (str(path), "摘要 / 生命周期组件不得使用 shared_ptr 别名载体", heap_type)
        assert not re.search(r"std::unique_ptr<\s*[^>]*" + heap_type, masked), \
            (str(path), "摘要 / 生命周期组件不得使用 unique_ptr 别名载体", heap_type)

# 按值成员 + 工作代际必须在产品头里成立，且不得退化成智能指针成员。
assert re.search(r"\b" + LIFECYCLE_TYPE + r"\s+" + LIFECYCLE_MEMBER + r"\s*;", CORE_H), \
    "必须以按值成员 " + LIFECYCLE_TYPE + " " + LIFECYCLE_MEMBER
assert not re.search(r"shared_ptr<[^>]*>\s*" + LIFECYCLE_MEMBER, CORE_H), \
    "生命周期组件成员不得是 shared_ptr 别名"
assert not re.search(r"\b" + PROGRESS_TYPE + r"\s+" + PROGRESS_MEMBER + r"\s*;", CORE_H), \
    "运行时不直接持有进度状态机（必须收进生命周期组件）"
assert '#include "war3_shadow_build_lifecycle.h"' in CORE_H
assert "uint64_t m_buildWorkGeneration" in CORE_H
assert "m_buildWorkGeneration" in CORE

# ---- 7. 生命周期齐全 + 同一次发布 ----
for method in ("beginBuild(", "publish(", "complete(", "cancel(", "reset()"):
    assert method in LIFECYCLE_H, ("生命周期组件缺少更新点", method)

# 7a. 接线变更（2026-09-17）：运行时**不得**再直接调用底层代际门 publish / complete / cancel；
#     每块发布 / 完成 / 取消都必须走 (work, generation) 成对携带的 *IfCurrent 版本。
for forbidden in (LIFECYCLE_MEMBER + ".publish(", LIFECYCLE_MEMBER + ".complete(",
                  LIFECYCLE_MEMBER + ".cancel("):
    assert forbidden not in CORE_MASK, \
        ("运行时不得再直接调用底层代际门（必须成对携带 work + 其固定代际）", forbidden)
for method, needle in (("beginBuild", LIFECYCLE_MEMBER + ".beginBuild("),
                       ("publishIfCurrent", LIFECYCLE_MEMBER + ".publishIfCurrent("),
                       ("completeIfCurrent", LIFECYCLE_MEMBER + ".completeIfCurrent("),
                       ("cancelIfCurrent", LIFECYCLE_MEMBER + ".cancelIfCurrent("),
                       ("isCurrent", LIFECYCLE_MEMBER + ".isCurrent("),
                       ("reset", LIFECYCLE_MEMBER + ".reset()")):
    assert needle in CORE_MASK, ("进度摘要生命周期缺少运行时更新点", method)

# 7d. 底层代际门必须 **private 且只声明一次**（2026-09-17 03:58 上级裁定）。
#     public 段里再声明一次同名同签名成员是**重复声明**（编译错误），因此这里按签名文本断言
#     "恰好出现一次、且位于 private: 之后、且 private 段到它之间不得再出现 public:"。
PUBLIC_AT = LIFECYCLE_H.index(" public:")
PRIVATE_AT = LIFECYCLE_H.index("\n private:")
assert PUBLIC_AT < PRIVATE_AT, "组件必须先有 public 段、再有 private 段"
LOW_LEVEL_SIGNATURES = (
    ("bool publish(uint64_t generation, const ShadowBuildProgressValues& values)",
     "publish"),
    ("bool complete(uint64_t generation)", "complete"),
    ("bool cancel(uint64_t generation)", "cancel"),
)
for signature, label in LOW_LEVEL_SIGNATURES:
    occurrences = LIFECYCLE_H.count(signature)
    assert occurrences == 1, \
        ("底层代际门必须恰好声明一次（public 复制=重复声明，编译失败）", label, occurrences)
    at = LIFECYCLE_H.index(signature)
    assert at > PRIVATE_AT, ("底层代际门必须在 private: 之后（不得为 public）", label)
    assert " public:" not in LIFECYCLE_H[PRIVATE_AT:at], \
        ("底层代际门必须留在 private 段内", label)
# 唯一合法的对外入口必须公开、且携带 (work, generation) 成对身份。
for signature, label in (
        ("bool publishIfCurrent(const void* work, uint64_t generation,", "publishIfCurrent"),
        ("bool completeIfCurrent(const void* work, uint64_t generation) {", "completeIfCurrent"),
        ("bool cancelIfCurrent(const void* work, uint64_t generation) {", "cancelIfCurrent")):
    assert signature in LIFECYCLE_H, ("组件缺少公开的成对入口", label)
    assert LIFECYCLE_H.index(signature) < PRIVATE_AT, \
        ("*IfCurrent 必须是公开入口（放进 private 段调用者就无法发布）", label)
assert "cancelIfCurrent(" in LIFECYCLE_H, "组件必须提供 cancelIfCurrent("

# 7e. isCurrent 必须额外要求"构建仍在进行中"：完成 / 取消之后不得再被当作当前工作回写。
IS_CURRENT_SIG = "bool isCurrent(const void* work, uint64_t generation) const {"
assert IS_CURRENT_SIG in LIFECYCLE_H, "组件缺少 isCurrent"
IS_CURRENT_BODY, _ = extract_function(LIFECYCLE_H, IS_CURRENT_SIG)
IS_CURRENT_CODE = mask_code(IS_CURRENT_BODY)
assert "work != nullptr && work == m_currentWork" in IS_CURRENT_CODE, \
    "isCurrent 必须同时校验工作对象身份"
assert "m_progress.building()" in IS_CURRENT_CODE, \
    "isCurrent 必须额外要求 m_progress.building()（终态后不得回写）"

# 7f. 终态后的 isCurrent 断言必须真的存在于宿主机测试里；且测试不得直接调用已 private 的代际门。
for label in ("C4.is-current-false-after-complete", "C5.is-current-false-after-cancel",
              "C6.is-current-false-after-cancel", "C7.is-current-false-after-complete"):
    assert label in LIFECYCLE_TEST_TEXT, \
        ("生命周期宿主机测试缺少终态后 isCurrent 必须为 false 的断言", label)
for forbidden in ("lifecycle.publish(", "lifecycle.complete(", "lifecycle.cancel("):
    assert forbidden not in LIFECYCLE_TEST_TEXT, \
        ("宿主机测试不得直接调用已 private 的底层代际门", forbidden)
assert re.search(r"m_buildWorkGeneration\s*=\s*" + re.escape(LIFECYCLE_MEMBER) +
                 r"\.beginBuild\(", CORE_MASK), "工作代际必须由组件 beginBuild 分配"
# beginBuild 必须绑定工作对象身份（m_buildWork.get()），不得退化成只分配代际。
assert re.search(r"m_buildWorkGeneration\s*=\s*" + re.escape(LIFECYCLE_MEMBER) +
                 r"\.beginBuild\(\s*m_buildWork\.get\(\)", CORE_MASK), \
    "beginBuild 必须携带工作对象指针 m_buildWork.get()（否则旧工作可被配上新 token）"
assert "m_buildWorkGeneration = 0u;" in CORE_MASK, "Reset 必须清零工作代际"

# 发布辅助函数：要求持锁调用；values 的帧号 / 发布 revision 来自同一次 work，统计同样来自它。
HELPER_SIG = "void ShadowValidationRuntime::publishBuildProgressLocked("
HELPER_BODY, HELPER_START = extract_function(CORE, HELPER_SIG)
HELPER_MASK = mask_code(HELPER_BODY)
assert re.search(
    r"void\s+ShadowValidationRuntime::publishBuildProgressLocked\(\s*"
    r"const\s+ShadowValidationBuildWork&\s+work\s*,\s*uint64_t\s+generation\s*\)",
    CORE_MASK), "发布辅助函数必须以 (work, generation) 为参数"
assert "values.workGeneration = generation;" in HELPER_MASK
assert "values.frameSerial =" in HELPER_MASK and "work.manifest->frameSerial" in HELPER_MASK
assert "values.publishRevision =" in HELPER_MASK and "work.manifest->publishRevision" in HELPER_MASK
assert "if (" + LIFECYCLE_MEMBER + \
    ".publishIfCurrent(&work, generation, values))" in HELPER_MASK, \
    "每块发布必须走 m_buildLifecycle.publishIfCurrent(&work, generation, values)（旧工作不得回写）"
assert re.search(r"if\s*\(\s*" + re.escape(LIFECYCLE_MEMBER) +
                 r"\.publishIfCurrent\(\s*&work\s*,\s*generation\s*,\s*values\s*\)\s*\)\s*" +
                 re.escape(PROGRESS_STATS_NAME) + r"\s*=\s*work\.stats\s*;", HELPER_MASK), \
    "统计必须与摘要同一次发布（publishIfCurrent 成功后才写 m_publishedBuildStats）"

# Publish 与 m_publishedBuildStats 必须处于同一临界区（判定方式见模块 docstring）。
publish_calls = [m.start() for m in re.finditer(re.escape(PROGRESS_PUBLISH_CALL), CORE_MASK)]
assert len(publish_calls) >= 3, ("开始 / 每块 / 完成都要发布", len(publish_calls))
for at in publish_calls:
    assert in_lock_scope(CORE_LOCK_SCOPES, at), ("发布必须在 m_mutex 临界区内", at)
assert_same_critical_section(CORE_MASK, LIFECYCLE_MEMBER + ".publishIfCurrent(",
                             PROGRESS_STATS_NAME + " =",
                             "publishIfCurrent 与 m_publishedBuildStats 同一次发布")
# 开始构建：beginBuild 与紧随其后的首次发布也必须在同一临界区（读者不会看到 building 但无值）。
assert_same_critical_section(CORE_MASK, LIFECYCLE_MEMBER + ".beginBuild(",
                             PROGRESS_PUBLISH_CALL,
                             "beginBuild 与首次发布同临界区")
# 完成 / 取消 / Reset 都要同步清理配对统计。
for token, label in ((LIFECYCLE_MEMBER + ".completeIfCurrent(", "Complete"),
                     (LIFECYCLE_MEMBER + ".cancelIfCurrent(", "Cancel"),
                     (LIFECYCLE_MEMBER + ".reset()", "Reset")):
    assert_same_critical_section(CORE_MASK, token, PROGRESS_STATS_NAME + " = {}",
                                 label + " 与配对统计清理同临界区")
assert CORE_MASK.count(PROGRESS_STATS_NAME + " = {}") >= 3, \
    "完成 / 取消 / Reset 都要清理配对统计"

# 7b. 完成块：**先核验、再修改任何对外结果**（按源码偏移判定）。
#     上级原话：完成块原来先写 m_lastStats/m_lastFrame；现在必须先核验 (work, generation)
#     仍是当前工作，失败则不更新摘要、部分帧、最终帧。
COMPLETE_CALL_AT = CORE_MASK.index(LIFECYCLE_MEMBER + ".completeIfCurrent(")
COMPLETE_SCOPES = [s for s in CORE_LOCK_SCOPES if s[0] <= COMPLETE_CALL_AT <= s[1]]
assert len(COMPLETE_SCOPES) == 1, \
    ("完成块必须位于唯一一个 unique_lock 临界区内", len(COMPLETE_SCOPES))
COMPLETE_SCOPE_START, COMPLETE_SCOPE_END = COMPLETE_SCOPES[0]
IS_CURRENT_PAIR = "isCurrent(buildWork.get(), buildWorkGeneration)"
COMPLETE_GATE_AT = CORE_MASK.rfind(IS_CURRENT_PAIR, COMPLETE_SCOPE_START, COMPLETE_CALL_AT)
assert COMPLETE_GATE_AT != -1, \
    "完成块必须在修改任何对外结果之前核验 " + IS_CURRENT_PAIR
COMPLETE_PUBLISH_AT = CORE_MASK.find(PROGRESS_PUBLISH_CALL, COMPLETE_GATE_AT)
assert COMPLETE_PUBLISH_AT != -1 and COMPLETE_PUBLISH_AT < COMPLETE_SCOPE_END, \
    "完成块必须在核验通过后才发布终值（且在同一临界区内）"
LAST_STATS_AT = CORE_MASK.find("m_lastStats = buildWork->stats;", COMPLETE_GATE_AT)
LAST_FRAME_AT = CORE_MASK.find("m_lastFrame = completedFrame;", COMPLETE_GATE_AT)
assert LAST_STATS_AT != -1, "完成块必须写 m_lastStats"
assert LAST_FRAME_AT != -1, "完成块必须写 m_lastFrame"
assert COMPLETE_GATE_AT < COMPLETE_PUBLISH_AT < LAST_STATS_AT < LAST_FRAME_AT, \
    "完成块必须**先**核验 isCurrent，**再**发布摘要并写 m_lastStats / m_lastFrame"
assert LAST_FRAME_AT < COMPLETE_SCOPE_END, "最终帧写入必须与核验处于同一临界区"
# 部分帧同样"先核验、再写入"（失败则不更新部分帧）。
PARTIAL_GATE_AT = CORE_MASK.find(IS_CURRENT_PAIR)
assert PARTIAL_GATE_AT != -1 and PARTIAL_GATE_AT < COMPLETE_GATE_AT, \
    "部分帧写入前必须先核验 isCurrent（且该核验早于完成块）"
PARTIAL_FRAME_AT = CORE_MASK.find("m_lastRenderableFrame = std::move(partialFrame);",
                                  PARTIAL_GATE_AT)
assert PARTIAL_FRAME_AT != -1 and PARTIAL_GATE_AT < PARTIAL_FRAME_AT, \
    "部分帧写入必须晚于 isCurrent 核验"
assert in_lock_scope(CORE_LOCK_SCOPES, PARTIAL_FRAME_AT), "部分帧写入必须在锁内"

# 7c. 锁内成对取得 (work, generation)：ensureFrameBuiltForContract 的 m_buildInProgress 分支。
CONTRACT_MASK = mask_code(CONTRACT_BODY)
IN_PROGRESS_AT = CONTRACT_MASK.find("if (m_buildInProgress) {")
assert IN_PROGRESS_AT != -1, "ensureFrameBuiltForContract 必须有 m_buildInProgress 分支"
BRANCH_START, BRANCH_END = brace_scope_at(CONTRACT_MASK,
                                         CONTRACT_MASK.index("{", IN_PROGRESS_AT))
WORK_REF_AT = CONTRACT_MASK.find("buildWork = m_buildWork;", BRANCH_START, BRANCH_END)
GEN_REF_AT = CONTRACT_MASK.find("buildWorkGeneration = m_buildWorkGeneration;",
                                BRANCH_START, BRANCH_END)
assert WORK_REF_AT != -1 and GEN_REF_AT != -1, \
    ("m_buildInProgress 分支必须成对取得 buildWork = m_buildWork; 与 "
     "buildWorkGeneration = m_buildWorkGeneration;")
assert WORK_REF_AT < GEN_REF_AT, "工作引用与其固定代际必须成对取得（引用在前，代际紧随）"
assert in_lock_scope(CORE_LOCK_SCOPES, CONTRACT_START + GEN_REF_AT), \
    "工作引用与其固定代际必须**在锁内**成对取得"
# 后续每块 / 完成都携带该对身份，而不是发布时再读成员 m_buildWorkGeneration。
assert "publishBuildProgressLocked(*buildWork, buildWorkGeneration)" in CONTRACT_MASK, \
    "每块发布必须携带锁内取得的工作引用 + 固定代际"
assert "publishBuildProgressLocked(*m_buildWork, buildWorkGeneration)" in CONTRACT_MASK, \
    "开始构建后的首次发布必须携带锁内取得的工作引用 + 固定代际"
assert LIFECYCLE_MEMBER + ".completeIfCurrent(buildWork.get(), buildWorkGeneration)" \
    in CONTRACT_MASK, "完成必须以锁内取得的 (work, generation) 成对调用 completeIfCurrent"

# ---- 8. reader 只读按值发布的摘要 ----
SNAP_BODY, _ = extract_function(
    CORE, "ShadowValidationFrameStats ShadowValidationRuntime::snapshot() const {")
STATE_BODY, _ = extract_function(
    CORE, "ShadowValidationBuildState ShadowValidationRuntime::buildStateSnapshot() const {")
for label, body in (("snapshot", SNAP_BODY), ("buildStateSnapshot", STATE_BODY)):
    # 代码级检查（注释除外）：reader 不得触碰 live 构建工作对象，也不得改写摘要。
    code = strip_comments(body)
    assert not re.search(r"m_buildWork(?!Generation)", code), \
        (label, "reader 不得读 live build work")
    assert LIFECYCLE_MEMBER + ".hasValues()" in code, (label, "reader 必须只读已发布摘要")
    assert LIFECYCLE_MEMBER + ".values()" in code, (label, "reader 必须按值读取摘要")
    for mutator in (".beginBuild(", ".publish(", ".complete(", ".cancel(", ".reset()",
                    ".publishIfCurrent(", ".completeIfCurrent("):
        assert LIFECYCLE_MEMBER + mutator not in code, (label, "reader 不得改写摘要", mutator)
assert PROGRESS_STATS_NAME in strip_comments(SNAP_BODY), \
    "snapshot 必须读与摘要同一次发布的统计"

# ---- 9. 禁止肯定式宣称（注释里也不允许）----
# 只在被改动的产品源码里扫描；文档里可以出现"不得宣称…闭合"这类否定式表述。
FORBIDDEN = [
    "已消除竞争", "竞争已消除", "消除数据竞争", "无数据竞争",
    "线程问题已闭合", "线程问题闭合", "线程修复已闭合", "线程修复完成",
    "线程问题已解决", "线程安全已建立", "内存序已建立", "数据竞争已修复",
    "race eliminated", "race is closed", "thread-safety established",
]
for label, text in LEGACY_SOURCES.items():
    for phrase in FORBIDDEN:
        assert phrase not in text, (label, phrase)

# ---- 10. 回归护栏：请求首行与既有调用者未被改动 ----
first_statement = LATEST_BODY.split("\n", 1)[1].strip()
assert first_statement.startswith("requestLatestFrameBuild();"), first_statement

drain_body, _ = extract_function(
    CORE, "void ShadowValidationRuntime::drainPendingBuildForControlPlane(")
assert "ensureLatestFrameBuilt();" in drain_body
observe_at = CORE.index("void ShadowValidationRuntime::runObserveValidation() {")
observe_tail = CORE[observe_at:observe_at + 400]
assert "ensureLatestFrameBuilt();" in observe_tail

# ---- 11. 对象级 palette 证据记录器（2026-09-17 03:58 上级复审同步）----
# 记录器（src/d3d9/war3/tools/war3_palette_object_evidence.h）与生产、宿主机测试共用同一份
# 定长状态机；这里按**签名文本 + 代码结构**钉死四条纪律，不引用任何运行时状态。
RECORDER_H_PATH = ROOT / "src/d3d9/war3/tools/war3_palette_object_evidence.h"
assert RECORDER_H_PATH.is_file(), str(RECORDER_H_PATH)
RECORDER_H = RECORDER_H_PATH.read_text(encoding="utf-8")


def _recorder_signature(name, tail_pattern):
    return re.compile(
        r"void\s+" + name + r"\(\s*const\s+PaletteObjectKey&\s+key\s*,\s*"
        r"PaletteObjectSource\s+source\s*,\s*uint64_t\s+hitKey\s*,\s*"
        r"const\s+PaletteObjectFrames&\s+frames\s*,\s*" + tail_pattern + r"\s*\)")


# 11a. NoteEnqueued / NoteDrawn 必须带 (key, source, hitKey, frames, selectionCleared) 形参：
#      "实际来源"必须是调用点按值给出的，不得由最近一次 Served 推测。
assert _recorder_signature(
    "NoteEnqueued", r"bool\s+selectionClearedByNativeOverride").search(RECORDER_H), \
    "NoteEnqueued 必须带 (key, source, hitKey, frames, selectionClearedByNativeOverride) 形参"
assert _recorder_signature(
    "NoteDrawn", r"bool\s+selectionClearedByNativeOverride").search(RECORDER_H), \
    "NoteDrawn 必须带 (key, source, hitKey, frames, selectionClearedByNativeOverride) 形参"
assert re.search(r"void\s+NoteServed\(\s*const\s+PaletteObjectKey&\s+key\s*,\s*"
                 r"PaletteObjectSource\s+source\s*,\s*uint64_t\s+hitKey\s*,\s*"
                 r"const\s+PaletteObjectFrames&\s+frames\s*\)", RECORDER_H), \
    "NoteServed 必须带 (key, source, hitKey, frames) 形参"
# 事件载体必须真的能携带按值来源与命中键（否则形参只是摆设）。
assert "PaletteObjectSource source = PaletteObjectSource::None;" in RECORDER_H, \
    "事件载体缺少按值 source 字段"
assert "uint64_t hitKey = 0u;" in RECORDER_H, "事件载体缺少 hitKey 字段"

# 11b. 终态真实预留：kNormalBudget 必须存在且等于 kTotalBudget - kTerminalReserve。
assert re.search(r"static\s+constexpr\s+uint64_t\s+kNormalBudget\s*=\s*"
                 r"kTotalBudget\s*-\s*kTerminalReserve\s*;", RECORDER_H), \
    "kNormalBudget 必须存在且等于 kTotalBudget - kTerminalReserve"
assert "static constexpr uint64_t kTotalBudget = 4096u;" in RECORDER_H, "总预算必须是 4096"
assert "static constexpr uint64_t kTerminalReserve = 512u;" in RECORDER_H, "终态预留必须是 512"
CAN_EMIT_BODY, _ = extract_function(RECORDER_H, "bool CanEmit(bool terminal) const {")
CAN_EMIT_CODE = mask_code(CAN_EMIT_BODY)
assert re.search(r"if\s*\(\s*!terminal\s*&&\s*m_counters\.emitted\s*>=\s*kNormalBudget\s*\)",
                 CAN_EMIT_CODE), "CanEmit 必须用 kNormalBudget 挡住普通事件（预留真实扣出）"
assert re.search(r"if\s*\(\s*terminal\s*&&\s*m_counters\.terminalEmitted\s*>=\s*"
                 r"kTerminalReserve\s*\)", CAN_EMIT_CODE), \
    "CanEmit 必须用 kTerminalReserve 约束终态"

# 11c. CloseWindow 不得清零每帧预算（否则同帧继续记录即可绕过 64 条上限）。
CLOSE_WINDOW_BODY, _ = extract_function(RECORDER_H, "void CloseWindow(uint64_t frame) {")
assert "m_frameBudgetUsed = 0u" not in strip_comments(CLOSE_WINDOW_BODY), \
    "CloseWindow 不得出现 m_frameBudgetUsed = 0u（不得清零每帧预算）"
BEGIN_FRAME_BODY, _ = extract_function(RECORDER_H, "void BeginFrame(uint64_t frame) {")
assert "m_frameBudgetUsed = 0u" in mask_code(BEGIN_FRAME_BODY), \
    "每帧预算只允许在 BeginFrame（换帧）时清零"

# 11d. 先预检、再改状态、再发射：CanEmit / EmitUnchecked 必须存在，
#      且四个 Note* 入口的预算预检必须早于任何条目写入。
assert "void EmitUnchecked(" in RECORDER_H, "记录器必须存在 EmitUnchecked"
assert "bool CanEmit(" in RECORDER_H, "记录器必须存在 CanEmit"
MUTATION_TOKENS = ("e->lastServedFrame = frames.renderFrame;",
                   "e->lastSubmitFrame = frames.renderFrame;",
                   "e->lastDrawFrame = frames.renderFrame;",
                   "e->lastRejectFrame = frames.renderFrame;",
                   "e->hitCount++", "e->sawServed = true;",
                   "e->sawSubmit = true;", "e->sawDraw = true;")
for signature, label, helper in (
        ("void NoteReject(const PaletteObjectKey& key,", "NoteReject", None),
        ("void NoteServed(const PaletteObjectKey& key,", "NoteServed", "void AdvanceServedOnChain("),
        ("void NoteEnqueued(const PaletteObjectKey& key,", "NoteEnqueued",
         "void AdvanceEnqueuedOnChain("),
        ("void NoteDrawn(const PaletteObjectKey& key,", "NoteDrawn", "void AdvanceDrawnOnChain(")):
    body = effective_body(signature)
    code = strip_comments(body)
    # 2026-09-18 P0-6（Astra 裁定 C1：明确关联后的逐链记录）—— **断言必须跟随代码**：
    # S/E/D 的预算预检搬进了**逐链** helper（一条事实可能记到两条链上 ⇒ 预检是「每链一次」），
    # 入口体改成委派。所以：先证明入口真的委派到那个 helper，再在 helper 内部检查次序。
    if "CanEmit(false)" not in code:
        helper_call = helper.replace("void ", "")  # 调用处只有名字，没有返回类型
        assert helper is not None and helper_call in code, \
            (label, "该入口既没有内联 CanEmit(false)，也没有委派到逐链 helper —— 两条路必须有其一")
        helper_body, _ = extract_function(RECORDER_H, helper)
        code = strip_comments(helper_body)
    guard_at = code.index("CanEmit(false)")
    mutations = [code.index(token) for token in MUTATION_TOKENS if token in code]
    assert mutations, (label, "该入口（或它委派的逐链 helper）必须真的修改条目状态")
    assert guard_at < min(mutations), \
        (label, "预算预检（CanEmit）必须早于任何 e-> 状态修改（被拒不得改状态）")

# 11e. 采集点必须把**当场取到的实际来源**按值传给记录器。
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
SHADOW_DRAW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
assert re.search(r"paletteObjectRecorder\.NoteServed\(\s*paletteObjectKey\s*,\s*"
                 r"paletteObjectSource\s*,\s*paletteObjectHitKey\s*,", DEVICE), \
    "S 采集点必须把当场选中的 (source, hitKey) 传给 NoteServed"
assert re.search(r"paletteObjectRecorder\.NoteEnqueued\(\s*paletteObjectKey\s*,\s*"
                 r"paletteObjectSource\s*,\s*paletteObjectHitKey\s*,", DEVICE), \
    "S 采集点必须把当场选中的 (source, hitKey) 传给 NoteEnqueued"
assert re.search(r"PaletteObjectRecorder\(\)\.NoteDrawn\(\s*paletteObjectKey\s*,\s*"
                 r"paletteObjectDrawSource\s*,", SHADOW_DRAW), \
    "D 采集点必须把 draw 当场携带的 source 传给 NoteDrawn"
assert "war3::tools::evidence::MapPaletteObjectSource(" in SHADOW_DRAW and \
       "paletteObjectDrawSource" in SHADOW_DRAW, \
    "D 采集点的来源必须由该次 draw 的按值来源映射而来（不得从最近 Served 推测）"

# 11f. 记录器宿主机测试必须真的实例化生产记录器并覆盖新形参与边界反例。
RECORDER_TEST_PATH = ROOT / "src/d3d9/war3/render/tests/war3_palette_object_evidence_test.cpp"
assert RECORDER_TEST_PATH.is_file(), str(RECORDER_TEST_PATH)
RECORDER_TEST_TEXT = RECORDER_TEST_PATH.read_text(encoding="utf-8")
for needle, label in (("PaletteObjectEvidence", "必须实例化生产记录器"),
                      ("NoteEnqueued(", "必须覆盖 Served/Enqueued/Drawn 全阶段"),
                      ("NoteDrawn(", "必须覆盖 Drawn 阶段"),
                      ("kNormalBudget", "必须覆盖普通事件 3584 上限"),
                      ("droppedTerminalReserve", "必须覆盖终态预留被拒"),
                      ("droppedDuplicatePerFrame", "必须覆盖同帧同阶段聚合"),
                      ("NoteObjectGone(", "必须覆盖墓碑删除路径"),
                      ("WindowExpired", "必须覆盖从未接住的终态"),
                      ("Unclosed", "必须覆盖未闭合链的终态")):
    assert needle in RECORDER_TEST_TEXT, ("记录器宿主机测试缺少覆盖", label, needle)

# 11g. 2026-09-17 主线程两处**有意修正**必须在产品头里保持（收紧为新行为，禁止回退）：
#   A) Insert 不得预写 Entry::lastRejectFrame —— 旧写法让"新建对象"的第一条 Rejected 在同一次
#      调用里被同帧去重吞掉；现在只有 NoteReject 的后续路径才允许写该字段。
INSERT_BODY, _ = extract_function(RECORDER_H, "Entry* Insert(")
INSERT_CODE = strip_comments(INSERT_BODY)
assert "lastRejectFrame" not in INSERT_CODE,     "Insert 不得写 lastRejectFrame（否则新建对象的第一条 Rejected 会被同帧去重吞掉）"
assert "e.firstRejectFrame = frame;" in INSERT_CODE,     "Insert 仍必须记录 firstRejectFrame（条目窗口起点）"
NOTE_REJECT_BODY, _ = extract_function(RECORDER_H, "void NoteReject(const PaletteObjectKey& key,")
NOTE_REJECT_CODE = strip_comments(NOTE_REJECT_BODY)
assert "e->lastRejectFrame = frames.renderFrame;" in NOTE_REJECT_CODE,     "NoteReject 仍必须写 lastRejectFrame（同一帧的第二次 Reject 才去重）"
assert NOTE_REJECT_CODE.index("CanEmit(false)") < \
       NOTE_REJECT_CODE.index("e->lastRejectFrame = frames.renderFrame;"), \
    "NoteReject 的 lastRejectFrame 写入必须晚于预算预检（被拒不得修改条目状态）"

#   B) 帧域清洗必须**保留调用点口径**：NoteReject / PrepareStage / CloseWindow / NoteObjectGone
#      都不得把 manifestUnknown/nativeUnknown 强制写成 true，且阶段事件必须按调用点帧域赋值。
for signature, label in (
        ("void NoteReject(const PaletteObjectKey& key,", "NoteReject"),
        ("PaletteObjectEventRecord PrepareStage(", "PrepareStage"),
        ("void CloseWindow(uint64_t frame) {", "CloseWindow"),
        ("void NoteObjectGone(const PaletteObjectKey& key) {", "NoteObjectGone")):
    body = effective_body(signature)
    code = strip_comments(body)
    assert not re.search(r"\.(?:manifestUnknown|nativeUnknown)\s*=\s*true\s*;", code), \
        (label, "不得把 manifest/native 帧域强制写成 unknown（必须保留调用点口径）")
PREPARE_BODY, _ = extract_function(RECORDER_H, "PaletteObjectEventRecord PrepareStage(")
assert "record.frames = frames;" in strip_comments(PREPARE_BODY), \
    "PrepareStage 必须按调用点帧域赋值（record.frames = frames;），不得沿用条目首次帧"
assert "record.frames = frames;" in NOTE_REJECT_CODE, \
    "NoteReject 必须按调用点帧域赋值（record.frames = frames;）"

# 11h. 记录器宿主机测试必须把上面两条修正**正面钉死**（不能只靠产品侧文本）：
#      建条目发射第一条 Rejected、建条目不计同帧重复、known 保持 known、
#      未知零与已知零可分辨、CloseWindow 保留调用点口径、被拒事件不推进 chainSequence。
for needle, label in (
        ("the object's FIRST Rejected event must be emitted when the entry is created",
         "必须见证建条目发射第一条 Rejected（修正 A）"),
        ("creating an entry must not be charged as a same-frame duplicate",
         "必须见证建条目不计同帧重复"),
        ("Insert does not pre-set lastRejectFrame (fix A)", "必须输出修正 A 的观察行"),
        ("a call-site known manifest/native frame domain must stay known",
         "必须钉死 known 保持 known（修正 B）"),
        ("known-zero and unknown-zero must stay distinguishable",
         "必须钉死未知零 vs 已知零可分辨"),
        ("the window-close terminal must keep the call-site manifest/native domains",
         "必须钉死 CloseWindow 保留调用点口径"),
        ("must not advance chainSequence", "必须钉死被拒事件不推进 chainSequence"),
        ("the entry-creation event is the chain's first Rejected with chainSequence 1",
         "必须见证建条目事件的 chainSequence 基准")):
    assert needle in RECORDER_TEST_TEXT, ("记录器宿主机测试缺少修正后的断言", label, needle)
# 旧口径的文字不得残留（防止把期望改回"强制 unknown / 首条拒绝被吞"）。
assert "force the manifest/native frame domains to unknown" not in RECORDER_TEST_TEXT, \
    "记录器宿主机测试不得再断言'强制 unknown'的旧行为"
assert "Insert pre-sets Entry::lastRejectFrame" not in RECORDER_TEST_TEXT, \
    "记录器宿主机测试不得再断言'首条拒绝被吞'的旧缺陷"

# 11j. 阶段序号与同帧去重仍然只由 NoteReject 负责（变异 M18/M19 的静态对应物）：
assert "record.chainSequence = ++e->chainSequence;" in NOTE_REJECT_CODE, \
    "NoteReject 必须推进 chainSequence（每条发射事件一个递增序号）"
# 2026-09-18 阶段 C：去重行加了空指针守卫（预算预检提前到 Insert 之前后，首次到达时 e 仍可能为
# nullptr）⇒ 本断言改为检查**要求本身**，不锁死字面形状。要求未变。
assert "e->lastRejectFrame == frames.renderFrame" in NOTE_REJECT_CODE and \
       "m_counters.droppedDuplicatePerFrame++;" in NOTE_REJECT_CODE, \
    "NoteReject 必须保留同一帧同阶段去重（第二次 Reject 计入 droppedDuplicatePerFrame）"
# 2026-09-18 阶段 C（复核 P1）：`NoteReject` 的**非终态预算预检必须先于 Insert**，
# 否则预算拒发时会留下「有终态、无链首」的条目（复核给出的第 65 键反例）。
assert NOTE_REJECT_CODE.index("if (!CanEmit(false)) {") < \
       NOTE_REJECT_CODE.index("e = Insert(key, frames.renderFrame, reason, frames"), \
    "NoteReject 必须先过非终态预算再建条目（否则留下无链首条目）"
# 表满判定必须先于非终态预算：TableFull 属**终态**预算（F1），若先判非终态预算就返回，
# 会在非终态预算耗尽时悄悄不再公告表满。
assert NOTE_REJECT_CODE.index("m_watchCount >= kWatchCapacity") < \
       NOTE_REJECT_CODE.index("if (!CanEmit(false)) {"), \
    "NoteReject 的表满判定必须先于非终态预算（TableFull 属终态预算）"

# 11i. lifecycleIdentity 必须贯通 wire：发射器写 data[2]，读方（解析器）声明同一槽位。
SINK = (ROOT / "src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp").read_text(
    encoding="utf-8")
assert "event.data[2] = record.key.lifecycleIdentity;" in SINK, \
    "发射器必须把 lifecycleIdentity 写入 wire data[2]"
ANALYZER = (ROOT / "AutoTest/analyze_palette_object_evidence.py").read_text(encoding="utf-8")
# 必须锚定行首/行尾：否则 PROPOSED_LIFECYCLE_IDENTITY_SLOT=('data',2) 会让本断言恒真
#（2026-09-17 变异 M26 实测发现，收紧为整行匹配）。
assert re.search(r"^LIFECYCLE_IDENTITY_SLOT=\('data',2\)$", ANALYZER, re.MULTILINE), \
    "解析器必须声明同一个 lifecycleIdentity 槽位 ('data', 2)（发射器与解析器已同步，不得回退）"


# ---- 11k. G1..G4 生产修复的静态钉死（2026-09-17 主线程；**只加不减**）----
# 往返测试（AutoTest/test_palette_object_wire_roundtrip.py）暴露的四个生产缺口已由主线程在生产
# 代码里修好；这里把修复后的**契约文本**钉死，防止回退。本段只新增断言，不改动上面任何一条。
EVIDENCE_CPP = (ROOT / "src/d3d9/war3/tools/war3_frame_evidence.cpp").read_text(
    encoding="utf-8")
EVIDENCE_CODE = strip_comments(EVIDENCE_CPP)

# 11k(a) G3：deltaFrames 只由 Rejected / ServedCandidate / **终态** 计算
#        （= renderFrame-firstRejectFrame）；live 的 Enqueued / Drawn 必须显式写 0。
_DELTA_COMPUTED = re.compile(r"record\.deltaFrames\s*=\s*[^;]*firstRejectFrame[^;]*;")
_LIVE_ZERO_DELTA = re.compile(r"record\.deltaFrames\s*=\s*0u\s*;")
for signature, label in (("void NoteReject(const PaletteObjectKey& key,", "NoteReject"),
                         ("void NoteServed(const PaletteObjectKey& key,", "NoteServed")):
    body = effective_body(signature)
    code = strip_comments(body)
    assert _DELTA_COMPUTED.search(code), \
        (label, "R/Served 必须携带 renderFrame-firstRejectFrame 的 deltaFrames")
CLOSE_WINDOW_DELTA_BODY, _ = extract_function(RECORDER_H, "void CloseWindow(uint64_t frame) {")
assert _DELTA_COMPUTED.search(strip_comments(CLOSE_WINDOW_DELTA_BODY)), \
    "终态（CloseWindow）必须携带 renderFrame-firstRejectFrame 的 deltaFrames"
for signature, label in (("void NoteEnqueued(const PaletteObjectKey& key,", "NoteEnqueued"),
                         ("void NoteDrawn(const PaletteObjectKey& key,", "NoteDrawn")):
    body = effective_body(signature)
    code = strip_comments(body)
    assert _LIVE_ZERO_DELTA.search(code), \
        (label, "live Enqueued/Drawn 必须显式写 record.deltaFrames = 0u（冻结读方契约 G3）")
    assert not _DELTA_COMPUTED.search(code), \
        (label, "live Enqueued/Drawn 不得计算 deltaFrames（非终态里只有 R/Served 带 delta）")

# 11k(b) G2 后半：CloseWindow 幂等且置窗口关闭位；Reset 清位；**五个** Note* 入口都早退。
_WINDOW_CLOSED_GUARD = re.compile(r"if\s*\(\s*m_windowClosed\s*\)\s*return\s*;")
# 2026-09-18 P0-6（复审丢弃路径清单）：**采集入口**的窗口关闭早退必须**具名计损**再返回。
# 旧断言只要求 `if (m_windowClosed) return;` —— 那正是「静默丢弃」的形状，现已加强；
# `CloseWindow` 自己的幂等返回仍用上面那条裸正则（它不是丢弃）。
_WINDOW_CLOSED_GUARD_COUNTED = re.compile(
    r"if\s*\(\s*m_windowClosed\s*\)\s*\{\s*"
    r"m_counters\.droppedWindowClosedEntry\+\+;\s*return;\s*\}")
assert _WINDOW_CLOSED_GUARD.search(strip_comments(CLOSE_WINDOW_DELTA_BODY)), \
    "CloseWindow 必须幂等（m_windowClosed 早退，窗口只结算一次）"
assert re.search(r"m_windowClosed\s*=\s*true\s*;", strip_comments(CLOSE_WINDOW_DELTA_BODY)), \
    "CloseWindow 必须置窗口关闭位（m_windowClosed = true;）"
RESET_BODY, _ = extract_function(
    RECORDER_H, "void Reset(uint64_t sessionGeneration, uint64_t mapEpoch) {")
assert re.search(r"m_windowClosed\s*=\s*false\s*;", strip_comments(RESET_BODY)), \
    "Reset 必须清窗口关闭位（m_windowClosed = false;），否则窗口永远打不开"
for signature, label in (("void NoteReject(const PaletteObjectKey& key,", "NoteReject"),
                         ("void NoteServed(const PaletteObjectKey& key,", "NoteServed"),
                         ("void NoteEnqueued(const PaletteObjectKey& key,", "NoteEnqueued"),
                         ("void NoteDrawn(const PaletteObjectKey& key,", "NoteDrawn"),
                         ("void NoteObjectGone(const PaletteObjectKey& key) {", "NoteObjectGone")):
    body = effective_body(signature)
    assert _WINDOW_CLOSED_GUARD_COUNTED.search(strip_comments(body)), \
        (label, "五个采集入口在窗口关闭后必须**具名计损**再返回（P0-6：不得静默丢弃）")

# 11k(c) G4 后半：发射器在 Record(...) == 0（环已冻结/未激活）时必须落一个可见丢失计数。
EMITTER_BODY, _ = extract_function(SINK, "void EmitPaletteObjectEvent(void*")
EMITTER_CODE = strip_comments(EMITTER_BODY)
assert re.search(r"if\s*\(\s*Record\(session,\s*event\)\s*==\s*0u\s*\)\s*"
                 r"g_paletteObjectEvidence\.NoteRingEviction\(1u\);", EMITTER_CODE), \
    "发射器必须在 Record(session, event) == 0u 时调用 NoteRingEviction(1u)（fail-visible）"
# 2026-09-18 P0-5（复审 C2）：**这条旧断言锁住的正是「无声 return」**。现在要求 fail-closed
# **并且**落一个具名计数（真实环路径实测：冻结/重入期间一条嵌套 emit 无计数被丢弃，读方据此
# emitted != exported ⇒ 整份不覆盖）⇒ 断言**更强**：既要求仍然早退，也要求计数。
assert re.search(r"if\s*\(\s*session\s*==\s*0u\s*\)\s*\{\s*"
                 r"g_paletteObjectDroppedNoSession\.fetch_add\(1u,\s*std::memory_order_relaxed\);\s*"
                 r"return;\s*\}", EMITTER_CODE), \
    "发射器必须对 session == 0 fail-closed 且**计入具名计数**（不得无声 return）"
assert re.search(r"if\s*\(\s*!EncodePaletteObjectEvent\(record,\s*session,\s*event\)\s*\)\s*\{\s*"
                 r"g_paletteObjectEncodeFailed\.fetch_add\(1u,\s*std::memory_order_relaxed\);\s*"
                 r"return;\s*\}", EMITTER_CODE), \
    "编码失败的早退同样必须计入具名计数（不得无声 return）"
assert re.search(r"if\s*\(\s*!PaletteObjectEvidenceEnabled\(\)\s*\)\s*return\s*;",
                 EMITTER_CODE), \
    "发射器仍必须对子门关闭 fail-closed"

# 11k(d) G1 / G2：watchCount 必须是 JSON 整数（counters 仍是规范十进制字符串）；
#         DisarmPaletteObjectEvidence() 只允许出现在 discard 分支。
HEADER_JSON_BODY, _ = extract_function(EVIDENCE_CPP, "json PaletteObjectHeaderJson() {")
HEADER_JSON_CODE = strip_comments(HEADER_JSON_BODY)
assert re.search(r'result\["watchCount"\]\s*=\s*header\.watchCount\s*;', HEADER_JSON_CODE), \
    'G1: watchCount 必须以 JSON 整数写出（result["watchCount"]=header.watchCount;）'
assert "std::to_string(header.watchCount)" not in HEADER_JSON_CODE, \
    "G1: watchCount 不得用 std::to_string（读方要求 JSON 整数）"
assert not re.search(r'counters\["watchCount"\]', HEADER_JSON_CODE), \
    "G1: watchCount 不得被塞进 counters 字符串块"
assert re.search(r'counters\["emitted"\]\s*=\s*std::to_string\(c\.emitted\)\s*;',
                 HEADER_JSON_CODE), \
    "counters 仍必须是规范十进制字符串（std::to_string）"


def _branch_scope(code, needle):
    """返回 needle 所在 else-if 分支体的 (起, 止) 偏移。

    注意：偏移必须在**同一份** strip_comments 文本上求（strip_comments 会删除注释字符，
    偏移与原文/mask_code 文本不一致；这两个分支的字符串字面量里都没有大括号，直接配平安全）。
    """
    at = code.index(needle)
    start = code.index("{", at)
    depth = 0
    for index in range(start, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return start, index
    raise AssertionError("unterminated branch: " + needle)


_DISCARD_SCOPE = _branch_scope(EVIDENCE_CODE, 'else if(action=="discard")')
_DISARM_CALLS = [match.start()
                 for match in re.finditer(r"DisarmPaletteObjectEvidence\(\)", EVIDENCE_CODE)]
assert len(_DISARM_CALLS) == 1, \
    ("G2: DisarmPaletteObjectEvidence() 只允许出现一次（discard 分支）", len(_DISARM_CALLS))
assert _DISCARD_SCOPE[0] <= _DISARM_CALLS[0] < _DISCARD_SCOPE[1], \
    "G2: DisarmPaletteObjectEvidence() 只允许出现在 discard 分支（freeze 必须保持计数可读）"

# 11k(e) G4：freeze 分支必须先 ClosePaletteObjectWindow() 再 ring.finish(（终态才进得了环）。
_FREEZE_SCOPE = _branch_scope(EVIDENCE_CODE, 'else if(action=="freeze")')
_CLOSE_AT = EVIDENCE_CODE.index("ClosePaletteObjectWindow()", _FREEZE_SCOPE[0])
_FINISH_AT = EVIDENCE_CODE.index("s->ring.finish(generation)", _FREEZE_SCOPE[0])
assert _FREEZE_SCOPE[0] <= _CLOSE_AT < _FREEZE_SCOPE[1] and \
       _FREEZE_SCOPE[0] <= _FINISH_AT < _FREEZE_SCOPE[1], \
    "G4: ClosePaletteObjectWindow() 与 ring.finish( 都必须位于 freeze 分支内"
assert _CLOSE_AT < _FINISH_AT, \
    "G4: freeze 分支必须先 ClosePaletteObjectWindow() 再 ring.finish(（否则终态永不进环）"

# 11k(f) F1/F2/F3（2026-09-17 主线程修复"单窗口终态上界 / 结算桶 / 终态 delta"）：
#   F1 终态**不再**受每帧预算约束，只受 kTerminalReserve（与总额）约束；live 事件才受 kPerFrameBudget。
CAN_EMIT_CODE = strip_comments(
    extract_function(RECORDER_H, "bool CanEmit(bool terminal) const {")[0])
assert re.search(r"if\s*\(\s*!terminal\s*&&\s*m_frameBudgetUsed\s*>=\s*kPerFrameBudget\s*\)",
                 CAN_EMIT_CODE), \
    "F1: 每帧预算只允许约束非终态（必须写成 !terminal && ...）"
assert not re.search(r"if\s*\(\s*m_frameBudgetUsed\s*>=\s*kPerFrameBudget\s*\)", CAN_EMIT_CODE), \
    "F1: 终态不得再受每帧 64 上限约束（不得出现无 !terminal 限定的每帧检查）"
assert re.search(r"if\s*\(\s*terminal\s*&&\s*m_counters\.terminalEmitted\s*>=\s*"
                 r"kTerminalReserve\s*\)", CAN_EMIT_CODE), \
    "F1: 终态必须只受 kTerminalReserve 约束"
assert re.search(r"if\s*\(\s*m_counters\.emitted\s*>=\s*kTotalBudget\s*\)", CAN_EMIT_CODE), \
    "F1: 总额预算仍是最后一道门"
ACCOUNT_DROP_CODE = strip_comments(
    extract_function(RECORDER_H, "void AccountDrop(bool terminal) {")[0])
assert re.search(r"if\s*\(\s*terminal\s*&&\s*m_counters\.terminalEmitted\s*>=\s*"
                 r"kTerminalReserve\s*\)\s*\{\s*m_counters\.droppedTerminalReserve\+\+;",
                 ACCOUNT_DROP_CODE), \
    "F1: 预留耗尽的终态必须记 droppedTerminalReserve（预留记账分支可达）"
assert re.search(r"if\s*\(\s*!terminal\s*&&\s*m_frameBudgetUsed\s*>=\s*kPerFrameBudget\s*\)"
                 r"\s*\{\s*m_counters\.droppedPerFrame\+\+;", ACCOUNT_DROP_CODE), \
    "F1: 每帧丢弃计数只允许针对非终态（终态不得再记 droppedPerFrame）"
assert not re.search(r"if\s*\(\s*m_frameBudgetUsed\s*>=\s*kPerFrameBudget\s*\)",
                     ACCOUNT_DROP_CODE), \
    "F1: AccountDrop 不得出现无 !terminal 限定的每帧分支"

#   F2 CloseWindow 的 closed* 结算桶只在终态**真的发出**（EmitUnchecked）之后递增。
CLOSE_WINDOW_CODE = strip_comments(CLOSE_WINDOW_DELTA_BODY)
_EMIT_AT = CLOSE_WINDOW_CODE.index("EmitUnchecked(record, true);")
for bucket in ("m_counters.closedRecovered++;", "m_counters.closedWindowExpired++;",
               "m_counters.closedUnclosed++;"):
    assert bucket in CLOSE_WINDOW_CODE, ("F2: CloseWindow 缺少结算桶", bucket)
    assert CLOSE_WINDOW_CODE.index(bucket) > _EMIT_AT, \
        ("F2: 结算桶必须只在终态真的发出后递增（不得在预算门之前）", bucket)

#   F3 ObjectGone / TableFull 两条终态路径必须**显式**写终态 delta 契约
#   2026-09-18 P0-3：ObjectGone 的结算被抽成**单链操作** `SettleObjectGone`（因为必须对两条独立链
#   各结算一次）⇒ 抽取目标随之改为该函数。**断言强度不变**，并新增两条更强的约束：
#   ① NoteObjectGone 必须对 RejectionRecovery 与 Observation **都**委派；
#   ② 观察链的 ObjectGone 阶段必须按实际最高秩回填，**不得硬写 Rejected**（伪造事实）。
# 2026-09-18 P0-3（复审 C1/C3 修正）：NoteObjectGone 现在**先取两条链**（各自 FindChain）再逐条结算；
# 第二条结算前**重新确认它仍可结算**（判据不是 m_windowClosed —— Reset 类失效会清表却不置该标志）。
_OBJECT_GONE_DELEGATE = strip_comments(
    extract_function(RECORDER_H, "void NoteObjectGone(const PaletteObjectKey& key) {")[0])
for _chain in ("PaletteObjectChainType::RejectionRecovery",
               "PaletteObjectChainType::Observation"):
    assert re.search(r"FindChain\(\s*key\s*,\s*" + re.escape(_chain) + r"\s*\)",
                     _OBJECT_GONE_DELEGATE), \
        ("P0-3: NoteObjectGone 必须对每条链各取一次（FindChain）", _chain)
assert re.search(r"SettleObjectGone\(\s*rejectChain\s*\)", _OBJECT_GONE_DELEGATE), \
    "P0-3: NoteObjectGone 必须结算拒绝恢复链"
assert _OBJECT_GONE_DELEGATE.count("SettleObjectGone(") >= 2, \
    "P0-3: NoteObjectGone 必须结算两条链（各一次），而不是只处理 FindOwner 的那一条"
# 2026-09-18 P0-6（复审 266e35c4 的潜伏缺口）：判据必须是**同一条链实例**，不只是「条目仍在」。
# 仅按存在性判断时，重入期间重开窗并重建同名链会让事实被**静默改挂到新链**。
assert re.search(r"Entry\*\s+const\s+stillSettleable\s*=\s*FindChain\(\s*key\s*,\s*"
                 r"PaletteObjectChainType::Observation\s*\)", _OBJECT_GONE_DELEGATE), \
    "P0-3/C3: 第二条链结算前必须**重新 FindChain** 确认它仍可结算（不得用 m_windowClosed 当判据）"
assert re.search(r"stillSettleable\s*!=\s*nullptr\s*&&\s*"
                 r"stillSettleable->instanceId\s*==\s*observationInstance",
                 _OBJECT_GONE_DELEGATE), \
    ("P0-6/266e35c4: 判据必须同时要求**同一条链实例**（instanceId 相等）—— 只看『条目仍在』"
     "会在重入重建同名链时把事实静默改挂到新链")
_OBJECT_GONE_CODE = strip_comments(
    extract_function(RECORDER_H, "void SettleObjectGone(Entry* e) {")[0])
assert _DELTA_COMPUTED.search(_OBJECT_GONE_CODE), \
    "F3: ObjectGone 必须显式写 renderFrame-firstRejectFrame 的 deltaFrames"
assert re.search(r"record\.stage\s*=\s*e->sawFirstSight\s*\?\s*StageOfRank\(e->maxStage\)",
                 _OBJECT_GONE_CODE), \
    "P0-3: 观察链的 ObjectGone 阶段必须按实际最高秩回填，不得硬写 Rejected"
# 2026-09-18 P0-3（复审 C1）：结算必须在**发射之前**认领条目 —— 否则 emit 内的嵌套结算
# （生产可达）会让同一条链拿到两个终态，而读方硬要求『一个对象至多一个终态事件』⇒ 整份导出被拒。
assert _OBJECT_GONE_CODE.index("e->used = false;") < _OBJECT_GONE_CODE.index("EmitUnchecked(record, true);"), \
    "P0-3/C1: SettleObjectGone 必须在 EmitUnchecked 之前认领条目（e->used = false）"
_TABLE_FULL_CODE = strip_comments(
    extract_function(RECORDER_H, "void NoteReject(const PaletteObjectKey& key,")[0])
_TF_AT = _TABLE_FULL_CODE.index("PaletteObjectTerminal::TableFull")
_TF_GATE = _TABLE_FULL_CODE.index("if (!CanEmit(true))", _TF_AT)
assert re.search(r"record\.deltaFrames\s*=\s*[^;]*;", _TABLE_FULL_CODE[_TF_AT:_TF_GATE]), \
    "F3: TableFull 终态必须显式写 deltaFrames（不得依赖结构体默认值）"

# 11k(g) 记录器宿主机测试与往返编排测试必须**正面见证**新契约（不能只靠产品侧文本）。
for needle, label in (
        ("live Enqueued deltaFrames must be 0",
         "宿主机测试必须见证 live Enqueued 的 deltaFrames 为 0（G3）"),
        ("live Drawn deltaFrames must be 0",
         "宿主机测试必须见证 live Drawn 的 deltaFrames 为 0（G3）"),
        ("[FINDING] single-window terminal reserve reachable",
         "宿主机测试必须如实打印修复后的终态预留正面见证（F1）"),
        ("CloseWindow closes the window (idempotent, Note* refused until Reset)",
         "宿主机测试必须覆盖窗口关闭后的拒绝纪律（G2 后半）"),
        ("the terminal reserve (512) must be fully reachable in one window",
         "宿主机测试必须正面断言 512 终态预留可达（F1）"),
        ("terminals must not be constrained by the per-frame budget",
         "宿主机测试必须正面断言终态不受每帧预算约束（F1）"),
        ("the total must be reachable as 3584 normal + 512 terminal",
         "宿主机测试必须正面断言 4096 总额可达（F1）"),
        ("the 513th terminal onward must be counted in droppedTerminalReserve",
         "宿主机测试必须正面断言预留耗尽的终态记 droppedTerminalReserve（F1）"),
        ("only the 512 emitted terminals may be counted as settled",
         "宿主机测试必须正面断言结算桶只计真的发出的终态（F2）"),
        ("ObjectGone must carry the terminal deltaFrames contract value",
         "宿主机测试必须见证 ObjectGone 的终态 delta 契约值（F3）"),
        ("TableFull must carry the terminal deltaFrames contract value",
         "宿主机测试必须见证 TableFull 的终态 delta 契约值（F3）")):
    assert needle in RECORDER_TEST_TEXT, ("记录器宿主机测试缺少新契约见证", label, needle)

ROUNDTRIP_TEST_PATH = ROOT / "AutoTest/test_palette_object_wire_roundtrip.py"
assert ROUNDTRIP_TEST_PATH.is_file(), str(ROUNDTRIP_TEST_PATH)
ROUNDTRIP_TEST_TEXT = ROUNDTRIP_TEST_PATH.read_text(encoding="utf-8")
for needle, label in (
        ("SPEC_SATISFIED_PRODUCTION", "往返编排测试必须逐场景打印生产满足度"),
        ("ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED", "往返编排测试必须要求认证通过"),
        ('check(type(block["watchCount"]) is int',
         "往返编排测试必须钉死 watchCount 是 JSON 整数（G1）"),
        ("analyzer.analyze(data)", "往返编排测试必须把真导出直接喂生产解析器"),
        ("sameObjectCertified", "往返编排测试必须钉死同对象认证判定"),
        ("the settled Recovered terminal must reach the export",
         "往返编排测试必须见证终态真的进了导出（G4）"),
        ("objectLevelEvidenceDropped", "往返编排测试必须钉死环淘汰可见（G4）"),
        ("noObjectEvidenceExported", "往返编排测试必须钉死空样本未覆盖")):
    assert needle in ROUNDTRIP_TEST_TEXT, ("往返编排测试缺少新契约断言", label, needle)
# 旧缺陷钉死必须消失（G1..G4 已修，不得再要求"生产导出被拒收"）。
for needle, label in (
        ("is expected to be REFUSED by the parser", "不得再要求生产导出被解析器拒收（G1 已修）"),
        ("must show the zeroed export header block", "不得再钉死导出头块恒为 0（G2 已修）"),
        ("frozenZeroDeltaViolated", "不得再钉死 live E/D 的 delta 违规（G3 已修）"),
        ("PINNED_WITH_SPEC_GAPS", "不得再保留『钉死缺陷判定』的旧裁决名"),
        ("projected_analysis", "不得再保留 G1 投影（生产路径已可直接读）")):
    assert needle not in ROUNDTRIP_TEST_TEXT, ("往返编排测试仍残留旧缺陷钉死", label, needle)

# ---- 11l. 记录器并发所有者协议 + 预冻结钩子（2026-09-17 上级裁定 1/2；只加不减）----
# (1) 记录器全部*状态*访问由一把 StateLock 串行化（Configure / Reset /
#     ResetForSessionTransition / CloseWindow / 五个 Note* / 六个读取器）；
# (2) 锁必须是**递归锁**：发射器在持锁路径内回调，该次 append 若触发容量冻结，环会调用预冻结钩子
#     → CloseWindow 再次进入记录器（同一线程）；非递归锁会自死锁（2026-09-17 子代理报告的结构性风险）。
# (3) NoteRingEviction 是**唯一**无锁入口（同一死锁原因），
#     只做 m_ringLosses 原子累加，并由 counters()/SnapshotCounters() 折叠进
#     ringEvictedAfterRecord（唯一无锁字段的可见出口）；
# (4) 证据环 freeze() 必须先调用预冻结钩子再置 Frozen（否则钩子内的 Record() 被拒，终态永不进环）；
# (5) arm 安装钩子、discard 清除；(6) 钩子函数体必须结算终态（ClosePaletteObjectWindow()）。
assert re.search(r"class\s+StateLock\s*\{", RECORDER_H), \
    "11l: 记录器必须有 StateLock（并发所有者协议）"
STATELOCK_CTOR, _ = extract_function(
    RECORDER_H, "explicit StateLock(const PaletteObjectEvidence& owner)")
assert "m_mutex.lock()" in strip_comments(STATELOCK_CTOR), "11l: StateLock 构造必须取 m_mutex"
assert re.search(r"~StateLock\(\)\s*\{\s*m_owner\.m_mutex\.unlock\(\)\s*;", RECORDER_H), \
    "11l: StateLock 析构必须释放同一个 m_mutex"
assert re.search(r"mutable\s+std::recursive_mutex\s+m_mutex\s*;", RECORDER_H), \
    "11l: 记录器必须持有 std::recursive_mutex（const 读取器也要能取锁；递归是防止预冻结钩子在持锁发射路径内重入时自死锁）"

_LOCK_GUARD = re.compile(r"StateLock\s+guard\(\*this\)\s*;")
LOCKED_ENTRIES = (
    ("void Configure(EmitFn emit, void* context) {", "Configure"),
    ("void Reset(uint64_t sessionGeneration, uint64_t mapEpoch) {", "Reset"),
    ("void ResetForSessionTransition(uint64_t sessionGeneration, uint64_t mapEpoch) {",
     "ResetForSessionTransition"),
    ("void CloseWindow(uint64_t frame) {", "CloseWindow"),
    ("void NoteReject(const PaletteObjectKey& key,", "NoteReject"),
    ("void NoteServed(const PaletteObjectKey& key,", "NoteServed"),
    ("void NoteEnqueued(const PaletteObjectKey& key,", "NoteEnqueued"),
    ("void NoteDrawn(const PaletteObjectKey& key,", "NoteDrawn"),
    ("void NoteObjectGone(const PaletteObjectKey& key) {", "NoteObjectGone"),
    ("Counters counters() const {", "counters"),
    ("uint32_t watchCount() const {", "watchCount"),
    ("uint64_t sessionGeneration() const {", "sessionGeneration"),
    ("uint64_t currentFrame() const {", "currentFrame"),
    ("uint64_t mapEpoch() const {", "mapEpoch"),
    ("void SnapshotCounters(", "SnapshotCounters"),
)
for _signature, _label in LOCKED_ENTRIES:
    _body = effective_body(_signature)
    _code = strip_comments(_body)
    assert _LOCK_GUARD.search(_code), (_label, "11l: 必须由 StateLock 串行化（并发所有者协议）")
    assert _code[_code.index("{") + 1:].lstrip().startswith("StateLock guard(*this);"), \
        (_label, "11l: StateLock 必须是函数体第一条语句（先取锁再做任何状态访问）")

NOTE_RING_BODY, _ = extract_function(RECORDER_H, "void NoteRingEviction(uint64_t count) {")
NOTE_RING_CODE = strip_comments(NOTE_RING_BODY)
assert "StateLock" not in NOTE_RING_CODE, \
    "11l: NoteRingEviction 不得取 StateLock（发射器在持锁路径内回调，取锁会自死锁）"
assert re.search(r"m_ringLosses\.fetch_add\(\s*count\s*,\s*std::memory_order_relaxed\s*\)",
                 NOTE_RING_CODE), \
    "11l: NoteRingEviction 必须是唯一无锁字段 m_ringLosses 的原子累加"
assert re.search(r"std::atomic<uint64_t>\s+m_ringLosses\s*\{", RECORDER_H), \
    "11l: m_ringLosses 必须是原子（记录器唯一无锁字段）"
for _signature, _label, _out_name in (
        ("Counters counters() const {", "counters", "copy"),
        ("void SnapshotCounters(", "SnapshotCounters", "outCounters")):
    _body = effective_body(_signature)
    _code = strip_comments(_body)
    assert re.search(_out_name + r"\.ringEvictedAfterRecord\s*\+=\s*m_ringLosses\.load\(", _code), \
        (_label, "11l: 无锁环损失必须折叠进 ringEvictedAfterRecord（导出头块才可见）")

CORE_H_PATH = ROOT / "src/d3d9/war3/tools/war3_frame_evidence_core.h"
CORE_H_TEXT = CORE_H_PATH.read_text(encoding="utf-8")
assert re.search(r"using\s+PreFreezeHook\s*=\s*void\s*\(\*\)\(\)\s*noexcept\s*;", CORE_H_TEXT), \
    "11l: 证据环必须有 PreFreezeHook 函数指针类型"
assert "void setPreFreezeHook(PreFreezeHook hook) noexcept" in CORE_H_TEXT, \
    "11l: 证据环必须能安装预冻结钩子"
FREEZE_BODY, _ = extract_function(CORE_H_TEXT, "void freeze(Reason why) noexcept {")
FREEZE_CODE = strip_comments(FREEZE_BODY)
assert re.search(r"if\s*\(\s*m_state\.load\(\)\s*!=\s*State::Frozen\s*\)", FREEZE_CODE), \
    "11l: freeze() 只在尚未 Frozen 时调用钩子"
_hook_at = FREEZE_CODE.index("hook();")
_frozen_at = FREEZE_CODE.index("m_state.store(State::Frozen)")
assert _hook_at < _frozen_at, \
    "11l: freeze() 必须先调用预冻结钩子再置 Frozen（否则钩子内的 Record() 被拒、终态永不进环）"
assert re.search(r"m_preFreeze\.load\(std::memory_order_acquire\)", FREEZE_CODE), \
    "11l: freeze() 必须读取挂载的预冻结钩子"
assert re.search(r"m_preFreezeRunning\.compare_exchange_strong", FREEZE_CODE), \
    "11l: 预冻结钩子调用必须带重入保护（m_preFreezeRunning）"

_ARM_SCOPE = _branch_scope(EVIDENCE_CODE, 'if(action=="arm")')
_HOOK_INSTALLS = [match.start() for match in
                  re.finditer(r"setPreFreezeHook\(&PaletteObjectPreFreezeHook\)", EVIDENCE_CODE)]
assert len(_HOOK_INSTALLS) == 1, ("11l: 预冻结钩子只允许安装一次", len(_HOOK_INSTALLS))
assert _ARM_SCOPE[0] <= _HOOK_INSTALLS[0] < _ARM_SCOPE[1], "11l: arm 分支必须安装预冻结钩子"
_HOOK_CLEARS = [match.start() for match in re.finditer(r"setPreFreezeHook\(nullptr\)", EVIDENCE_CODE)]
assert len(_HOOK_CLEARS) == 1, ("11l: 预冻结钩子只允许清除一次", len(_HOOK_CLEARS))
assert _DISCARD_SCOPE[0] <= _HOOK_CLEARS[0] < _DISCARD_SCOPE[1], "11l: discard 分支必须清除预冻结钩子"

HOOK_BODY, _ = extract_function(SINK, "void PaletteObjectPreFreezeHook() noexcept {")
assert "ClosePaletteObjectWindow();" in strip_comments(HOOK_BODY), \
    "11l: 预冻结钩子函数体必须调用 ClosePaletteObjectWindow()（任何冻结原因前先结算终态）"

# 11l(e) 测试侧见证：并发场景 E 与自动冻结场景 F 必须真的存在并被编排测试断言。
ROUNDTRIP_CPP_PATH = ROOT / "src/d3d9/war3/render/tests/war3_palette_object_wire_roundtrip_test.cpp"
ROUNDTRIP_CPP = ROUNDTRIP_CPP_PATH.read_text(encoding="utf-8")
for _needle, _label in (("void ScenarioE(const ScenarioIO& io) {", "并发场景 E 必须存在"),
                        ("std::thread control(ConcurrencyControl", "并发场景必须有独立控制线程"),
                        ("CONCURRENCY threads=", "并发场景必须打印线程数/事件数负载"),
                        ('PrintCountersFull("E_AFTER_POST_CLOSE")',
                         "并发场景必须打印关闭后的全字段头块（对比用）"),
                        ("void ScenarioF(const ScenarioIO& io) {", "自动冻结场景 F 必须存在"),
                        ("AUTOFREEZE postPresents=", "自动冻结场景必须打印 state/reason"),
                        ('RunAutoFreezeScenario("F", &ScenarioF', "场景 F 必须走无 freeze 的运行器")):
    assert _needle in ROUNDTRIP_CPP, ("11l", _label, _needle)
_SCENARIO_F_BODY, _ = extract_function(ROUNDTRIP_CPP, "void ScenarioF(const ScenarioIO& io) {")
assert '"action", "freeze"' not in strip_comments(_SCENARIO_F_BODY), \
    "11l: 场景 F 不得调用 Control(freeze)（冻结必须由环在 post-window 结束时自己发生）"
for _needle, _label in (("SPEC_SATISFIED_PRODUCTION[E]", "编排测试必须逐场景打印 E 的满足度"),
                        ("SPEC_SATISFIED_PRODUCTION[F]", "编排测试必须逐场景打印 F 的满足度"),
                        ("postCloseObservationsChangeNothing",
                         "编排测试必须钉死关闭后到达的观测不改变计数"),
                        ("autoFrozenWithoutControlFreeze",
                         "编排测试必须钉死自动冻结（导出 reason 必须是 PostWindow）"),
                        ("droppedCountersAllZero", "编排测试必须钉死并发场景零丢失计数"),
                        ("def check_scenario_f_is_not_manual_freeze",
                         "编排测试必须自查场景 F 不调用 Control(freeze)")):
    assert _needle in ROUNDTRIP_TEST_TEXT, ("11l", _label, _needle)

# 11l(f) 宿主机**运行时反向证据**（2026-09-17 本次新增；只加不减）：
#   §11l(2) 只钉死"锁必须是 std::recursive_mutex"这一**静态**事实。真正的运行时反向证据是宿主机
#   案例 20/21：发射器在**持 StateLock** 的路径内回调，而回调里同线程再次进入记录器。
#   案例 20（直接重入）：形态 A = 每次发射都重入 CloseWindow（由一条会发射的 NoteReject 触发）；
#   形态 B = 第 4 次发射（live Drawn）时重入 ⇒ 嵌套结算一条完整链 ⇒ Recovered 终态必须可见。
#   案例 21（容量冻结触发条件）：用**真实生产 Ring** + 真实记录器制造"post 区溢出触发
#   Ring::freeze(Reason::Capacity)，且冻结发生在 palette 发射回调内"，断言不挂死、钩子恰好跑一次、
#   嵌套终态在记录器里可见、且环收不下的事件全部计入 ringEvictedAfterRecord（fail-visible）。
#   两个案例都在**独立工作线程**上跑、主线程按超时做**看门狗**，超时即打印 FAIL 并 std::_Exit(2)；
#   因此"把 m_mutex 改回 std::mutex"（变异 M41）会被本用例**杀死**（非零退出），而不是把门禁挂死。
for _needle, _label in (
        ("bool Case20ReentrantCloseFromEmitter()",
         "宿主机必须存在『发射器回调内重入 CloseWindow 不得挂死』的案例 20"),
        ("void ReentrantCloseEmitterEvery(void* context, const PaletteObjectEventRecord& record) {",
         "案例 20 形态 A 必须有一个每次发射都同线程重入的发射器"),
        ("Recorder().CloseWindow(kReentryCloseFrame);",
         "案例 20 的重入必须真的再次进入记录器（CloseWindow）"),
        ("20 emitter-callback reentry into CloseWindow must not deadlock",
         "案例 20 必须以『不得挂死』命名并注册到 main"),
        ("21 capacity freeze during a palette emission must not deadlock",
         "案例 21 必须以『不得挂死』命名并注册到 main"),
        ("out.hookEmitDepth == 1u",
         "案例 21 必须钉死『冻结发生在发射回调内』（容量溢出由 palette 观测触发，而非 Present）"),
        ("out.ringReason == static_cast<uint32_t>(Reason::Capacity)",
         "案例 21 必须钉死环的冻结原因是 Capacity"),
        ("out.ringEvicted == 3u",
         "案例 21 必须钉死终态丢失计入 ringEvictedAfterRecord（fail-visible）"),
        ("out.emitted == static_cast<uint64_t>(out.appendAccepted) + out.ringEvicted",
         "案例 21 必须钉死 emitted == 环收下 + 环淘汰 的记账自洽"),
        ("out.recoveredTerminalDelta == kCapacityFreezeCloseFrame - 10u",
         "案例 21 必须钉死嵌套终态仍携带 renderFrame-firstRejectFrame"),
        ("../../tools/war3_frame_evidence_core.h",
         "案例 21 必须直接使用真实生产证据环头（不得复制 Ring 实现）"),
        ("CapacityFreezeRing().append(kCapacityFreezeSession, event)",
         "案例 21 必须真的调用生产 Ring::append"),
        ("RunCaseUnderWatchdog(",
         "重入案例必须在独立工作线程 + 超时看门狗下运行"),
        ("std::_Exit(2);", "看门狗超时必须判失败并以非零状态终止进程"),
        ("non-recursive state lock => self-deadlock",
         "看门狗超时必须打印非递归锁自死锁的判据")):
    assert _needle in RECORDER_TEST_TEXT, ("11l", _label, _needle)

# ---- 11m. D6：同帧同阶段去重必须**先比较载荷**（2026-09-17 上级裁定；只加不减）----
# 上级裁定：同键同帧同阶段**只有在载荷可证等价时**才能压缩成一条；载荷冲突必须计数并阻止认证
# （不得为了让报告好看而忽略）。生产头的四处载荷定义（与 §11m(a) 的正则一一对应）：
#   Rejected:         Entry::lastRejectReason
#   ServedCandidate:  Entry::lastSource + Entry::lastHitKey
#   Enqueued:         Entry::submitSource + Entry::submitHitKey + Entry::submitSelectionCleared
#   Drawn:            Entry::drawSource + Entry::drawHitKey + Entry::drawSelectionCleared
# 计数契约：载荷等价 ⇒ m_counters.droppedDuplicatePerFrame++（按原样，不算损失）；
#          载荷冲突 ⇒ m_counters.droppedPayloadConflict++（缺链必须可见；解析器列为损失）。
D6_DEDUP_CASES = (
    ("void NoteReject(const PaletteObjectKey& key,", "Rejected",
     r"if\s*\(\s*e->lastRejectReason\s*==\s*reason\s*\)"),
    ("void NoteServed(const PaletteObjectKey& key,", "ServedCandidate",
     r"if\s*\(\s*e->lastSource\s*==\s*source\s*&&\s*e->lastHitKey\s*==\s*hitKey\s*\)"),
    ("void NoteEnqueued(const PaletteObjectKey& key,", "Enqueued",
     r"if\s*\(\s*e->submitSource\s*==\s*source\s*&&\s*e->submitHitKey\s*==\s*hitKey\s*&&"
     r"\s*e->submitSelectionCleared\s*==\s*selectionClearedByNativeOverride\s*\)"),
    ("void NoteDrawn(const PaletteObjectKey& key,", "Drawn",
     r"if\s*\(\s*e->drawSource\s*==\s*source\s*&&\s*e->drawHitKey\s*==\s*hitKey\s*&&"
     r"\s*e->drawSelectionCleared\s*==\s*selectionClearedByNativeOverride\s*\)"),
)
_DUP_COUNTER = "m_counters.droppedDuplicatePerFrame++;"
_CONFLICT_COUNTER = "m_counters.droppedPayloadConflict++;"
for _signature, _label, _payload_cond in D6_DEDUP_CASES:
    _body = effective_body(_signature)
    _code = strip_comments(_body)
    assert _code.count(_DUP_COUNTER) == 1, \
        (_label, "11m(a): 该入口必须恰好有一个 droppedDuplicatePerFrame++",
         _code.count(_DUP_COUNTER))
    assert _code.count(_CONFLICT_COUNTER) == 1, \
        (_label, "11m(a): 该入口必须恰好有一个 droppedPayloadConflict++（载荷冲突必须可见）",
         _code.count(_CONFLICT_COUNTER))
    _cond = re.search(_payload_cond, _code)
    assert _cond, \
        (_label, "11m(a): 同帧去重必须**先比较载荷**（缺载荷比较 = 变异 M42 的旧行为）",
         _payload_cond)
    _dup_at = _code.index(_DUP_COUNTER)
    _conflict_at = _code.index(_CONFLICT_COUNTER)
    assert _cond.end() <= _dup_at, \
        (_label, "11m(a): 载荷比较必须先于 droppedDuplicatePerFrame++")
    assert _code[_cond.end():_dup_at].strip() == "", \
        (_label, "11m(a): droppedDuplicatePerFrame++ 必须**直接**由该载荷比较的 if 分支守卫",
         _code[_cond.end():_dup_at])
    assert _dup_at < _conflict_at, \
        (_label, "11m(a): 冲突计数必须在等价重复计数之后（if / else 两分支）")
    assert re.search(r"\belse\b", _code[_dup_at:_conflict_at]), \
        (_label, "11m(a): 两个计数必须在同一 if/else 里（conflict 属于 else 分支）")

# 11m(b) 计数本体 + 导出链：记录器 Counters 字段、导出头块的规范十进制字符串。
assert re.search(r"uint64_t\s+droppedPayloadConflict\s*=\s*0u\s*;", RECORDER_H), \
    "11m(b): 记录器 Counters 必须有 droppedPayloadConflict 字段（uint64_t，初值 0）"
assert re.search(r'counters\["droppedPayloadConflict"\]\s*=\s*'
                 r'std::to_string\(\s*c\.droppedPayloadConflict\s*\)\s*;', HEADER_JSON_CODE), \
    "11m(b): 导出头块必须把 droppedPayloadConflict 写成规范十进制字符串"

# 11m(c) 解析器必须把该计数**当损失**：既在精确相等的 COUNTER_FIELDS 里（漏了会让所有真实
#        导出被判字段不匹配），也在 LOSS_FIELDS 里（非零 ⇒ 缺链/未覆盖，fail-visible）。
assert re.search(r"COUNTER_FIELDS=\{[^}]*'droppedPayloadConflict'", ANALYZER, re.S), \
    "11m(c): 解析器 COUNTER_FIELDS 必须包含 droppedPayloadConflict（根信封是精确相等集）"
assert re.search(r"LOSS_FIELDS=\([^)]*'droppedPayloadConflict'", ANALYZER, re.S), \
    "11m(c): 解析器必须把 droppedPayloadConflict 列为损失（非零 ⇒ 缺链/未覆盖）"
# 文本正则之外再做一次**运行时**成员判定（导入生产解析器模块本体，不是副本）。
import sys  # noqa: E402  （本节自足：门禁脚本原本不需要 sys）
sys.path.insert(0, str(ROOT / "AutoTest"))
import analyze_palette_object_evidence as _palette_analyzer  # noqa: E402
assert "droppedPayloadConflict" in _palette_analyzer.COUNTER_FIELDS, \
    ("11m(c)", "生产解析器模块的 COUNTER_FIELDS 必须真的包含 droppedPayloadConflict")
assert "droppedPayloadConflict" in _palette_analyzer.LOSS_FIELDS, \
    ("11m(c)", "生产解析器模块的 LOSS_FIELDS 必须真的包含 droppedPayloadConflict")

# 11m(d) Entry::lastRejectReason 必须与 lastRejectFrame **同处赋值**（同一拒绝路径、相邻两行），
#        且 Insert 不得预写这两个"本帧已发过拒绝事件"的标记（否则首条 Rejected 被吞）。
assert re.search(r"PaletteObjectRejectReason\s+lastRejectReason\s*=\s*"
                 r"PaletteObjectRejectReason::Unknown\s*;", RECORDER_H), \
    "11m(d): Entry 必须有 lastRejectReason 字段（初值 Unknown，不得用 NotChecked 冒充）"
assert "lastRejectReason" not in INSERT_CODE, \
    "11m(d): Insert 不得写 lastRejectReason（建条目那次必须发射第一条 Rejected）"
assert re.search(r"e->lastRejectFrame\s*=\s*frames\.renderFrame\s*;\s*"
                 r"e->lastRejectReason\s*=\s*reason\s*;", NOTE_REJECT_CODE), \
    "11m(d): lastRejectReason 必须与 lastRejectFrame 同处赋值（相邻，同一拒绝路径）"

# 11m(e) 宿主机测试必须含上级反例（同帧 D 载荷冲突）的见证文本与两向正面见证。
for _needle, _label in (
        ("bool Case22DrawnPayloadConflictWitness()",
         "11m(e): 宿主机测试必须有上级反例（同帧 D 载荷冲突）的直接见证案例"),
        ("22 same-frame Drawn payload conflict is visible (the 上级 counterexample)",
         "11m(e): 案例 22 必须以载荷冲突命名并注册到 main"),
        ("the same-frame Drawn payload conflict (cleared palette / other hitKey)",
         "11m(e): 案例 22 必须把冲突断言到 droppedPayloadConflict"),
        ("droppedPayloadConflict must be >= 1 for the 上级 counterexample",
         "11m(e): 案例 22 必须断言上级反例下 droppedPayloadConflict >= 1"),
        ("[OBSERVATION] D6 上级反例", "11m(e): 案例 22 必须打印实测行（[OBSERVATION]）"),
        ("[FINDING] 同键同帧同阶段的第二次 Drawn",
         "11m(e): 案例 22 必须打印 [FINDING] 实测行"),
        ("Rejected: a same-frame reject with a DIFFERENT reason",
         "11m(e): 案例 16 必须正面见证 Rejected 载荷冲突"),
        ("ServedCandidate: a same-frame serve with a DIFFERENT source",
         "11m(e): 案例 16 必须正面见证 ServedCandidate 载荷冲突"),
        ("Enqueued: a same-frame enqueue with a DIFFERENT selectionCleared flag",
         "11m(e): 案例 16 必须正面见证 Enqueued 载荷冲突"),
        ("Drawn: a same-frame draw with a DIFFERENT hitKey",
         "11m(e): 案例 16 必须正面见证 Drawn 载荷冲突"),
        ("a payload-equivalent duplicate must be counted as droppedDuplicatePerFrame",
         "11m(e): 案例 8 必须正面见证载荷等价 ⇒ droppedDuplicatePerFrame"),
        ("a payload-conflicting same-frame reject still may not add a second event",
         "11m(e): 案例 8 必须正面见证冲突不新增事件（不得像什么都没发生）"),
        ("PrintCounterTotals",
         "11m(e): 宿主机测试必须在 SUMMARY 之前打印含新计数的总计数"),
        ("[COUNTERS] shared-recorder snapshot before SUMMARY",
         "11m(e): 总计数打印必须包含 SUMMARY 之前的全字段快照"),
        ("droppedPayloadConflict",
         "11m(e): 宿主机测试必须覆盖新计数 droppedPayloadConflict")):
    assert _needle in RECORDER_TEST_TEXT, ("11m", _label, _needle)
# 旧行为（不看载荷一律记 droppedDuplicatePerFrame）的断言文字不得残留。
assert "the duplicate must be counted as droppedDuplicatePerFrame" not in RECORDER_TEST_TEXT, \
    "11m(e): 案例 8 不得再断言旧行为（同帧第二次 Reject 一律算已证明等价的重复）"
for _stale in ("the merged ServedCandidate must be counted as droppedDuplicatePerFrame",
               "the merged Enqueued must be counted as droppedDuplicatePerFrame",
               "the merged Drawn must be counted as droppedDuplicatePerFrame"):
    assert _stale not in RECORDER_TEST_TEXT, \
        ("11m(e)", "案例 16 旧口径残留（载荷冲突不得再被断言为等价重复）", _stale)

# 11m(f) 下游必须**自动**继承新损失：往返编排测试的零丢失集合由解析器 LOSS_FIELDS 派生，
#        且 C++ 往返夹具的全字段头块逐项打印必须包含新计数。
assert re.search(r"E_ZERO_COUNTERS\s*=\s*tuple\(sorted\(set\(analyzer\.LOSS_FIELDS\)",
                 ROUNDTRIP_TEST_TEXT), \
    "11m(f): 往返测试的并发零丢失集合必须由解析器 LOSS_FIELDS 派生（新损失自动纳入）"
assert re.search(r'" droppedPayloadConflict="\s*<<\s*c\.droppedPayloadConflict', ROUNDTRIP_CPP), \
    "11m(f): 往返夹具的全字段头块打印必须逐项包含 droppedPayloadConflict"

# ---- 11n. 记录器哈希混淆 + 布隆前置否定（2026-09-17 成本测量驱动；只加不减）----
# 成本测量实测：FNV-1a 低位在低熵键族上聚集（1024 键只落 6 个起始槽、最坏探测 1022）；
# 且线性探测在满表时退化（表满 + 未知键 ≈ 719 ns/次、每次走满 1024 槽）。
# 因此要求：(a) 取索引前做一次强混淆；(b) Find 先用布隆否定；(c) 位数足够大（否则被填满而失去过滤能力，
# 实测 1024 位时 1001 个未知键仍有 771 个走满全表）；(d) 插入置位、Reset 清空。
assert re.search(r'h \^= h >> 33;', RECORDER_H), \
    '11n(a): HashKey 必须做一次强混淆（低位聚集会让索引只落到极少数起始槽）'
assert '0xff51afd7ed558ccdull' in RECORDER_H and '0xc4ceb9fe1a85ec53ull' in RECORDER_H, \
    '11n(a): 混淆必须使用完整的 finalizer（两个 mix 常量）'
assert re.search(r'static\s+constexpr\s+uint32_t\s+kBloomBits\s*=\s*(\d+)u', RECORDER_H), \
    '11n(c): 必须有布隆位数常量'
_BLOOM_BITS = int(re.search(r'static\s+constexpr\s+uint32_t\s+kBloomBits\s*=\s*(\d+)u', RECORDER_H).group(1))
assert _BLOOM_BITS >= 16384, ('11n(c): 布隆位数必须足够大', _BLOOM_BITS)
# 2026-09-18 P0-6（复审 C1-1 的连带清理）：**链型无关的 Find / FindOwner 已删除** ——
# 它们正是此前两条静默失效的机制（FindOwner ⇒ 拒绝链拿不到恢复事实；Find ⇒ 观察链拿不到任何路径事实），
# 而且误用它们**能编译通过**（复审做路由变异时是静默成功的）⇒ 删除本身就是防回归手段。
# 断言因此改为：① 二者必须**不存在**；② 布隆否定优化必须保留在**唯一剩下的**键查找 FindChain 上。
assert 'Entry* Find(const PaletteObjectKey& key)' not in RECORDER_H, \
    '11n(b)/C1: 链型无关的 Find() 必须已被删除（否则 S/E/D 可能静默退回类型无关查找）'
assert 'Entry* FindOwner(' not in RECORDER_H, \
    '11n(b)/C1: FindOwner() 必须已被删除（它让拒绝链拿不到恢复事实）'
_FIND_BODY, _ = extract_function(RECORDER_H, 'Entry* FindChain(')
_FIND_CODE = strip_comments(_FIND_BODY)
assert 'BloomMight' in _FIND_CODE and 'HashKey' in _FIND_CODE, \
    '11n(b): 唯一剩下的键查找必须先用布隆否定（把满表下的未知键从全表走查变成常数）'
assert _FIND_CODE.index('BloomMight') < _FIND_CODE.index('m_entries'), \
    '11n(b): 布隆检查必须先于任何槽位走查'
_INSERT_BODY, _ = extract_function(RECORDER_H, 'Entry* Insert(const PaletteObjectKey& key, uint64_t frame,')
assert 'BloomSet' in strip_comments(_INSERT_BODY), '11n(d): 插入成功路径必须置位布隆'
for _reset_name in ('void Reset(uint64_t sessionGeneration, uint64_t mapEpoch)',
                    'void ResetForSessionTransition(uint64_t sessionGeneration, uint64_t mapEpoch)'):
    _body, _ = extract_function(RECORDER_H, _reset_name)
    assert 'm_bloom' in strip_comments(_body), ('11n(d): Reset 必须清空布隆', _reset_name)

# ---- 11o. D 点独立小型按值诊断载荷（2026-09-17 上级裁定；只加不减）----
# 缺陷：D 点原读 draw.inputSkinSelection，而它只在 InputsEnabled()（高内存原始输入取证）下赋值，
# 因此正常录制里 D 的来源/帧标签恒不可得。要求：独立小载荷**无条件**赋值、且 D 点只读它。
_SCENE_H = (ROOT / 'src/d3d9/d3d9_war3_scene.h').read_text(encoding='utf-8', errors='replace')
assert re.search(r'struct\s+PaletteObjectDiagnostics\s*\{', _SCENE_H), \
    '11o(a): 场景绘制命令必须有独立的小型按值诊断载荷结构'
assert re.search(r'paletteDiagnostics\s*;', _SCENE_H), '11o(a): 该载荷必须是绘制命令的成员'
_DEVICE_TEXT = (ROOT / 'src/d3d9/d3d9_device.cpp').read_text(encoding='utf-8', errors='replace')
assert 'draw.paletteDiagnostics.source = static_cast<uint32_t>(selectedPalette.source);' in _DEVICE_TEXT, \
    '11o(b): 该载荷必须在生成绘制命令时赋值'
_INPUTS_ANCHOR = 'if (war3::tools::evidence::InputsEnabled()) {'
assert _INPUTS_ANCHOR in _DEVICE_TEXT, '11o(b): 找不到 InputsEnabled 分支（基线变化，需人工复核）'
_assign_at = _DEVICE_TEXT.index('draw.paletteDiagnostics.source =')
_inputs_at = _DEVICE_TEXT.index(_INPUTS_ANCHOR)
assert _assign_at < _inputs_at, \
    '11o(b): 诊断载荷赋值必须**在** InputsEnabled() 分支之前（否则又变成只有开启原始输入取证才可得）'
assert 'draw.paletteDiagnostics = {};' in _DEVICE_TEXT, \
    '11o(c): native override 清空 Selection 的同一处必须同时清空诊断载荷'
_SHADOW_TEXT = (ROOT / 'src/d3d9/d3d9_war3_shadow.cpp').read_text(encoding='utf-8', errors='replace')
assert 'draw.paletteDiagnostics.frameTag' in _SHADOW_TEXT and \
       'draw.paletteDiagnostics.source' in _SHADOW_TEXT and \
       'draw.paletteDiagnostics.slot' in _SHADOW_TEXT, \
    '11o(d): D 点必须读独立诊断载荷'
_drawn_at = _SHADOW_TEXT.index('NoteDrawn(')
_drawn_window = _SHADOW_TEXT[_drawn_at:_drawn_at + 900]
assert 'draw.inputSkinSelection' not in _drawn_window, \
    '11o(d): D 点不得再读 inputSkinSelection（那正是只在 InputsEnabled() 下赋值的字段）'

# ---- 11p. 冻结格式的**显式**版本化 + 统一接入（2026-09-17 上级裁定 ⑦；只加不减）------
# 缺陷：导出仍写 schema 7，但根对象多了 paletteObject 块 —— 通用读方（根字段精确相等）对**每个**
# 真实 palette 导出报 "root fields mismatch"；palette 读方只能"投影掉该块再委托"；
# history/watcher 入口（analyze_frame_history / frame_history_watch）根本读不了这类导出。
# 修后合同：扩展在通用读方**显式登记**并由它自己判定版本；palette 读方把**整个根**交给通用读方；
# 旧产物（无 version 的冻结形状）仍按旧合同可读；未知版本 / 未知形状显式拒绝。
_FRAME_READER_PATH = ROOT / 'AutoTest/analyze_frame_evidence.py'
assert _FRAME_READER_PATH.is_file(), str(_FRAME_READER_PATH)
_FRAME_READER_TEXT = _FRAME_READER_PATH.read_text(encoding='utf-8')
for _needle, _label in (
        ('PALETTE_OBJECT_EXTENSION=', '必须登记扩展名常量'),
        ('PALETTE_OBJECT_VERSION_FIELD=', '必须登记显式版本字段名'),
        ('PALETTE_OBJECT_LEGACY_VERSION=', '必须有旧格式（冻结形状）版本常量'),
        ('PALETTE_OBJECT_SEGMENTED_VERSION=', '必须有新格式（分段）版本常量'),
        ('PALETTE_OBJECT_LEGACY_BLOCK_FIELDS=frozenset(', '必须登记旧格式的冻结块形状'),
        ('PALETTE_OBJECT_BLOCK_FIELDS={', '必须登记各版本的块字段集'),
        ('FROZEN_EXTENSIONS={', '必须有已登记扩展表'),
        ('def extension_version(name,block):', '必须有**唯一**的扩展版本判定入口'),
        ('def root_extensions(d):', '必须有根扩展枚举入口')):
    assert _needle in _FRAME_READER_TEXT, ('11p', _label, _needle)
assert re.search(r"require\(declared in PALETTE_OBJECT_BLOCK_FIELDS,", _FRAME_READER_TEXT), \
    '11p: 未知版本必须被显式拒绝（declared in 登记版本集合）'
assert re.search(r"^SCHEMA=7$", _FRAME_READER_TEXT, re.MULTILINE), '11p: 冻结 schema 仍是 7'
# 通用读方必须在根字段精确相等检查里**放行已登记扩展**（否则真实导出仍被拒）。
assert re.search(r"^\s*require\(set\(d\)==allowed\|extension_fields,", _FRAME_READER_TEXT,
                 re.MULTILINE), \
    '11p: 根字段检查必须放行**已登记**扩展（未登记字段仍是硬错误）'
# 版本常量在通用读方与 palette 读方之间必须**同源**（单一来源，不得各写一份）。
import sys as _sys  # noqa: E402  （本节自足）
_sys.path.insert(0, str(ROOT / 'AutoTest'))
import analyze_frame_evidence as _frame_reader  # noqa: E402
assert _frame_reader.PALETTE_OBJECT_LEGACY_VERSION == 1, '11p: 旧格式版本必须是 1'
assert _frame_reader.PALETTE_OBJECT_SEGMENTED_VERSION == 2, '11p: 新格式版本必须是 2'
assert _palette_analyzer.PALETTE_OBJECT_LEGACY_VERSION == \
       _frame_reader.PALETTE_OBJECT_LEGACY_VERSION, '11p: 版本常量必须同源（legacy）'
assert _palette_analyzer.PALETTE_OBJECT_SEGMENTED_VERSION == \
       _frame_reader.PALETTE_OBJECT_SEGMENTED_VERSION, '11p: 版本常量必须同源（segmented）'
assert _palette_analyzer.PALETTE_OBJECT_VERSION_FIELD == \
       _frame_reader.PALETTE_OBJECT_VERSION_FIELD, '11p: 版本字段名必须同源'
# 统一接入：(1) palette 读方必须把整个根交给通用读方，投影绕行必须消失；
#           (2) history/watcher 入口只能通过通用读方读根（不得自带扩展判定）。
assert re.search(r'^\s*frame_analysis=frame_evidence\.analyze\(d\)$', ANALYZER, re.MULTILINE), \
    '11p: palette 读方必须把整个根交给通用读方（统一入口）'
assert "if name!='paletteObject'" not in ANALYZER, \
    '11p: "投影掉 paletteObject 再委托"的绕行必须消失'
for _entry in ('analyze_frame_history.py', 'frame_history_watch.py'):
    _entry_text = (ROOT / 'AutoTest' / _entry).read_text(encoding='utf-8')
    assert 'from analyze_frame_evidence import load,analyze' in _entry_text, \
        ('11p: history/watcher 入口必须走通用读方', _entry)
    assert 'paletteObject' not in _entry_text, \
        ('11p: history/watcher 入口不得自带扩展/根校验', _entry)


def _extension_root(version_marker=None, block_extra=None):
    """最小但**完整**的 schema-7 根（events 为空）：只用来判定扩展版本合同。"""
    root = {'schema': 7, 'state': 3, 'reason': 1, 'session': '1', 'processId': 4321,
            'processNonce': '987654321', 'qpcFrequency': '10000000', 'accepted': '0',
            'evicted': '0', 'triggerSequence': '0', 'producerLosses': '0', 'reserved': '0',
            'capacity': 4096, 'postRemaining': 0,
            'effectiveConfiguration': {'frameEvidence': True, 'rawInputs': False,
                                       'paletteObjectEvidence': True,
                                       'skinPaletteContract': False,
                                       'localRecorderOwner': False},
            'captureComplete': False, 'rootCauseReady': False,
            'capabilities': {name: name == 'cpuBoundaryEvents'
                             for name in _frame_reader.CAPABILITIES},
            'events': []}
    block = {'watchCount': 0,
             'counters': {name: '0' for name in _palette_analyzer.COUNTER_FIELDS}}
    if version_marker is not None:
        block['version'] = version_marker
    block.update(block_extra or {})
    root['paletteObject'] = block
    return root


# 运行期：已登记扩展被接受并如实上报版本；未知版本/未知形状被拒；无扩展的旧产物仍可读。
assert _frame_reader.analyze(_extension_root())['extensions'] == {'paletteObject': 1}, \
    '11p: 无 version 的冻结形状必须按旧合同读作版本 1'
assert _frame_reader.analyze(_extension_root(2))['extensions'] == {'paletteObject': 2}, \
    '11p: 显式 version=2 必须被接受并上报'
assert _frame_reader.analyze(_extension_root(1))['extensions'] == {'paletteObject': 1}, \
    '11p: 显式 version=1 必须被接受并上报'
# 2026-09-18 更正：3 已是**已登记**版本（v3 = 分段 + 正常观察链 FirstSight）。
# 此前这里（与解析器测试同处）把 3 当非法版本，而写方会按 firstSightUsed() 发 v3 ⇒
# 两处断言都会把**正确行为**判为错误。故移除 3，并新增 v3 的正向见证。
assert _frame_reader.analyze(_extension_root(3))['extensions'] == {'paletteObject': 3}, \
    '11p: 显式 version=3 必须被接受并上报'
# 2026-09-18 阶段 C：4 也已成为**已登记**版本（v4 = 链型 + ObservationClosed）⇒
# 从非法列表中移除 4、改用真正未登记的 5，并新增 v4 的**正向见证**（与 v3 对称）。
# **要求未变**：未登记版本必须被拒绝；已登记版本必须被接受并上报。
assert _frame_reader.analyze(_extension_root(4))['extensions'] == {'paletteObject': 4}, \
    '11p: 显式 version=4 必须被接受并上报'
for _bad in (0, 5, 7, 99, -1, '2', 2.0, True):
    try:
        _frame_reader.analyze(_extension_root(_bad))
    except ValueError:
        pass
    else:
        raise AssertionError(('11p: 未知扩展版本必须被拒绝', _bad))
for _extra in ({'invented': 1}, {'counters2': {}}):
    for _marker in (None, 2):
        try:
            _frame_reader.analyze(_extension_root(_marker, _extra))
        except ValueError:
            pass
        else:
            raise AssertionError(('11p: 未知块形状必须被拒绝', _marker, sorted(_extra)))
_legacy_root = _extension_root()
del _legacy_root['paletteObject']
assert _frame_reader.analyze(_legacy_root)['extensions'] == {}, \
    '11p: 旧实机产物（没有扩展块）必须仍按旧合同可读'
_unknown_root = _extension_root()
_unknown_root['paletteObjectV2'] = {'watchCount': 0}
try:
    _frame_reader.analyze(_unknown_root)
except ValueError:
    pass
else:
    raise AssertionError('11p: 未登记的根扩展字段必须被拒绝')

# 11p(e) 字段存在 ≠ 身份已证明：身份证明必须**载明**（登记种类的载体），
#        版本 1 必须如实报告"只是记录器自己的 flags 声明，不是证明"。
assert re.search(r"^IDENTITY_PROOF_KINDS=\{0:'NoIdentityProof',1:'InstanceLifecycleIdentityProof'\}$",
                 ANALYZER, re.MULTILINE), '11p(e): 必须登记身份证明种类（0 = 未载明）'
assert re.search(r"^REFUSAL_IDENTITY_NOT_PROVEN='identityNotProven'$", ANALYZER, re.MULTILINE), \
    '11p(e): 未载明身份证明必须具名拒绝（identityNotProven）'
assert re.search(r"^IDENTITY_BASIS_FLAGS_ONLY='recorderFlagClaimOnly'$", ANALYZER, re.MULTILINE), \
    '11p(e): 版本 1 必须如实标注"只有记录器 flags 声明"'
assert re.search(r"identity_proven=bool\(segmented and proof_kinds==\[1\]", ANALYZER), \
    '11p(e): identityProven 必须要求**载明的证明 + 非零身份值**（identityWeak=0 本身不足）'

# ---- 11q. 窗口/Reset 的**可识别**分段（2026-09-17 上级裁定 ⑧；只加不减）-------------
# 清表 ≠ 分段：同会话、同地图、deviceEpoch 未知时，Reset 前后的记录可以八元组完全相同。
# 分段必须是**记录级**载体并进入分组键；旧格式没有标签时必须 fail-closed，不得按顺序猜。
assert re.search(r"^WINDOW_SEGMENT_FIELD='windowSegment'$", ANALYZER, re.MULTILINE), \
    '11q: 必须有分段字段名'
assert re.search(r"^WINDOW_SEGMENT_SLOT=\('data',3\)$", ANALYZER, re.MULTILINE), \
    '11q: 分段量必须有登记的记录级载体槽位'
assert re.search(r"^IDENTITY_PROOF_SLOT=\('words32',15\)$", ANALYZER, re.MULTILINE), \
    '11q: 身份证明必须有登记的记录级载体槽位'
assert 'def chain_group_identity(event):' in ANALYZER, \
    '11q: 分组键必须由一个显式函数给出（八元组 + 分段量）'
assert 'grouped.setdefault(chain_group_identity(event),[])' in ANALYZER, \
    '11q: 离线分组必须使用含分段量的分组键'
# 2026-09-18 阶段 C（Q2）：分组键还必须含**链型** —— 同一对象现在可以同时持有观察链与
# 拒绝恢复链两条独立条目，只按八元组 + 分段分组会把它们并成一条链。
# 分段标签必须仍在**末位**（调用方依赖 item[0][-1] 判断"有无分段标签"）⇒ 本断言比原来更强。
assert re.search(r'return chain_identity\(event\)\+\(event\["chainType"\],event\[WINDOW_SEGMENT_FIELD\]\)', ANALYZER), \
    '11q: 分组键必须真的包含分段量'
assert 'return data[WINDOW_SEGMENT_SLOT[1]]' in ANALYZER, \
    '11q: 分段量必须逐记录读取（冻结后摘要不得因后续状态变化）'
assert '{event[WINDOW_SEGMENT_FIELD] for event in events}' in ANALYZER, \
    '11q: 导出级分段摘要只能由记录自身导出'
assert re.search(r"palette window segments must be non-decreasing", ANALYZER), \
    '11q: 分段必须非递减（分段块不得在更晚的分段之后再现）'
# 旧合同不放宽：版本 1 下两个新槽位仍是必须为 0 的保留槽；版本 2 才启用。
assert 3 in _palette_analyzer.reserved_data_indices(), \
    '11q: 版本 1 下 data[3] 必须仍是保留零'
assert 15 in _palette_analyzer.reserved_bit_indices(), \
    '11q: 版本 1 下 words32[15] 必须仍是保留零'
assert 3 not in _palette_analyzer.reserved_data_indices(2), \
    '11q: 版本 2 下 data[3] 是分段载体（不得再算保留零）'
assert 15 not in _palette_analyzer.reserved_bit_indices(2), \
    '11q: 版本 2 下 words32[15] 是身份证明载体（不得再算保留零）'

# 运行期反例（修前/修后同一份夹具）：同键 + Reset 前后各一条完整链。
_palette_fixture = __import__('test_palette_object_evidence_analysis_static')
_window_one = _palette_fixture.segment_chain_events(part=1, segment=1, sequence_base=1,
                                                    frame_base=500, chain_sequence_base=1)
_window_two = _palette_fixture.segment_chain_events(part=1, segment=2, sequence_base=6,
                                                    frame_base=700, chain_sequence_base=1)
_segmented = _palette_analyzer.analyze(_palette_fixture.envelope(
    _window_one + _window_two, format_version=2))
assert _segmented['objectCount'] == 2 and _segmented['windowSegments'] == [1, 2], \
    ('11q: 同键不同窗口必须分成两条链', _segmented['objectCount'], _segmented['windowSegments'])
assert len(_segmented['recovered']) == 2 and all(
    chain['sameObjectCertified'] and chain['identityProven']
    for chain in _segmented['chains']), '11q: 两条分段完整链都必须被认证'
assert [chain['windowSegment'] for chain in _segmented['chains']] == [1, 2], \
    '11q: 每条链必须如实报告自己的分段量'
# 同键但**没有**分段量（旧合同）：必须 fail-closed（整份导出被拒），绝不按顺序启发式拆分。
_legacy_one = _palette_fixture.closed_chain_events(part=1, sequence_base=1, frame_base=500)
_legacy_two = _palette_fixture.closed_chain_events(part=1, sequence_base=6, frame_base=700)
try:
    _palette_analyzer.analyze(_palette_fixture.envelope(_legacy_one + _legacy_two))
except ValueError:
    pass
else:
    raise AssertionError('11q: 旧格式同键跨 Reset 的两条终态链必须被拒（不得被猜成两条或一条）')
# 版本 2 缺少分段量（全 0）必须被拒。
try:
    _palette_analyzer.analyze(_palette_fixture.envelope(
        _palette_fixture.segment_chain_events(segment=0), format_version=2))
except ValueError:
    pass
else:
    raise AssertionError('11q: 版本 2 缺分段量必须被拒（不得退化成猜）')
# 版本 2 未载明身份证明：阶段观察可以完整，但**不得**认证同对象恢复。
_unproven = _palette_analyzer.analyze(_palette_fixture.envelope(
    _palette_fixture.segment_chain_events(segment=1, identity_proof=0), format_version=2))
assert _unproven['recovered'] == [] and \
       'identityNotProven' in _unproven['chains'][0]['certificationRefusals'], \
    '11q/11p: identityWeak 的 0 本身不足以认证同对象恢复'

# ---- 11r. D2 生产写入侧：记录级分段 + 载明的身份证明种类（2026-09-17 上级裁定 ⑦⑧；只加不减）----
# D2 之前读方合同已就绪，但生产写入侧是版本 1（data[3]/words32[15] 仍必须是保留零）。D2 之后生产
# **确实**写分段与证明种类，所以头块版本必须是 2。本段钉死四项，全部从**生产源码文本**重推：
#   (a) 分段：Reset() 置第 1 个窗口；ResetForSessionTransition() **递增**（不回绕），清表 ≠ 分段；
#   (b) 记录级盖章：MakeRecord()（static 改 const 成员）与 NoteReject 的一次性 TableFull 终态；
#   (c) 证明种类：唯一推导规则 DeriveIdentityProofKind()，三个条件缺一不可、无 setter 后门，
#       且只有记录器能写它（sink 只搬值、测试不得从外部写）；
#   (d) wire：sink 写 data[3]/words32[15]，其余保留位仍不得被写；头块 version=2。
_RECORDER_RESET_BODY, _ = extract_function(
    RECORDER_H, "void Reset(uint64_t sessionGeneration, uint64_t mapEpoch) {")
_RESET_CODE = strip_comments(_RECORDER_RESET_BODY)
assert re.search(r"m_windowSegment\s*=\s*1u\s*;", _RESET_CODE), \
    "11r(a): Reset() 必须把窗口序号置为 1（新会话 = 第 1 个窗口）"
_RECORDER_TRANSITION_BODY, _ = extract_function(
    RECORDER_H, "void ResetForSessionTransition(uint64_t sessionGeneration, uint64_t mapEpoch) {")
_TRANSITION_CODE = strip_comments(_RECORDER_TRANSITION_BODY)
assert re.search(r"\+\+\s*m_windowSegment\s*;", _TRANSITION_CODE), \
    "11r(a): ResetForSessionTransition() 必须**递增**窗口序号（清表 ≠ 分段）"
assert "m_windowSegment = 1u" not in _TRANSITION_CODE, \
    "11r(a): 换图/设备代际/显式清表不得把窗口序号回绕到 1（否则新旧窗口会再次相撞）"
assert re.search(r"uint64_t\s+m_windowSegment\s*=\s*0u\s*;", RECORDER_H), \
    "11r(a): 记录器必须有窗口序号成员（0 = 从未开过窗口；版本 2 读方要求每条分段 >= 1）"

_MAKE_RECORD_BODY, _ = extract_function(
    RECORDER_H, "PaletteObjectEventRecord MakeRecord(const Entry& e) const {")
_MAKE_RECORD_CODE = strip_comments(_MAKE_RECORD_BODY)
assert re.search(r"record\.windowSegment\s*=\s*m_windowSegment\s*;", _MAKE_RECORD_CODE), \
    "11r(b): MakeRecord() 必须盖记录级分段（发出时写一次）"
assert re.search(r"record\.identityProofKind\s*=\s*DeriveIdentityProofKind\(e\.key\)\s*;",
                 _MAKE_RECORD_CODE), \
    "11r(b): MakeRecord() 必须按 DeriveIdentityProofKind(e.key) 盖证明种类（不得由调用点给值）"
assert _MAKE_RECORD_CODE.count("record.windowSegment") == 1 and \
       _MAKE_RECORD_CODE.count("record.identityProofKind") == 1, \
    "11r(b): 两个声明载体在 MakeRecord() 里必须各恰好写一次"
_TABLE_FULL_CODE_11R = strip_comments(
    extract_function(RECORDER_H, "void NoteReject(const PaletteObjectKey& key,")[0])
assert re.search(r"record\.windowSegment\s*=\s*m_windowSegment\s*;", _TABLE_FULL_CODE_11R) and \
       re.search(r"record\.identityProofKind\s*=\s*DeriveIdentityProofKind\(key\)\s*;",
                 _TABLE_FULL_CODE_11R), \
    "11r(b): 一次性 TableFull 终态（不过 MakeRecord）也必须盖分段与证明种类"

assert re.search(r"enum\s+class\s+PaletteObjectIdentityProofKind\s*:\s*uint32_t\s*\{", RECORDER_H), \
    "11r(c): 必须显式登记载明的身份证明种类枚举"
for _kind, _value in (("NoIdentityProof", "0u"), ("InstanceLifecycleIdentityProof", "1u")):
    assert re.search(r"\b" + _kind + r"\s*=\s*" + _value + r"\s*,", RECORDER_H), \
        ("11r(c): 证明种类登记必须与读方 IDENTITY_PROOF_KINDS 同源", _kind, _value)
_DERIVE_BODY, _ = extract_function(
    RECORDER_H, "static uint32_t DeriveIdentityProofKind(const PaletteObjectKey& key) {")
_DERIVE_CODE = strip_comments(_DERIVE_BODY)
assert re.search(r"key\.lifecycleIdentity\s*!=\s*0u", _DERIVE_CODE), \
    "11r(c): 推导规则的第一个条件必须是非零实例生命周期身份"
assert re.search(r"!\s*key\.identityWeak\b", _DERIVE_CODE), \
    "11r(c): 推导规则必须要求 identityWeak == false"
assert re.search(r"!\s*key\.epochUnknown\b", _DERIVE_CODE), \
    "11r(c): 推导规则必须要求 epochUnknown == false"
assert re.search(r"\?\s*static_cast<uint32_t>\(\s*"
                 r"PaletteObjectIdentityProofKind::InstanceLifecycleIdentityProof\s*\)", _DERIVE_CODE) and \
       re.search(r":\s*static_cast<uint32_t>\(\s*"
                 r"PaletteObjectIdentityProofKind::NoIdentityProof\s*\)", _DERIVE_CODE), \
    "11r(c): 三个条件同时成立才是 InstanceLifecycleIdentityProof，其余一律 NoIdentityProof"
# 无后门：不得有任何 setter / 测试专用写入口；证明种类的赋值只允许出现在记录器头。
assert not re.search(r"(?i)set[_A-Za-z0-9]*identity[_A-Za-z0-9]*proof", RECORDER_H + SINK), \
    "11r(c): 不得新增身份证明种类的 setter/后门（推导规则是唯一写入方）"
assert len(re.findall(r"identityProofKind\s*=(?!=)", RECORDER_H)) == 3, \
    ("11r(c): 证明种类只允许在记录器头出现三次赋值（字段默认值 + MakeRecord + TableFull）",
     len(re.findall(r"identityProofKind\s*=(?!=)", RECORDER_H)))
for _text, _label in ((RECORDER_TEST_TEXT, "记录器宿主机测试"),
                      (ROUNDTRIP_CPP, "往返 C++ 夹具")):
    assert not re.search(r"identityProofKind\s*=(?!=)", _text), \
        ("11r(c): 测试不得从外部写证明种类（必须由记录器推导）", _label)
_ENCODER_BODY_11R, _ = extract_function(SINK, "bool EncodePaletteObjectEvent(")
_ENCODER_CODE_11R = strip_comments(_ENCODER_BODY_11R)
assert len(re.findall(r"identityProofKind", _ENCODER_CODE_11R)) == 1 and \
       "record.identityProofKind" in _ENCODER_CODE_11R, \
    "11r(c): 转换点只允许**读**证明种类一次（不得在 sink 里重新推导/改写）"

capture_h = (ROOT / "src/d3d9/war3/tools/war3_palette_object_capture.h").read_text(encoding="utf-8")
for _needle, _label in (("key.lifecycleIdentity = 0u;", "生产采集点必须写 lifecycleIdentity = 0"),
                        ("key.identityWeak = true;", "生产采集点必须写 identityWeak = true"),
                        ("key.epochUnknown = true;", "生产采集点必须写 epochUnknown = true")):
    assert _needle in capture_h, ("11r(c): 三个采集点必须保持未认证口径（生产 proofKind 恒 0）", _label)

assert re.search(r"event\.data\[3\]\s*=\s*record\.windowSegment\s*;", SINK), \
    "11r(d): 转换点必须写 data[3] = record.windowSegment"
assert re.search(r"event\.bits\[15\]\s*=\s*static_cast<uint32_t>\(record\.identityProofKind\)\s*;",
                 SINK), \
    "11r(d): 转换点必须写 words32[15] = record.identityProofKind"
# 其余保留位仍不得被写：写入集合 | 版本 1 保留集合 必须恰好铺满全部槽位（两个方向都不许漏）。
_sink_data_written = {int(index) for index in re.findall(r"event\.data\[(\d+)\]\s*=", SINK)}
_sink_bits_written = {int(index) for index in re.findall(r"event\.bits\[(\d+)\]\s*=", SINK)}
assert _sink_data_written | set(_palette_analyzer.reserved_data_indices()) == set(range(12)), \
    "11r(d): data 槽位必须恰好被「写入 ∪ 版本 1 保留零」铺满（其余保留位不得被写）"
assert _sink_bits_written | set(_palette_analyzer.reserved_bit_indices()) == set(range(48)), \
    "11r(d): words32 槽位必须恰好被「写入 ∪ 版本 1 保留零」铺满（其余保留位不得被写）"
assert 3 in _sink_data_written and 15 in _sink_bits_written, \
    "11r(d): 两个版本 2 载体必须由生产转换点写入"
assert _sink_data_written == {0, 1, 2, 3, 4, 9, 10, 11} and \
       _sink_bits_written == {0, 1, 2, 5, 10, 12, 14, 15, 16, 18, 20, 21, 22, 23, 24, 25, 26, 27,
                              28, 29, 30, 31, 32, 33}, \
    ("11r(d): 转换点的写入集合不得新增未登记的槽位",
     sorted(_sink_data_written), sorted(_sink_bits_written))
assert 3 not in _palette_analyzer.reserved_data_indices(2) and \
       15 not in _palette_analyzer.reserved_bit_indices(2) and \
       3 in _palette_analyzer.reserved_data_indices(1) and \
       15 in _palette_analyzer.reserved_bit_indices(1), \
    "11r(d): 版本 1 仍把两个槽当保留零，版本 2 才启用（旧产物合同一位不放宽）"

# 2026-09-18 批次 3（外部独立复审批定）：正常观察链引入阶段 FirstSight(5)，写方据此按
# `firstSightUsed() ? 3 : 2` 抬版本；未使用首见链的导出仍是版本 2（旧读方行为一位不变）。
# 因此本断言接受「字面 2」或「条件表达式 3:2」两种显式写法；下面的 version=1 仍被拒绝。
# 阶段 C（2026-09-18）：写方改为**恒定 v4**。版本是契约，不再按 firstSightUsed() 动态选择，
# 因此本断言由「字面 2 或条件表达式 3:2」收紧为「**必须恰好** version=4」，
# 并**显式禁止**动态选择回归 —— 比原断言更强，不是放宽。
assert re.search(r'result\["version"\]\s*=\s*4\s*;', HEADER_JSON_CODE), \
    "11r(d): 头块必须**恒定**写 version=4（阶段 C 的链型 + ObservationClosed 契约）"
assert not re.search(
        r'result\["version"\]\s*=\s*PaletteObjectRecorder\(\)\.firstSightUsed\(\)',
        HEADER_JSON_CODE), \
    "11r(d): 头块不得再按 firstSightUsed() 动态选择版本"
assert not re.search(r'result\["version"\]\s*=\s*1\s*;', HEADER_JSON_CODE), \
    "11r(d): 头块不得再写 version=1（生产已经开始写分段与证明种类）"

# 取证：宿主机测试必须有记录器出口的运行时见证（分段 1/2/3 + 生产键 kind=0 + 合成键 kind=1），
# 往返编排测试必须有「声明后的语义」断言（取代旧的"保留位必须为 0"）。
for _needle, _label in (
        ("bool Case23WindowSegmentAndIdentityProofKind()",
         "宿主机测试必须有 D2 的直接见证案例 23"),
        ('RunCase("23 window segment + carried identity proof kind (D2)"',
         "案例 23 必须注册到 main"),
        ("the first window after Reset() must be windowSegment 1",
         "案例 23 必须断言 Reset 之后第一条记录的分段是 1"),
        ("ResetForSessionTransition must open window 2 (incremented, never back to 1)",
         "案例 23 必须断言换图清表开窗口 2（递增）"),
        ("identityProofKind requires ALL THREE conditions",
         "案例 23 必须断言三个条件缺一不可"),
        ("a production-shaped key must carry NoIdentityProof(0)",
         "案例 23 必须实测生产口径键 proofKind=0"),
        ("a synthetic certified key must carry InstanceLifecycleIdentityProof(1)",
         "案例 23 必须实测合成认证键 proofKind=1（认证路径仍可执行）"),
        ("[OBSERVATION] D2 window segment / identity proof kind",
         "案例 23 必须打印实测行"),
        ("[FINDING] 2026-09-17 D2 production write side",
         "案例 23 必须打印 [FINDING] 实测行")):
    assert _needle in RECORDER_TEST_TEXT, ("11r", _label, _needle)
assert "kCertifiedLifecycleIdentity" in ROUNDTRIP_CPP, \
    "11r: 往返 C++ 夹具必须有合成实例生命周期身份常量（生产不可达、合成键可达）"
for _needle, _label in (
        ("data[3] must carry the record-level windowSegment",
         "往返编排测试必须按声明后的语义断言 data[3] == windowSegment"),
        ("words32[15] must carry identityProofKind",
         "往返编排测试必须按声明后的语义断言 words32[15] == identityProofKind"),
        ("must carry the record-level window segment",
         "往返编排测试必须在解析器结果上断言分段量"),
        ("the production export must declare extension version 2",
         "往返编排测试必须钉死生产导出 = 版本 2"),
        ("identityProven must follow the *declared* proof",
         "往返编排测试必须钉死 identityProven 只由**载明的**证明决定"),
        ("CERTIFIED_LIFECYCLE_IDENTITY",
         "往返编排测试必须钉死合成认证身份值（A/F 场景）"),
        ("F: every auto-frozen record must carry the declared window segment",
         "往返编排测试必须钉死自动冻结路径也携带分段与证明种类")):
    assert _needle in ROUNDTRIP_TEST_TEXT, ("11r", _label, _needle)
# 旧断言必须消失：不得再要求 data[3]/words32[15] 恒为 0。
assert "data[3..8] reserved must be zero" not in ROUNDTRIP_CPP, \
    "11r: 往返夹具不得再断言 data[3..8] 全为保留零（data[3] 已是声明后的语义）"
assert "bits[15] reserved must be zero" not in ROUNDTRIP_CPP, \
    "11r: 往返夹具不得再断言 words32[15] 为保留零（它已是声明后的语义）"
assert "data[3..8] reserved (0)" in ANALYZER, \
    "11r: palette 读方 docstring 必须仍然描述旧合同的 data[3..8] 保留形状（版本 1 不放宽）"

print("semantic build thread gate static checks passed")

# 2026-09-18 P0-5（复审 C2）：发射路径的两条早退**必须计数**，不得无声丢弃。
# （真实环路径实测过：冻结/重入期间一条嵌套 emit 无计数被丢弃，读方据此 emitted != exported。）
for _needle, _label in (("g_paletteObjectDroppedNoSession.fetch_add", "会话未激活"),
                        ("g_paletteObjectEncodeFailed.fetch_add", "编码失败")):
    assert _needle in SINK, \
        ("C2: 发射路径的早退必须计入具名计数（否则是无声丢失）", _label)

