# R1/Q1：资源代际复用与 Query 生命周期

开发 checkpoint；未接游戏、未部署、未提交推送或发布。接续
[Presenter隔离验证](2026-10-22-x64-d3d9-presenter-isolation.md)。

## 本次收紧的数据链

x86 COM的资源身份 → 有界C2协议 → x64唯一owner → 原DXVK typed slot → 原GPU完成/资源退休。
共享 `d3d9_resource_registry.h` 管理64个逻辑槽、预约/提交/取消与代际墓碑；
CPU身份回收不代替GPU完成，host先完成原backend销毁，proxy只在精确成功回执后回收。
失败前检不消耗代际；不确定结果fail-closed；代际耗尽不回绕。

D9C2/D9R2明确拒绝旧版本。Draw的VB/IB/texture/VS/PS/declaration都携带完整ID+generation，
六者全部通过type/代际检查后才映射backend slot。decode失败不修改输出。
固定预算未扩大：64逻辑槽、8buffer/4texture/4VS/4PS/4declaration，buffer最大256KiB，packet64KiB。

EVENT Query不再按累计Issue次数占槽：Query有自己的资源身份与递增revision，
Issue/Poll同时绑定revision与GPU serial；GPU没有新工作时serial允许相同，但revision仍递增。
Query创建/销毁是CPU逻辑操作，不伪造GPU提交；Finish仍要求所有逻辑句柄归零。

## 实跑与失败保留

`AutoTest/artifacts/x64-resource-lifetime-20261022-r1/receipt.json` 保持false：
COM在45秒整例watchdog下超时并强制清场，未得到结束摘要；日志只证到初始化完成，卡顿位置未知。
未放宽deadline、未缩减用例。r2仅为fixture增加每16轮/64次Query进度、host增加冷路径退出标记；
Connection、backend与通过标准未改。不能把r2通过写成r1根因已修复。

`AutoTest/artifacts/x64-resource-lifetime-20261022-r2/receipt.json` 为true，经独立只读审计复核原始日志：

| 实测 | COM | Raw |
|---|---:|---:|
| Draw / Readback | 96 / 96 | 51 / 51 |
| 像素检查 / 失败 | 319488 / 0 | 169728 / 0 |
| accepted / rejected / backendCalls | 2244 / 0 / 2242 | 734 / 61 / 730 |
| GPU提交 / 完成 / 退休 | 673 / 673 / 673 | 162 / 162 / 162 |
| Query Issue / Poll | 256 / 256 | 257 / 270 |
| 宿主正常退出等待 | 47ms | 109ms |
| 实际记录离线变异拒绝 | 39 / 39 | 344 / 344 |

COM每轮先把六个公开引用释放到0再绘制：576个public release不冒充宿主销毁。
前95轮解绑，最后六个private binding跨过256次Query直到device析构；
host的577逻辑句柄全部销毁、570复用、所有live=0独立证明远端收口。
Raw的48资源反例后各重新绘制/检查像素，13 Query反例后各合法Poll；
257次同GPU serial精确为159，最后恢复绘制推进到162。

CPU registry测试x86/x64各6805项、wire各1315项通过；wire DrawBytes=1164。
7次native launch都在真正非输入Desktop，exit0/Job active0、无超时/强杀/cleanup error；
输入Desktop保持Default且专用Desktop关闭。1118源码、6执行副本、编译数据库和参考/玩家DLL均保全。
这些离线变异不是额外GPU次数；补充child_settled分类不冒充持有原始句柄，原始句柄等待另有native证据。

## 当前共享 cache host 的 R1/Q1 r3 重验

`AutoTest/artifacts/x64-resource-lifetime-20261022-r3/receipt.json` 为 true，
SHA-256 `594CC6ABC1818C5BF9721DD4F9881C1FEA38C048913E0105F750251AF8C1AAB0`。
它绑定本批 host `888B801C6587AAFB4873B30A9FE048C0CC2F575EFE4D25E6A4E143F72DD3B4C3`，
不是把旧 r2 结果套到新二进制上。独立只读审计重新调用实际读方并逐条核对 raw。

- COM 96轮、576次公开资源释放后96次有效绑定绘制、319488像素零错；Query 256次Issue/Poll。
- raw 61条准确拒绝后有48次像素恢复、13次查询恢复，169728像素零错；257次serial均159。
- COM/raw的GPU提交、完成、退休分别为673/673/673与162/162/162；最终逻辑资源、字节、句柄、Query归零。
  公开Release不是即时远端销毁，最后六个私有绑定仍按原合同保留至设备析构。
- 当前1128个源码、9个二进制、6个执行副本及7份初始raw清单重新核验无漂移；读方变异39+344全部拒绝。
- 32/64位registry各6805、wire各1315检查通过；7次native启动均在restricted-Medium非输入Desktop。
  COM/raw子进程自然退出等待79/312ms，父进程exit0、Job空、专用Desktop关闭，输入桌面未切换。

当前参考DLL `6805CC0B…`在本门中仅为受保护身份，未执行它的像素A/B；三路shader等价另有独立回执。
本次通过不覆盖Presenter r10初始化超时、r8/r9退出失败或下列游戏/长时/真实pending Query边界。

## 未完成与下一条主线

- r1的间歇性卡顿、Presenter r8的退出延迟仍未取得因果根证，不能被本次通过覆盖。
- Query未Issue就Poll、UINT64 revision耗尽、持Query时Finish拒绝没有在本native组合单独执行。
  本次也没有强制制造真实pending S_FALSE→S_OK窗口。
- x86仍保留完整VB/IB/texture backing；IB校验还读取本地索引。
  SYSTEMMEM Surface不在64槽registry里，shader capacity也不等于logical bytes。
  没有实测游戏VA/commit下降，不能以“x86未加载Vulkan”换算节省量。
- 普通API仍同步RPC，4槽队列不是产品异步/背压owner；Game.dll拒绝保持。
  FFP、adapter/format协商、完整texture/Lock、Reset、WarVK各pass与游戏画面均尚未迁移。
- 后续顺序：继续定位并关闭间歇性退出/卡顿；原二进制VVL+同步验证；
  接入分阶段真实双进程内存和backing容量账户，再推进产品异步owner与最窄资源backing退役。
  不提高预算、不以少绘制或关闭特性冒充减量，不将实验切片百分比当整个渲染器迁移百分比。
