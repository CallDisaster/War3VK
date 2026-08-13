# DirectGrouped 索引工作区性能候选

## 证据与范围

2026-08-12 三份同场景报告表明，关闭被否决的 coherent REAL 路线后，
`Populate` 与 `DirectGrouped/BuildEligible` 仍是 WarVK 自有生产层的主要可控 CPU
开销之一；其中 `BuildEligible` 约为 0.51 ms/frame。报告同时证明此前看到的
`ConsumerBuild` 累计次数不能直接除以报告窗口帧数：语义计数是进程累计值，
窗口内实际 `Populate` 仍为每帧一次。

本候选只减少 `War3TryPopulateDirectCurrentDrawGrouped` 内部的数据搬运：

- immutable `directRecords` 快照继续作为唯一合同记录来源；
- preselection、stable sort 与最终选择只携带 `uint32_t` 快照索引，不再多次复制和
  移动完整 `CurrentDrawContractRecord`；
- Compact WorkTable 仍按选择后的 build 顺序读取；
- 只复用索引和排序键的 thread-local 容量，不保留 record、packet、`Rc<>` 或 GPU
  资源；selection lease 容器仍为当次调用的局部值；
- fallback 通过 `iota` 生成原始顺序索引，保持历史顺序。

没有改变 caster admission、alpha/material、source generation、map/device epoch、
GPU backing、Arena、replay validation、publication 或任何 Off/Observe/Consume 默认值。

## 离线验证

- 81 项定向静态合同通过：Compact WorkTable、Producer Claim、性能归因、Stage11
  static-world、桥/坡面、metadata lifecycle 与 replay domain。
- 3/3 Win32 runnable 通过：producer completeness、replay validation、persistent
  package/current-draw equivalence。
- Win32 `d3d9.dll` 以最多 `-j2` 增量构建通过。

## 尚未完成的门

该改动只形成未部署的性能候选。尚未在同一进程、同一镜头、相同采样窗口下完成
Off/A/B/A 前台物理性能对照，因此不能从离线结果宣称 FPS 或毫秒收益。若收益小于
0.15 ms/frame，应保留其正确性中性基础价值但不把它描述为一次显著性能更新。

后续仍应优先依据低开销 profiler 处理 `BuildEligible` 的实际子阶段；未经 10,000
帧零误判 Observe 证据，不启用 Producer Claim、联合剔除或 Persistent Package Consume。
