# 2026-07-16 阴影基线恢复与现代功能保留矩阵

## 输入与回退

- 作者归档：`src/d3d9.zip`
- 归档时间：2026-07-16 10:23:12
- 归档 SHA-256：`8A733A5CB551A11AC74CBCCC4FBD29492D1618C7A98AB1146846CA98F153772D`
- 解压审计目录：`output/warvk_d3d9_archive_20260716_8A733A5C/d3d9`
- 修改前当前源码备份：`output/pre_archive_restore_current_d3d9_20260728.zip`
- 修改前备份 SHA-256：`01382C28B345F13544A98FF2D01D116D20237F3EF9522DE55AC66BF06CC79EA0`

归档包含 348 个文件；当前 `src/d3d9` 在同名文件中有 96 个发生变化，并新增
17 个文件。恢复必须按功能边界进行，禁止直接用整个目录覆盖后丢失独立的新功能。

## 归档作为权威来源的范围

以下内容恢复到 7 月 16 日合同：

- Stage11 CurrentDraw / semantic Caster 发布、消费和快照合同；
- shadow object registry 与 semantic runtime bridge；
- visible manifest 的 Caster 生产与跨帧可见性合同；
- alpha-test payload 的旧权威传播路径；
- `D3D9DeviceEx` 内的 semantic packet、direct grouped、draw-time producer、
  scene populate 和 draw-time Caster 捕获函数；
- 后续加入的 Stage10/S12/S13 owner、tombstone、metadata 几何捷径、S1 early
  retention、draw-time fast append 等不得继续控制生产路径。

## 必须保留的当前实现

### 点光源、点阴影与体积光

- `d3d9_war3_volumetric_light.{h,cpp}` 及当前 shader；
- `d3d9_war3_shadow.{h,cpp}` 中 point-light snapshot、point-shadow worker、
  point-shadow receiver bias、Hi-Z 和 Volume Sun 入口；
- 2026-07-27 点阴影 texel footprint/slope bias 摩尔纹修复；
- 2026-07-16 体积光地表端积分、cascade blend、far caster depth extension 和
  当前资源生命周期。

### 性能报告与调试

- `war3_hook_perf.h`、Hook inventory、Hook/NativeOriginal/WarVKHookLogic 动态分账；
- `war3_perf_monitor.{h,cpp}`、`war3_perf_report_template.h`、control plane、frame
  capture、internal test API；
- Present-to-Present 帧根、Main/CS/Worker 泳道、工作量序列、p50/p95/MAD、环境变量
  快照和原子 HTML 导出；
- 隔离桌面 runner、final-caster JSONL 与 exact backbuffer 对齐工具继续保留。

### 已证明安全且值得保留的 7 月 16 日后优化

- T1-1 点阴影 worker 小 POD；T1-2 registry 只读 shared lock；T1-3 快照指针读取；
- T1-4 热路径诊断门控；T1-5 thread-local 临时存储与 replay const-reference；
- T1-6 主 CSM/Volume 统计隔离；
- persistent geometry 每帧一次 GC、miss 后避免二次必败 lookup；
- widget negative cache 8 帧与 `executionRoute()` 单次读取；
- resource snapshot 共享、D3D allocator 尾段/映射失败修复；
- D3D9 interface/swapchain/shader 生命周期安全修复；
- Storm managed realloc 16-byte 对齐修复；
- ConsumerBuild rotate gate 和 SummaryRefreshRequest 归因拆分。

上述优化若与归档 Caster 合同发生冲突，以画面正确性为先；只能重新移植独立、已验证
等价的部分，不允许保留会跨帧重放易失几何的优化。

## 明确不保留的后续阴影实验

- Stage13 sparse/late descriptor/retention/unique-semantic cache；
- S1 fallback early cache 与任何 frame-arena 跨帧 backing；
- draw-time VB cache、semantic fast append、prebuild bypass；
- current-frame metadata 几何生产捷径及其额外 geometry ledger；
- 用 lease/grace/last-good hold 掩盖当前帧 Caster 缺口；
- TAA v2、Stage owner/lifecycle/tombstone 对 Caster 集合的生产控制。

## 发布门

1. Release Win32 编译与 no-work；
2. 点光、点阴影、体积光三项运行矩阵仍有实际 section/draw；
3. 普通单位/可破坏物、透明树木、路径阻断器、桥/斜坡、高压图 exact backbuffer；
4. 至少 240 帧 final-caster trace，对原点巨型几何、alpha gap、blocker leak 和同步
   disappearance 做闭合；
5. 性能只比较同 DLL 工作量比例，不用受前台负载影响的绝对 FPS；
6. 未通过物理屏前不提交、不推送。
