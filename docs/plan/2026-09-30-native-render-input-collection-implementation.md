# 原生渲染输入采集：独立于共享核心的第一批实施

日期：2026-09-30。用户要求先补数据收集，不等待共享上游。此工单不是共享核心 B、完整 renderer 接管或实机授权。

## 本批范围

沿现有生产链补齐一条可核验的交接：当前设备的 D3D9 draw 状态 → exact draw-time/alpha 元数据 → 最终选中 caster → resolved replay → 既有有界原始输入导出 → 严格读方。注意 `SetMaterial` 已含 WarVK Ambient 改写，不能称为未经修改的暴雪/MDX 原材质。

- 复用现有实例/网格/代际/provenance、几何/矩阵字节和录制器；不建新历史环、全场景缓存或 GPU 回收系统。
- 新增定长 draw 材质观察 POD，保留 render/texture-stage/sampler 状态、D3DMATERIAL9 和纹理变换的原始位。
- 数据随最终来源交接；native snapshot 或 exact alpha payload 覆写时同时覆写观察，不能沿用旧证明。
- resolved input 另存 finalShadowMaterial，显式区分它与原画面状态。保存 live view/sampler 身份不等于保存纹理像素。
- 关闭原始输入诊断时不额外读设备状态；不开新隐藏开关，不改变 caster 准入、渲染预算、shader 或原版 draw。新增诊断元数据大小必须计入既有内存准入；POD 本身增大 entry/draw，即使关闭诊断也不能称为严格零成本。
- 新字段使 input manifest 升到 schema 3；旧 schema 1/2 的字节、字段语义与读法保留。不改变 CPU event wire 或共享核心 A2 容器。
- 测试实际生产 POD/转换/解析，包含 unknown、位模式、最终来源替换、陈旧身份、有限值及元数据容量账户。

决策所有者仍是现有最终输入选择入口。本批只记录事实，不选择另一份 palette，也不重新读取游戏裸指针。

## 本批不能声称完成的内容

1. 完整原生实例生命周期、MDX 原 geoset 对应和原组 remap：仍需资产/原生采集证明。
2. 已求值 GEOA alpha/RGB、UV/材质动画、完整动画时钟：已有 D3D9 material/texture factor 是提交状态，不是这些上层渠道的证明。
3. 全场景输入：现有 raw input provider 是有界 Stage11 shadow replay 样本，不含所有主画面 pass、粒子、条带、UI 或战争迷雾合同。
4. 可直接切换共享 renderer、纹理重建、GPU 执行或像素正确性：input 捕获位于 prepared replay 点，实际命令须另用既有 batch/index draw 事件对拍。
5. 性能收益/全游戏零开销：仅报告本次组件成本与新增大小，完整帧 p95/p99 与游戏覆盖未做不写通过。

## 本轮收口

2026-09-30 第一条链已实施并通过定向离线回归，详情见 [实施回执](../research/2026-09-30-native-render-input-collection-checkpoint.md)。不等于上述六项缺失已闭合，不等于共享 renderer 准入。DLL 只构建、不部署。

## 验证与收口

- 开工 HEAD `a2054a9776e40edc3f01f6f390c06037965b8ef2`，分支 `codex/v1.22-main-tree-20260920`，已有 93 个 dirty 路径（92 个可直接 hash，另一个为 StormBreaker）。原改动保留；本批实际路径在 checkpoint 列清。
- 构建只用已有独立配置且先核配置/依赖，不把旧 build32 当发布目录；编译 BelowNormal、最多 `-j2`。不部署、不触碰游戏现场、不启动游戏/GPU、不提交推送。
- 无需共享库即可跑 POD/读方与生产编码往返；格式正确不代表缺失输入补齐。
- 每次实际验证写 DEVELOPMENT_CHANGELOG；本批成果是第一条生产采集链，不宣布整个数据收集模块全部完成。

后续：先核原生资产/实例/组映射与 GEOA 来源，再加入独立帧输入签发；共享库就绪后才按作者通知进入 B/联调。
