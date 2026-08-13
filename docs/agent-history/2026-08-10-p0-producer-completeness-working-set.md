# P0：Stage11 producer completeness 与活动静态缓存

本候选针对低视角、大量树木时的局部阴影闪烁，收紧两项 producer 侧合同：

- Exact Stage11 和 fallback/Arena 路径会在必需 caster 因分配预算、分配失败、字节预算、Arena admission 或 freeze 失败而缺席时写入值语义 completeness seal；blocker、非 caster 与透明 fail-closed 拒绝不计为 omission。
- CSM、体积太阳和点阴影在清 target、启动异步 prepare 或录制 replay 前验证 seal。stamp 不同或有 omission 时，本帧不发布部分输出；方向光仅保留既有同 epoch/resource 的八帧 last-complete 规则。
- Draw-time static cache 的 64 MiB 改为 **inactive** LRU 目标。最近 Stage11 工作集受短窗口保护；活动集合超过该目标时记录 over-cap，而不是将可见对象逐出后让其重新撞上每帧 32 次分配门。

新增运行时与性能字段记录 omission 原因、unique deferred caster、活动/保护/over-cap/驱逐字节和 producer-incomplete 帧。该候选仍须通过用户的前台低视角树木实机验证；离线合同和 DLL 构建不能证明视觉闪烁或 TDR 已解决。
