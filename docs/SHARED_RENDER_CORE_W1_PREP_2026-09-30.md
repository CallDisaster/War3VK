# WarVK 共享核心 W0/W1：A 部分准备回执

日期：2026-09-30。**三份方案已完成；代码、工具实施及 W1 数据/数值验收未完成。B 未开始，等待作者通知。**

## 1. 依据与已经确定的方向

- 本次工单：[PROMPT_SHARED_CORE_W0W1_WARVK_20260930.md](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/PROMPT_SHARED_CORE_W0W1_WARVK_20260930.md)。
- 已读 [Claude 可行性复审](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/claude-outbox/0930-warvk-shared-core-feasibility.md) 及 [FORGE 对应 W0/W1 工单](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/PROMPT_SHARED_CORE_W0W1_FORGE_20260930.md)。
- 前置报告：[共享核心可行性](SHARED_RENDER_CORE_FEASIBILITY_2026-09-30.md)；长期边界：[渐进式架构收敛](plan/2026-09-15-incremental-architecture-convergence.md)。

作者已决定：共享核心 GPLv3+作者保留另行授权权利，资产/姿态/材质/蒙皮规则共用，游戏 Vulkan、编辑器默认 D3D11，War3RenderCore 为唯一上游。前置报告中的旧授权备选不再作为待决策项；具体文件版权来源核验仍由上游逐项完成，不把方向授权写成来源审计完成。

## 2. A 部分交付索引

| 方案 | 文件 | 核心结论 |
| --- | --- | --- |
| A1 数据映射 | [Footman 映射](plan/2026-09-30-shared-core-footman-data-mapping.md) | runtime geoset 序号/组号不是 MDX 原序号；通过全流与具名 remap 建立对应。优先保存 native World，不在 W 组合关系未证时求逆 |
| A2 导出与比对 | [帧输入格式](plan/2026-09-30-shared-core-native-frame-export-contract.md) | 一个实例/一次目标消费；f32 原位模式、独立帧域/渠道/来源、严格格式与缺口；区分跨编译器同输入、外部交接、原版语义对照 |
| A3 宿主边界 | [WarVK adapter](plan/2026-09-30-shared-core-warvk-host-adapter.md) | 原生事实→唯一验证/签发边界→公开核心；小 C++20 桥而非整个 WarVK 升级；CPU 数据寿命与 GPU 最后使用分开 |

本轮按“先做 A 的三份方案”完成 **文档层**。A2 包含 parser/comparator 的输入输出、严格门禁、首差异定位与正反例设计，但未新增可执行工具，不能把它写成已跑通格式/往返。工具实施应在上游 golden/格式确认后统一推进，避免产生第三套协议。

## 3. 当前源码核对得到的关键事实

1. ModelInstanceRecord 提供多条实例关联与观察帧；ShadowGeosetResourceRecord 提供已拥有的几何/组数据与不可变代际。它们没有自动给出 MDX 内容身份或完整原生对象生命周期。
2. 资源缓存将数组 i 写入 geosetIndex，原文件顺序尚需证明。原生 group dedup/remap 已有 RVA/保存证据，不能直接用 objectId 或原组号索引 runtime palette。
3. 原生 48B group kernel 公式及不 normalize 法线已有 ASM 依据。World→ModelLocal 需要 **该实例该部件当次消费的 W** 与组合见证；CPU-skinned draw 的 D3DTS_WORLD identity 不是这个 W 的证明。
4. 当前 PoseRecord 的 sequence/time 不能当可靠动作时钟；旧共享 ExternalGroups 仍检查 sequenceIndex、要求 ModelLocal 并求 GEOA。这与作者批准的“直接接受已求值矩阵/alpha/RGB”的新合同不同。
5. 当前 canonical/draw-time alpha 状态不足以替代当前 GEOA alpha/RGB，渠道来源待定位。缺失必须拒绝完整共享输入，不能补 1。
6. native snapshot override 会清空 inputSkinSelection。采集必须记最终实际来源，不能把早先尝试过的 semantic palette 当实际消费。
7. Claude 补充的依赖 `MdxNormalPreview.h` 已在当前 CompiledModel.cpp include 中核实，列入上游依赖闭包请求。

### 已读的 FORGE Footman 基线

回执：[step3/w1-footman-20260929-020716/report.json](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/step3/w1-footman-20260929-020716/report.json)，本次重新计算 SHA-256：

`879B45A75FBE2292FC5298B0E66D8BBE1F8EB44B559B3942F85024ED64242F20`

它记录 MDX SHA `7EE25577…13FC13`、Stand 500、Walk 250→283、重复 283，quality=PreviewApproximate。此次未打开图片、未重跑旧 WARP 测试；没有把回执中的图像/矩阵摘要当作完整新 golden 数值数组或原版真值。

## 4. 请 FORGE/共享上游配合的内容

| 项目 | 所需交付 | 如果缺失 |
| --- | --- | --- |
| 唯一上游与来源闭包 | 仓库固定提交/干净状态、依赖清单与来源/许可；含 MdxNormalPreview、MDX/内部头、Result 所需部分 | 不复制 FORGE 源码，也不编造闭包 |
| 已求值外部输入 | 公开 World/ModelLocal、组矩阵及实际 alpha/RGB；不要求动作时钟、不重算渠道；严格数量/版本/空间检查 | WarVK 不伪造 sequenceIndex 来绕过旧 API |
| Footman golden | 原 geoset 顺序/有序组成员/数据摘要、完整矩阵/alpha/RGB、生成配置/编译器/quality | 只有截图/hash不能完成同输入数值对照 |
| 资产视图规格 | 位置/法线/索引/UV/组/材质层/来源的公开读取规格与缺项 | 不读 ModelData 私有字段、不临时拼完整 renderer |
| 格式会签 | A2 容器/schema、摘要字节域、空间、渠道语义、正常/错误夹具 | 两边先按同一规格定稿，再写工具 |

FORGE/上游新接口的实现归属不转移给本轮 WarVK；本回执只是明确调用方要求。WarVK 自己仍欠原生挂载源、实例/部件绑定、空间组合及 GEOA 数值来源的运行证据。

## 5. B 与后续实跑状态

| 工作 | 本轮状态 | 启动条件 |
| --- | --- | --- |
| i686 MinGW C++20 独立静态核心构建与自带测试 | **未开始** | 作者通知共享仓库就绪，提供固定提交/依赖/接口 |
| MSVC/MinGW 同输入 golden 比较 | **未开始** | 固定输入与 MSVC 原始输出，B 构建成功；记录逐位/首差异而非预设一致 |
| parser/comparator/adapter 源码 | **未实施** | 会签格式/公开接口后按明确范围实施，仍不复制上游源码 |
| 游戏采集、部署、画面替换 | **未进行** | 作者在场且另行安排；本轮不继承以前的实机授权 |
| W2 共享绘制、W4/W5 takeover | **未开始** | W1 对齐与各后续 GPU/视觉/性能门，另行范围 |

即使本机上游目录已被另一会话创建，本轮也不自动启动 B。共享库编译成功不等于模型/动作/阴影/材质等价，更不等于能立即减少原生 CPU 工作。

## 6. 需要作者或两边实施者确认的剩余事项

1. 由 FORGE 接受或修订 A2 的共同格式和渠道/空间语义；确认首轮 golden 数据包含 A1 所需对应表。
2. 后续 **原生采集** 的在场时间、目标实例与预算/恢复边界。现在不请求立刻启动游戏。
3. 若希望在上游会签前先写纯离线工具，可另行给出该实施范围；当前不把方案设计冒充工具实现。

授权、后端方向、唯一上游三项已确定，无需作者重复决定。缺运行证据不是方向被否决；应先做能独立验证的对应/接口工作，不通过猜时钟、放宽解析或全拒绝 caster 推进。

## 7. 本轮范围与复核

读源码时 WarVK HEAD=`a2054a9776e40edc3f01f6f390c06037965b8ef2`，分支 `codex/v1.22-main-tree-20260920`，原有 89 个 dirty 路径；FORGE 当前读到 HEAD=`d2c27ec6a26b41e4965f3d1df80851ea5f6f1cb3`，它仍有并行工作，HEAD 不代表其全部读到的工作树内容。本文件是源码观察 checkpoint，不是冻结候选源码清单。

仅新增本回执和 A1/A2/A3 三份方案，按项目规则在开发日志追加本轮记录。复核结果：status **89→93**，正好新增这 4 个文档路径；原有 88 个可直接 hash 的文件中，除声明的开发日志外 **87/87 SHA-256 不变**，无原路径移除，HEAD/分支、StormBreaker HEAD/status 不变。开发日志移除本轮新增段后重新计算 SHA 与开工前相同，证明旧内容逐字节保留。旧 dirty 源码、测试、构建/版本配置及前置可行性报告未改。

4 个新文档的本地链接全部可解析、围栏配对、无行末空白；`git diff --check` exit 0。以上仅是文档/工作树检查，不以历史测试数量作为本次验证，不意味着运行/数值验收。

没有修改 C++/Shader/Meson/runner/API；没有复制 FORGE 源码或创建共享仓库；没有调用 IDA、执行 IDA 脚本、运行 Ninja/编译/测试/游戏、部署、打包、提交、推送或公开。**本轮不形成新 DLL。**
