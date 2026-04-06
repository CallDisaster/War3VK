# 09 - 2026-02-24 夜间专项结项报告（第五轮 + 第六轮）

## 结项目标
1. 将本夜渲染层优化与逻辑层优化进行组合兼容验证。  
2. 找出导致崩溃/异常的具体开关组合并修复。  
3. 给出“性能模式”和“分析模式”的推荐配置边界。  

---

## 本夜主要改动（代码面）

### 一、渲染层（第五轮）
1. `RenderQueue` 热路径降噪：默认关闭高频诊断统计。  
2. 保守接管阈值自适应：  
   - 无透明队列时降低 Opaque 最小门槛；  
   - 高 Opaque 压力下放宽透明阈值。  
3. 透明排序快路径：透明队列已按 `sortKey` 有序时跳过 `std::sort`。  
4. `ShadowMap` 自适应更新：高 caster 且相机稳定时启用隔帧复用。  

### 二、逻辑层（第六轮）
1. 以“渲染优化 × 逻辑优化”做组合矩阵测试（8 组）。  
2. 初次定位唯一故障项：`kNativeJassVmDeepHooksEnabled` 组合。  
3. 根因修复：  
   - `executeJassFunctionInternal` 地址由中段 `0x7F2D92` 修正为函数入口 `0x7F2B40`；  
   - 深层 JASS Hook 新增函数序言校验（避免中段误挂）。  

---

## 自动化验证（AutoTest）

### 关键报告
- 基线（优化前）：`war3_perf_report_auto_2026_02_24_04_03_01.html`  
- 第五轮阶段最佳：`war3_perf_report_auto_2026_02_24_04_14_19.html`  
- 第六轮矩阵结果：`AutoTest/artifacts/round6_matrix/round6_matrix_results.json`  
- C6 修复后复测：`war3_perf_report_auto_2026_02_24_04_32_38.html`  

### 指标对比（核心）
- `avgFps`: `100.127 -> 112.122`（+11.99%）  
- `avgFrameTimeMs`: `9.987 -> 8.919`（-1.068 ms）  
- `avgGpuTimeMs`: `2.142 -> 1.635`（-0.507 ms）  

### 组合兼容结果（修复后）
- 矩阵 8 组全部通过（编译、进图、截图基线、报告导出）。  
- `C6_logic_jass_deep_hooks` 从“启动前退出”修复为稳定通过。  

---

## 性能障碍结论

### 渲染层障碍（当前）
1. `Hook_FlushAndReset/Orig` 仍是固定热路径开销。  
2. `Shadow/Main + ShadowMap + ShadowCapture` 构成稳定阴影成本。  

### 逻辑层障碍（当前）
1. 深层主循环/Wait/JASS 追踪开启后会引入观测开销。  
2. JASS 深层 Hook 对地址精确性要求极高，错误地址会直接崩溃。  

### 已验证策略
1. 默认发行建议使用“性能模式”：关闭重型逻辑追踪开关。  
2. 排障时再启用“分析模式”：提高覆盖率但接受性能回落。  

---

## 推荐配置（结项建议）

### 默认（发行/压测）
- 保留渲染优化：保守接管、Tag/Stage 缓存、ShadowMap 自适应。  
- 关闭重型逻辑追踪：`MainLoopDeepPhaseHook / WaitHook / JassVmPerfTracking`。  

### 分析（定位）
- 按需开启深层逻辑追踪，优先短窗口采样。  
- 深层 JASS Hook 仅在地址簿与函数序言同时通过时启用。  

---

## 结项状态
- 第五轮：✅ 完成（渲染优化落地 + 回归通过）。  
- 第六轮：✅ 完成（组合兼容 + 故障修复 + 复测通过）。  
- 本夜专项：✅ 结项。  

