# v1.22 发布准备与本轮代码审查

状态：准备材料，不执行commit/tag/push/Release。用户明确要求解决撕裂后才实际推送。

## 本轮审查和修订

- 统一CPU sourceKey与截图服务的真实Present序号，废弃仅采集期间递增的伪全局帧号。
- screenshot prepare、CS copy记录、GPU已完成保存分开；保存继承请求session，防止跨会话串证。
- caster rawcode与geometry hash分字段保存，不做不可逆OR混合；offset/size保留uint64。
- typed kind替代把所有数据塞入Camera/Trigger；schema1/2只读兼容，当前schema3包含历史copy
  与CSM状态。完整性恒false，能力缺失不能由静态测试或数量统计晋升。
- 阴影状态采集移动到公共Run收口，不只覆盖特殊测试入口；早退保留实际本轮计数。
- ring默认关闭；arm阶段一次分配，steady图像copy不算hash/写盘/申请路径字符串。
- GPU历史使用独立device-local图像；通过已有DxvkContext copy进行依赖管理，图像队列后
  才允许读回；现有精确timeline query继续控制CPU读取。失败保留/隔离，Reset不强行等GPU。
- 控制端不得最后释放GPU资源所有者：registry在正常swapchain teardown释放；不可达
  进程卸载对象按现有退出合同由OS回收，不在loader lock等待线程。
- 实際draw压力门发现全局try-lock会丢记录；已换成独立cell并发队列、全局ticket与写者退场门。
  追圈冲突和旧ticket不可覆盖新cell；Frozen导出前须无写者；seq_cst生命周期门防止释放竞争。
  QPC只在各CPU线程内要求单调，不把跨线程ticket次序误当GPU执行次序。保留旧schema读取。
- 冻结导出按64KiB批次写，不为每个事件再建一份巨型JSON内存树。
- watcher新增无render锁`peek`，避免为了监控状态频繁争抢采集锁；仅输出初始化/终态日志。
- 原版模型自动点光 producer/consumer改为明确opt-in，避免诊断DLL继续默认启用搁置路线。

## 仍阻断正式发布

1. #8短暂多边形暗影根因/修复/玩家前台回归未通过；不得写成已修复。
2. 完整帧取证仍缺中间GPU图像、完整原始几何/纹理和draw-ID归因；初版工具不是全部完成。
3. 旧CPU try-lock在180秒实机中出现5次、draw压力中出现1158次丢失，该版本不接受完整draw取证。
   最新独立cell队列R10/R11为0丢失；R10 68帧实际draw数量闭合，但不是所有GPU原始数据已齐。
4. 新组合的前台FPS、1GiB历史分配成本、低视角Guide、长时/切图/Reset及全量JASS API门仍需
   分别核验；此前49个公开函数限定地图用例不能扩成所有API均通过。
5. 当前worktree含大量未提交的候选；尚未形成clean-base发布提交。需要逐项复核版本资源、
   SDK打包、依赖子模块身份、许可证和所有排除项，再在clean树构建重放指定门。

## 发布范围草案

- 拟纳入：无消费者native同步读回优化原理；真实异步截图与录制独立门；退出收口；
  已验证的metadata查询优化；太阳开关JASS语义与typed桥修复；已有Froxel/局部雾/Guide组合验收。
- 不纳入：未完成Water改造、原版模型自动点光/接管阴影、未验收实验优化。
- 开发工具单独目录/默认关闭：frame evidence/history；不占据普通玩家默认显存。
- 保留已知限制：单进程跨地图、随机裂缝及尚未复核的图形组合，不用“全面稳定”概括。

## 准备包结构

候选DLL+说明+本地控制/分析工具+JASS作者SDK；开发日志与待发布说明；源码审查ZIP含
当前src/include/WarVK/AutoTest/docs及构建输入、Git基线说明和逐文件SHA。排除.git、
build32、游戏二进制/MPQ/地图/用户MDX、ignored大工件和崩溃dump。
所有输出CreateNew，不覆盖旧试玩包。源码ZIP是dirty工作树快照，不是一个已提交release tag。

## 真正推送前清单

- 玩家确认对应SHA已解决撕裂；收口对应GPU/完整性/视觉回归，不转移旧candidate结论。
- clean checkout整合、全量相关static/runnable、组合DLL source-consistency及许可证清单。
- 将Meson/version.rc/JAPI显示文字和协议测试等统一为1.22.00，保留Shader API和JASS wire ABI。
- 核正式包默认关诊断/自动模型灯；SDK列明实验接口边界；生成最终manifest/SHA与回退说明。
- 用户明确推送时才执行commit/tag/push/Release；本轮仅准备，不调用GitHub写接口。
