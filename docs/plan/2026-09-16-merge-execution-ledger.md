# 2026-09-16 — 合并执行台账：A 树独有改动逐项移植（B 为唯一集成主线）

> 决策来源：用户 2026-09-16 晚拍板。**B（本树）为唯一集成主线；A 树
> （E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk）保留为只读参考，
> 逐项移植确有价值的独有改动。** 差异清单全文见 A 树
> docs/plan/2026-09-16-tree-merge-diff-inventory.md。

## 用户三点纠正（每项移植的前置约束）

1. **palette/蒙皮来源**：以 B 的 skin palette publication 合同为基础
   （它约束实际矩阵副本、对象身份、当前帧、坐标空间、来源）；
   A 的 09-16 provenance 只检查槽位缓存身份，**不是等价替代品，禁止用 A 替换 B**。
   正确做法：对照 A 的检查找 B 合同是否还有缝隙，有则作为补充移植。
2. **帧时间线 vs 坏帧取证**：不是二选一。timeline 回答"时间花在哪里"，
   取证回答"这一帧到底用了什么数据"。可共享基础设施，**两套都保留，禁止删任何一套**。
3. **禁止"严格超集"式推断**：diffstat/行数只用于定位。每个移植项执行前必须逐项核对
   hook 接线、生命周期、配置默认值、shader 接口的行为等价性；
   清单旧描述以 F855A67C 诊断构建为最新事实校准
   （skin palette 合同在该构建编译期默认开，隔离 R5 已捕获 16 个 palette-selection 事件；
   普通/Release 构建仍为 0）。

## 用户裁定（2026-09-16 深夜，权威）

1. **doodad 贴花默认值**：**本轮保持 B 既有默认（透传）**，不顺带改变玩家画面。
   → P2 批次 1 的默认值翻转部分**不执行**；如需 A 的阻断行为，另立候选、单独实机门。
2. **A 的 Observe-only 原生灯（P6）**：**保留历史、暂不移植、也不删除**。
3. **下一次实机**：单独组织**组合候选**验证，重点包含：高压低视角、往返移动、
   **压力解除后的阴影恢复**。

## 用户对"完成度"与"验收"的纠正（必须遵守）

- **不得用百分比概括整个项目进度**。测试接线、重复代码整理、诊断移植完成得较多，
  但长期重构的核心仍是：**统一数据选择入口、限制旧路径绕行、资源生命周期收口**。
  进度按具体交付项判断。
- **"证明失败就不画"不能自动算修复**（当前正在调查阴影消失）。五源提交闸/EntryGate 类
  改动必须同时证明三件事，缺一不可：
  1. 不该投影的 path-blocker 被拦住；
  2. **正常单位、建筑、桥梁、装饰物仍能投影**；
  3. **失败后的恢复正常**，不是持续遗漏。
  静态检查只能验证接线，不能代替这三项画面行为。
- **两处 palette 缝隙（B-S1/B-S2）优先处理，但不能只改成返回失败**。验收必须检查：
  **错误矩阵不再被使用**，**同时合法对象仍有正确的替代路径**；
  不得只凭"撕裂减少 / 拒绝计数增加"宣布通过。
- **Stage13 常驻几何（批次 6）涉及资源保留**：必须检查**占用与回收**，
  不得未经验证就认定它能改善当前**累积超预算**现象。
- **不要让"搬完所有旧功能"挤掉眼前的发布问题**。

## 移植流程（每项都走一遍）

1. 行为核对：读 A 侧实现与 B 侧对应面，回答"A 多了什么、B 是否已覆盖、默认值/生命周期/
   接线是否冲突"；结论写进本台账对应条目。
2. 移植：以 hunk 为单位落到 B，不整文件覆盖；保持 B 的现有合同不被削弱。
3. 门禁（B 树）：build32 构建 exit 0 → ninja -C build32 -n no-work → meson test 全过 →
   AutoTest 静态全量过 → 更新本树 DEVELOPMENT_CHANGELOG。
   注意：B 当前 build32 是 F855 诊断配置（warvk_skin_palette_contract_candidate=true 等），
   移植期门禁用该配置跑并如实记录；发布候选再按 B 的既有规则关诊断选项重建。
4. 不部署、不启动游戏，除非用户明确请求。

## B 树门禁基线（2026-09-16 晚，合并起点）

- meson test -C build32：**68/68**（注意：build32 是 F855 诊断配置，
  warvk_skin_palette_contract_candidate=true 等；发布候选须按既有规则关诊断选项重建）。
- AutoTest 静态全量：起点 227/231 → 本台账第一批修复后 **231/231**（见 P10）。
- A 树对照基线：meson 28/28、静态 88/88（A 的 AutoTest 文件数远少于 B——
  B 的 153 个静态测试含 100 个共有模块防退化网，这正是"B 为主树"的红利之一）。

## 移植项台账

| # | 项 | 来源 | 核对要点 | 状态 |
| --- | --- | --- | --- | --- |
| P1 | palette provenance 补充查漏（含 JSON 导出接线） | A 09-16 | 对照 B 合同找缝隙；A 的 mismatch 计数器/缓存复核是否补充 B 未覆盖场景 | **核对完成 09-16 晚**：缝隙分析见 docs/research/2026-09-16-palette-provenance-gap-analysis.md。结论：合同 ON 主路径已覆盖 A 的三模式且更严；真正要补的是 B 自己的两个未设防缓存（B-S1 合同 OFF 的 useCachedEntry、B-S2 shadow-core 的 FindOrUpdatePaletteSlotCache）+ 可选 B-S3。A 的 TLS 三元组/快路径/FROZEN 刷新**不移植**；A 的 mismatch 计数器与 JSON 接线也**不移植**（B 已有 skin-selection/v1 证据事件与 canonical reason，避免两套平行诊断）。执行待排期（属阶段 2 数据链范畴，一次只动一条链） |
| P2 | stage13 content-persistent + native static shadow + path-blocker（88089cd 批） | A 已提交 | hook_shadow 74 行、native_runtime 249 行 B 缺失原因；生命周期与 Present 安全点 | **核对完成 09-16 晚**：行为级核对见 docs/plan/2026-09-16-stage13-port-audit.md。7 单元（U1-U7）、批次 0-7 切分已定。两个硬依赖：U5 domain 隔离先于 U4；U1 默认值翻转须先等用户裁定 doodad gate 方向。风险点：device.cpp 分叉 2500+ 行禁整文件覆盖、MAX_AGE 拆独立 env、B 的 doodad 测试锁定默认关需先改。**批次 0 完成 09-16 晚**：观测层入库（14 Query API + StaticStamp 计数路径 + DecideRegisterImage 完整策略 + summary/control_plane/perf 导出）。hook 安装 constexpr 仍为 false；doodad gate 仍 false；dormant 默认阻断常量全 false。门禁：build32 exit 0、ninja no-work、meson 72/72、静态 235/235。DLL 35,936,689 bytes / SHA-256 496944C36DE0BCF8BBFC55B1848F89F4C89E482B2EC457ABF4602784A64EAA6E。未部署。**批次 2 完成 09-16 晚**：path-blocker 五源提交闸入库（公共头 war3_path_blocker_evidence.h + CanonicalShouldSubmitPacket / SemanticCoreShouldSubmitResolvedPacket 包装）。semantic buildFrameChunk 两处 + ensureFrameBuiltForContract、canonical buildCanonicalFrame emplace 前 fail-closed 跳过。env 默认未改。门禁：build32 exit 0、ninja no-work、meson 72/72、静态 236/236。DLL 35,946,291 bytes / SHA-256 D180E4C32E021797F73B5B1044D93019A10A8C25C13B2EB89DC2D0A37C6414F6。未部署。**批次 3 完成 09-16 晚**：legacy EntryGate 收窄（Terrain 非 S1 需对象证据；WorldObject 需对象证据或 Common/Special/TransparentType0 dispatch；capture objectCasterByStage 需 dispatch-backed）。pose 路径未改。静态通过 ≠ 画面已验证；待实机 (a) blocker 拦住 (b) 单位/建筑/桥/装饰物仍投影 (c) 失败可恢复。门禁：build32 exit 0、ninja no-work、meson 72/72、静态 237/237。DLL 35,946,291 bytes / SHA-256 2242185F1AA6303E2BFD5BCE4B44C7618918E66DA0F59209A66DFA265C4594DA。未部署。批次 1 仍等用户裁定；未做批次 4/5/6 |
| P3 | native frame sync 读回候选（默认关） | A 09-13/14 | 与 B 09-14 已移植的同步部分逐行对齐版本差；闪退债状态 | **核对完成 09-16 晚**：核心合同头 war3_native_frame_sync_owner.h/policy.h 两树内容完全一致（B 09-14 13:52 移植版）；B 另有 native_capture.cpp/h 集成。A 独有的剩余增量 = ① hook_lifecycle.cpp 里 187 行 engine tick/wait/sleep 探针（帧账本挂载面，随 P5 一起裁定）② swapchain.cpp 双边增量（B +127/−52 含 B 自身演进，执行阶段逐 hunk 核对）③ A 侧分析/验证脚本组（analyze_native_frame_sync、backbuffer census 等，归 P7 同类诊断移植） |
| P4 | 异步原生截图差异 | A vs B 两版 | B 已移植一轮；A 版 09-14 后是否有更新增量 | **核对完成 09-16 晚，裁定放弃 A 版**：该文件在两树均未提交（untracked）；B 版（09-15 mtime）= A 版（09-14 mtime）+59/−6 的严格演进——Copy 结构新增 evidenceSession/serial/presentOrdinal、prepare 增 advanceBurst 参数、新增 takeHistory 取证接口；B 删除的 6 行均为被扩展逻辑替代的旧写法，A 无 B 缺少的内容 |
| P5 | 数据采集树 + frame timeline | A 09-13 | 与 B 取证栈并存接线；不改 B recorder 语义 | **P5 全部完成（08-16 晚）**：切片1 独立库入库；切片2 perf_monitor JSON/recording+HTML；切片3 Present/device/surface 观察点；切片4 lifecycle 17 Scope（deep-phase 门不移植 Enabled()）；切片5 114/114 SCOPE+manifest；切片6 AutoTest 守卫+分析器。门禁 meson 72/72、静态 234/234。NativeFrameSync 第一刀不接。核对见 docs/plan/2026-09-16-p5-collection-timeline-port-audit.md。 |
| P6 | native model light（render/ 侧 Observe-only） | A | DEV 门控是否与 B 的 model/ 侧搁置实现冲突 | **建议归档不移植（待用户否决）**：B 已有更完整的同题搁置实现（model/war3_native_light_*，含 JAPI/消费），用户 09-14 已搁置整条路线；再移植 A 的 Observe-only 版会在 B 内制造第二套同题实现。A 侧代码留作只读参考，研究文档已随 P11 入 B |
| P7 | issue8 图像审计工具链（origin/relocation audit + 分析脚本） | A 09-14 | 纯诊断；与 B 取证栈的关系 | 已界定范围 09-16 晚：dxvk_image_origin_audit.h/cpp + dxvk_image_relocation_audit.h/cpp + 3 个静态测试 + 分析脚本 + A 侧 dxvk_image.cpp 集成点（B 的 dxvk_image.cpp 有自身分叉，集成点需逐 hunk 核对）。待排期移植 |
| P8 | shader 公共化（war3_shadow_common.glsl + caster interface 单一来源） | A 09-16 | 重放到 B 的 shader 版本上并重跑 spirv-dis 等价验证；B 的 frame_input_gather.comp 不受影响 | **完成 09-16 晚**：逐函数核对后只抽 B 与 A 规范化相同的 9 函数+kPoisson25（以 B 函数体为准，分段 include 保序）。8 个漂移函数（shadowMapDepth/shadowCompare/casterMaskValue/kPoisson16/sampleShadowGrid/Poisson16/Poisson25/sampleShadowPcf/computeViewNormal）留在各自 shader，不强行合并。caster interface.h 数值与 A 相同已宏化。spirv-dis 函数体逐行相同，仅各多 2 行 GL_GOOGLE_* OpSourceExtension；spirv-val 通过。门禁：build32 exit 0、ninja no-work、meson 70/70、静态 232/232。DLL 35,884,733 bytes / SHA-256 D12C940882B39C6137E3E8742CB5980C902832A82970E819E4E75CBB65AEA9EF。证据 %TEMP%\warvk_p8_evidence\ |
| P9 | 4 个孤立测试登记（meson） | A 09-16 | B 的 meson.build 已有 526 行差异，核对是否已含等价目标 | **部分完成 09-16 晚**：cpu_skin_mt_controller_contract 与 recording_authority 两个目标已登记进 B meson（源文件 git 级与 A 相同，仅行尾差异），构建+测试通过；守卫测试同步改写为"必须登记且不进 d3d9_src"。frame_timeline / native_model_light_bridge 两个目标依赖 P5/P6 的模块先移植 |
| P10 | AutoTest 09-16 恢复项（mcp stub、doodad gate、守卫测试改写） | A 09-16 | B 的对应测试版本是否已有等价修复 | **部分完成 09-16 晚**：mcp 2.x FastMCP stub 已逐字移植（B 的 FastMCP 用法与 A 同形态：实例化+tool 装饰器+run），3 个失败测试（bridge_ramp_low_disk_probe / gpu_skin_p4_safe_index_proof / issue5_shadow_observe_analysis）转绿；另修复 B 侧 F855 引入的过期断言 test_trusted_current_palette_rebuild_bypass（锚点 const auto→auto 因 22566 行重赋值，并补 ContractEnabled/CanReplace 合同门断言）。doodad gate 与守卫测试在 B 侧本就通过，待逐项核对内容等价性 |
| P11 | docs 24 篇（A 09-13/14/16 研究文档） | A | 直接拷贝 | **完成 09-16 晚**：24/24 复制并逐字节 SHA 校验一致，无覆盖冲突 |

## 已发现的默认值分叉（用户纠正第 3 条的实例，合并时必须显式裁定）

- **kNativeDoodadStaticStampRuntimeGateDefault（玩家可见，需用户裁定）**：
  B/v1.21.00 = false（完整透传魔兽原生贴花阴影，仅 DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW=1
  时阻断）；A = true（08-26 候选决定：生产默认阻断 type=0 enable 写入、保留 cleanup 透传、
  env=0 可 A/B 回退，让玩家看到 WarVK 阴影而非原生贴花）。
  证据：A/B war3_internal_test_config.h:1365/1361 + 两树 test_native_doodad_static_stamp_gate
  各自钉死本树默认值。这是行为默认值分歧而非代码冲突：合并时选哪边=选择产品默认画面。
  **等待用户裁定；未裁定前 B 保持 false 不变。**

- **kNativeDoodadStaticStampRuntimeGateDefault**：A=true（08-26 候选决定：阻断 type=0 enable 写入、
  保留 cleanup、env A/B 退出），B=false。两树测试各自钉死了本树默认值且都通过。
  这属于 stage13/native-shadow 批（P2）的一部分，移植时须显式裁定默认值并同步两树测试语义。

## 明确不做

- 不整文件覆盖 B 的任何分叉文件；不把 A 的未提交工作树当可提交批次。
- 不碰玩家现场、不部署、不启动游戏。
- 不引入 B 已判退/搁置的路线（原生灯消费、Water、coherent Consume）。

## 阶段 2 待办：P1 缝隙结论的落地补丁点（本轮只登记，不实施）

已核实（主线程读码确认，与 P1 报告一致）：

- **B-S1**（合同 OFF 路径，= 普通/Release 构建的生产路径）：
  src/d3d9/d3d9_device.cpp:8633-8644 `useCachedEntry`——第 8635-8638 行在 `+0x08` 合法时直接
  供出并覆盖 entry；第 8639-8642 行 producer 命中则采用；**第 8644 行 producer miss 时仍
  `return entry.paletteSlotIndex`（fail-open）**。B 已在 8610-8613 取得 groupCount/frameTag
  但未存入 entry。补丁候选：(a) 最小——producer miss 返回 0xFFFFFFFF（fail-visible）；
  (b) 完整——entry 携带三元组并供出前复核（A 的做法，较重）。P1 建议 (a)。
  注意：这会改变生产默认行为（证明丢失时最多 1 帧回退），须与阶段 1 的 palette 实机门
  同口径验收，不能只跑静态。
- **B-S2**：src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp:703-762
  `FindOrUpdatePaletteSlotCache`——742-745 行在 `currentSlotIndex` 非法时直接返回上一帧缓存
  slot（连 producer 都不问），调用点 6482-6496 `tryEngineDirectPosePalette` 用它乘 48 回读
  Game.dll+0xBC6BD0 arena。补丁候选：`ContractEnabled()` 时禁用该回退（返回 0xFFFFFFFF）。
- **B-S3（可选）**：src/d3d9/war3/model/war3_model_hook.cpp:9355-9356 CapturedWriter 新鲜度
  只比 frame 相等，可加 `QueryRenderablePartPaletteSlot` 绑定再确认。

执行纪律：一次只动一条数据链；先 B-S2（合同管不到、最裸）或先 B-S1（生产路径）由阶段 2
排期决定；每项都要有"同一地址被另一对象重用 / 矩阵数量不足 / 旧帧结果晚到"三类情形
能抓住接错线的测试（复审阶段二的完成标志）。

## 子代理路由（2026-09-16 深夜更新）

- 主线程模型已切至 **DeepSeek V4.1 Flash**；子代理可用模型（cli-proxy-api）：
  **kimi-k3**（复杂任务首选）、grok-4.6（备用）、gemini-3.8-flash-high（简单任务）。
- 委派时必须显式指定 provider + model，禁止省略。
- 既有教训：子代理曾出现"写入失败却声称成功"，故委派要求**完整报告直接放在回复正文**，
  并由主线程独立复核关键产物（哈希/门禁/字节一致性）。

## 优先级重排（按用户 2026-09-16 深夜指示）

不再按"批次 0→7 顺序搬完"执行；改为围绕眼前的发布问题排期：

| 优先级 | 交付项 | 说明与验收要求 |
| --- | --- | --- |
| **P0** | **palette 两处缝隙（B-S1 / B-S2）** | 见下表补丁点。**必须给出合法对象的替代路径**，只改成返回失败不算完成；验收需证明错误矩阵不再被使用 **且** 合法对象仍有正确路径，不得只凭撕裂减少/拒绝计数增加 |
| **P1** | 组合候选实机验证 | 单独组织；重点：高压低视角、往返移动、**压力解除后的阴影恢复**。需用户明确授权部署 |
| **P2** | 五源闸 + EntryGate 的画面三门验证 | 拦住不该投影的 blocker；正常单位/建筑/桥梁/装饰物仍可投影；失败后能恢复——静态不能代替 |
| **P3** | 批次 4（registry domain 隔离）、批次 5（alias 迁移） | 属资源生命周期收口，与"统一数据选择入口/限制旧路径绕行"同向 |
| **P4** | 批次 6（Stage13 常驻几何） | **前置**：必须先验证 CPU proof 占用与回收、GPU 池占用，不得未验证就认定能改善累积超预算 |
| — | 批次 1（doodad 默认翻转 + hook 安装） | **本轮不执行**（用户裁定保持 B 默认） |
| — | P6（A 侧 Observe-only 原生灯） | 保留历史、暂不移植、也不删除 |
| — | 批次 7（Type5 地址表注释） | 可选，最低优先 |

**注意**：批次 2 已落地五源闸、批次 3 正在做 EntryGate 收窄——这两项属"限制旧路径绕行"方向，
已实施；但它们的**完成**必须等到 P2 的画面三门验证通过，当前只能记为"接线完成、画面未验证"。

## P0 实机验证方案（2026-09-16 深夜，主线程产出）

- 方案文档：B:docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md
- 组合候选三段场景：高压低视角 / 往返移动 / 压力解除后恢复。
- 判定用既有来源分类 + `SourceChurnCount` 作主仪表；Gap B/A 新计数器需先导出。
- 前置未完成前不得开跑；且**必须用户明确授权部署**。

## 统一数据选择入口：现状清册与统一化设计（2026-09-16 深夜，主线程产出）

- 文档：B:docs/plan/2026-09-16-unified-data-selection-entry-design.md
- 事实基础（主线程读码）：B 现存**五个**调色板/蒙皮数据选择入口——
  1. `War3TryBuildLiveRuntimeGroupPalette`（d3d9_device.cpp:8501，合同 ON 最强／OFF 退化）
  2. `TryBuildRuntimeGroupPalette`（war3_shadow_renderer_core.cpp:6509）
  3. `TryBuildRuntimeGroupPalette`（**同名不同签名**，war3_upper_layer_shadow.cpp:98）
  4. producer query 家族（war3_model_hook.cpp，含严格度不同的三个 slot 读取器）
  5. 合同层 `skin::Selection` + `Usable`/`CanReplace`（war3_skin_palette_selection.h）
- 关键判断：**统一化的原语已经存在（`skin::Selection`），问题是只有入口 1 的合同 ON 路径在用**；
  因此目标是"让所有入口产出并用同一个 Selection 裁决"，不是发明新抽象。
- 分阶段：S1 = 当前 P0（Gap A/B，消除未复核记忆槽位读取）；S2 = 合并入口 2/3 同名双实现；
  S3 = 三个 slot 读取器收敛（生产只允许 Exact）；S4 = 合同 OFF 的 legacy arena 读取降级为默认关的诊断开关。
- 每一步都必须同时证明"正常单位/建筑/桥梁/装饰物未失去投影路径"，不得用入口数量减少当完成度。

## Stage13 前置验证清单（2026-09-16 深夜，主线程产出）

- 文档：B:docs/plan/2026-09-16-stage13-retention-verification-prereq.md
- 度量面已核实：`m_war3ShadowPersistentBytesUsed`（d3d9_device.h:2980）、
  池上限 `War3GetShadowPersistentPoolCapBytes()`（默认 512MB）、
  GC 点 21550/21576/21658、准入 21820-21823、
  超预算指标 `budgetExceeded`（scene.h:1857）/ `framesBudgetExceeded`（perf_monitor.h:651）、
  Arena 检疫/提交字节（diagnostics_hub.h:138/139/143/835/841）。
- 判定条款：峰值 ≤ cap 且准入拒绝不激增；长期**不得单调增长**且须观察到实际退役；
  超预算指标不上升；必须有修复前基线；否则**不得宣称改善**。
- 仍禁止照抄 A 的 persistent idle 240→3600（B 的 S1 地形 cache 共用同一 maxAge）。

## 语义职责迁出 d3d9_device.cpp：清册与迁移方案（2026-09-16 深夜，主线程产出）

- 文档：B:docs/plan/2026-09-16-device-semantic-responsibility-migration.md
- 实测：device.cpp **52,387 行**；语义/contract 1454、palette 1329、Stage13/S1 790、
  CurrentDraw 409、Arena/budget 344、pathBlocker 328、draw-time cache 227；
  **36 个语义选择类函数定义**（代表符号见文档）。
- 迁移分四步 M1 纯判定 → M2 调色板（与统一入口设计合并）→ M3 persistent 判定 →
  M4 CurrentDraw/draw-time（最后，涉 Present 时序）。
- 新增防回退门禁：`test_device_semantic_responsibility_budget_static.py`
  钉住 device.cpp 允许的语义符号集合，**迁移后只允许减少**。

## 防回退门禁已落地（2026-09-16 深夜，主线程）

- `AutoTest/test_device_semantic_responsibility_budget_static.py`：冻结 device.cpp 的 37 个
  语义选择符号，**只允许减少**；新增/改名即失败。门禁自身已用合成符号验证有效。
- 静态脚本总数 238 → 239。
- 迁移判据与四步方案见 B:docs/plan/2026-09-16-device-semantic-responsibility-migration.md。

## P0 Gap A 完成记录（2026-09-16 深夜，kimi-k3 实施）

- 范围：工单 `docs/plan/2026-09-16-palette-gap-fix-workorder.md` 的 Gap A（B-S1），
  仅 `src/d3d9/d3d9_device.cpp`；Gap B（shadow-core）文件未动。
- 修复性质：**路由式**。`useCachedEntry` 在 +0x08 槽位非法时不再无条件供出记忆槽位；
  仅当 producer 绑定命中、槽位与记忆一致、groupCount ≥ requiredPaletteCount 三要素
  全满足才供出（并刷新 entry 的 slot/groupCount/frameTag）；否则返回 `0xFFFFFFFFu`，
  调用方跳过 slot 键读者（Game.dll 全局 arena / QueryBlendedPaletteBySlotIndex），
  落到 PoseFallback 替代路径。无负缓存，下帧可恢复。
- 计数器：`g_devicePaletteSlotCacheServedAfterConfirmCount` /
  `g_devicePaletteSlotCacheRejectedStaleCount`（本地原子，与 Gap B 同风格，
  **尚未导出到报告**，待与 Gap B 三个计数器一起接线）。
- 门禁（B 树，F855 诊断配置）：build32_safe exit 0 → ninja -n no-work →
  meson **72/72** → AutoTest 静态 **241/241**（240 + 1 新增
  `test_device_palette_slot_cache_producer_confirmation_static.py`；
  `test_live_palette_slot_cache_accelerator_static.py` 仅锚点更新）。
  DLL 35,979,538 bytes / SHA-256
  `F73F49B0295A46DF230BC83DCED85CB9F5D40114BAB23335E94A2621C293C039`。未部署、未实机。
- 待办：① Gap A/B 五个计数器导出到报告；② 三项画面证明（错误矩阵不再被使用 /
  合法对象经替代路径成功投影 / 失败后可恢复）与反例门，须随 P0 组合候选实机验证。

## P0 Gap A 主线程独立验收（2026-09-16 深夜）

- 复核方式：不采信子代理自述，主线程自行读取产物与重跑门禁。
- 已核实：DLL 35,979,538 bytes / SHA-256
  `F73F49B0295A46DF230BC83DCED85CB9F5D40114BAB23335E94A2621C293C039`（与自述一致）；
  meson `Ok: 72 / Fail: 0`；**静态全量 TOTAL=241 FAIL=0**（主线程跑完全量，非抽查）；
  两个 palette 测试单独通过。
- 代码复核：`useCachedEntry`（d3d9_device.cpp:8656-8686）仅在"producer 命中 && 槽位相符 &&
  producerGroupCount >= requiredPaletteCount"时供出记忆槽位；否则 rejected + `return 0xFFFFFFFFu`；
  **拒绝路径不写任何状态（无负缓存、无墓碑）→ 下帧绑定恢复即可重新命中**，满足"失败后可恢复"。
  唯一的 `return entry.paletteSlotIndex` 位于复核通过分支内（8681）。
- 结论：**Gap A 通过验收**；但按用户纪律，P0 仍未完成——5 个计数器未导出、三项画面证明未做。

## 运行时开关三分类清册完成（2026-09-16 深夜）

- 文档：B:docs/plan/2026-09-16-runtime-switch-triage.md（1319 行 / ~347 KiB）。
- 计数：**总数 412**（字符串字面量）；**A 玩家/生产 = 0**，B 诊断/取证 = 24，C dev-only/实验 = 44，
  待判定 = 344；另有 9 个非字面量（宏/注释）单独列出。
- **主线程独立复核**（抽查 4 行 + 验证 A=0）：
  · `DXVK_WAR3_AA` → d3d9_war3_pipeline.cpp:447 `ParseEnvInt("DXVK_WAR3_AA", aaMode)` ✓
  · `DXVK_WAR3_ENABLE_HOOKS` → d3d9_war3_hook.cpp:576/790 `GetEnvBool(..., true)` 默认开 ✓
  · `DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW` → war3_hook_shadow.cpp:1507 getenv，B 默认透传 ✓
  · `DXVK_WAR3_SKIN_PALETTE_CONTRACT` → war3_skin_palette_selection.h:30 读 env，缺省取宏 ✓
  · 玩家向文档（`WarVK/`、`README*`）**确实未点名任何 DXVK_WAR3_*** → A=0 成立。
- **A=0 的含义（需用户裁定，已列入待裁定）**：本项目的玩家控制面实际是 **Ctrl+F1 面板 + JAPI**，
  环境变量属内部面。因此三分类的可执行输出有两个互斥方向：
  (a) 把确有玩家价值的开关（AA / 体积光 / 点阴影 / TAA / doodad gate 等）**正式写入玩家文档**并定义
      与面板的优先级；或
  (b) 明确声明 **env 全属内部面**，玩家入口只认面板，并在发布门禁中校验。
  在用户裁定前，不擅自改文档口径，也不改任何默认值。
- 待判定 344 的主要缺证原因：无玩家文档 + 默认非关/未证关 + 无编译期门 + 未证明只产诊断
  （~196）；诊断名但默认值未抽出（~53）；画面类缺玩家向 env 点名（~35）；诊断名但默认开（~2）。

## 2026-09-17 凌晨 checkpoint（新会话接手，主线程执行）

- **基线复核**：build32 exit 0、ninja no-work、meson **72/72**、静态 **241/241**
  （交接文档预期 240，实际多 1 = Gap A 的
  `test_device_palette_slot_cache_producer_confirmation_static.py`——上一会话子代理
  实际已完成 Gap A 全部编辑并补了该测试，交接文档 §4.1 的"其余 4 处编辑待做"为过期信息）。
- **Gap A 独立复核结论**：d3d9_device.cpp 的 useCachedEntry 已按工单实现
  （producer 命中 + 槽位一致 + groupCount≥required 才供出，否则 0xFFFFFFFF 落 PoseFallback；
  合同 ON 分支未动；加速器合同保留），静态测试锚点与语义均成立。**代码与静态层面完成；
  三项画面证明（错误矩阵不再被用 / 合法对象有替代路径 / 失败可恢复）仍待实机。**
- **P0 计数器接线完成（本轮主线程亲手实施）**：Gap A 2 个 + Gap B 3 个计数器
  经 war3_diag/shadow:: Query 访问器 → bridge summary → diagnostics hub /
  control plane / perf monitor JSON 全链接通；新增守卫测试
  `test_palette_slot_cache_counters_export_static.py`（静态 241→**242**）。
- **门禁四步原始结论**：build32 exit 0；ninja -C build32 -n no-work；meson **72/72**；
  AutoTest 静态 **242/242**（全部 exit 0）。
  DLL 35,984,240 bytes / SHA-256 `8CECC495595D3BC6EC9D25AFBD1F0BA9F44CF9B18BBFB7E784B60B74C9569ADE`。
- **未做**：部署、启动游戏、git 写操作；实机组合候选验证仍等用户授权。
- **运行时开关三分类清册：完成（2026-09-17 凌晨，子代理 DeepSeek V4.1 Flash 执行，
  主线程独立复核通过）**。注意：上一会话已有一版 347 KiB 清册（A=0/B=24/C=44/待判定=344），
  交接文档 §4.2 却记为"文档未写入"（过期信息）；本轮按新分工指令重新生成并**覆盖**，
  新版 137,417 bytes / 615 行 / SHA-256 `E0AFD406B21E3090BC7250A0B78446C69F7CF1C98A6F02AD0B6EB832EEA352EA`，
  分类为 **A=156（全部标注"缺玩家向文档"）/ B=152 / C=19 / 待判定=85 + 宏注释 9 + 文档孤立 3**。
  分类口径差异：本会话指令允许"无玩家文档但有生产语义证据 → 列 A 并标注缺文档"，
  上一会话口径则把这类全部放进待判定；两版口径都记录在案，不互相冒充。
  主线程复核：SHA-256 一致；§3-§7 行数 156/152/19/85/9 逐项相符；412 个开关
  **零重复、零遗漏**（与主线程独立枚举的 412 名单完全对齐，文档内 426 个名字 =
  412 + 9 宏注释 + 3 文档孤立 + 2 仅 md 双引号）；抽查 `DXVK_WAR3_SHADOW_WORLD_UP`
  不可达发现（s_forcedUp==1 恒使 env 分支不进入）与 `BEFOREUI_TIER1` 默认开
  （!= "0"）均与源码一致。
- **记录纠正（主线程两种方法一致复核）**：交接文档 §4.2 的"63 个开关有玩家向文档命中"
  **无法复现，应为 0**——WarVK/、README*、CHANGELOG*、docs/RELEASE_* 不点名任何
  DXVK_WAR3_* 环境变量（README 仅写"环境变量不能绕过发布冻结"）。"63"的疑似来源：
  递归 README* 会扫进 docs/research|plan 的内部工程 README（59 个），那些不是玩家向文档。
- **需要用户裁定的分叉**：玩家配置面实际是 Ctrl+F1 面板 + JAPI，env 全属内部面。
  方向 (a) 给确有玩家价值的开关补正式玩家文档；或 (b) 明文声明 env 全内部并在发布门禁校验。
  未裁定前不改任何默认值、不改文档口径。

## 2026-09-17 凌晨：上级裁定（codex 线程 01a02e0b）与方案冻结

- 授权链：用户无法在线，明确指示把待裁定项提交 codex 线程
  `codex://threads/01a02e0b-1d1e-7762-b40b-63a00bbb3449`；本会话经 App 自带 CLI 的
  `codex queue` 投递问题（npm 版 0.121 不认 App 的 `service_tier="default"`，
  `exec resume` 撞 App 线程写锁——均失败未发出，最终 `queue` 成功，
  消息 ID `01a0ab31-f124-7f21-b333-7d99e3ca0d90`）。
- **裁定全文要点**（原文存于 codex 线程，此处是要点不是全文）：
  - **Q1 实机：条件批准**。候选冻结 35,984,240 bytes / `8CECC495…`；
    仅 `E:\Work\War3` 测试现场，结束恢复原 DLL（不得套用历史 055F 恢复源）；
    先合同 ON 后 OFF、各 fresh process、不自动重试；隔离桌面 2560×1440 零输入无并发编译；
    关闭大容量帧采集；停止条件=崩溃/device lost/恢复失败/身份不符；
    第三段恢复观察不得提前退出。**证明口径修正**：计数器看分段增量；
    三证明须同对象/part/帧/代际关联；关联不上记"未覆盖"。
  - **Q2 基线：否决** `1C314090…` 当完整修复前基线（它已含 Gap B）；允许无基线观察轮，
    但禁止宣称性能收益/修复前后因果。
  - **Q3：批准**同 DLL 用 `DXVK_WAR3_SKIN_PALETTE_CONTRACT=0/1` 进程级覆盖
    （selection.h:29 已核实；静态缓存 ⇒ 须重启进程）。合同 OFF 不是修复回退开关。
  - **Q4：选 (b)**——env 全属内部面，玩家入口只认 Ctrl+F1 面板+JAPI；
    不授权删开关/改默认/使既有启动配置失效；清册后续补充所有者/读取时机/重启需求/优先级列；
    85 项待判定不强行归类。
  - **Q5：条件批准收窄版 S2**——只共享纯计算内核+薄适配+生产函数测试，不合并来源选择链；
    **实机事务期间暂停 B 树源码/构建修改**；后续离线构建 Below Normal、-j2。
  - 顺序锁定：修正方案 → 冻结 P0 → 有界实机观察 → 恢复结算 → S2。
- **裁定发现的代码缺口（记录在案，实机结算后处理）**：Gap B 复核
  （war3_shadow_renderer_core.cpp:799-805）取回 `boundGroupCount` 但未参与判定、
  无帧新鲜度校验；快照优先调用未接收 frameTag。⇒ 本轮实机结论上限为
  "观察结果与路径覆盖"，**不能宣称"完全排除错误矩阵"**。补强会改变 DLL 哈希，
  不在冻结候选内修改。
- 方案文档已按裁定重写冻结：`docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md`
  （环境矩阵无"视需要"、三段各 10 分钟、段边界取快照看增量、判定矩阵修正版、
  停止/恢复条款、宣称限制）。
- 测试现场只读核验（2026-09-17 凌晨）：`E:\Work\War3\d3d9.dll` =
  35,486,415 bytes / `74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73`
  （前缀与裁定记录一致，以本实测为准）；玩家目录 `E:\Work\Warcraft III\d3d9.dll`
  仍为 F855 构建 35,884,733 bytes / `F855A67C…`，未触碰。
- 待执行：部署候选到测试现场 → 合同 ON 轮 → 恢复门 → 合同 OFF 轮 → 恢复结算。

## 2026-09-17 深夜并行推进（主线程 + 5 个 DeepSeek V4.1 Flash 子代理）

- **计数器接线的运行时证明（第一批实机数据）**：合同 ON 轮 boot 快照
  `gapCounters` 中 5 个新键全部真实出现：
  semanticSceneSkinnedPaletteSlotCache{Device,ShadowCore}ServedAfterConfirm/RejectedStale
  + ShadowCoreProducerSnapshotFallback（boot 时均为 0，符合预期）。
  ⇒ "接线完成"从静态升级为**运行时可见**（仍不等于画面行为证明）。
- **子代理交付与主线程独立复核**（全部只读 B 树/只写指定文件，未构建未 git）：
  | 子代理 | 产物 | 主线程复核 |
  | --- | --- | --- |
  | A 驱动 | `C:\Windows\Temp\warvk_p0\warvk_p0_phase_driver.py` 84,057 B / 1,883 行 / SHA-256 `72C41C97…` | py_compile 0；--dry-run 哈希+env 矩阵+模块装载全通过；三段实现与止损条款逐项核对 |
  | C S2 设计 | `docs/plan/2026-09-17-s2-shared-compute-kernel-design.md` 41,557 B / 587 行 / `776394D6…` | 哈希一致；52 行逐语句相同片段 + D1-D8 差异 + 内核签名 + 13 组生产函数测试 |
  | D 清册扩展 | `docs/plan/2026-09-16-runtime-switch-triage.md` 340,116 B / 1,440 行 / `DA217FF9…` | 哈希一致；第 10 章 412 行/412 唯一名；截断名发现已对源码核实 |
  | E GapB 补强 | `docs/plan/2026-09-17-gapb-confirmation-strengthening-design.md` 53,080 B / 816 行 / `B3E4B59F…` | 哈希一致；requiredCount(6553)/快照上限 64(338)/帧 API 存在性/device 侧已在用 全部核实 |
  | F 分析器 | `C:\Windows\Temp\warvk_p0\warvk_p0_analyze.py` 34,692 B / 750 行 / `A64C1F4D…` | 哈希一致；三段判定口径严格（无对象级归因一律"未覆盖"） |
- **Gap B 补强设计要点（待实机结算后另立候选）**：boundGroupCount 未参与判定
  （core.cpp:800 取回/803 传入/805 判定只用 slot）、无绑定帧新鲜度、无逐槽帧见证、
  快照未收 frameTag 且用 `>=` 数量比较、`slot+requiredCount` 未判上界。
  补丁 H1-H9 + 6 个细分计数器；实施会改变 DLL 哈希 ⇒ 不在冻结候选内改。
- **S2 收窄设计要点**：核心事实纠正——两个 TryBuildRuntimeGroupPalette 体内**都没有 hash 计算**
  （HashMatrixPalette 是 TU 级自由函数，在 core.cpp:835 与 model_hook.cpp:927 重复）。
  纯计算片段 52 行逐语句相同；来源链/第 5 步 uniform/诊断面/计数派生 6 项本质不同 ⇒ 不合并。
- **开关清册 Q4 扩展**：公开玩家接口 = 否 412/412（玩家面是 Ctrl+F1 面板 + JAPI）；
  读取时机 static 271/每次 135/混合 6；重启 是 386/否 25/待判定 1；发布可达 是 393/否 19；
  诊断风险 高 108/中 100/低 134/待判定 70。
- **实机事务状态**：候选 8CECC495 已部署到 `E:\Work\War3`（备份 `74CC676B…DB73` 在
  `C:\Windows\Temp\warvk_p0\backup\`）；合同 ON 轮于 01:41 启动，phase1 正常推进；
  **分辨率偏差已记录**（会话桌面 2048×1152，非冻结的 2560×1440）。

## 2026-09-17 02:1x：P0 pilot 实机轮（合同 ON + M1）结果与矩阵修正

- **pilot 轮完整通过**（驱动 exit 0、statusLabel done）：三段各 observedSec ≈ 600.0s
  （600.082 / 600.061 / 600.016），sampleCounts 1101/1108/1115，deviceLost=false，
  stopReason 空，newGpuIncidents=[]，newGpuEvents=0，dllUnchanged=true，
  restoreOk={camera,visibility,stopped}=true，92 份 perf HTML 抽取成功。
  产物：`C:\Windows\Temp\warvk_p0\round1_contract_on\`（boundary_00..04、
  shadow_summary_00..04、samples.jsonl 46 MB、counter_deltas.json/csv、driver_result.json）。
- **5 个 GAP 计数器键已确认在实机快照中真实出现**（gapCounters）——接线从静态升级为
  运行时可见；但在 M1 下数值恒 0，原因是结构性的（见下）。
- **只读诊断结论（子代理 G，主线程已逐条对源码复核）**：
  1. 快照标量是**当帧值**不是累计值（g_shadowSceneStats 每帧整体替换，
     bridge.cpp:4217-4568）⇒ 单帧 0 不等于整轮未发生；semanticSceneSubmitted 实测
     0..178、54% 采样非 0。
  2. **palette 来源分类 7 计数器结构性恒 0**：整个分类块在
     `if (skinned && War3SemanticPaletteDiagnosticsRuntime())`（device.cpp:23318），
     该门默认 0（2590-2598）。
  3. **Gap A 结构性不可达**：合同编译默认 1（meson warvk_skin_palette_contract_candidate=true
     ⇒ -DWARVK_SKIN_PALETTE_CONTRACT_DEFAULT=1），War3TryBuildLiveRuntimeGroupPalette
     在 8585-8603 提前 return，8691/8695 自增整段不可达 ⇒ pilot 的
     DXVK_WAR3_SKIN_PALETTE_CONTRACT=1 是 no-op。
  4. **语义 core 整轮未构建**（coreFrameSerial/coreResolved 全程 0、
     BuildRequestPending 全程 true）；core 唯一进程内消费点 war3_renderer.cpp:385
     runObserveValidation()，门默认 false（..._ENDFRAME_BUILD，gate:69-71）。
  5. 真正活跃的是 direct/draw-time 生产者那半边（replay/casters 400~870/帧，
     shadow map 90.7% 采样执行；PopulateLastReturnReason=10）。
  6. 四个"23"类计数器实测是同一序列（canonical NoWorldTransform ==
     direct recordCapAppendFail == direct lastEligible == currentDraw resolveReadyRejected），
     判定点在 war3_canonical_draw.cpp:259-260（world.valid 五来源）。
- **矩阵修正 M2**（已写入 P0 方案 §1.1）：M1 + PALETTE_DIAGNOSTICS=1 +
  SHADOW_ENDFRAME_BUILD=1 + PUBLISH_REGISTRIES_BEFORE_SCENE=1；已核实无效的
  BOOTSTRAP_CATCHUP/TAIL_FALLBACK/ENDFRAME_FLUSH/SCENE_SUBMISSION/PRE_READY/PREVIEW
  不再设。正式轮 **R2=ON+M2（安全门）→ R3=OFF+M2（Gap A 唯一可达）**，
  pilot 不计入 M2 的 ON/OFF 判定。
- 现场 DLL 仍为冻结候选 `8CECC495…`（未恢复，恢复在全部轮次结束后统一执行）；
  备份 `74CC676B…DB73` 完好。

## 2026-09-17 02:5x：R2（ON+M2）与 R3（OFF+M2）实机中间事实

- **R2（合同 ON + M2）完整通过**：三段各 600s、deviceLost=false、无新 incident、
  DLL 未改、restoreOk 全 true、91 份 perf 报告。**但 5 个 Gap 计数器与 8 个来源分类
  计数器在 3370 个样本中全程为 0**（即使 PALETTE_DIAGNOSTICS=1）。
  已核实原因：分类块所在函数 D3D9DeviceEx::War3TryAppendSemanticShadowPacket
  （device.cpp:22044 起）在 22067 先被 ShadowProducerPolicyAllows(
  SemanticDirectGrouped, ...) 拦下；core 亦全程不构建（coreResolved=0，即使
  ENDFRAME_BUILD=1）。⇒ **这些仪表挂在当前配置不走的路径上，M2 不足以产生 P0 证据。**
- **R3（合同 OFF + M2）出现决定性正面证据**（phase1 期间 505 个含键样本）：
  - `semanticSceneSkinnedPaletteSlotCacheDeviceRejectedStaleCount` **全部样本非零**，
    min=116 / max=28711 ⇒ **device 侧记忆槽位复核确实在生产路径上工作，
    并大量拒绝陈旧槽位**——即旧 fail-open 洞是被真实命中的（不是理论风险）。
  - `…DeviceServedAfterConfirmCount` 仍为 0 ⇒ 本轮"经 producer 确认的合法快路径"
    一次都没命中；**不得据此宣布通过**（用户明确禁止只用拒绝计数当证明）。
  - 来源分类在本轮部分活跃：`SourceSubmitTimeGlobalSlotCount`（危险读者①）峰值 16/帧
    （51/505 非零）、`SourceDrawTimeCapturedCount` 峰值 1（9/505 非零）；
    `SourceNoneCount` 全程 0、`OwnedPartSnapshot`/`PublishedRegistry` 全程 0。
  - Gap B（shadow-core）三计数器仍 0（core 未构建，shadow-core 蒙皮路径未运行）。
- 结论口径（写入执行记录时必须保持）：R3 提供了 ① 的部分证据（复核生效、陈旧被拒、
  SourceNone 未上升）；② ③ 仍缺同对象/part 关联与合法快路径命中证据 ⇒ 记
  **"观察结果 / 未覆盖"**，不得宣布修复通过。
- 下一步：R3 结束后恢复现场 DLL → 汇总分析（分段增量）→ 写 P0 执行记录 →
  实机事务解除后再动源码（Gap B 补强 / S2）。

## 2026-09-17 03:2x：P0 实机三轮完成、现场已恢复结算

- 三轮全部 exit 0、三段各 ≈600.0s、deviceLost=false、无 GPU incident、DLL 未被改、
  restoreOk 全 true。**现场已恢复原 DLL `74CC676B…DB73` 并核验哈希一致；玩家目录未触碰；
  无残留进程/文件。** 实机事务结束，B 树源码冻结解除。
- **P0 判定（详见 `docs/plan/2026-09-17-p0-real-machine-execution-record.md`）**：
  ① 观察结果（部分）：OFF 轮 Gap A 陈旧拒绝 Δ21=57,045 / Δ32=2,113（累计原子，单调增），
  证明复核机制在生产路径上真实工作；
  ② **未覆盖**：ServedAfterConfirm 全程 0、OwnedPartSnapshot/PublishedRegistry/DrawTimeCaptured
  采样瞬时为 0；
  ③ **未覆盖（仅有趋势）**：拒绝增量在恢复段下降，但无同对象重投影证据；
  反例门 **未覆盖**（无画面级证据）。**总判定：运行观察 + 路径覆盖，不宣布修复通过。**
- **本轮暴露的关键结构问题**（下一候选必读）：
  1. Gap B 不可度量：core 构建完成但 manifest 记录在 core.cpp:7519-7561 四键资源查找全 miss
     ⇒ resolved=0 ⇒ shadow-core 蒙皮路径不运行；
  2. 来源分类仪表在合同 ON 下结构性不可达（canonical 就绪门 device.cpp:23166-23172 100% 拒绝；
     23172 之后含 23318 分类块与 24882/25917 提交计数均为死代码），而真实 skinned 提交来自
     draw-time 路径（25917/29772）**且无来源归属** ⇒ 若要证明"合法对象有替代路径"，
     必须在活跃路径补仪表；
  3. 无修复前基线（Q2 裁定）⇒ 不做改善幅度宣称。
- 分辨率偏差（2048×1152 vs 冻结 2560×1440）已随结论声明。

## 2026-09-17 04:4x-05:0x：Gap B 补强与 S2 共享计算内核双双入库（均未部署）

- **Gap B 补强**（主线程实施）：`FindOrUpdatePaletteSlotCache` 增加 requiredPaletteCount；
  记忆槽位供出改为 A0-A5 全链（绑定命中/槽位域/[slot,slot+requiredCount) 上界/与记忆一致/
  groupCount 覆盖/绑定帧不旧/逐槽区间帧同源）；快照路径 S0-S3（上限 64、精确数量、
  快照帧新鲜度）；新增 6 个本地计数（导出 5 个）沿四出口接线。容差
  `kPaletteSlotCacheMaxFrameTagDelta = 0u`（严格同帧）。**未做 S4** ⇒ 不得宣称快照已排除陈旧。
  门禁：build32 exit 0 / ninja no-work / meson 72/72 / 静态 242/242。
  DLL 35,994,449 / `05BEBA32…`（后被 S2 覆盖为下一身份）。
- **S2 共享纯计算内核**（子代理实施 + 主线程独立复核）：新增
  `src/d3d9/war3/render/war3_runtime_group_palette_kernel.h`（312 行）与
  `tests/war3_runtime_group_palette_kernel_test.cpp`（908 行，T1-T13 生产函数级）；
  core.cpp 9778 行（-88）、upper 358 行（-98）改为薄适配层；meson 新增目标。
  等价性：30 万随机输入差分（4 步与 5 步两变体）**mismatches=0**；差分还独立证明
  适配层三条前置校验必须保留（不建模时出现 4399/300000 差异）。
  复核门禁：ninja no-work、内核实测 all T1-T13 passed、meson **73/73**、静态全量 **242/242**。
  DLL 35,996,016 / `F03A84E3303E4FE552855459868196460AAC87035BBA796160C3914B2FEE55D0`。
  **未部署、未实机；性能与画面未测。**
- **Gap B 不可度量的根因已定位**（子代理只读诊断，逐条 file:line）：
  manifest 的 2 条记录都通过 stable identity / resolved geoset，却全部栽在四键资源查找；
  `key3 (runtimeModelPtr,geosetIndex)` 结构性永不命中（`m_byRuntimeModel` 唯一写入者
  `bindRuntimeModelAlias` 的 alias 来源恒空，实机 `shadowModelResourceCount==0`），
  key4 同源为空；真正第一刀是 key1/key2，因为 manifest 的 runtime geoset 每帧从**活体内存**
  解析（`LegacyCacheHit+RawScan==CopyTotalScanned` 四轮精确成立），从不来自缓存。
  三个能写 index/modelResourcePtr 的 `noteRuntimeGeosetBinding` 调用者全部被**编译期常量**
  关掉（非合同、非 env）。**无纯 env 解。**
- 下一会话建议顺序：① 纯 env 零风险 trace（`DXVK_WAR3_SEMANTIC_SHADOW_TRACE=1` +
  `..._CONTRACT_CAPTURE_PERIOD=1`）把 key1/key2 定死；② 一处谓词修复
  `IsContractUnitCandidate`（放宽为"已解析 geoset 的场景记录"），验收用
  `semanticCoreResolved>0 / SkippedResourceMiss→0 / SubmittedDrawCount>0` + Arena/deviceLost + ABBA；
  ③ manifest 只有 1-4 条是上游 starvation（抓取点看到近乎空的写快照，已发布快照有 236 条），
  与 ① 无因果关系，不得混验收。
- **清册已按 Q4(b) 回写（2026-09-17，仅文档）**：`docs/plan/2026-09-16-runtime-switch-triage.md` 340,116 B／1,439 行／`DA217FF9…` → **341,920 B／1,454 行／`B25E1411…`**；新增 §0 口径裁定（412 全属内部面、玩家入口只认 Ctrl+F1 面板与 JAPI/`warvk:v1`、不删不改默认、85 待判定保持）与 §10.6 关闭项，§1.1-2／§3 前言／§9.3 的“缺玩家向文档”表述改为 Q4(b) 预期状态；412 数据行与 156/152/19/85 等计数未改（本文档行的旧身份声明由本行取代）。

## 2026-09-17 05:0x：两候选独立对抗性复核结论与修复（未部署）

- **最重要纠正**：Gap B 补强的 A0-A5 **只覆盖"记忆槽位"一条供出路径**；
  `FindOrUpdatePaletteSlotCache` 的 (a) 命中且 `+0x08` 合法 ⇒ 覆写并直接供出、
  (b) 未命中且 `+0x08` 合法 ⇒ 首见插入并直接供出，两条**仍为未证兜底**（有意保留，
  与 2026-09-16 架构复审口径一致）。设计 §8① 与静态测试注释的"未经帧证明的 arena 读不可达"
  属过度宣称，**已更正**；后续报告不得复述该措辞。
- 已修：快照陈旧计数加 `currentReadable && snapshotFrameTag != 0u` 门（避免污染 kDelta 决策）；
  槽位域判定改溢出安全形式；内核补 `matrixGroupSizes == nullptr && groupCount != 0` 守卫；
  快照上限 64 增加两侧单源一致性静态断言。
- 待办（P3）：槽位**所有权**缺口（FROZEN 携带 + 槽位重分配 ⇒ A0-A5 可全过）需实机 trace
  或 per-slot owner 见证；A5 读非原子槽位缓存的线程亲和性待记录。
- S2 结论：**未发现行为不等价**（基线为 2026-09-15 artifact；upper 侧哈希与设计声明逐字节一致，
  core 侧因 09-16 改动改用区域逐行匹配）。差分门 harness 未入库 ⇒ "30 万随机输入 0 mismatch"
  目前**不可复现**，已列为测试盲区待补。

## 2026-09-17 08:2x：上级（codex 01a02e0b）审查裁定（摘要，全文见 DEVELOPMENT_CHANGELOG 同日条目）

- Q-A：**不批准**放宽 `IsContractUnitCandidate`；先做对象级键/代际/发布/拒绝原因记录，
  再走**默认关闭的 dev 候选**。**"纯 env trace 零风险"作废**（CAPTURE_PERIOD 改抓取时序，需独立冻结预算）。
- Q-B：仪表 MVP-1 **条件批准无条件编译**，但第 4 项改为**draw-time 提交分母**；
  **否决** `gpuSkinLeaseBacked→OwnedPartSnapshot` 与几何 cache hit→`DrawTimeCaptured` 两类冒充映射；
  四项只证明路径到达与提交数量。
- Q-C：两候选**仅批准保留在未提交工作树**（不 commit/不发布）；S2 需补可复现差分（保留程序/种子/
  旧实现/输出并覆盖适配层）与"core 二次扫描 / 适配层新建 vector 失去容量复用"的成本口径；
  Gap B **仅是未验收的部分加固**，三处证明口径需修正（arena vs slot cache 的同帧标签、
  `>=`→`==` 未新增排除证明、`BindingMiss`/`SnapshotFrameStale` 不属同一分母）。
- Q-D：**不授予本轮开跑权限**；任何变化需新冻结记录 + 实机裁定；M2 可作显式诊断矩阵，
  但仪表工单"ON diagnostics=0 / OFF=1"与"只差合同"矛盾须修正；无法满足 2560×1440 应停下申请替代分辨率。
- Q-E：①改为"已观察到记忆槽位复核拒绝路径；'错误矩阵不再被使用'尚未证明"；统计统一到
  `DeviceRejectedStale` 样本 max 91,034；0.5s 采样非逐帧；三轮为 `forced=true` 受控终止；
  `CanonicalReadyCount=0` 不得定性为结构性死代码。已全部落入执行记录/工单。
- 下一会话顺序（上级）：修订记录与工单 → 落四项纯路径计数（不改 caster 准入）→ 补 S2 可复现差
  分与成本检查 + 修 Gap B 口径 → 单独提资源键诊断（有界对象级）→ 离线验证+冻结+再申请实机。
- **当前不晋升稳定、不提交、不发布。**

## 2026-09-17：长期状态校正与计划修订（用户复核后）

- 用户复核明确：上一轮 Goal 结项只代表其限定交付完成，**不等于重构完成、更不等于阴影问题解决**；
  「数据来源统一、生命周期统一、实机正确性证明」仍未闭合；未形成新的稳定发布基线。
- 长线计划已按此修订：`docs/plan/2026-09-16-architecture-review-longterm-plan.md` 新增
  “2026-09-17 修订：状态校正与收敛序列”（权威状态表 / 三个真正卡点 / 上级裁定要点 /
  S1–S5 收敛序列与每步完成标志 / 更新后的固定门禁 / 子代理路由 / 明确不做）。
- 本轮按收敛序列 S1 已落地/产出：
  1. 四项活跃路径纯计数（含 11 处/字段导出链、新静态门禁、工单按上级 Q-D 修正）；
  2. **P0 线程关系证明**：A5 的“同线程”前提**不成立**（控制面 drain 在管道分离线程消费构建；
     A5 逐槽帧读无守卫；AutoTest MCP 实际开启该 drain）——修复方案 A/B/C 待上级裁定；
  3. P0 对象级证据工单（61,438 B）：复用 `war3::tools::evidence` + 有界“后随观察表”，
     槽位所有权路线 A/B 比较，10 项待裁定；
  4. S2 可复现差分与成本检查：实施中（目标含旧实现副本 SHA-256、固定种子、≥30 万输入/组、
     适配层派生建模、分配/扫描趟数实测）。
- 门禁：`ninja -C build32 -j4` exit 0、`ninja -C build32 -n` no work、内核 T1-T17、
  meson 73/73、静态 244/244；DLL 36,000,018 B / `7DF26FF7…`（未部署/未实机/未提交）。
- 资源纪律：并发线程总量 **≤4**，构建统一 `-j4`。
