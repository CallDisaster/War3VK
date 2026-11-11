# W1-A2：某帧、某实例离线导出与比对格式草案

状态：2026-09-30 **格式/工具设计，不是已实现的 exporter/parser**。本轮仅完成用户要求的三份方案；未新增工具源码、未采集游戏数据。上游黄金样本与外部已求值输入合同确定后，再统一实现解析/比对工具，避免两边先长出不兼容格式。总回执见 [W1 准备](../SHARED_RENDER_CORE_W1_PREP_2026-09-30.md)。

## 1. 格式要解决什么

把 **一个实际实例的一次目标消费** 保存成可独立检查的输入包：原始组矩阵、alpha/RGB、原 geoset 对应、实例/部件/资源身份、帧域与空间证明。一个样本不跨两个实例、不把“最后一次观察到的各字段”拼成一帧。

两类生产者共用容器，来源语义不能混用：

- `ForgeComputed`：固定动作/时钟，经共享核心求值的离线黄金样本。它是编译器/规则基线，现阶段不是原版真值。
- `WarVKNativeObserved`：当次原生消费的已求值数据。动作未知允许如实 unknown；不能编造 sequenceIndex=0 让旧 ExternalGroups 接口通过。

再区分三种实验：**同输入跨编译器**、**外部输入无修改交接**、**原版与 FORGE 语义对照**。前两项成功不能替代第三项。A1 的 [数据映射证明](2026-09-30-shared-core-footman-data-mapping.md) 是第三项的前置。

## 2. 容器与数值编码（拟定 v1）

```text
<CreateNew 样本目录>/
  manifest.json       精确 schema、身份、范围、来源、缺口、文件摘要
  inputs.bin          16B 文件头 + 明确排列的输入数值块
  geoset-map.json     原 geoset/runtime geoset/组 remap 与证明范围
  comparison.json     离线工具另行 CreateNew 生成；不属于原始输入
```

不要求图片，也不在这个格式中混入原 MDX/BLP、整张地图、GPU 资源或 C++ 对象。作者本机模型用于解析/复算；公开测试只包含许可允许的计算数值与摘要。原始包生成后只读，任何再分析结果必须另起文件，不能回填原 manifest。

`inputs.bin` 拟定布局：

| 字段 | 长度 | 约定 |
| --- | --- | --- |
| magic | 8B | ASCII `W3FRAME1` |
| containerVersion | u16 | little-endian，1 |
| flags | u16 | 0；未知位拒绝 |
| payloadBytes | u32 | little-endian；必须等于文件大小−16 |
| payload | 明确长度 | 各块紧密排列；无对象 padding，无隐式附加尾部 |

块元数据 `offsetBytes` 从 **文件 payload 首字节**（文件偏移 16）计，不从各数组或结构体起点计。`elementType` 仅接受明示的 `f32-le/u32-le/u16-le/u8`；`count × elementBytes` 必须等于 `sizeBytes`，做溢出安全核验。每块 SHA-256 覆盖该块原始字节；文件 SHA 覆盖头和 payload。不同语义块不重叠，全部 payload 字节必须恰被已声明块覆盖。

所有 float 保留 IEEE-754 f32 **原位模式**。不经 JSON 小数往返，不把 −0 改为 +0，不对原始输入 clamp/normalize/transpose。解析出的非有限值允许在失败诊断中定位，但完整可用输入必须拒绝 NaN/Inf。

矩阵布局只以公式定义，不以宿主类名定义：

- `BasisColumns3x4`：12 个 f32，`m[column*3+row]`；四列分别是 X/Y/Z 基与平移，隐含底行 0,0,0,1，48B。
- 原始 D3D9/DXVK 4×4 见证可另存 `Native4x4`，必须附原数组布局和乘法公式；未知布局不自动转成上项。
- raw world、候选 instance W、派生 local **不同块**；派生块引用输入块 SHA 和转换器版本。保留未转换值以复查组合关系。

## 3. Manifest 的必备字段

顶层拟定精确键集：`schema,producer,asset,sample,geosets,blocks,files,coverage`。schema 为 `war3-shared-frame/1`；字段扩张须显式升版本/协商，不采用 last-key-wins 或“只读某几个 sections”的宽松解析。

64 位序号/代际用固定 16 位小写十六进制字符串（无 `0x`），地址观察值用固定 8 位十六进制字符串；32 位数量用 JSON 整数。身份不是数值强转地址。每项未知用 `{known:false,value:null,reason:<具名码>}`，已知零用 `{known:true,value:<编码零>}`，不得混用。可选见证另列 `applicable`；NotUsedBySelectedRoute 与“应取得但未知”不同，必需输入不能借不适用逃过验证。

| 对象 | 必备内容 | 关键限制 |
| --- | --- | --- |
| producer | role、toolVersion、sourceCommit、sourceTreeManifestSha256、targetABI、compiler/FP 选项；DLL/Game.dll/map 的 size/SHA（不适用或未知分列） | dirty 树不能只写 HEAD；源清单与二进制各自记录，版本号不是源身份 |
| asset | normalizedVirtualPath、sourceLayer、mdx size/SHA、asset ID/generation、WarVK immutable generation | MDX 摘要与 runtime geometry/FNV 各为不同域；未核真实覆盖层不能给 SourceVerified |
| sample.identity | runNonce、session/map/device epoch、实例 token 与代际、runtimeModel/part/meshPayload 观察值、jHandle/rawcode、identityStrength | token 为宿主会话关联号，不自动代表原生对象生命周期；跨进程不按指针 join |
| sample.frames | renderFrame、presentFrame、nativePaletteFrameTag、writerInvocation/captureSerial、publicationTicket、manifestFrame/revision、recordFrame、QPC/tickDomain、ownerThread | 各域分别 known/unknown；不以一个 frame 填满四项；32 位 native 标签不得直接比较 64 位 frame |
| sample.animation | 实际动作序号、sequenceFrame/elapsed、global clocks、camera（含空间）、各自来源 | 非必须时可 unknown，但不能据此认证与固定时间黄金样本为同一姿态；global 不能凭系统运行时间构造 |
| geosets[] | 原 MDX ordinal、runtime ordinal、part/layer 关联、asset/map/immutable generation、组域/数量/remap、每项数据块引用、空间与渠道来源 | 逐原 geoset 列全表；不可见、刚体、无组、未匹配、未捕获区别标明 |
| files/blocks | 名称、大小、SHA、类型、数量、stride、offset、layout、space、worldApplied、numeric semantics | 不依赖 C++ sizeof/enum 当前编号；名称/空间使用 schema 冻结的符号值 |
| coverage | required/observed geosets、各证明级别、缺失/截断/采集上限/身份弱/未知比例、completeInput、nativeEquivalent、productAuthorized | 两个授权/等价标志初始 false；没有生产/视觉证据不能抬升 |

`sample.frames.manifestFrame/revision` 当前采集点未必可得；选中来源依赖 semantic manifest 证明时，unknown 只能成为 **不完整样本**。纯 native writer 路线若根本不消费该 manifest，标 `applicable=false/NotUsedBySelectedRoute`，不虚构一条依赖，也不将其填成 renderFrame。当前实例/part、采集与实际消费绑定、active-map/资源代际、数据空间和必需渠道仍必须证明。`nativePaletteFrameTag=0` 是否有效须由原生标签合同判断，不能由值恰好为 0 推定 unknown。

### 3.1 geoset-map.json

精确根键拟定为 `schema,assetDigest,entries,coverage`，schema=`war3-geoset-map/1`。entries 按 MDX 原序号，含：

- 原/runtime geoset 序号及 `Exact/Remapped/Ambiguous/Unsupported` 状态。
- 各几何流摘要及域定义、vertexGroup 原/runtime 摘要。
- 原组有序成员摘要，原组→runtime 组的显式 map；多对一允许但必须有相同成员序列见证。
- 原节点 objectId→runtime pose-slot 映射状态；未证明时不能用 resolved pose 数组重建混合组。
- 映射适用的资源代际、地图和来源；链接的原始 capture/消费身份。

摘要字节域固定为：算法名 `sha256`、域标签 ASCII+NUL、schema u32-le、元素类型、元素计数与原序元素字节。组成员另编码组数、每组成员数和有序成员；不包含运行时指针。域标签及具体编码应由上游批准后形成共同规格，两边不得各自定义同名不同算法。

## 4. 组矩阵、alpha/RGB 与实际消费者

每个 geoset 单独引用：

- `groupMatricesRaw`：实际 writer/消费绑定的组数组，domain=`VertexGroups/RuntimeRemappedGroups`，space=`World/ModelLocal`，stride=48，是否已乘 world 必填。
- `resolvedPoseWitness`：可选辅助数组，domain=`NativeResolvedPoseSlots`，不得冒充上述组数组。
- `instanceWorldWitness` 与可选 `groupMatricesDerivedLocal`：来源/帧/转换链独立。
- `geoaAlpha`：每原 geoset 1 个 f32；`geoaRgb`：每原 geoset 3 个 f32、RGB 顺序，明确为已求值的**绝对 GEOA 数值**。
- 可选 `layerAlpha/instanceModulation/finalDrawModulation`：语义单列，不能以 alphaRef 或最终混合乘数填 geoaAlpha。

数量按共享合同精确核验，不以“至少够读前 N 项”充当完整输入。不支持 alpha/RGB 默认值填充；只有证明资产本就无该动画、静态值由唯一资产规则给出时，才以该规则具名输出，而不是采集缺失时补 1。

采集点须锁定 **最终实际被用的来源**。现有 [device 路径](../../src/d3d9/d3d9_device.cpp) 21717/21762 会采用 native snapshot 的 capturedWorldMatrix 并清空 inputSkinSelection。遇到这种 draw，记录 `semanticPaletteNotConsumed`；早先尝试过的 selectedPalette 只能是辅助观察，不能称实际消费输入。候选入队、记录绘制命令、GPU 执行、最终像素各有证据级别。

本格式不把既有 palette-object/v1 事件扩成第二套 wire；manifest 引用既有事件 session/sequence 可作关联，但自身是新的 **离线样本合同**，不修改既有记录器解析规则。

## 5. 采集所有权与预算设计（未安装）

采用作者显式选中的一实例/一目标消费，不扫描全世界，不记录全程视频。hook 原函数照常执行，在现有所有者路径上取得该次已发布数据副本；原生临时范围不能交给后台线程再读。后台只处理拥有的字节、元数据和安全的不可变资产引用。

拟定首轮诊断硬上限（须在以后实跑申请中冻结，不是本轮变更产品池预算）：

| 项目 | 上限/处理 |
| --- | --- |
| 实例 | 1 个已选实例/包；双实例实验产生两个明确样本，不能混包 |
| 原 geosets | 64；超过则拒绝完整输入认证，不静默裁掉 |
| 每原生组范围 | ≤256，且原始数量等于实际复制数量 |
| 总矩阵数 | ≤4096（含辅助/派生块），另有文件字节上限 |
| inputs payload | ≤1 MiB；捕获存储池预分配，总诊断内存另行核定 ≤2 MiB |
| manifest / map | 各 ≤128 KiB；字符串长度、entries 与成员表总量也有上限 |
| 采集窗口 | ≤8 个目标 render frames；不足即关闭并写缺口，不重试补成另一帧 |
| I/O | 所有者线程无写盘、无同步 GPU 读回、无等待后台完成；队列忙则明确拒绝 |

一个包不需要完整 VB GPU readback。若后续要对照 native kernel 的 position/normal 输出，另行在已证明可读的映射时点复制有界输出；不得因为已有 Rc 就强行 map device-local/GPU 正在写的数据。该需求不是本轮授权。

Reset、换图、设备更换、实例身份失效、重入或上限失败时，冻结已拥有的诊断字节与原因，禁止拼接旧会话。所有者线程/队列安全没有生产证明前，不能接进 DLL。写盘采用 CreateNew、不覆盖旧包；失败留下具名 incomplete 状态，不能把部分文件识别成 completed。

## 6. 严格解析与比较工具设计

拟新增工具仅是后续路径建议，本轮 **未创建/执行**：

```text
AutoTest/parse_shared_core_frame.py
AutoTest/compare_shared_core_frames.py
AutoTest/test_shared_core_frame_contract.py
```

解析器分三层：

1. **容器安全**：递归 duplicate-key 拒绝、精确根字段/版本、禁止 NaN JSON 常量、整数/字符串严格类型、文件名仅固定包内文件（拒绝绝对/.. /链接逃逸）、无尾部垃圾、大小上限和重新计算 SHA。
2. **输入结构**：所有块对齐/范围/不重叠/精确数量；序号唯一完整、remap 有界、矩阵有限、空间/阶段明确、版本一致；unknown 不作 wildcard。
3. **证据准入**：原 geoset/instance/帧/space/channel 的链是否证明。`structurallyValid=true` 可以与 `completeInput=false` 同时出现；零事件/空 geoset 表不得 all() 空通过。

比较模式与结果：

| 模式 | 前置 | 报告上限 |
| --- | --- | --- |
| same-input-cross-compiler | 同源核心固定提交、相同资产/请求/输入字节、精确配置与 FP 选项 | 输入输出逐位一致与首个差异；不是原版等价 |
| external-ingest | 同一个已求值包，两宿主通过公开新接口接收、无时间重算/隐式转换 | 组/alpha/RGB 逐位保全、空间和身份保全 |
| native-vs-forge | 同资产、映射/实例/动作时点或其他同姿态证明、空间和数值渠道对齐 | 哪些 native 语义一致/不同/未覆盖；近似告警不能消失 |

逐位比较顺序冻结：MDX 原 geoset 升序→组序号→矩阵 coefficient 0..11→alpha→RGB 0..2。首差异含两侧 f32 十六进制、解码值、abs/relative/ULP 距离、源块 offset。完整摘要保留 maxAbs/maxULP、不一致数量及每语义域统计；不能只报平均误差。

空间求逆派生值与法线不同规则不能加入逐位 pass 的同一分母。数值误差只做 **测量**；若以后需要容差接受，须在实验前按语义域另行批准，不允许边跑边扩阈值。动作时点未知则 native-vs-forge 返回 Uncovered，而不是拿“看起来相近”的 golden 行自动匹配。

拟定退出码：0=该模式冻结的证据/比较门全部通过；2=合法但缺证/不支持；3=可比输入存在差异；4=格式/身份/摘要/范围不合法。`comparison.json` 另含 `comparisonMode,structurallyValid,completeInput,bitwiseEqual,firstDifference,coverage,refusals,nativeEquivalent,productAuthorized`。末两项不由一个工具退出码自动设 true。

## 7. 离线正反例与以后实跑出口

工具实施时至少覆盖：

- FORGE 两实例 Stand/Walk 及重复时间点；同数据改 poseVersion 不应造成数值差异。
- 已求值 World/ModelLocal 明确分开；非法 worldApplied 组合、布局错、groupCount 差 1、缺 alpha/RGB、NaN/Inf、块溢出/重叠/尾部数据。
- JSON 重复根/嵌套键、重复 geoset 序号、SHA 错、模型来源错、未匹配/同数不同内容 geoset、多对一组映射、损坏 remap。
- part 相同但实例不同、指针复用、map/device epoch 更换、未知生命周期、native 标签回绕/零值的域处理；不能跨域碰巧相等即认证。
- writer 数量截断、工作限额、当前 draw 实际改用 native snapshot；未消费的语义 palette 不计为消费。
- 原生/预览法线长度不同；known-zero 与 unknown-zero；合法结构但零样本；GPU/pixel 证据缺失。
- 首差异位置与报告测量用独立夹具验证；parser 不能借 summary 自述跳过原字节检查。

后续实跑需作者在场与另行冻结：目标资产/实例、地图、候选 DLL/生效配置、采集点/窗口/内存、隔离与恢复边界。**本轮无实跑，也未借旧样本拼出新的原版输入。**

## 8. 给 FORGE/上游确认的合同点

1. 批准或修订容器 magic/schema/数值布局/摘要字节域；格式只能一处定义，生产者允许输出同语义的 sidecar 元数据。
2. Golden 输出完整矩阵/alpha/RGB/组成员，而不是只有 screenshot/hash；记录生成器/源提交/FP 选项/quality。
3. 新已求值接口保留 World 原始值并接收 alpha/RGB；不隐式求逆、不要求虚假 sequenceIndex、不重算 GEOA。
4. 明确材质数值的阶段、RGB 顺序/量化/颜色编码及原 geoset 顺序；未知阶段禁止自动转换。
5. 支持公开读取资产对应信息，不暴露/读取 ModelData 私有实现；无法导出的项列为尚缺能力。

以上是两边联调规格请求，不是 WarVK 对共享仓库的代码修改，也不是上游现有 API 已具备这些能力的声明。
