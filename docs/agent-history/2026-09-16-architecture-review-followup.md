# 2026-09-16 — 架构复审后续整理 checkpoint

> 性质：**维护性 checkpoint**（maintenance），不是功能候选，不是稳定版更新。
> 触发：2026-09-15 外部架构复审（针对 v1.22-integration-20260914 研究包）给出的阶段一/二/四建议。
> 边界：未部署、未启动游戏、未做玩家前台或性能验证；不改变任何已发布稳定基线。

## 背景

复审指出四条最优先事项：恢复可持续基线、把"本次绘制使用什么数据"收归验证入口、
登记孤立测试、整理 Shader 重复与 CPU/GPU 手写接口常量。本轮在当前主树
（codex/native-shadow-stable-baseline-20260830 + 未提交的 09-13/14 工作）上执行了其中可安全落地、
行为可证明不变的部分。两棵并行树（本树与 dxvk-v1.22-integration-20260914）的合并/主树决策不在本轮范围，待用户拍板。

## 1. AutoTest 基线恢复（阶段一）

- 工作树此前继承 258 个 tracked `AutoTest/**` 删除（2026-08-30 起），静态测试只剩 8 个可跑。
  本轮 `git restore -- AutoTest` 全部恢复（恢复前逐一核对与 62 个 untracked 新文件零路径冲突；
  另清除了一个 18 小时前的零字节残留 `.git/index.lock`，无活跃 git 进程）。
- 恢复后基线首跑 82/86，4 个既有失败全部修复：
  - 3 个（bridge_ramp_low_disk_probe / gpu_skin_p4_safe_index_proof / issue5_shadow_observe_analysis）
    死于同一传递导入：`AutoTest/war3_autotest_mcp.py` 顶层 `from mcp.server.fastmcp import FastMCP`
    在本机 mcp 2.x 下抛 ModuleNotFoundError。现改为带 stub 的兼容导入：模块保持可导入供静态/离线
    分析复用，真正 `mcp.run()` 时才报"mcp<2 required"的清晰错误。
  - 1 个（native_doodad_static_stamp_gate）是断言过期：测试仍期望
    `kNativeDoodadStaticStampRuntimeGateDefault = false`，而 2026-08-26 候选（当前 HEAD 线）已按
    AGENTS.md 与配置注释的意图默认置 true（阻断 type=0 enable 写入、保留 cleanup、env A/B 退出）。
    已更新断言并在测试内注明依据。
- 基线结果：**86/86**（日志 AutoTest/_baseline_static_sweep_20260916.log 保留了修复前的失败证据）。

## 2. 孤立测试登记（阶段一）

复审批评"测试存在但未接入日常构建"。本轮把 4 个未登记测试登记进 `src/d3d9/meson.build`
（+103 行纯追加，沿用既有惯例）：

| 测试 | 内容 |
| --- | --- |
| war3_cpu_skin_mt_controller_contract | CPU-MT 蒙皮控制器合同（producer/commit 状态机、证明门） |
| war3_persistent_gpu_package_recording_authority | 私有凭据铸造、一次性终态 emit、分配失败回滚 |
| war3_frame_timeline | 头文件级帧时间线账本（无需生产 .cpp） |
| war3_native_model_light_bridge_compile | dev 门控桥接头的编译合同（目标级 -DWARVK_NATIVE_MODEL_LIGHTS_DEV=1） |

- `war3/native/war3_native_hooks_test.cpp` 不登记：181 行、无 main()，导出 extern "C" 函数用于游戏
  进程内 VirtualProtect+JMP 补丁实验，功能已被生产路径 war3_native_hooks.cpp 取代，属废弃代码。
- 冲突消解：两个旧守卫静态测试断言"meson 不含对应 cpp"（把复审批评的状态固化为合同）。
  已按新意图改写为：**测试目标必须已登记，且对应 cpp 不得出现在 d3d9_src（DLL 源列表）区域**——
  生产隔离合同不变，登记不再是违规。

## 3. palette slot 身份证明复核（阶段二最小切口）

复审指认：`d3d9_device.cpp` 的 `resolvePaletteSlotIndex` 查询了 slot+groupCount+frameTag 三个证明
却只用 slot；4096 项 thread_local 缓存只按 (renderablePart, mapEpoch) 键控，slot 被其他对象重用后
可能继续供出旧 slot。

本轮修复（d3d9_device.cpp +106/−8、d3d9_war3_scene.h +4）：

- 缓存条目携带完整三元组；凡生产者查询成功即存下证明。
- 供出缓存前复核：**groupCount 失配**（两侧都 >1 且不同——SimpleFallback producer 恒记 1，
  规避 producer 类型交替的合法抖动）或 **frameTag 倒退**（双方非 0；会话内单调不减是代码库既有假设，
  见 war3_current_draw_contract.cpp 对倒退的一律 reject）→ 作废旧证明、采信新鲜三元组；
  **曾有证明但生产者不再确认绑定**（哈希逐出或指针重用而无记录）→ 条目失效并返回 0xFFFFFFFF，
  调用方原有路径已将其作为无效/fail-visible 处理。
- 从未取得证明的条目（slot 来自 +0x08 直读）保持原 FROZEN cadence 兜底行为，一字未改；
  直读有效的快速路径行为不变。
- 新统计 `semanticScenePaletteSlotProvenanceMismatchCount`（file-scope atomic 折进 shadowStats，
  沿用 path-blocker 计数器模式）。
- 新测试 AutoTest/test_palette_slot_provenance_static.py（5 用例：三元组存储、供出前复核、
  失配 fail-visible、统计链路、生产者侧锚定）。

遗留（不在本轮）：该计数器未接 JSON 导出管线；producer 表自身"指针重用但新对象尚未捕获"的
staleness 窗口属 war3_model_hook.cpp 层面，需要另一轮处理。

## 4. Shader 公共化与 CPU/GPU 常量单一来源（阶段四前半）

- 复审时核对的 5 个相同函数实测为 **17 个**（去除注释/空白后函数体完全相同）：抽入新共享文件
  `subprojects/war3fx/shaders/war3_shadow_common.glsl`（含 kPoisson16/25 常量表，逐字节取自 HEAD 版
  receiver.frag），receiver.frag 与 visibility.frag 改为 `#include`（GL_GOOGLE_include_directive，
  与 src/dxvk 既有用法一致）。有差异的 3 个函数与两份 ShadowData UBO 未动。
- caster GPU-skin 接口常量新建单一来源 `subprojects/war3fx/shaders/war3_shadow_caster_interface.h`
  （纯 #define，C++/GLSL 预处理器通用）：8 个 flag、3 个 shift、METADATA_MASK、FORMAT2_LAYOUT1_UV1。
  d3d9_war3_shadow.cpp 的 12 个 constexpr 改为引用宏（数值不变，static_assert 从 3 个扩到 9 个）；
  caster_vert.vert 的 8 处硬编码字面值全部换宏。
- 新测试 AutoTest/test_shadow_caster_interface_constants_static.py（6 用例：解析头数值、CPU 别名、
  shader 宏引用、硬编码清除、Pack 公式）；既有的 test_shadow_gpu_skin_direct_caster_static.py
  3 处断言随之更新为宏形式。

**行为保持证据**：用 meson 同款 glslangValidator（--target-env vulkan1.3）独立编译，改动前后
spirv-dis 反汇编逐行相同（receiver 8195 行、visibility 2746 行、caster vert 全量），唯一差异是各多
2 行 OpSourceExtension 元数据（include 扩展声明，无语义影响）；预处理展开文本去 #line/注释后逐行
相同；spirv-val 前后均 exit 0；depfile 正确追踪两个新共享文件。

## 验证汇总

| 项 | 结果 |
| --- | --- |
| build32_safe.cmd src/d3d9/d3d9.dll -j8 | exit 0 |
| 最终 DLL | 34,610,527 bytes，SHA-256 `FD75DA2C8C4441AB2F99D5D9EE84AE0290DAB9FE3C108E52788902AE1062C8A2` |
| ninja -C build32 -n | no work to do |
| meson test -C build32 | **28/28 OK**（24 原有 + 4 新登记） |
| AutoTest test_*_static.py 全量 | **88/88**（日志 AutoTest/_final_static_sweep_20260916.log） |

注：shader 定制 target 的实际再生成发生在前一轮构建窗口内（生成头 mtime 晚于全部 shader 源编辑），
本轮复构建仅重编译 d3d9_war3_shadow.cpp 并重链接；最终 no-work 确认依赖图一致。

## 判定

maintenance checkpoint：以上整理全部以"行为可证明不变 / fail-visible 收紧"为边界；
未部署、未做玩家前台验证，不得描述为性能或画面收益。palette 复核属于 fail-visible 收紧，
理论影响面为"producer 绑定证明丢失时最多 1 帧回退"，实机是否有可见回退未验证。
