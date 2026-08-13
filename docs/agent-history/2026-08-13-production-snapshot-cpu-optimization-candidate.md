# 2026-08-13 CurrentDraw 生产层 CPU 优化候选

## 范围与边界

本阶段从 `b2a1d192` 开始，集中降低 CurrentDraw/DirectGrouped 阴影生产层的
逐帧分配、复制、哈希、注册表查询和重复初始化开销。没有改变 Caster 选择、
CSM 分辨率/级联、PCF/alpha、Arena 上限、map/device epoch、GPU fence 或最终
replay fail-closed 合同；Compact WorkTable、Producer Claim、剔除及其它实验性
Consume 路线仍保持默认关闭。

本阶段没有部署 DLL、启动 Warcraft、运行玩家前台性能门或推送 GitHub。
因此以下结果只证明源码和离线合同闭合，不能声称已经取得实机 FPS 收益。

## 已完成的减税

- 资源快照只在最终 revision 构建，并为重建容量预分配；运行时模型别名改用
  同代索引，避免重复扫描。
- Exact Manifest、CurrentDraw 输出、palette/group-slot 临时存储和 frame hash
  索引复用调用线程 scratch，减少稳定帧的 allocator 压力。
- palette 解码与 hash 合并为单遍；可信 palette 直接打包；immutable geoset
  已证明的 group-slot 摘要和字节可直接借用，不重复扫描/复制。
- 多个仅作分类统计的原子计数由既有 canonical 结果推导，移除重复查询和 hash。
- 默认关闭的全局 fallback、prebuild 和实验 sidecar 不再对每个 Caster 执行
  无意义的完整初始化或运行时门解析。
- 材质 layer 解析范围收紧到真实候选；材质 cache key 使用稳定代际；同一
  packet 的 world transform、prepared slice 和 visible identity 不再重复解析。
- Instance/Pose/ShadowObject 的一帧 snapshot cache 使用 generation 验证，命中
  只复制一次，miss 直接写最终输出；缓存固定存储跨帧保留但 authority 每帧失效。
- grouped preselector 只在命中时保存 Visible 值，并以 cache-owned pointer 做
  一次性交接；BuildEligible 的 record/key/hint 三条数组合并为 16-byte build ref。
- 成功 Manifest 记录在最后一次 identity 读取后 move 进发布向量，避免完整 POD
  二次复制；正常 packet/sample 只在 builder 权威入口重置一次。
- BuildEligible/packet 细分计时对象仅在采样帧构造，默认 Release 路径不再为
  每个 Caster 初始化大组 timing reference。

## 提交与规模

- 提交范围：`d249f2d` 至 `d09e6f4`，共 33 个本地提交。
- 相对 `b2a1d192`：74 个文件，约 3141 行新增、477 行删除；新增内容主要是
  纯值 helper、Win32 runnable 和静态合同，不是新渲染功能。
- 分支：`codex/perf-production-snapshot-20260812`。

## 离线验证

- 本批次变更对应静态脚本：39/39 通过。
- `test_shadow_metadata_lifecycle_static.py` 中本阶段相关的 unsafe-prebuild gate
  合同单独通过；该历史脚本另有两条与本阶段无关的旧源码形状断言，未借本阶段
  范围修改。
- Win32 runnable：8/8 通过，包括 palette pack/hash、immutable group binding、
  compact palette storage、frame hash index、visible instance projection、classified
  counter 和 direct packet scratch。
- `build32/src/d3d9/d3d9.dll` 已完成最多 `-j2` 的增量构建；精确目标 dry-run
  为 no-work。
- DLL：35,096,184 bytes；SHA-256
  `6B22118900598D8C9F1DFCD44F2A9E4C45F29D4A614C74F12830DAFCB33D3F2D`。

## 下一物理门

1. 以前台可见桌面、同一地图和固定镜头，对旧稳定 DLL 与本候选执行 A-B-B-A。
2. 关闭重型 breakdown 后比较 DirectGrouped、BuildEligible、SnapshotPreselect、
   Populate、主线程 p95 和总帧时间；最低转正门仍为可重复净收益 0.15 ms/frame。
3. 同时确认 caster 数、alpha/blocker、producer completeness、Arena/replay reject
   和 device lost 全部不回归。
4. 只有该门通过后，才继续基于 10,000 帧 Observe 证据推进 terrain C2/C3
   提前剔除；不得把本阶段 CPU 减税冒充剔除已经启用。
