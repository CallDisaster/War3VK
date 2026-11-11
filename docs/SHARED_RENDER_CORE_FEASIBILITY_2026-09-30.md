# WarVK 与 FORGE 共用模型渲染核心：可行性评估

日期：2026-09-30。状态：只读评估，等待作者裁定；不是接入工单、产品验收或实施授权。

本报告回应 FORGE 提供的 [评估任务](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/PROMPT_WARVK_SHARED_CORE_FEASIBILITY_20260930.md)。本轮只检查两边源码、已有研究及测试回执，并新增本报告；没有移植代码、修改 FORGE、编译、部署、启动游戏、操作 IDA、提交或发布。

## 1. 结论先说

**值得做，技术上可行，但不能把“接入现有 RenderCore”与“已经共用完整渲染器”当成同一件事。**

FORGE 已有可独立构建的 CPU 姿态核心，以及另一套 D3D11 模型绘制后端。WarVK 已有原生矩阵捕获、不可变几何、对象/部件身份、实际绘制输入和 Vulkan 蒙皮实验。这些基础足以支持一个小范围的 W2 验证，不必从头研究魔兽模型格式和蒙皮公式。

但目前还有四道实质门槛：

1. **授权尚未打通。** 两边现行许可证不允许直接相互搬代码；共享部分及其依赖需要作者另行授权。
2. **数据可以对齐，但尚未整链证明。** 原生矩阵的空间、部件组索引、实例身份和资产版本，必须与实际 draw 一起绑定。不能只因为两边矩阵都是 48 字节就直接交接。
3. **FORGE 的姿态核心不是完整模型绘制核心。** 材质、纹理、绘制次序、蒙皮着色器和后端资源安装仍在核心外，需要再抽出一层两边共同使用的绘制合同与算法。
4. **W3 的时钟与动画控制状态还不齐。** WarVK 现有 `sequenceId/sequenceTime` 字段不等于已经可靠获取当前 MDX 动作序号和动作内时间；原版过渡、全局序列时钟和部件可见性也不能靠猜。

推荐路线是：**先确定授权与接口边界 → 对齐一个模型的原生输入 → 共用小范围绘制算法 → 并行姿态对照 → 有条件替换 → 最后才考虑跳过原版计算。**

第一轮应保留原版全部绘制和动画，只做离线输入核对及随后另行授权的离屏绘制对照。不建议立即替换主画面，更不建议以这项工作同时重写全部阴影、动画、缓存和资源管理。

## 2. 本次评估实际核对了什么

### 2.1 源码边界

| 项目 | 本次读取的工作树 | 读取时 HEAD | 需要注意 |
| --- | --- | --- | --- |
| WarVK | `War3MapReforge/Core/Base/Graphics/dxvk` | `a2054a9776` | 主树有既有未提交改动；评估包含实际工作树，不把 HEAD 当作全部源码身份 |
| FORGE | `War3MapEditor64` | `342705287c` | 同样有未提交改动且仍在开发；本报告不冻结其工作，不代表随后改动也经过审查 |

源码事实以第 11 节的文件为依据；带日期的研究文档只用于解释来源，不自动代表当前运行时通过。尤其没有使用旧的 RenderStub 状态来判断今天的 FORGE。

### 2.2 FORGE 已有能力与边界

已核对的事实：

- `War3RenderCore::CPU` 是独立静态库，C++20，不依赖 Qt、Windows 窗口或 GPU。它把已解析的 MDX 编译为自有的不可变资产，再按实例批量求值。
- 输出包含组矩阵、geoset 透明度/RGB、回执及诊断；组矩阵为模型局部空间、四列各三个 float、步长 48 字节。顶点组号是 **geoset 内局部组号**，不是整个模型的组号。[F1–F3]
- 求值器是单线程所有者；预热后不自动扩容。输出视图在下一次求值、移动或销毁时失效，失败求值也会使旧视图失效。跨线程交接必须复制到宿主拥有的帧包。[F1、F3]
- 当前质量标签只有 `PreviewApproximate`。同输入在受控编译条件下稳定，不等于已经与原版动画、原版法线或原版像素等价。[F1、F2]
- 实际 D3D11 绘制在 `src/Render/Backend/D3D11/`，蒙皮/材质着色器在 `src/Render/Shaders/`；`RenderCore` 的编译资产没有完整的索引、UV、材质层、纹理及绘制后端接口。[F4–F6]
- 读取到的 Win32 既有测试日志为 **12/12**，并只读检查了两份相关测试程序的 PE 头，机器类型均为 `0x014c`。这支持“核心及相关测试能运行于 x86”的判断；**本轮没有重新编译、重跑，也没有证明能直接链接进 WarVK**。[F9]

一个容易遗漏的依赖问题：核心虽然不依赖编辑器运行时，但公开编译接口仍用 `War3Map::Result`，实现用 `War3MapCore/Render/MdxModel.h`。因此还不是只取 `src/RenderCore/` 一个目录就能交付的完全独立源码包。[F3、F4]

任务中提到的 2452 个路径扫描，应区分“文件能读取”“MDX 能解析”“指定动作能求值”“材质/粒子能完整绘制”。不能把路径扫描数字当成全模型原版等价率，本报告不据此给出覆盖率承诺。

## 3. 问题一：WarVK 最自然的接入点在哪里

### 3.1 使用现有来源与所有者，不另开一套对象追踪

WarVK 已经有三层互补数据，适合成为共享核心的宿主适配层：

| 层 | 已有入口/载体 | 能提供什么 | 不能单独证明什么 |
| --- | --- | --- | --- |
| 原生生产 | `war3_model_hook.cpp` 的矩阵写入、范围复制及部件绑定；原生 upload/kernel 观察 | 原版正在使用的矩阵、几何、组号、来源和帧标签 | 单凭共享 geoset 指针不能确定属于哪个实例 |
| 对象与不可变资源 | `ModelInstanceRecord`、`ShadowGeosetResourceRecord` | 实例关联、模型/部件、positions/normals/UV/indices/矩阵组、地图及不可变代际 | 路径和局部几何 hash 不是原 MDX 文件内容身份 |
| 实际消费 | 渲染队列 dispatch 合同、CurrentDraw、D3D9 draw/native bridge | 这一次绘制的部件、切片、索引域、原生状态和最终选中来源 | 已经蒙皮后的 VB 不能冒充模型局部 bind-pose 几何 |

依据：[W1–W6]。`d3d9_war3_hook.cpp` 主要负责接线和地址安装，不适合承载共享核心全部选择规则。

**建议分为两个接入位置：**

1. 在原版矩阵生成/绑定及 native upload 观察处，取得当前数据，立即形成有界、自有的来源快照；不要把原生临时矩阵池指针留给异步消费者。
2. 在当前 draw 的身份、切片和状态已经齐备时，由一个验证入口签发共享绘制包，再由 DXVK 命令流/Vulkan 后端消费。该入口应是独立的小模块，不继续把所有新策略堆入 `d3d9_device.cpp`。

`War3TryAppendSemanticShadowPacket` 可复用身份、几何、来源交接经验，但它是**阴影候选构造**，不是主画面完整模型绘制接口。不能把“阴影 caster 入队”当成“已经能够替换原版单位绘制”。

现有 draw-time override 会改用已蒙皮的原生快照并清空语义 `inputSkinSelection`。共享路径必须记录**最终真正消费的数据**，不能保留之前尝试过的 palette 来源，给一份已经世界变换过的 VB 再蒙皮一次。[W5]

### 3.2 能否稳定拿到实例、动作、时间、palette

| 项目 | 当前判断 | 还缺的证明 |
| --- | --- | --- |
| 模型实例/部件 | 有较完整关联链，W2 可选择已证明的小范围实例 | 指针复用、附件、共享部件、多实例同一模型、换图/Reset 的整链核对；不能以 rawcode 代替实例 |
| 不可变几何 | 已有自有缓存和不可变代际，可直接作为对齐基础 | 与 FORGE 原 MDX geoset 的一一映射、被游戏合并/重排后的索引关系 |
| 原生组 palette | 有明确生产位置和已复制发布合同 | 与**同一次 draw、同一实例/部件**的绑定、空间、完整数量和寿命；缺失必须显式报告 |
| MDX 动作序号 | **尚不能宣称可靠获取** | 当前动作属性到具体 MDX sequence 的映射、随机变体、过渡/控制器状态 |
| 动作内时间 | **尚不能宣称可靠获取** | 原生控制器当前时间、速率、暂停/循环/切换，以及采样时刻 |
| 全局序列时钟 | FORGE 有原生只读研究，WarVK 还需形成实例级输入 | 时钟初始化/重置/相位，是否能由一个共同基时表示 |

特别说明两个已查到的事实：

- `PoseRecord` 的确有 `sequenceId/sequenceTime`，但 `TryReadSpriteFrameTransform` 读取的是 `AnimationTimeOverrideEnabled/Value`；相关 `noteSpriteFrame` 调用传入的序号为 `0u`。**结构里有字段，不等于字段已经代表当前动作。**[W1]
- 新的 Walk API 用 `CUnit+0x2E0` 观察动画属性，原生 setter 位于 `Game.dll+0x67BB90`。它是动作属性选择入口，不是已经证明的 MDX 动作索引或播放时钟接口。[W7]

所以 W2 应只借用原版已经完成的姿态；W3 才补动作控制状态。不要为了凑齐 `PoseRequest`，把序号填零、把帧墙钟填成动作时间。

### 3.3 可以直接复用的已有研究

- 渲染队列至实际 draw：`0x6F1380A0 → 0x6F13A5E0 → 0x6F13A710/0x6F138EE0 → 0x6F0EEA50 → 0x6F0EDDC0 → D3D9 DrawIndexedPrimitive`，有既存原生 ABI 与输入说明。[W8]
- 原始几何、每顶点组号、最多两组 UV、mapped output 与 palette 的关系，以及 native kernel 的标量运算顺序。[W8]
- 矩阵组平均、CModel 目标矩阵数组、部件依赖条件的保存反编译结果。[W9]
- WarVK 已有 consumer-fenced input lease、不可变 GPU 包和显式 GPU skin 路线。可作为生命周期基础，不能当作共享核心已经接好。[W6、W8]

当前正式收敛开关 `kReleaseFreezeExperimentalShadowRoutes=true` 会把 GPU skin 实验模式冻结为 Disabled；历史显式实验回执不等于当前产品默认开启。已有研究还记录过接管路径的 CPU 侧负收益，说明“少做一次蒙皮”并不保证整个方案更快。[W6、W8]

## 4. 问题二：原生 palette 与 FORGE 组矩阵能不能对齐

**能，位置变换的基本公式已经能对应；主要风险在空间、组序号、实例绑定和有效期，而非 48 字节长度本身。**

### 4.1 先分清三种不同数据

| 数据 | 已有证据 | 对共享核心的意义 |
| --- | --- | --- |
| 按 geoset 组平均后的矩阵 | `0x6F12E600` 读取 `CGeosetData+0xF0` 组数，逐组调用 `0x6F12E200`，每组输出 48 字节；后者含单骨骼拷贝及多骨骼平均分支 | 最贴近 W2 的直接输入，不要再平均一遍 |
| CModel 的节点/姿态矩阵数组 | `0x6F12FDC0` 从栈源范围复制 `CModel+0x5C` 条矩阵，**写入** `CModel+0x60` 指向的目标 | 可辅助 W3 分层核对；不能把它天然当成 geoset 已平均组矩阵 |
| 最终原生 VB | 原生 `0x6F0EDDC0` 用顶点组号与组矩阵逐顶点计算，输出 FVF 顶点 | 是 W2 顶点对照的参考，不是共享核心的 bind-pose 输入 |

证据见 [W8、W9]。旧研究名称或注释若与函数体不符，以实参、读取和写入操作为准。

`0x6F777FE0` 的保存函数体检查部件对应浮点权重 `>0`，再检查依赖索引为 `255` 或依赖标志位；**不能凭旧名把它解释为 LOS/战争迷雾判定**，也不能简单删除这个条件来增加覆盖。[W9]

上述地址按既存 1.27a `Game.dll` 的 `0x6F000000` 基址表达；安装时必须用实际模块基址、RVA 和版本/字节守卫。它们不是任意魔兽版本的通用地址。本轮没有重新进行 IDA 或运行时验证。

### 4.2 布局能对应，空间不能假装对应

FORGE 的位置公式为：

`x' = m0*x + m3*y + m6*z + m9`，其余两轴同理。

WarVK 保存的 native kernel 合同和现有 GPU direct-input 着色器也使用这组标量排列。[F1、W8、W10] 因此不应仅看到 WarVK 某处 `row_major` 就把原生 48 字节全盘转置；应使用基向量、平移及非均匀缩放夹具验证具体适配器。

**真正不一致的是当前接受的坐标空间：**

- FORGE `ExternalGroups` 只接受 `ModelLocal`、`worldTransformApplied=0`。
- WarVK `skin::Selection::Usable/OwnedSnapshotMatches` 的可消费组输入目前要求 `Space::World`。
- 原生已蒙皮单位的 draw-time 顶点通常已经在世界空间，而未蒙皮刚体仍可能需要单独世界矩阵。[F7、W4、W5]

建议第一步选择世界变换可逆的小范围实例，用已经证明的世界变换把组矩阵换到模型局部，再核对顶点。若列向量形式为 `G_world = W * G_local`，才可使用 `G_local = inverse(W) * G_world`；**先证明这一组合关系，再做逆变换**。零缩放、近奇异、附件空间不明或缺世界矩阵时拒绝共享样本，保留原版，不猜 identity。

另一条可行路线是让未来的**共享绘制包**明确支持 World/ModelLocal 两种输入，并按类型选择是否乘世界变换。那是接口扩展，不是把 World 字节标成 ModelLocal，也不能直接违反今天 `ExternalGroups` 的合同。W3 可把双方结果都变到同一世界空间再比较，避免依赖逆变换。

### 4.3 组索引必须逐 geoset 对齐

FORGE 按原 geoset 顺序拼接组矩阵，并在绘制时使用 `instance.firstGroup + mesh.firstGroup + vertex.group`。游戏 geoset 可能经过运行时重排、合并或绑定；一个 CModel 的节点索引也不保证等于 MDX `objectId`。[F2、F5、W2]

需要一次性建立并验证：

- 原 MDX geoset ↔ 游戏不可变 geoset/mesh payload；
- 原 MTGC/MATS ↔ 游戏组大小、成员及运行时节点映射；
- 顶点组号 ↔ 该部件 native palette 范围；
- 最终索引域、baseVertex、UV 层及对应材质层。

这份映射归不可变资产代际拥有，不能每帧重建，也不能按数组长度相同就认定顺序相同。无组的刚体与有组的蒙皮应分别描述；原生无组分支的单矩阵回退不能被偷换成“资产有一个骨骼组”。

### 4.4 一个已确认的法线差异

保存的 native kernel 证据显示：法线使用同一组矩阵的 3×3，不加平移，**该 kernel 不做归一化**。FORGE 的 CPU `SkinPreviewGeoset` 和 `SkinModel.hlsl` 则做组变换后的归一化，之后还有实例法线变换。[W8、F4、F6]

这不证明当前 FORGE 图像一定错误，也不证明原版后续照明从不归一化；它证明**两边“蒙皮步骤”尚不能被称为逐位等价**。第一轮先分别核对位置与 kernel 法线输出，保留原版语义；不要顺带改成逆转置或更“标准”的通用法线算法。最终光照等价还要核对后续实际照明状态。

### 4.5 模型路径不足以标识资产版本

WarVK 已有 `modelPath/modelKey`、几何内容 hash、map epoch 和不可变代际；FORGE 的 `AssetSource` 要求路径、来源层及实际内容摘要。二者不能直接用 rawcode 或路径字符串合并。[W1、W2、F2]

推荐资产身份分两层：

1. **跨项目内容身份**：实际被加载的 MDX 字节摘要、解析/编译合同版本；材质纹理另外带其实际来源摘要，或纳入明确的依赖清单。
2. **本进程运行身份**：地图会话、资源不可变代际、实例生命周期代际、设备代际。GPU 最后使用另由现有资源跟踪证明。

必须确认资源解析优先级：地图导入文件、补丁 MPQ、基础 MPQ、可选本地覆盖等，不能固定读取 `War3x.mpq` 后认定它就是游戏正在用的版本。初版可冻结一个已确认来源的测试模型；完整产品需要在资源加载/已有资源服务边界绑定来源，离开热路径后做摘要。**禁止在每次 draw 读文件、解 MPQ 或全模型哈希。**

## 5. 问题三：x86 内存、线程、每帧成本和回退

### 5.1 共享核心不会自动把内存搬离游戏

CPU 核心在 32 位进程里运行，就仍占该进程地址空间。静态 GPU 数据也可能伴随上传页、映射地址、驱动元数据和在途版本；不能仅用显卡总容量推导可用的进程预算。

需要分别记账：原版资源、MDX 解析临时数据、编译资产、求值器有效存储、宿主帧包、GPU 静态包、上传页及在途退休资源。**核心的 `maxBytes` 不是整个进程 RSS、VA 或 GPU 驻留上限。**[F1、F2]

组矩阵本体的计算很直接：`实例数 × 每实例组数 × 48`。例如 2000 实例、各 60 组，单帧矩阵为 5,760,000 字节，约 5.49 MiB；三份在途矩阵即约 16.48 MiB，尚未计 alpha/RGB、资产、诊断和驱动开销。CPU 帧包与 GPU 版本的数量也不能都假设只有一份。

FORGE 的紧凑输出模式减少节点**输出存储**，Computed 路径仍计算完整骨架；默认求值预算 64 MiB、编译资产入口默认上限 128 MiB，也都不是 WarVK 的推荐运行预算。[F1–F4]

建议开发首样本最多 16 个已确认实例，按实测需要预热一个小批次；新增受控 CPU/GPU 存储分别以 16/32 MiB 为**拟议试验上限**，包括自有帧交接存储，但另报进程 VA/commit 与驱动差额。超过则停止该实验，而不是抬预算或遗漏必要 caster。这些数字不是实测结果、产品默认值或实机授权。

静态模型只建立一份共同资产视图/上传包，姿态只按实例更新。原版仍存在时难免有一部分双份资源，需把它作为 W2/W3 的真实成本报告；只有 W4/W5 覆盖闭合后才能讨论消除旧副本。不要用长期保存所有场景 post-skin 快照来实现“共用模型核心”。

### 5.2 所有者与帧交接

建议继续使用 WarVK 的已确定游戏/渲染所有者，在同一所有者上生成或核对姿态；不要让命名管道、控制台或后台资源线程同步推进同一个求值器。

异步读取 MDX/编译资产可以独立进行，但只在安全点发布不可变结果。当前 native palette 必须在其原生有效窗口内复制；求值器 span 必须在下一次求值前转成自有帧包。命令流拿到的是**数据和证明一起封装的包**，不是查询游戏裸指针的权限。

换图/Reset/模型替换使新包失效；旧 GPU 包仍按 DXVK 最后消费者和完成序号退役。资产代际、姿态帧、地图/设备代际、GPU 完成不能并成一个 `generation`。这与现有 [渐进式架构收敛计划](plan/2026-09-15-incremental-architecture-convergence.md) 是同一方向，不需要另建平行生命周期系统。

### 5.3 热路径成本

W2 读取已算好的组矩阵，不应再计算骨架；同一实例/姿态只校验并上传一次，主画面、阴影和多个材质层只引用结果。W3 才增加独立姿态求值，作为有界并行对照。

禁止新增 per-draw 堆分配、完整 `PoseRecord` 深拷贝、全场景哈希、全局锁和同步 GPU 读回。需要记录：完整帧 CPU 时间与 p95/p99、额外查询/扫描/拷贝字节、分配/锁次数、上传及驻留、诊断关闭成本、开启覆盖与丢样原因。核对开销和绘制开销分开报告。

当前矩阵 Hook 还有每帧 20000 次采集上限；原版照常执行，但其后的部分插件采集会跳过。这说明既有捕获并不天然是全量输入服务。W2 应以具名范围和缺失原因验证覆盖，不能把未捕获对象当成不存在，更不能取消上限后直接投入主画面。[W1]

### 5.4 失败与回退

- **W2/W3**：原版动画、CPU 蒙皮与绘制完整保留；共享绘制只做离屏/诊断对照。核心整批失败则整批样本无效，原版画面不受影响。
- **W4**：只在当前对象所需几何、姿态、材质、纹理、各必要消费者和资源安装全部通过预检后替换。预检失败必须在跳过原版之前返回原路径，不能先挡住原 draw 再等待后续资源成功。
- **W5**：一旦原生 kernel 被跳过，本帧动态 VB 可能已无有效内容；此时不能再简单“回退到原版 VB”。现有 native bypass 研究已有此边界。需要独立的提交前准备、所有消费者结算及不可逆故障处理，不宜和 W4 合在一起。[W8]

已经发生的 GPU 执行或终态 device lost 不存在“把同一帧撤回重画”的通用保证。正常状态下下一帧禁用实验，与终态设备故障的 fail-stop 是两件事。CPU 接收/命令记录成功也不能冒充 GPU 完成或像素正确。

## 6. 绘制应该放哪边：继续走 Vulkan

推荐：**游戏内使用 WarVK/DXVK 的现有 Vulkan 设备和命令流；FORGE 暂时保留 D3D11。**

不建议为了调用 FORGE 现有插件，在游戏里另开 D3D11 设备再跨设备复制最终图像。那会增加纹理/深度/同步/显存与混合次序问题，且无法直接复用 WarVK 的阴影、后处理和最后消费者跟踪。

理想边界如下，图中“共享绘制前端”是建议新增的能力，**不是现有代码已实现**：

```text
FORGE：解析的真实资产 + 预览实例/时钟 ─┐
                                      ├→ 共同资产/姿态合同
WarVK：实际加载资产 + 原生实例/组矩阵 ─┘          │
                                                 ↓
                                共享模型绘制前端/材质与蒙皮算法
                                      ┌──────────┴──────────┐
                                 D3D11 后端             Vulkan 后端
                                 FORGE 宿主              WarVK 宿主
```

“共享”应至少包括资产/geoset 映射、蒙皮和材质规则、绘制包与验证、共同测试向量。设备对象、命令缓冲、资源安装和退役由各后端负责；不把 D3D11 COM 指针或 `Rc<DxvkBuffer>` 混入 CPU 核心。

如果作者要求连绘制命令生成和效果管线都只有一份代码，长期可为 FORGE 再提供同一 Vulkan 后端。这比“两套后端共用算法”投入大，不是 W2 的必要条件，也不要求把编辑器 UI 改为 Vulkan。

不能把 HLSL 翻译为 GLSL 或两份公式看起来相同就视为一致：需要固定运算顺序、精度、布局、裁剪空间、纹理色彩空间、采样/混合/深度条件，并做同输入测试。不同后端的像素比较应冻结合理容差，不预先承诺逐位一致。

**编辑器阴影/光照所见即所得是后续目标。** 当前 FORGE 环境接口是预览模型光照、雾和静态地表阴影，不是 WarVK 的完整 CSM、点阴影、Froxel、局部雾和后处理合同。[F5、F6] 还要共用光源/相机/材质/阴影规则，并由宿主提供动态状态。不能仅统一姿态，就宣布这些效果已经所见即所得。

## 7. 问题四：工具链能不能直接编核心

**源码层面有较高可行性；现成的 MSVC 静态库不能直接作为 WarVK 的 C++ 库链接。**

| 项目 | 已核对情况 | 建议 |
| --- | --- | --- |
| WarVK | Meson，全局 C++17；当前 build32 元数据为 i686 MinGW GCC/G++ 15.2.0 | 为共享核心建立独立 C++20 静态库目标，不先升级整个工程 |
| FORGE 核心 | CMake 独立入口，C++20；MSVC `/fp:precise`，GCC/Clang `-ffp-contract=off` | 授权后用同一套 WarVK MinGW 工具链重新编源码，保留浮点约束 |
| 核心公共接口 | 有 `span/shared_ptr/string/Result` 等 C++ 类型 | 同编译器同运行库下进程内使用；不能跨不同 C++ ABI 直接传这些对象 |
| x86 Hook 边界 | WarVK 对 i386 使用较低首选栈对齐，并有 SSE 配置 | 确认从游戏进入新库的调用边界及所需对齐，不能只凭 Win32 编译通过排除 ABI 问题 |

依据：[W11、F3、F4]。本轮只读了现有构建配置与已有二进制，没有做 MinGW 编译/链接实验。

若坚持使用 MSVC 构建的独立 DLL，则需要有版本、尺寸、错误码及分配/释放所有者的 C 接口，禁止跨 DLL 边界传 STL 容器、C++ 异常或让另一运行库释放内存。初版同工具链源码构建更简单，但它也不能解决授权问题。

另需把核心依赖闭包打包清楚：公共 DTO、`Result` 类型、MDX 数据结构、实际解析器及其辅助头、测试参考实现、着色器各属于什么授权。CMake 不必成为整个 WarVK 的新构建系统；独立测试入口可保留，两边锁定同一个核心版本。

同输入逐位稳定目前是在限定编译条件下的实现性质。MSVC/MinGW、CPU/GPU、原版 x87/SSE 等不同路径都应另测；仅关闭 FMA 或 fast-math 不足以证明原版逐位等价。

## 8. 问题五：W2 最小版本、风险和 W3–W5 前置条件

### 8.1 一个必须先纠正的 W2 接口误区

今天的 `PoseBatchEvaluator::Evaluate(ExternalGroups)` **不是纯矩阵搬运器**：它仍校验 sequence、计算 sequenceFrame，并按请求时间求 geoset alpha/RGB。[F7]

因此在 W2 还没有可靠动作时钟时，不能为了使用此入口编造时间。推荐共享**绘制包**允许直接携带原生已求值的组矩阵、实际 alpha/RGB/材质状态及其来源；W2 绕开独立姿态求值，W3 再使用 Computed 路径。或者先限定确实静态的材质/颜色并明确限制。这是未来接口设计要求，不是本轮修改许可。

### 8.2 W2 的最小可验证范围

建议从一个已冻结实际 MDX 版本、无附件/特殊 billboard/材质层动画的普通模型开始，至少两实例、不同动画和非同一世界变换；先选不透明层，再增加透明测试层。暂不覆盖粒子、条带、所有装饰物或战争迷雾替代逻辑。

验收应同时包含：

1. **身份与映射**：每个样本对应实际实例、部件、geoset、资源内容、帧与 palette 范围；缺失具名，不使用弱身份认证通过。
2. **输入复算**：共享算法生成的位置、kernel 法线与同一次原生输出逐顶点核对，记录首个差异的实例、组和顶点；组平均至少覆盖单骨骼和多骨骼夹具。
3. **后端绘制**：游戏 Vulkan 离屏真正画出同输入几何；FORGE 后端消费同一冻结包。分开比较几何/深度/alpha silhouette 和最终色彩，不把光照差异归给姿态。
4. **正确拒绝**：错误组数、陈旧帧、来源替换、模型版本不同、空间不明、缺纹理等被拒，原版仍连续绘制。
5. **寿命与性能**：移动、同模型多实例、连续采样、换图/Reset/退出；记录增量内存、回收、每帧开销和丢样；不扩大既有预算掩盖成本。

这可以证明“共同绘制规则能在游戏里消费原生姿态”。**只增加一个使用 WarVK 旧 shader 的辅助 draw，或只接通 CPU 求值器，都不足以完成作者要求的 W2。** 但它们可作为明确命名的前置实验。

### 8.3 风险清单

| 风险 | 本次已有实据/缺口 | 处理方式 |
| --- | --- | --- |
| 空间或索引错配 | World 与 ModelLocal 不同，骨骼数组与组数组不同 | 分类型适配、数值夹具及实际 native 输出对照 |
| 多实例串用矩阵 | geoset 资源会共享，临时池/指针可被覆盖 | 部件/实例绑定后复制发布，不能按 geoset 指针独占缓存 |
| 材质不能完整复现 | FORGE `MdxLayer` 的 KMTA/KMTF 只标记存在，仍取静态值；完整 UV/材质动画未进入当前核心 | 首版明确白名单；实际状态借原版；后续单独建立材质求值合同 [F8] |
| “Transparent”及深度语义 | FORGE 有新原生研究，但完整 pass flag 来源及同帧像素门仍缺 | 保存原状态与来源，不把整个场景透明层一次性改成同一规则 [F10] |
| 看似减工作却更慢 | 既有 native GPU skin 研究记录管理成本回归 | W2 不承诺 FPS；按完整帧 A/B 与尾延迟测收益 |
| x86 双份资产/在途包 | 原版仍占用内存，核心/后端另有副本 | 独立增量账、加载临时数据及时释放、有界工作集 |
| 核心失败误伤正常单位 | 整批失败与不支持模型存在 | 按已知能力分小批，失败批保留完整原版；不弱化核心的失败合同 |

### 8.4 各阶段的前置条件

**W3：独立姿态逐帧对照**

先取得动作序号/动作内时间、播放速率、循环/暂停/切换和必要过渡状态；确认实例全局序列时钟。若原版各时钟不能由一个共同基时取模表示，当前单个 `globalElapsedMs` 接口也需扩展，不能用进程运行时间顶替。

FORGE 已有零周期、全局键时间原点的地址证据，但仍声明末键回首键及部分大时钟行为未完成等价验证；通用插值、节点继承、相机朝向、部件依赖和过渡仍需各自取证。[F10] 原版 `0x6F777FE0` 的部件条件也是这一范围的一部分。

先比较双方**同空间的组结果**；出现差异再沿已冻结映射下钻节点/轨迹。原版数组、共享核心输出和事件必须来自同一逻辑采样窗口。与 FORGE 自己的参考实现对拍不能替代原版 oracle。

**W4：小范围替换绘制**

要求该范围的材质、UV/贴图、颜色/透明度、可见性、原生 draw 次序、主画面/阴影/描边等消费者都闭合；有持续绘制、故障回退和换图/Reset 证据。W3 暂不等价的模型仍走原版。不能以漏画对象消除差异。

**W5：跳过原版计算**

先区分“动画控制/节点姿态”“组平均”“逐顶点蒙皮”“动态上传”：四者不是一项可一起删除的工作。姿态可能还被附件、粒子、模型事件或其他逻辑消费者使用。只有必要结果替代、这些消费者及原版副作用全部证明后，才判断哪段可跳过。

跳过 CPU 蒙皮不必等价于跳过动画；反之独立姿态可算出，也不证明能关闭原版动画控制器。必须单独验证不可逆跳过后的资源/回退与真实净收益，不能按已有显式 bypass 的名字自动放行。

### 8.5 工作量估算

以下是**范围估算，不是交付承诺**：一名熟悉两边的渲染工程师主导，有 FORGE 接口配合；以经典 MDX、少量普通单位、现有设备后端为范围。作者授权等待、额外逆向及复杂地图反馈不计入可控工期。

| 工作 | 估计工程日 | 完成的含义 |
| --- | ---: | --- |
| W0：授权/依赖闭包/共享边界 | 1–3 | 可移植文件及双方规则写清，不代表法律签署完成 |
| W1：空间、组映射、身份和离线夹具 | 3–5 | 一个冻结模型的数据链能复算，不替换游戏 |
| W2a：有界 native 输入适配 + Vulkan 离屏首次绘制 | 6–10 | 真正在游戏进程消费同帧输入，原版保留 |
| W2b：共同绘制前端/算法 + 两后端对照与寿命门 | 5–10 | 小范围达到“共享绘制规则”，不是全模型所见即所得 |
| W3：有限模型姿态对照 | 10–20 起 | 限定动作/语义；原版过渡等缺口可能显著延长 |
| W4：有限范围接管与回退验收 | 5–10 起 | 在 W2/W3 及材质/消费者前置完成之后 |
| W5：省掉原版工作 | 暂不报固定天数 | 先完成副作用/消费者审计与成本实测，再形成单独工单 |

W0 至有意义的 W2 约 **15–28 工程日，单主导约 3–6 个工作周**。若目标只是离线编通库或一次辅助绘制，可更快，但不能将其称为全部 W2。两边并行做核心打包、后端适配和夹具可以缩短日历时间；不能并行跳过接口冻结与原生语义验证。

完整模型覆盖、粒子/条带、编辑器动态阴影/体积效果所见即所得另行估算，不包含在上表。

## 9. 问题六：推荐路线，以及需要作者拍板的事项

### 9.1 建议比原 W2–W5 多加两步

1. **W0：授权和源码包边界。** 先解决“能否合法共用”，并明确共用的是姿态还是完整绘制规则。
2. **W1：共同数据合同与 native 适配。** 用一个模型证明空间、实例、组号与资产版本对齐，不改画面。
3. **W2a/W2b：原生姿态驱动的小范围共享绘制。** 先离屏，再完整输入对照；暂不独立求动画，也不跳过原版。
4. **W3：共享核心姿态与原版逐帧对照。** 差异具名到模型/节点或组/时钟/帧。
5. **W4：受限替换。** 只给已经闭合的范围，保持故障与不支持情况的原版入口。
6. **W5：按消费者与收益逐项减掉旧计算。** 不能将“统一架构”当作自动删除原版的理由。

既有 WarVK 的跨地图、palette 来源及内存维护门仍须保留；共享核心不是这些问题的免检路线。未完成 Water、原版自动模型灯和 64 位渲染宿主也不因此回到本轮范围。

### 9.2 授权方案：作者决定，本报告不替作者重授权

本地 FORGE `LICENSE.txt` 为 PolyForm Noncommercial 并要求商业用途另行书面许可；WarVK 根 `LICENSE` 为 GPLv3，原始 DXVK 为 zlib。[本地 FORGE 许可证](E:/Mycode/Source/Repos/War3MapEditor64/LICENSE.txt)、[WarVK 许可证](../LICENSE)、[DXVK 官方许可证](https://github.com/doitsujin/dxvk/blob/master/LICENSE)。

PolyForm 限定允许用途；GPL 不接受叠加“仅非商业”等进一步使用限制。按作者给定的授权边界，**不将当前 FORGE 代码直接纳入可分发 WarVK，也不反向搬 WarVK 的 GPL 实现到 FORGE。** 私下研究与公开分发仍应区别，本轮没有做任何代码组合。[PolyForm 正式条款](https://polyformproject.org/licenses/noncommercial/1.0.0)、[GNU 关于用途限制的说明](https://www.gnu.org/licenses/gpl-faq.html#NoMilitary)。

可供作者选择：

| 方案 | 对 WarVK | 对 FORGE/作者 |
| --- | --- | --- |
| 共享核心单独使用 MIT/Expat 等 GPL 兼容宽松授权 | 可以随 GPLv3 程序组合分发，保留相应声明 | 其余编辑器仍可保持现有授权；核心本身将允许商业使用 |
| 作者有权的共享部分采用 GPLv3 + 另行授权 | WarVK 选择 GPLv3 一侧 | FORGE 必须取得真正覆盖该部分的另一授权；明确后续贡献者权限与回流规则 |
| 暂不共用源码，只共同维护格式/测试样本、各自实现 | 不形成直接代码移植 | 保留现有授权，但不满足“同一份实现”，重复维护成本仍在 |

MIT/Expat 的兼容性可参考 [GNU 许可证分类](https://www.gnu.org/licenses/license-list.en.html#Expat)；著作权人可以给予不同许可，但须拥有相关权利，见 [GNU 对多重授权的说明](https://www.gnu.org/licenses/gpl-faq.en.html#ReleaseUnderGPLAndNF)。

“作者自留”不是一份自动生效的第二许可证。必须明确到文件/版本/权利范围，包括 DTO/解析器/算法/测试/着色器依赖闭包；不能只给 CPU 核心另许可，却遗漏它包含的 FORGE 头文件。第三方或他人贡献不能仅靠作者声明改为 MIT。原版模型及反编译材料也不是可随核心源码包再授权的自有资产。

从工程简化角度，我倾向共享的宿主无关部分使用兼容的宽松授权，双方保留自己的应用/后端许可证；但这会影响作者的商业策略，**只作为建议，由作者及必要的专业法律审查决定**。

### 9.3 其他需要拍板的事项

- 近期目标是否是“共同姿态与绘制规则、双后端”，还是必须尽快“同一完整 Vulkan 渲染器”？建议先前者。
- 首轮冻结哪些模型、哪些材质/动画，如何判断成功；建议明确小范围，不接受全资产比例替代同帧证明。
- 是否将它作为独立的新研发里程碑，并保留当前 WarVK 生产路线/发布门；不要未经实机验收切产品默认。
- 来源获取接口由谁维护：实际 MDX/纹理字节及优先级属于宿主，共享核心不自行访问游戏 MPQ。
- 两边谁维护共享版本、兼容规则及合并审查；应是一份主源码和两个宿主适配器，而不是复制后各改各的“共享目录”。

## 10. 需要 FORGE 配合交付的接口/数据

建议先提供一个小而完整的共享包规格，**作者确认授权前不实际复制包**：

1. **授权与依赖清单**：CPU 核心、公共类型、MDX DTO/解析器、参考测试与 shader 的文件级来源；第三方/贡献者边界。
2. **不可变绘制资产视图**：完整原 geoset 顺序、bind-pose positions/normals、indices、所有受支持 UV、组范围/成员、材质层、纹理来源；明确未支持内容。今天 `CompiledModel` 不暴露完整绘制资产，不宜让 WarVK依赖其私有 `ModelData`。
3. **原生外部帧输入合同**：允许 W2 输入已经求值的组矩阵及实际 alpha/RGB/材质状态，而不强迫先获得动作时钟；空间、数据来源及版本都显式。
4. **共同材质/蒙皮语义表**：预览规则与已证明 native 规则分开；注明 KMTA/KMTF/UV 动画、normal、透明测试、blend/depth/cull/sampler 的支持与未知状态。
5. **黄金夹具与 oracle**：独立数组/内容摘要、组平均、平移/旋转/缩放、多 geoset、多实例、global sequence、alpha/RGB；标明哪些是核心参考对拍，哪些来自原版同帧。
6. **可定位诊断**：资产内容、实例、节点/组、动作/时钟、源键及帧号；运行时有限容量、丢样可见，不长期导出全场景大数据。
7. **完整 Win32 回执与编译限制**：源码版本、工具链/浮点选项、已有测试日志；允许 WarVK 在后续获准后做 MinGW 独立构建和同输入数值对照。
8. **共享源码版本策略**：谁是唯一上游、接口/语义版本如何升级、双方何时同步；C++ 布局变化与数值语义变化分别记录。

对 FORGE 的原生研究，建议双方首先核对同一 `Game.dll` 身份和关键地址的实际读写语义；别把不同时间取的命名直接合并为同一结论。

## 11. 源码与证据索引

下面列出本报告实际使用的关键来源。行号是此次工作树的定位提示，后续改动会移动；事实应回到函数体/合同，不以历史行号作为永久证明。

### WarVK

- **W1**：[模型 Hook](../src/d3d9/war3/model/war3_model_hook.cpp)（`Hook_RuntimeMatrixWrite` 约 7921、`Hook_RuntimeMatrixRangeCopy` 约 8289、`TryReadSpriteFrameTransform` 约 5980、`noteSpriteFrame` 约 7412）；[资源/实例/姿态记录](../src/d3d9/war3/model/war3_model_registry.h)（21、34、162）。
- **W2**：[不可变 geoset 资源合同](../src/d3d9/war3/model/war3_model_resource_cache.h)（`ShadowGeosetResourceRecord`、不可变 payload 比较）。
- **W3**：[Hook 接线入口](../src/d3d9/d3d9_war3_hook.cpp)；[渲染队列逆向](research/war3_render_issues/20_renderqueue_dispatch_layer_reverse/README.md)。
- **W4**：[palette 选择与空间/域合同](../src/d3d9/war3/render/war3_skin_palette_selection.h)（`Selection`、`OwnedSnapshotMatches`、`Usable`）。
- **W5**：[device 消费路径](../src/d3d9/d3d9_device.cpp)（`War3TryAppendSemanticShadowPacket` 约 19817；native override 清空 selection 约 21762）；[draw-time 自有切片](../src/d3d9/d3d9_device.h)（`War3DrawTimeVBEntry` 约 2659）。
- **W6**：[GPU skin 输入/消费 lease 类型](../src/d3d9/war3/gpu_skin/war3_gpu_skin_types.h)；[运行配置](../src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp)（`fromEnvironment` 约 200）；[正式冻结策略](../src/d3d9/war3/core/war3_internal_test_config.h)（23）。
- **W7**：[单位动画 API 原生边界](../src/d3d9/war3/game/war3_unit_animation_api.cpp)（setter RVA、动作属性偏移及字节守卫）。
- **W8**：[原生 GPU 蒙皮接管研究](research/war3_render_issues/28_gpu_skinning_takeover_feasibility/README.md)；[原生 ABI/输入与 kernel 合同](research/war3_render_issues/28_gpu_skinning_takeover_feasibility/native_asm_contract.md)（尤其 3×4 位置/法线公式及不归一化边界）。历史回执未在本轮重跑。
- **W9**：[保存的原生反编译文本](research/ida_export/war3_render_tree_C_20260504_034552.txt)：`0x6F12E600` 约 193、`0x6F12E200` 约 4318、`0x6F777FE0` 约 9087、`0x6F12FDC0` 约 11962。仅引用本地证据，不复制其内容至 FORGE。
- **W10**：[阴影 direct-input 顶点着色器](../subprojects/war3fx/shaders/war3_shadow_caster_vert.vert)（48 字节字段与标量位置计算）。
- **W11**：[Meson 配置](../meson.build)（C++17、i386 栈/浮点及运行库选项）；本地 `build32/meson-info/intro-compilers.json` 为工具链读取依据，未作为发布配置验收。

### FORGE（本地仓库，只读）

- **F1**：[RenderCore README](E:/Mycode/Source/Repos/War3MapEditor64/src/RenderCore/README.md)：寿命、空间、近似标签、内存及紧凑输出边界。
- **F2**：[PoseTypes](E:/Mycode/Source/Repos/War3MapEditor64/include/War3RenderCore/PoseTypes.h)：身份、组布局、时钟、回执、预算。
- **F3**：[核心 CMake](E:/Mycode/Source/Repos/War3MapEditor64/src/RenderCore/CMakeLists.txt)；[PoseBatch 接口](E:/Mycode/Source/Repos/War3MapEditor64/include/War3RenderCore/PoseBatch.h)。
- **F4**：[CompiledModel 接口](E:/Mycode/Source/Repos/War3MapEditor64/include/War3RenderCore/CompiledModel.h)；[核心内部资产数据](E:/Mycode/Source/Repos/War3MapEditor64/src/RenderCore/ModelData.h)；[CPU 预览蒙皮](E:/Mycode/Source/Repos/War3MapEditor64/src/RenderCore/PreviewSkin.cpp)。
- **F5**：[插件模型/蒙皮合同](E:/Mycode/Source/Repos/War3MapEditor64/include/War3MapCore/RenderPluginV2.h)；[D3D11 模型安装/绘制](E:/Mycode/Source/Repos/War3MapEditor64/src/Render/Backend/D3D11/D3D11Models.cpp)；[动态矩阵安装](E:/Mycode/Source/Repos/War3MapEditor64/src/Render/Backend/D3D11/D3D11Skin.cpp)。
- **F6**：[蒙皮 HLSL](E:/Mycode/Source/Repos/War3MapEditor64/src/Render/Shaders/SkinModel.hlsl)；[场景/模型照明 HLSL](E:/Mycode/Source/Repos/War3MapEditor64/src/Render/Shaders/Scene.hlsl)。
- **F7**：[PoseBatch 实现](E:/Mycode/Source/Repos/War3MapEditor64/src/RenderCore/PoseBatch.cpp)（约 38–77：外部空间、sequence 校验及外部矩阵之后仍求 alpha/RGB）。
- **F8**：[MDX 数据结构](E:/Mycode/Source/Repos/War3MapEditor64/include/War3MapCore/Render/MdxModel.h)；[MDX 解析器](E:/Mycode/Source/Repos/War3MapEditor64/src/Render/Assets/MdxModel.cpp)（`MdxLayer`、`ParseMaterials` 的静态值/轨迹标记边界）。
- **F9**：[既有 Win32 alpha 测试日志](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/reviewed-main-20260930/alpha-win32-ctest.log)；[global 测试日志](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/reviewed-main-20260930/global-win32-ctest.log)；[PE 登记](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/reviewed-main-20260930/alpha-win32-binaries.json)。本轮另只读核对前两份相关程序的 PE 头。
- **F10**：[零周期全局序列研究](E:/Mycode/Source/Repos/War3MapEditor64/docs/MDX_ZERO_GLOBAL_SEQUENCE_2026-09-29.md)；[全局键时间原点后续修正](E:/Mycode/Source/Repos/War3MapEditor64/docs/MDX_GLOBAL_TRACK_ORIGIN_2026-09-30.md)；[粒子原生研究](E:/Mycode/Source/Repos/War3MapEditor64/docs/MDX_PARTICLE_NATIVE_2026-09-29.md)；[仍缺证的原生材质/循环/深度项](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/render-followups-20260930/remaining.md)。

## 12. 最小下一步

请作者先裁定**共享部分的授权**和**是否接受先共同算法、暂留双后端**。确认后，第一份实施工单应只覆盖：独立依赖包、一个冻结模型的原生组/实例/空间映射、同工具链离线夹具。完成这一步，再申请 W2 离屏接入，不立即替换玩家绘制。

最终判断：**可以共用，现有基础有价值；尚不能直接接库即获得游戏等价，更不能据此删除原版计算。应把正确的数据交接与共享绘制边界先做成默认规则，再逐步扩大。**
