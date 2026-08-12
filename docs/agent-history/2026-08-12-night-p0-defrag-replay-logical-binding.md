# 2026-08-12 夜间 P0：defrag-safe shadow replay 逻辑绑定候选

## 问题边界

既有 `War3ShadowCasterDraw` 会跨生产线程与 CS 录制线程保存捕获时的
`VkBuffer/offset`。DXVK memory defrag 可以在两者之间把同一个逻辑
`DxvkBuffer` 从 backing A 迁移到 backing B；旧 replay 随后可能向已退役或未绑定的
A 录制 draw。此前 device-fault 的 `WRITE_INVALID` 与 defrag-on / defrag-off A/B
符合这条风险链，但本候选尚未经过真实 device-lost 注入或玩家物理门，不能把静态闭合
描述为 TDR 已修复。

## 实现

- 新增有界的逻辑子范围合同，以 checked subtraction/addition 表达 owner 内的
  offset/length，拒绝零长度、越界和整数溢出。
- 每个 position/index/blend/UV stream 保存 owner、可选 pinned allocation、
  map/device/source generation 及逻辑范围。
- producer 在封印 completeness 之前捕获逻辑绑定；persistent geometry 在全部 backing
  创建完成后捕获一次，并在每次 cache hit 时把同一逻辑身份复制给 draw。
- CS 线程在 hash、point-shadow worker、validator、sort 和首次 clear/draw 前生成一份
  value-owned resolved snapshot：
  - relocatable stream 从当前 `DxvkBuffer::getSliceInfo()` 解析，因此 A→B 后绑定 B；
  - pinned stream 从固定 allocation 解析，并继续由 consumer command list 追踪；
  - owner、epoch、source generation 或 range 不匹配时整批 fail-closed。
- directional CSM、terrain mask、volume sun、point shadow 和两条 outline 路径消费同一类
  resolved snapshot。validator 新增 `UnresolvedBufferBinding`；其 `requiredEnd` 保存细分
  binding reject reason，继续通过现有 runtime/flight replay 诊断链输出。

## 离线验证

- `AutoTest/test_shadow_replay_logical_binding_static.py`: 6/6。
- cross-map lifecycle: 24/24；producer completeness: 9/9；isolated desktop safety:
  7/7。
- release hardening/review、outline replay、replay domain、stage/metadata/tombstone lifecycle
  和 producer flight 共 75/75。
- Win32 runnable：replay validation、producer completeness、shadow lifecycle、Arena
  lifecycle、Arena budget 共 5/5。
- Win32 target build `-j2` 成功；`ninja -C build32 -n src/d3d9/d3d9.dll`
  为 no-work。构建依赖数据库曾损坏，旧文件已保留为
  `build32/.ninja_deps.corrupt-20260812-0044`，随后由一次不中断目标构建重建。
- DLL SHA-256：
  `B91EE9DC9C5D44A989E399184F7E96D2E92778F036889A61282E97D40A5E8FD3`。

## 未完成物理门

- 2026-08-12 默认 Release 构建 `93245824...A8BB` 已在隔离桌面完成三轮
  “生与死”低视角 10 分钟门：artifact 分别为 `20260812_075908`、
  `20260812_080948`、`20260812_082027`，实测时长为
  `602.091/602.426/602.176` 秒。三轮均无 device lost、新 Event 153/4101
  或 GPU incident，且每轮退出后均恢复稳定 DLL `79CA8DB4...B2A4`。
- 这三轮只闭合了 TDR 稳定门，没有闭合阴影完整性门：每轮仍有 `79/84/76`
  个 producer-incomplete/budget-exceeded 帧及约 `9.5k` 个
  `fallbackByteBudget` omission，Arena 峰值均触及约 384 MiB。它们不会发布
  partial CSM，但会表现为有界的无阴影窗口。
- 尚未完成普通对战 20 分钟和用户前台视觉门，因此不能宣称首次历史 TDR 的
  唯一根因已证明或整个阶段已可发布。
- 尚未证明首次 TDR 的唯一成因；若仍发生 device lost，必须结合本候选的 binding reject
  和 device-fault incident 形成新假设后再改二进制。
