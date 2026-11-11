# W1-A3：WarVK 宿主适配层接口与生命周期草案

状态：2026-09-30 **接口/模块方案，尚未增加产品代码或链接共享核心**。共享仓库由 FORGE 拆出，唯一上游拟为 `E:/Mycode/Source/Repos/War3RenderCore`；作者通知后再做 B。总回执见 [W1 准备](../SHARED_RENDER_CORE_W1_PREP_2026-09-30.md)。

## 1. 收敛目标与明确不做的事

宿主适配器只做一件事：把当前原生数据连同身份、空间、有效期，转换成 **公开共享输入合同认可的一份包**。不成为第二个 MDX 解析器、第二个 palette 仲裁器或第二个 GPU 回收系统。

```text
WarVK 原生生产者/现有 registry/不可变 snapshot
             │ 当次最终来源 + 数据 + 证明
             ▼
NativeFrameAdapter：绑定、域/空间/寿命验证，签发受限输入
             │ ValidatedNativeFrame（CPU 数据，不是 GPU 许可）
             ▼
C++20 CoreBridge → 固定版本 War3RenderCore 公开接口
             │ 核心回执 + 有时限的输出视图
             ▼
WarVK owner 保有的输出包 → 既有 Vulkan 上传/跟踪
                         （未来 W2/W4；本轮未接）
```

编辑器的 D3D11 安装和游戏的 Vulkan 安装仍各自负责。共享矩阵/材质语义不等于共享 VkDevice、D3D11Device、命令队列或 GPU 存储。W1 的 exporter/离线工具只是这一边界的消费者，不要求开启 GPU takeover。

本阶段不关闭原生 CPU skin、不绕过动态 VB 上传、不替换主画面、不修改阴影 caster 准入、预算或产品默认值。共享准备失败时，只拒绝新的诊断/共享输入，当前原游戏和 WarVK 原有流程继续按现行合同执行。

## 2. 模块与决策所有者

以下名称是 **以后实施的建议位置**，本轮未创建这些源码文件。

| 模块 | 建议位置 | 单一责任 | 不得承担 |
| --- | --- | --- | --- |
| NativeFrameAdapter | `src/d3d9/war3/render/war3_shared_core_native_adapter.{h,cpp}` | 宿主身份、当前消费绑定、范围/空间验证与转换，签发输入 ticket | 重新选择另一份 palette、解析 MDX、访问 GPU fence |
| CoreBridge | `src/d3d9/war3/render/shared_core/` 的小型 C++20 私有目标 | 把已验证数据送入公开上游 API，翻译状态码与拥有输出 | 读游戏裸指针、偷读 ModelData、隐式补动作/alpha/RGB |
| AssetBinding | 优先依附现有 `ShadowModelResourceCache`/模型模块；适配器持有映射视图 | 固定资产内容/原 geoset 映射，按代际失效 | 复制全套模型缓存或按 draw 全流 hash |
| SampleExport | 复用 `war3/tools/` 现有 owner/有界交接及异步写盘模式 | A2 离线包编码、CreateNew、错误与占用账 | 原生数据晚读、同步 GPU readback、另建大历史环 |
| Vulkan consumer | 既有 `war3/render/`、`gpu_skin/` 与 DXVK 资源跟踪 | 实际上传、dispatch/draw、消费者最后使用 | 用 CPU 计算回执当 GPU 完成 |

设备类未来最多提供具名的薄交接调用及实际 draw 上下文，不包含映射规则、world/local 猜测或完整共享算法。原生 hook 在原函数执行后提供事实，不由一个“Manager”转调回 device 再决定一遍。

决定采用哪份输入，沿用现有最终来源选择入口。适配器只能验证它收到的 **最终实际来源**；若 native draw-time override 已清空语义 selection，就不能以早先 selectedPalette 代替。如果覆盖变化，旧 ticket 失效，必须重新验证新输入。

## 3. 输入、凭据与回执的形状

接口名称只是草案，实际共享 DTO 由唯一上游定稿；不把下面的概念表当作已有 API 或固定二进制 ABI。

### 3.1 原始观察视图：NativeFrameObservation

由同一所有者消费点装配，至少包含：

- 目标 run/session、active map/device epoch、当前 render frame、原生 frameTag、capture/publication serial。
- 实例及 part、runtimeModel、meshPayload 的当场关联见证；原始指针只在宿主内用于核对，不能作为跨宿主 ID。
- 不可变资产 snapshot 与其 active-map/immutable generation、MDX 源身份、原 geoset 映射视图。
- 已拥有的组矩阵、准确数量/范围、最终 skin::Selection；已求值 GEOA alpha/RGB 与各自语义/来源。
- 原始/派生矩阵空间、布局、是否已应用 world、原始 W 及组合关系证明；相关最终几何域。
- 缺失/弱身份/截断/数据未实际消费的显式位与原因。

此类型不授予读取旧 slot、缓存别名或后台裸指针的权利。验证必需字段未知可以进入失败报告，不能签发有效共享输入；选中路线不使用的可选见证要显式标不适用，不为整齐而虚构 manifest/动作时钟依赖。

### 3.2 构造受限的 ValidatedNativeFrame / FrameInputTicket

只由适配器成功核验后构造，调用方不能自行填一个 `validated=true`：

| 内容 | 意义 |
| --- | --- |
| asset + instance identity/generation | 上游范围内的固定宽度身份；原生生命周期证明仍由宿主负责 |
| mapping version + original geoset/group ranges | 数据具体属于哪个原网格与组域，不用可见 draw 序号代替 |
| frame lease + source selection | 已拥有的当次 CPU 数据及精确来源，不能混用旧来源证明 |
| space/layout/worldApplied + channel semantics | 不允许消费端猜 transpose、补 alpha 或重复 world |
| map/device/evaluation generations | 不同作用域分别携带；不是一个万能 generation |
| evidence/coverage + validity | 有效输入与诊断候选分开；未知不作 wildcard |

建议接口的逻辑顺序：

```text
BindAsset(immutable snapshot, source identity, mapping) → AssetBinding 或具名拒绝
ValidateCurrentFrame(observation, frame storage) → FrameInputTicket 或拒绝
EvaluateObservedFrame(ticket, output storage) → SharedFrameReceipt 或拒绝
CopyToExport(ticket, receipt, bounded storage) → 离线样本或缺口
Invalidate(map/device/session change) → 仅失效新消费许可，既有 GPU owner 正常退休
```

`ValidateCurrentFrame` 不再选择来源；`EvaluateObservedFrame` 不重新求动作/GEOA。失败回执包含稳定枚举码、发生的层和可选字段位置，不在热路径分配动态错误字符串。

### 3.3 输出/视图的寿命

现有 FORGE [PoseBatch](E:/Mycode/Source/Repos/War3MapEditor64/include/War3RenderCore/PoseBatch.h) 的视图属于求值器。输出不能在下一次 Evaluate/重置之后仍作为未拥有的 span 借给 Vulkan 工作线程。

宿主要么在明确的消费期内同步核对，要么把选中实例输出写入预分配的自有 FrameStorage；后台/CS 消费持有该帧包的 owner。不能用“核心返回 success”延长输出视图的寿命，也不能把共享核心的 shared_ptr 当作原生对象所有权。

## 4. 32 位/语言边界，不升级整个 WarVK

当前上游公开头已有 C++20 `span`、defaulted equality；不能将其直接 include 到仍按旧标准编译的设备/Hook TU，再声称“只升了静态库”。

建议一个 **同进程、同 i686 MinGW 工具链** 的私有桥：

- WarVK 普通头只暴露 C++17 可读的句柄/定宽 POD 与显式数组范围。
- 只有 CoreBridge 实现目标按 C++20 编译，持有共享对象并 include 上游公共头。
- 必要的 C 风格私有桥入口采用固定调用约定、structSize/version、指针+数量，仅传本进程借用视图；同步调用期有效，返回后的异步使用必须变成自有帧包。
- 不在边界上传 STL、std::string、shared_ptr、异常或由另一侧任意 delete 的对象。创建/销毁配对在 owner 一侧；异常在桥内转具名失败，失败输出不可消费。
- 这是语言隔离，不是 IPC，也不是 64 位渲染进程。无需引入插件加载器、共享内存或第二套跨进程协议。

B 阶段首先只是独立目录编译核心与离线 driver；是否把这个桥链接进 DLL 属后续明确范围。FP 要保持上游 `/fp:precise` 或 MinGW `-ffp-contract=off`，不新增 fast-math，也不为“跑绿”改原生 f32 顺序。

同一组共享 DTO 由上游定义，桥代码负责显式字段转换。上述 host POD 不成为另一份 MDX/pose 语义实现。若上游可提供兼容的薄边界，则优先直接使用它，不各自维护同题 API。

## 5. 五种寿命、谁释放

| 资源/身份 | Owner | 有效期与失效条件 | 不可替代的证明 |
| --- | --- | --- | --- |
| 原生实例/part 关联 | 游戏对象，WarVK 只观察/核验 | 同次调用/明确对象生命周期；销毁/指针复用/换图失效 | jHandle/rawcode/指针相等不是永远同对象 |
| 不可变几何与资产映射 | 现有 model cache snapshot owner；适配器只持引用/映射 | 内容代际与 map/source 有效；源变化重新映射 | snapshot 不变不证明捕获来源正确 |
| 当前 pose/material 输入 | 既有原生捕获发布 owner + 预分配帧存储 | 目标实例的该次消费；单独 render/native 帧域 | 帧号相等不证明槽位归属，ticket 不授权回读 arena |
| 共享 evaluator/output | CoreBridge 单所有者 | evaluator epoch、pose version；下次 Evaluate 可能覆盖视图 | CPU success 不保活 Vulkan slice |
| 上传 slice/实际 GPU buffer | 原有 DXVK/上传 owner | 最后实际 GPU 消费完成后退休 | producer fence、引用计数或 map reset 不能替代 consumer completion |

当前工程的线程门/Present 安全点继续适用。arm/export 管道线程不能直接替渲染 owner 推进 pose 采集；跨线程交接只能是已拥有、不可变且有界的包。

换图/Reset：先阻止旧 ticket 再消费、换 CPU/map/device/evaluation 域，再由原有 GPU retirement 结算在途包。不得直接 free 正在用的 GPU 资源，也不得用释放 CPU registry 的动作冒充 GPU 已完成。没有 GPU 接入的 W1 样本，只结算自身 CPU 存储。

## 6. 失败回退合同

| 失败 | 新适配层动作 | 现有路径 |
| --- | --- | --- |
| 资产源/原 geoset 不匹配 | 不签发 ticket；导出候选与具体差异 | 正常原生渲染照常，不修改模型或 caster |
| instance/part/epoch/freshness 弱或冲突 | Identity/Frame 拒绝；不借上一帧补齐 | 仍按当前已验证生产合同决定旧路径 |
| palette 被另一个最终来源替换 | 旧 ticket 作废；按新来源重新验证或未覆盖 | 不消费早先尝试的 semantic palette |
| space/world 组合未证、奇异 W | 保留原始 World 诊断；无隐式 local 转换 | 不动 D3DTS_WORLD、不改现有 shader |
| GEOA alpha/RGB 缺失 | 不填默认，报告渠道缺口 | 不关闭物体，不借 alphaRef 冒充 |
| capacity/输出/核心失败 | 整个选中样本失败、丢弃新输出、落有界原因 | 不 skip 原生 skin/upload、不发布半个新 frame |
| 换图/Reset/退出 | 失效许可、CPU 存储安全结算；GPU 用现有退休 | 不建立平行释放路径 |

未来 W4 若要实际替换工作，需要“在原生不可逆步骤之前已完成新路径资源与输出”的事务证明。尤其不能先跳过 native kernel 再在失败后回退到已经陈旧的 VB。[既有 native upload 合同](../research/war3_render_issues/28_gpu_skinning_takeover_feasibility/native_asm_contract.md) 已说明外层 `0x0EEA50` 的锁定、ring、状态与异常副作用不能整体绕过。该内容是后续边界，不是本轮接管计划。

## 7. 不新增热路径技术债

首个 W1 样本为显式诊断；预分配限额见 [A2](2026-09-30-shared-core-native-frame-export-contract.md)。关闭时不做额外实例扫描、矩阵全拷贝/模型 hash、锁/日志/记录器操作；开启只处理选中目标。资产指纹/映射在加载阶段完成，不能每个 draw 新建 vector 再 move 丢掉容量复用。

这仍是需要测量的设计约束，**不宣称零成本已证明**。未来实现必须记录：关闭分支成本、选中窗口拷贝字节/次数、分配次数、锁/等待、CPU/VA/commit、采集失败、完整帧均值/p95/p99。成功率和合法对象覆盖必须一起看，不通过全拒绝换低耗时。

Shared CPU input 的 semantic/data proof 不进入 GPU fence 系统。后端需要额外的实际 slice/range/格式证明；若未来共享绘制尚无 UV/索引/材质公开视图，应回报 Unsupported，不偷读私有 ModelData 拼 draw。

## 8. 验证层与实施依赖

| 阶段 | 可以验证 | 不能宣称 |
| --- | --- | --- |
| 当前 A 文档 | 源字段/入口、依赖、合同与未知项 | adapter 已运行、数据对齐完成 |
| B 独立构建/离线 | i686 MinGW C++20 核心可编译、公开 API 可调用、同输入跨 MSVC/MinGW 数值差异 | 已改 DLL/已接游戏/帧率提升 |
| 后续生产组件交接测试 | 原生 payload→受限 ticket→核心输出→导出/解析；所有拒绝路径、代际与视图寿命 | 当前游戏渲染已经一致 |
| 作者在场的一实例采集 | Footman 源/映射/当前消费/空间/渠道及两实例不串 | 全模型/全动画或像素等价 |
| W2/W4 GPU/视觉门 | 指定后端实际使用输入、覆盖/恢复、画面与性能 | 其他未覆盖设备与场景 |

生产组件反例至少包括：map/device/evaluator epoch 改变、同地址新实例、旧 part、新矩阵旧证明、原/runtime 组 remap、截断、重复 layer/geoset、unknown 空间、双 world、缺 alpha/RGB、输出覆盖、后台晚读、容量失败和恢复。测试应调用实际签发/桥接函数，不能只检查源字符串或另写模型算法。

实施顺序：

1. 先等上游固定提交、来源清单、公开已求值输入、Footman golden 和格式约定。
2. 作者通知后做 B：独立 MinGW 构建与同输入数值差分，不改上游；缺头/依赖/编译问题写建议。
3. 再申请 WarVK 生产适配器与有界采集点实现范围，沿实际最终路径做交接测试，补本轮缺的身份/渠道来源。
4. 作者在场后另行安排原生采集；完整 W1 证明之后才考虑 W2 共享离屏绘制。

## 9. 唯一上游、版权来源和维护退出

按作者决定，共享核心对外 GPLv3、作者保留另行授权权利；FORGE 作为版权人使用，游戏 Vulkan/编辑器 D3D11 方向已定，**本轮不重新请求这三项拍板**。

作者决定不等于每个文件的来源核查已经完成。上游须提供实际依赖闭包及逐文件来源声明，已知还包括 `MdxNormalPreview.h`、MDX 实现/内部头和所需 Result 类型。WarVK 非作者 GPL 贡献、其他 GPL 项目代码不能因为都叫 GPL 就搬进上游；DXVK zlib 代码也须保留原许可/来源并先申报是否确需引入。此处记录工程权限边界，不是法律文本审定。

WarVK 只固定引用 War3RenderCore 的提交与内容状态：配置路径可注入、默认相对路径，不把本机绝对路径写进构建项目；缺目录/锁不一致/上游脏文件须明确拒绝或进入具名本地审查模式，不能默认静默接受“HEAD 一样”。共享规则/MDX parser/DTO 只有一个上游，问题上报作者，由上游修复并重新锁版本，不复制到 WarVK 各改各的。

新增宿主代码以后先说明原创/来源及文件归属。当前纯文档没有搬移任何实现，也未创建共享仓库/远程。上游首次包含进 WarVK 的公开版本前须按作者决定公开；本轮不执行公开或发布。
