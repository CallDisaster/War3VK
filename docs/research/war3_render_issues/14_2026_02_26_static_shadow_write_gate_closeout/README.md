# 14 - 2026-02-26 静态阴影写入端五轮收官报告

## 2026-04-04 后续校正
1. 本页保留为 2026-02-26 当时基于 `RegisterImageEntry` 路线的阶段性收官记录。
2. 后续更深一轮 IDA 逆向确认：
   - `TerrainShadow_RegisterImageEntry(0x713250)` 不是 ListA 建筑静态阴影共享 mask 的主根因；
   - `ListA` 静态 blob 阴影真正更上游的主写入链是
     `ShadowPath_StaticStamp_Toggle(0x74E420) -> ShadowStamp_WriteByName(0x713B20) -> ShadowStamp_WriteCore(0x713920) -> shared shadow mask`。
3. 因此，本页关于“以 RegisterImageEntry 为中心治理 ListA 静态阴影”的结论，应视为旧阶段方案，而不是当前最高置信度事实基线。
4. 最新主结论请优先看：
   - `../23_blob_shadow_lista_upstream_reverse/README.md`

## 背景与目标
1. 项目已具备 CSM 阴影，但原生静态贴花阴影（建筑/可破坏物）仍会叠加，影响观感。  
2. `ListA` 混合了雾/边界/阴影，末端粗拦截误伤风险高。  
3. 本轮锁定“写入端主控”路线：以 `TerrainShadow_RegisterImageEntry` 为中心治理，`UpdateWrite/ListB` 仅兜底。  

## 本轮执行结论（先给结论）
1. 五轮计划已执行到收官，主干策略已落地并可稳定运行。  
2. 核心痛点已实质缓解：`WithParams + UberSplat` 可在写入端稳定阻断（`calls=201, blocked=27`）。  
3. 生产档性能与稳定性通过（R5 相对 R1 无回退）。  
4. 严格意义“完整验收”仍有证据缺口：未采到 `ownerBuilding/ownerDestructible` 正命中样本，`Selection/Occlusion` 来源在自动场景未触发。  

---

## 代码与架构落地

### 1) 写入端策略
1. `war3_shadow_filter_policy.cpp` 增加 `WithParams + UberSplat` 精确阻断规则：  
   - `reason=Mode1_BlockWithParamsUberSplat`。  
2. `RegisterImage` 来源识别使用返回地址精确匹配，避免函数范围误判。  

### 2) 早装链路
1. `d3d9_war3_hook.cpp` 增加 `TryInstallShadowHooksEarly(...)`，在 `MainRunner` 入口前抢先安装 Shadow Hook。  
2. `war3_hook_lifecycle.cpp` 在 `Hook_MainRunner/Hook_MainRunner_Alt` 入口调用早装逻辑。  
3. 增加 `g_shadowHooksEarlyAttempted` 门控，避免热路径重复探测。  

### 3) 行业化结构矫正（第四/第五次排队收口）
1. `d3d9_war3_hook.h` 增加 `ActivateWar3Runtime/TryInstallShadowHooksEarly` 正式声明，移除生命周期域隐式 `extern` 耦合。  
2. `d3d9_war3_hook.cpp` 抽出 `BuildShadowHookAddresses(...)`，统一早装/常规安装路径地址构建，降低漂移风险。  

---

## 五轮执行与产物

### A. 五轮矩阵主产物
1. `AutoTest/artifacts/static_shadow_write_gate_matrix/20260226_034406/matrix_results.json`  
2. `AutoTest/artifacts/static_shadow_write_gate_matrix/20260226_034406/autotest_summary.json`  

### B. 关键补证据产物
1. 早装探针：  
   - `.../acceptance_writepath_probe_20260226_3rd_queue/case_ft_echoisles_earlyhook/`  
2. UberSplat 阻断：  
   - `.../acceptance_writepath_probe_20260226_3rd_queue/case_ft_echoisles_earlyhook_block_ubersplat/`  
3. mode0/mode1 A/B：  
   - `.../acceptance_mode_ab_20260226_3rd_queue/summary.json`  

---

## 关键证据（可复现日志）

### 1) 早装生效后写入覆盖提升
1. `case_ft_echoisles_earlyhook` 末尾统计：  
   - `calls=201 blocked=0`  
   - `srcWithParams=0/27 srcFromTwoPoints=0/174`  

### 2) 加入 UberSplat 规则后的精确阻断
1. `case_ft_echoisles_earlyhook_block_ubersplat` 末尾统计：  
   - `calls=201 blocked=27`  
   - `srcWithParams=27/27`  
   - `srcSelection=0/0 srcOcclusion=0/0`（未触发、无误拦）  
2. 典型阻断 key：  
   - `GoldmineUberSplat`  
   - `HumanUberSplat`  
   - `OrcUberSplat`  
   - `UndeadUberSplat`  
   - `HumanTownHallUberSplat`  

### 3) 五轮性能门限
来自 `20260226_034406/autotest_summary.json`：
1. R1 baseline：`avgFps=103.244`，`avgMainThreadCpuMs=3.741`  
2. R5（生产档）：`avgFps=103.092`，`avgMainThreadCpuMs=3.715`  
3. 相对 R1：  
   - `fpsDeltaPct=-0.147%`（通过 `>= -3%`）  
   - `mainThreadCpuDeltaMs=-0.026ms`（通过 `<= +0.2ms`）  

### 4) 稳定性
1. 五轮可执行轮次均 `ok=true`，无崩溃。  
2. R4 按计划条件跳过（未提供 callback 黑名单且无强制开关）。  

---

## 验收结论（按原口径逐条）
1. `mode=1` 建筑/可破坏物原生贴花抑制：`部分通过`。  
说明：UberSplat 主路径已稳定拦截，但 owner 分类仍多落 `Unit/Unknown`，缺 `ownerBuilding/ownerDestructible` 直接命中证据。
2. 雾/边界保真：`部分通过`。  
说明：未观察到白名单来源被拦截；但自动场景未触发 `Selection/Occlusion` 来源，证据强度不足。
3. `mode=0` 行为不回归：`通过（自动化层面）`。  
说明：A/B 场景均可正常进图、截图、出报告；无崩溃。
4. 稳定性：`通过`。  
说明：矩阵与补测均 `ok=true`。
5. 性能门限：`通过`。  
说明：R5 相对 R1 在 FPS 与主线程 CPU 均满足门限。

---

## 是否“彻底解决”
1. 结论：`未达到严格意义的“彻底解决”`，但`主痛点已被工程化抑制`。  
2. 当前可交付状态：  
   - 可在 `mode=1` 下稳定抑制主流静态贴花阴影（UberSplat 路径）。  
   - 保持生产档稳定与性能门限。  
3. 剩余风险：  
   - owner 语义仍有歧义（`ownerBuilding/ownerDestructible` 样本缺失）；  
   - 白名单来源需在可控场景做显式触发验证。  

---

## 建议下一步（收尾后的最小增量）
1. 做一张“建筑/可破坏物可控触发图”用于 owner 语义闭环采样。  
2. 增加 `Selection/Occlusion` 强制触发步骤（自动选中/遮挡）做白名单保真硬证据。  
3. 若仍有残留，再启用 R4 callback 黑名单精确收口（仅针对已证实残留回调）。  
