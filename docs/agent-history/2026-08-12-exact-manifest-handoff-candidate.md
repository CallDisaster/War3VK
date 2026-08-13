# Stage11 exact manifest 单次遍历交接候选

## 问题

发布默认按 `Stage11 exact producer → DirectGrouped supplement` 的顺序工作。exact
producer 已经遍历 `m_war3DrawTimeVBCache`、完成当前帧 caster 的身份、blocker、alpha、
backing 与 publication 判定；随后 DirectGrouped 的 BuildEligible 初始化又遍历一次同一
cache，只为把 `exactSubmittedFrameSerial == current frame` 的 entry 重新转换成 manifest
record。

性能报告中约 107 个 CurrentDraw record/frame、71 个 eligible record/frame，且
`drawTimeSemanticProducerOwnedDirectGroupedSkipCount` 与 DirectGrouped 的重复输入规模一致。
这项重复扫描位于约 0.51 ms/frame 的 BuildEligible 路径内。

## 修改

- exact producer 接受调用方拥有的 `CurrentDrawContractRecord` vector；
- 只有 caster 实际进入 `m_war3Scene.shadowCasters` 并写入
  `exactSubmittedFrameSerial` 后，才追加历史等价的 manifest value；
- 同一 host frame 的重复调用如果已经由 exact owner 发布，会依据正向 submitted marker
  重新追加同一 value，不能因 early continue 丢失 manifest；
- DirectGrouped 只消费该当次调用的按值交接，不再第二次扫描
  `m_war3DrawTimeVBCache`；
- blocker、alpha fail-closed、未完成 backing 和只 claim 未 publish 的 entry 均不进入交接；
- 交接不存入 device member、thread-local 或跨帧 cache，不包含新的 GPU owner/fence 语义。

manifest 字段保持旧扫描器的确切分类：使用 entry 自身的 rawcode/objectKind/layer/stage，
仅对已知 static-world rawcode 保持原有 Destructible 归一化。当前 visible-record enrichment
只影响真实 caster，不提升 manifest 中的 Unknown 身份。

## 验证与边界

- 新增 5 项交接静态合同，并更新 Stage11、桥/坡面、metadata、attachment、blocker 与
  persistent-package 既有合同；本阶段共 124 项相关静态断言通过。
- 全部 37/37 Win32 runnable 通过。
- Win32 DLL 以最多 `-j2` 构建通过，精确目标 dry-run 为 no-work。

该候选未部署，也未进行同场景前台 A/B；离线验证只能证明控制流和生命周期边界，不能
宣称已经回收多少毫秒。预期收益来自少一次 cache 全表遍历与 record 重建，应在下一次
固定镜头性能报告中观察 `BuildEligible`、`DirectGrouped` 与主线程 p95。
