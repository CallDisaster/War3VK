# 2026-09-20 夜班：页压力、阴影停更与候选边界

## 已取得的结论

本次不是阴影消失修复通过。两个完成的隔离运行都复现了池容量压力与阴影图停止刷新；其中上传索引范围开关 ON 的运行没有命中该优化路径。不能用两轮拒绝累计数的差异宣称收益。

当前更具体的源码/账户解释是：大量已用区间不再被 cache 引用，但同页仍有少量长期引用，使整个页无法从活动页池移走。一份必需 caster 的快照被拒绝后，整份阴影候选因完整性失败而不能发布；有限保留期结束后接收端改为无完整图/零阴影强度。这解释了“不是仅少几个对象，而是全场景失影”的传播机制，但尚未证明所有容量增长源及完整修复。

## 实机身份与条件

- 唯一源码树：`dxvk-v1.22-integration-20260914`，HEAD `ae890542d766470d1703f5bea7f5b73636039733`。
- 两轮同 DLL：36,399,883 B / `8B40212050A9C9CC7F0397F490C5D5F81D633F4D16F65A1DB205934DC3EE788E`。
- 同一源码冻结：`AutoTest/artifacts/night-shift-20260920/reports/architecture-b14-r1-freeze.json`，2,969 个文件；SHA `A8E4575678FE939281A7D6F7FDD49BF756DCDD05DE6B27121F039FCACB2E8AD8`。
- 地图：`(4)生与死v1.28读档bug修复.w3x`，62,290,145 B / `548101C395F30853D9B117BFAF85258329EE528F26488F9C94878350218F968F`。
- 非交互隔离桌面、2560×1440、零 global input；同一20秒起始、6×10秒移动、80秒返回窗口；384 MiB不变。仅P2a范围开关及run/scenario不同。
- 重型帧图像/输入取证关闭，轻量页census开启；使用既有三槽异步单张截图，不是全程录像。
- 测试目标为 `E:/Work/Warcraft III`；`E:/Work/War3`保护现场未部署。未提交、合并或发布。

## 事务与取证

| 运行 | PID | wrapper / 原生进程退出 | 运行产物 |
| --- | ---: | --- | --- |
| OFF03 | 37788 | 0 / 自然0 | `AutoTest/artifacts/night_stage11_census_off_20260920_03/` |
| ON01 | 34868 | 0 / 自然0 | `AutoTest/artifacts/night_stage11_census_on_20260920_01/` |

两轮均完成路线、9份HTML和4张BMP；`transactionOk/restoreOk/settlementOk=true`、零新增dump/GPU事件。原始hProcess/创建时间/路径一致，未按句柄终止、未发PID终止命令；最终原生句柄、隔离桌面、视频模式完成结算。

父任务复核恢复现场：36,359,521 B / `BF938FBB6FBBB0B84A64EFE3A3E32545AAB98402D17180D2BDD6CA466A460CB3`。保护现场：36,271,456 B / `A0A51AF2BB9091B2C677DC34BEBDF76C02347049A89D0BB2AEF4C37518E52134`。两轮结束后均核零游戏/编译相关进程。游戏资源已释放。

视觉只审查OFF normal-start/pressure-2/relief-end及ON pressure-2/relief-end，未重复打开旧图。相机移动/返回可见，但中央难度选择对话框仍在，不能称选完难度后的正常玩法、全场景图像恢复或玩家前台接受。

## OFF03：真实页账户

下表cache列是持有slice **capacity的区间并集**，不是有用顶点字节，也不表示全部可见对象。

| cut / 业务帧 | 活动页MiB | cache区间MiB | used中无cache引用MiB | 未分配尾部MiB |
| --- | ---: | ---: | ---: | ---: |
| stage0 / 10034 | 176 | 170.483 | 0 | 5.517 |
| stage1 / 13198 | 384 | 186.006 | 186.909 | 11.085 |
| stage2 / 13318 | 384 | 172.486 | 200.429 | 11.085 |
| stage3 / 18758 | 384 | 171.985 | 200.930 | 11.085 |

stage3在reliefEnd18742之后/退出窗口，不是压力解除后的有效恢复证据。

每cut满足used+tail=capacity、cache并集+无cache引用used=used。4个16 MiB页（id6/10/33/3）分别仅余约0.500/0.501/0.502/1.000 MiB静态标签引用，合计2.503 MiB却仍占64 MiB活动页容量。此为分配布局/保留寿命问题的实据，**不是**允许覆写页洞或立即释放GPU内存的权限：census仍标`physicalBackingComplete=false/gpuCompletionKnown=false`。

原始来源及逐页表见`reports/memory-b17.md`及`memory-b17/off03-summary.json`（均位于night-shift-20260920目录）。不能把NotStatic标签当已证明动态，也不能把Touched（尝试或访问）当实际可见/绘制。

## 阴影拒绝如何放大成整图停更

OFF业务帧13190..13197：terrainDoodad+S1 prepared=561+257，级联绘制非零，图序号3156..3163。
13198首个容量拒绝cut：活动容量384 MiB；prepared与8项terrain级联绘制归零，图序号3164。后续返回阶段图序号最大值仍3164。

源码链为：snapshot `ResidentCapacity` → 必需caster遗漏记录 → producer completeness拒绝整份候选 → shadowMap在clear/draw前返回 → 最多8帧同代旧图保留 → 无完整图/零强度。06:40:49报告631个incomplete累计=8个有限保留+623个no-complete/zero-strength。stage10局部captureAccepted仍高不代表其他合法caster或整份候选完整。

不能延长旧图保留、少画caster、放宽身份或提高预算来认证修复。精确首个遗漏对象/流类型仍未记录，不把聚合计数逐帧摊派。

## ON01与工具修订

- ON仍达到384 MiB，返回窗口图序号最大/首/末均2990；P2a两轮`uploadRangeHits=0`，无范围快路覆盖/收益证明。
- 冻结one-run及pair分析均exit1；OFF返回尾106帧、ON尾177帧没有HTML覆盖，不忽略。
- 已发现冻结读方两个问题：0占位会重置fresh high-water，从而把旧序号算成新图；pair只使用最后HTML，退出后的报告会使阶段对比误报missing。原始读方输出保留不改；后续修订版结果必须另存。
- OFF冻结fresh12、ONfresh1均不是真实新高。原读方仍因serial-decrease等失败，未误认证为PASS。

## 小范围源码收尾与未完成项

已独立确认成功no-UV捕获可保留上次独立UV页引用，当前消费者因format/stride无效不使用它。最小候选在本次捕获成功后解除无用UV引用/证明，保留位置、索引、有效UV重用和排队命令各自Rc；不直接释放GPU或覆写页。它尚无实际节省字节/压力恢复证明。

分页寿命分离候选须默认关闭、同一共享预算、同类尾部优先、不能因分类而放弃原分配器可用的合法尾部。真实页出生代际、创建失败回退和退役规则必须先闭合，不能直接把尚未接线的纯策略当已实现。

截至本记录：预算/阴影恢复发布门未过，v1.22不合并发布；x64、Water与自动模型灯继续延期。下一候选的编译/实机证据另记，不能继承8B4021的运行身份。
