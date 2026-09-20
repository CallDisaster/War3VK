# 2026-09-16 — stage13/native static shadow（A 88089cd → B）行为级移植核对

> 来源：合并台账 P2 项的子代理只读核对（grok-4.6）。用户约束：逐项核对 hook/生命周期/
> 默认值/shader 接口，禁止按 diffstat 推断；禁止整文件覆盖、禁止 cherry-pick 88089cd。

## 结论摘要

88089cd 共 35 文件 +4270/−143，按行为分 7 个单元：
- U1 Native static shadow producer 治理（RegisterImage/StaticStamp 直写/doodad type=0 默认拦截）
- U2 Path-blocker 五源一致性（semantic core + NativeD3D9 canonical + legacy capture）
- U3 Draw-time VB cache 静态别名迁移
- U4 Stage13 content-persistent geometry（默认开、内容快照+私有字段/字节 proof、64MiB CPU cap）
- U5 Registry publish/GC/reset domain 隔离（Generic vs Stage13Exact）
- U6 观测/诊断字段
- U7 地址表注释（Type5，默认不安钩）

B 完全没有（代码级缺失）：CanonicalShouldSubmitPacket、SemanticCoreShouldSubmitResolvedPacket、
Stage13ExactIdentityProof/IdentityDomain/proof 字节账、m_war3DrawTimeStaticAliasIndex、
Default_Block* 一组、QueryShadowRegisterImage*/QueryShadowPathStaticStamp*、斜杠 shadow key、
三件套测试、control_plane stage13ContentPersistent* 字段。

关键风险（详见报告）：d3d9_device.cpp 两树已分叉 2500+ 行禁止整文件覆盖；
MAX_AGE 240→3600 不是 Stage13 私有默认（会改 B 地形 cache 行为，建议拆独立 env）；
U5 domain 隔离必须先于 U4（否则 S1 会把 Stage13Exact 当 generic 命中）；
B 的 doodad gate 测试锁定"默认关"，移植 U1 必须先改测试；
B 的 RegisterImage/StaticStampPath hook 编译期未安装（constexpr=false）。

## 建议移植顺序（批次 0-7）

0 观测先行（计数器/query API/字段，不改默认不安钩）
1 Producer 默认治理（U1，含 doodad gate 测试改写；玩家可见，须先等用户裁定默认值方向）
2 Path-blocker 提交闸（U2 semantic+canonical，抽公共头优于复制粘贴）
3 Legacy EntryGate 收窄 + Stage13 blocker 预检（独立批，不与 U1 捆绑）
4 Registry domain 隔离（U5 无 Stage13 创建；idle 保持 240）
5 Static alias migration（U3，可与 4 并行）
6 Stage13 content-persistent（U4，依赖 4，建议 2+3 已入；MAX_AGE 拆开）
7 Type5 地址表（可选，默认不安钩）

完整 file:line 证据、编译失败级/行为冲突级清单、每单元测试要求与实机门见会话存档；
本文档为摘要版。执行时必须遵守：先改测试再改默认；无 U5 不开 U4；shader/lifecycle/
palette slot 合同不在本批。
