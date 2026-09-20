# v1.22 内存恢复：P0/P1a 首批离线 checkpoint

状态：**离线候选，未部署、未实机、未提交、非稳定版**。没有增大 384 MiB 上限、关闭 caster 或放宽完整性门。
本记录只覆盖以下差异；不能把全部既有 dirty 工作与历史测试认作本轮成果。

## 1. 本批变更

- `war3_perf_monitor.cpp`：仅删除同一 `shadowBudgetSummary` 中第二组 Cutout/AlphaBlend emitter，第一组及 aggregation 保留。严格解析器不变，旧无效报告保持无效。
- `war3_frame_recorder_profile.h`：PRE/POST 覆盖只能缩小**所选 profile**；内部 pre 96 不再能扩到 256，post 4 不再能扩到 16。非法、部分数字及溢出整体拒绝并保留所选值。两档默认不变，没有增加新开关。
- `d3d9_device.cpp/.h`：捕获尝试与成功寿命分离。共享栈上 transaction 在改写 entry 前撤销资格；任何 break/异常离开都记账但不刷新成功帧；最终完整 backing 且 GPU-skin settlement 结束后才提交成功帧并激活 full-key ledger。
- `war3_draw_time_snapshot_lifetime.h`：共享计量及 position/UV-alias 解除操作。GPU-skin 旧租约替换/settlement 失败两点同时解除 Page、pin、offset、buffer、info、capacity，保留独立 IB/UV 的重试策略。
- 新生产组件测试、源接线门及 producer emitter 测试；旧 active-ledger / exact-index 两项静态门迁移到共享 helper，保留原始资格与清理要求并补 Page/pin/offset 约束。

## 2. 所有权与记账合同

| 字段/资源 | 本批责任与边界 |
| --- | --- |
| `position/index/uvBuffer`、allocation pin | entry 持有的缓存引用；CS closure / DXVK 使用跟踪仍有各自持有者，本批不修改其释放时机 |
| SnapshotPage、offset、capacity | 关联缓存切片；解除 position 时同时解除 UV alias，不能只清 Buffer 而留下 Page |
| 独立 UV / IB | position-only 解除时保留；indexed→nonindexed 的既有 IB 保留策略不变，保留容量继续入账 |
| `ownedGpuBytes` | position + index + 非 alias UV 的逻辑容量；**不是**整页 resident、实际在用几何量或物理显存；失败保留也计入 |
| `lastAttemptFrameSerial` | 最近进入捕获事务的尝试，诊断字段，不授予发布资格 |
| `frameSerial` / `lastAccessFrameSerial` | 完整捕获或既有成功复用才刷新；失败重试不再使旧 backing 永久保持最近成功状态 |
| `captureComplete` / identity / proof | 开始改写前撤销；失败保持无效；未改 key、代际、范围/材质门或成功复用判据 |
| 页回收与 GPU 完成 | 未改整页回收、fence、预算、分配器；不 rewind 页，不允许凭 Page 无 owner 重用 GPU 在途区间 |

事务仅在所有者路径栈上保存 entry 引用、帧号和 bool，无额外容器、锁或 heap 分配。
这是源码结构性质；关闭取证后的主线程/GPU p95/p99 成本尚未实测。

## 3. 本轮实际验证

- exact DLL：Below Normal 父进程、`ninja -C build32 -j2 src/d3d9/d3d9.dll` exit 0；因添加测试目标触发 Meson regeneration，执行依赖重新构建。没有 clean/default/all 或部署。
- exact target `ninja -n`：no work。
- PE `pei-i386` / `i386`，36,359,521 B，SHA-256 `BF938FBB6FBBB0B84A64EFE3A3E32545AAB98402D17180D2BDD6CA466A460CB3`。
- 逐项构建现有 Meson 测试可执行目标（Below Normal / -j2），随后 `meson test --no-rebuild -j2`：**87/87**。
- 新 snapshot-lifetime 生产模板测试 **617 assertions**：100 次 position 成功/IB 失败不刷新成功寿命、失败占用记账、恢复、异常、alias 去重/解除、独立 UV/IB 保留、其他引用仍存活。轻量 Entry/ref-owner 夹具不是完整 D3D9 entry 或 GPU 实测。
- Recorder defaults 的真实 getenv 适配与两种构建默认共 10 个用例均在上述 Meson 中运行，覆盖 0..300、非法/溢出/尾随字符和所选 profile 边界。
- 目录所有 `test_*_static.py`：首轮 **263/265**（两项仍锁旧代码形状），迁移合同后完整重跑 **265/265**。不是全部 Python 测试、更不是实机回归。
- `test_perf_canonical_ready_schema.py` **4/4**：编译真实 emitter 片段并严格解析零/非零/最大值及重复键反例；另按完整 budget 对象边界检查 literal emitter 唯一。
- 上项**不是整个 `generateJsonDataFromSnapshot` TU 的完整导出往返**。P0 的新实机完整报告 root 验收仍待运行，不以片段测试冒充完成。
- 新/修改四份 Python 测试 py_compile 与 git diff --check 通过。
- 内存内源变异 7/7 被拒（提前成功帧、无条件激活、漏失败资格撤销/记账、漏 Page/pin 解除、漏完整性门）；两次原样对照通过，未改工作树源文件。

日志/原文件快照位于 ignored `AutoTest/artifacts/v122-memory-recovery-20260919/`。旧审核 ZIP、原始坏帧和玩家 DLL 不修改。
对审核包 manifest 的 1,963 个 src/subprojects/render_host 既有文件逐 SHA 复核：仅本批五个预期源码/构建文本不同，其余 1,958 个字节不变；新 helper 单列，不混入旧清单分母。
收尾 git status（`--untracked-files=all`）860→867：新增五个源/测试/记录路径，另两份此前 clean 的静态门本轮修改；没有 Git 提交。
本构建仍是当前内部诊断配置，不是正式 release 配置；版本号没有因本次候选改动而晋升。

## 4. 结论上限与下一步

已修正可由源码和 CPU 测试确定的失败寿命/记账/解除缺陷；**没有证明它们在主样本占多少页，也没有证明阴影恢复**。
GPU-skin 条件分支缺陷不能直接解释主样本 GPU-skin 关闭时的失败。

1. P1b：默认关闭、总新增 x86 元数据/出口 ≤4 MiB 的页/切片 census，分开 active/retired、cache/CS/GPU owner、范围并集/tail/未知；不为诊断额外持有资源。
2. P2：在 CPU 权威上传边界取得源身份/内容代际绑定的范围摘要，解决小 draw 冻结整个大 VB；不在每 draw 扫非缓存映射，不引入 GPU→CPU 等待。
3. P3：依据 census 决定混合寿命、失败保留与碎片回收，不能把新增 GC 或提高上限视作既定答案。
4. P4：冻结新组合后独立申请隔离压力→解除→返回原地点、完整性/像素恢复、VA/显存/成本门。当前没有启动游戏、没有改玩家现场。

v1.22 排除所有 64 位产品接入、未完成 Water 与自动原生模型点光；原显式作者 API 不因此删除。根稳定 CHANGELOG 不动。

## 5. DSH 机械复核与主线程裁定

- 按 DSH skill 沿用用户指定 `commandcode / deepseek-v4.1-flash`，独立只读检查捕获区间、出口、settlement 顺序及九个相关 Python 静态脚本。未发现本批接线的确定性反例；它未获准编译，故其“未跑 C++”不否定主线程的真实编译/测试日志。
- 采纳：补录既有 PRE/POST 内部 knob；计划加实施前向链接；将发布草稿原生模型灯的“历史默认开启”与后续 opt-in 区分，旧研究验收不混入本版；新增测试文件 SHA 清单。
- 原报告把它先前跑到的旧静态失败作为最终 265/265 不可信的反例，主线程不采纳此推断。真实文件 mtime 为 19:13:25 / 19:14:36（UTC+8），首个完整修订后结果 19:16:55；报告的 19:18:52 mtime 不正确。另行按 SHA 前后校验重跑以留下更强回执，不追认首轮失败。
- SHA 绑定重跑于 19:29:26 完成：265/265、全部测试文件前后 SHA 不变；清单 `static-suite-manifest.json` SHA `B8A6C012A7A4C4631529FE5057A7893BD402B3F747B0984704D7D5EE85194CFB`。DSH turn 已 completed、session idle，末次 DLL 复核仍 BF938FBB，相关编译/游戏进程为零。
- `lastAttemptFrameSerial` 当前尚无导出读者，P1b 要接入 census；本批只完成内部寿命分离，未关闭 P1 全部目标。真实 CS/GPU 在途使用及完整报告 root 仍未验收。
- 工具纪律偏差：DSH 无 apply_patch 却用 Set-Content 写入唯一授权的 ignored 报告，主线程已要求停止文件写入、仅最终文字回传。源码未被其改动；后续委派先核编辑工具，没有指定工具就返回补丁建议，不改用其他写法。
