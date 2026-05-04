# Automation Exchange

Date: 2026-04-18

## Purpose

这个目录是后续自动化提示、长时间接力开发、以及多轮状态交换的固定入口。

使用规则固定为：

1. 先读 `AUTOMATION_MISSION_2026_04_18.md`
2. 再读 `CURRENT_STATUS_2026_04_18.md`
3. 每完成一轮有效推进后，必须回写 `CURRENT_STATUS_2026_04_18.md`
4. 若本轮方法已被后台验证证明无效，也必须把“为什么无效”写回状态页，避免重复试错

## Files

1. `AUTOMATION_MISSION_2026_04_18.md`
   说明项目最终目标、完成标准、禁止回退点、自动化执行约束。
2. `CURRENT_STATUS_2026_04_18.md`
   说明当前真实状态、已经完成到哪一步、当前主阻塞、下一步优先级。
3. `STATUS_UPDATE_TEMPLATE_2026_04_18.md`
   规定每轮更新状态时的固定格式。

## Current Ground Truth

当前最重要的几份前置文档仍然是：

1. `docs/plan/semantic_shadow_control_plane_status_2026_04_17.md`
2. `docs/plan/upper_layer_shadow_cutover_status_2026_04_16.md`
3. `docs/plan/war3_runtime_rtti_ground_truth_2026_04_16.md`
4. `docs/plan/war3_runtime_model_geoset_alias_ground_truth_2026_04_17.md`
5. `docs/plan/war3_unit_shadow_mesh_stream_probe_2026_04_17.md`

这个目录不替代以上研究资料，而是作为“自动化接力入口”和“当前工作面汇总”。
