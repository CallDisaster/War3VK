# 2026-09-19：内存与64位宿主独立研究包 checkpoint

范围：用户授权整理源码、证据和研究提示词，交给独立研究模型。未修改C++/Shader/运行配置，未构建、部署、启动游戏或上传文件；根稳定CHANGELOG不变。

工作树：B / codex/v1.22-release-integration-20260914，HEAD ae890542d766470d1703f5bea7f5b73636039733。本轮前dirty857；除本文件与DEVELOPMENT_CHANGELOG以外既有dirty内容须SHA不变，打包工具强制检查。

待交付文件：`C:/Users/Administrator/Desktop/WarVK-memory-x64-review-20260919-r2.zip`。
可编辑整理目录：`E:/Work/WarVK-memory-x64-audit-20260919-r1/`；最终size/SHA/校验结果以该目录 `delivery_receipt.json` 为准，未生成回执前不得称交付完成。

收尾回执现已生成：ZIP 68,190,330 bytes / SHA-256 `CE7204CBB0727BE38231838A43EADA96D5350076785D865A6941535380D43165`；3300文件（含3254份工作树源文本/文档），ZIP CRC与每个成员SHA全通过。独立法证提取断言通过，但性能HTML严格根仍失败（两个重复key各值0/0），不是运行门通过。git diff --check exit0，dirty858，既有内容除获批两文档外不变。外层ZIP身份不回写压缩包内部，以免循环改变其SHA；此收尾只追加在本地开发记录。

## 审核入口与重要更正

包内00_READ_FIRST→01_RESEARCH_PROMPT→02_FACTS_AND_CORRECTIONS→derived证据→03_SOURCE_MAP。完整原交接仍保留；更正以原始事件和源码为据，不覆盖旧证据。

- 主样本PID13980/15:41：1231条snapshot-alloc/v1全部ResidentCapacity；4548条是更早PID41360/15:20。
- CPU事件64060起prepared/draw=0且拒绝31 ProducerIncomplete；计划replay数不能当绘制成功。64059最后正常serial4954，旧complete map只保留8帧，64068失效；报告business帧域另列，不强行等号合并。
- prepared=0不证明静态几何在replay列表前被丢弃；校验可在prepare之前拒绝全帧。聚合max=1不等于每帧有效。
- 384MiB是应用页池上限；used是页分配游标，不是live切片字节。整页回收/混合持有存在放大机制，但实际各页持有者/空洞占比未采齐，不能声称最终根因已定量定位。
- E2 x86 active约5.11MB是开销不是节省量；host有98MiB历史，仍为CPU lab，不能声称游戏节省98MiB或已实现渲染卸载。

## 冻结和检查

首轮整理的严格JSON检查发现15:41性能HTML仍含重复key，首项semanticSceneCanonicalReadyCutoutCount；因此停止r1，不产生成功回执。r2保留严格根拒收结论，附逐对保留的duplicate审计，省略歧义键后仅法证提取无歧义workload/meta，不last-wins、不追认运行通过。CPU事件仍走严格解析。原始报告与生产parser未改，producer schema问题加入研究提示词。

打包工具以CreateNew写新输出；源文本、原始日志/inputs/manifest/少量原图均原字节复制，记录路径、size与SHA。附原始report/CPU/raw inputs、两轮分目录证据、E2回执、当前源码/Shader/测试/逆向说明、build元数据与tracked rendering diff。

包含提取器与全文件manifest校验器；独立提取断言、ZIP CRC与每个成员SHA检查由交付脚本执行，失败则不产成功回执。没有重新运行项目回归，既有测试日志明确标为历史证据。

不含游戏程序、MPQ/地图/模型、UE受限源码、驱动、内存dump、DLL/EXE、Git数据库或完整大视频。提示词要求区分直接故障链与底层分配根因，并分别产出短期回收修复和长期64位进程边界/协议/兼容/验收路线，不能以漏阴影或抬上限宣称修复。
