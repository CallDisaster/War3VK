# 门禁覆盖审计：`test_*_static.py` **之外**的 40 个测试文件 — 2026-09-18（round 73）

> 起因：round 72 查出 `test_analyze_frame_evidence.py` 曾有**假失败**，而它**不在**
> `test_*_static.py` 门禁内（所以长期没被发现）。
> ⇒ 那就把**门禁外**的文件**全部实跑**一遍：那里有没有别的红？

## 1. 覆盖面

```
AutoTest 下 test_*.py            = 303
  门禁内 (test_*_static.py)      = 263      ← 我每轮报告的就是这一批
  门禁外                        =  40      ← **13%** 从不参与任何门禁运行
```

## 2. 实跑结果（门禁外 40 个，本轮跑了 36 个）

### ✅ 绿（33 个）

```
test_recorder_event_wire_golden.py            exit=0  checks=24 PASS
test_self_contained_recorder_gate.py          exit=0  OK
test_recorder_control_lease_gate.py           exit=0  OK
test_analyze_frame_evidence.py                exit=0  OK
test_analyze_frame_history.py                 exit=0  OK
test_analyze_frame_timeline.py                exit=0  OK
test_frame_inputs.py                          exit=0  OK
test_hot_shadow_final_publication_contract.py exit=0  PASS: 16/16
test_analyze_skin_palette_selection.py        exit=0  OK
test_analyze_data_collection_perf_report.py   exit=0  OK
test_analyze_player_fissure_pairs_20260914.py exit=0  OK
test_analyze_recorder_offload_e2.py           exit=0  OK
test_architecture_python_checks_runner.py     exit=0  OK
test_autotest_sessions.py                     exit=0  OK
test_d3d9_memory_chunk_census_offline.py      exit=0  OK
test_fissure_localized_analysis.py            exit=0  OK
test_native_light_multi_endpoint.py           exit=0  OK
test_persistent_expiry_queue_model.py         exit=0  OK
test_registry_domain_count_export.py          exit=0  OK
test_registry_domain_model.py                 exit=0  OK
test_render_host_producer_inbox_gate.py       exit=0  OK
test_render_host_sample_worker_gate.py        exit=0  OK
test_render_host_shared_slots_gate.py         exit=0  OK
test_render_host_transport_gate.py            exit=0  OK
test_resource_residency_chunk_census_offline.py exit=0  OK
test_skin_palette_crash_diagnostic.py         exit=0  OK
test_stormbreaker_stable_policy_offline.py    exit=0  OK
test_war3_autotest_process_liveness.py        exit=0  OK
test_ydhost_lan_adapter.py                    exit=0  OK
test_ydwe_instance_launcher.py                exit=0  OK
test_ydwe_job_owner.py                        exit=0  OK
test_ydwe_lan_protocol.py                     exit=0  OK
test_ydwe_runtime_catalog.py                  exit=0  OK
```

### 🔴 红（3 个）—— **都在基线包里逐字相同 ⇒ 不是我造成的**

这三处不是猜测：我核对了它们在**不可变的 A 阶段基线包**里的字节，**内容相同 = true**。

| 文件 | 退出 | 具名失败 | 判定 |
| --- | --- | --- | --- |
| `test_gpu_skin_static_snapshot_share_offline.py` | 1 | `FAIL: test_gpu_queue_and_resource_share_snapshot`（failures=3） | **源码契约断言陈旧**：它把 C++ 源码文本钉在旧形状上（失败输出里直接回显了当前源码） |
| `test_d3d9_memory_chunk_tail_offline.py` | 1 | failures=1, errors=1 | 同类（静态源码契约） |
| `test_ydhost_adapter.py` | 1 | `ERROR: test_mapdump_generator_is_temporary_by_default_and_sha_bound_when_applied`、`ERROR: test_default_is_dry_run_apply_is_idempotent_and_drift_is_rejected`、`ERROR: test_snapshot_copy_detects_source_change_after_audit` | **环境性**：`expected=…\ADMINI~1\…` vs `actual=…\Administrator\…`（8.3 短路径 vs 长路径的临时目录不匹配） |

⇒ **它们是"门禁之外的既有红"**：既不是我本轮改的，也不是本计划造成的（内容与基线逐字相同）。

### ⏭️ 未跑（4 个，有明确理由）

| 文件 | 理由 |
| --- | --- |
| `test_isolated_desktop_noninteractive.py` | 需要**隔离桌面 / 实机**；本会话不启动游戏或桌面会话 |
| `test_native_async_screenshot.py` | 同上（原生截图需实机） |
| `test_war3_autotest_mcp_ydwe.py` | YDWE/MCP 启动器，会**启动外部进程**（AGENTS.md：需用户明确请求） |
| `test_palette_object_wire_roundtrip.py` | **需要 EXE 参数**；我每轮都用 `war3_palette_object_wire_roundtrip_test.exe` 跑它（CHECKS=1160 FAILURES=0）✅ |

## 3. ⚠️ 一条重要的"不能声称"

`test_recorder_event_wire_golden.py` 虽然 **PASS（checks=24）**，但它全文 **paletteObject 出现 0 次**
（实测 grep）⇒ **它 PASS 不能说明我们的 paletteObject v4 改动被它验证过**。
它覆盖的是**另一条** wire（frame evidence），不是 palette 那条。

## 4. 我的门禁陈述应当如何被理解（更正口径）

我每轮写的「`STATIC=263/0`」**准确含义**是：

```
263 个 test_*_static.py 全部通过；
另有 40 个 test_*.py **不在**该门禁内，其中 **3 个当前为红（既有、非本计划造成）**。
```

⇒ 因此「263/0」**不等于**"AutoTest 全绿"，我此前也没有这样声称，但现在有了**确切数字**。

## 5. 我**没有**做的处置（以及为什么）

我**没有**去修那 3 个红。理由：

1. 它们**不在**本计划的范围内（palette 证据 / 会话协议 / 实机），修它们属于**扩大改动面**；
2. 判断"是**源码**过时还是**测试期望**过时"需要 GPU-skin / memory-tail 领域的判断，
   而裁定明确要求**不得为通过而放宽判据** —— 若我为了让它们变绿而改断言，那正是被禁止的行为；
3. 更危险的是相反方向：若我改**源码**去迎合旧断言，可能**掩盖真实回归**。

⇒ 正确处置是**具名报告 + 留待裁定**，而不是自己决定。

## 6. 不声称

- **不**声称门禁外的 40 个文件**只有**这 3 个红（我跑了 36 个；**4 个未跑**，理由见上表）；
- **不**声称那 3 个红是"无害的"（我只证明它们**与基线逐字相同**、因而**非本计划造成**；
  它们是否代表真实回归**未判定**）；
- **不**声称已修复它们（**未修**，见 §5）；
- **不**声称 `test_recorder_event_wire_golden.py` 验证了 paletteObject wire（它根本不涉及，见 §3）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。