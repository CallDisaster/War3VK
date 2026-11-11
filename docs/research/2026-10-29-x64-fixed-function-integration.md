# 2026-10-29：完整游戏接入前的 Factory / Clear / FVF / 固定功能接线

用户本轮顺序：先完整接入 Warcraft，再检查 Storm 高水位与客户端迁移开／关内存。
因此本批没有进行新的进程内存采样，也没有把 10-24 的本地 TLS 减量计作迁移收益。
**本文不是完整游戏、WarVK 全效果、性能或发布验收。**

## 已收紧的数据链

- Factory 查询使用 x64 的原 D3D9Adapter 规则，在创建设备前返回真实物理信息。
  同一 factory 实例/adapter 交给设备创建，查询本身不创建 Vulkan device。
  公开 caps 仍取物理能力与已实现代理语义的交集。
- 子 viewport / 多矩形 Clear 先完整检查并与 viewport 相交，再交给原 DxvkContext。
  颜色和深度分开；不把局部 Clear 解释成整面 Clear。
- 原 FVF decoder、反向映射、声明大小规则提取到共用头。代理支持有界 stream-0
  声明、实际 extent 与紧凑顶点输入，不再硬编码唯一 40-byte 布局。
  UDEC3/DEC3N、多个流、instancing 等仍明确拒绝。
- 原生产 FF key builder 与变换常量准备由本地设备和 x64 共用。
  IPC 携带具名 API 状态、矩阵、材质和灯光标量；不发送 C++ 指针或编译器位域。
  FF/SM3 混用通过 typed resource generation 解析；FF 未声明输入沿原 null-stream 规则。
- FF 使用原 generated uber shader、DXVK context/submission/use tracking。
  首批 FF draw 有同步有序边界，未宣称已具备现有 SM3 批处理性能。
  FF 常量和 null-stream buffer 纳入原 owner 的 use 检查及退出清理。

## 已保留的回执

| 回执目录（AutoTest/artifacts 下） | 结果及含义 |
| --- | --- |
| x64-factory-clear-build-20261029-r1 | 失败：缺少原 d3d9_names 链接对象；未接受该产物 |
| x64-factory-clear-build-20261029-r2 | 原 Factory / Clear 独立构建及 no-work 通过 |
| x64-factory-clear-gpu-20261029-r1 | CPU 测试失败：旧的非法 opcode 上界已成为有效 ClearRegions；修正测试 |
| x64-factory-clear-gpu-20261029-r2 | 六场景通过；x86/x64 各 749,307 CPU 检查，四个渲染场景各 147,456 通道检查；元数据远端场景未初始化 GPU device |
| x64-fixed-function-build-20261029-r1 | 首批 FF 宿主构建通过；不是后续候选的验收 |
| x64-fixed-function-gpu-20261029-r1 | wire CPU 编译失败：纯 FVF 头间接引入 log_debug 的 unused parameter；改为纯 d3d9/标准头依赖 |
| x64-fixed-function-build-20261029-r2 | 修订 FF 宿主构建通过 |
| x64-fixed-function-gpu-20261029-r2 | CPU 各 1,149,647 检查通过；本地 material roundtrip 测试失败，定位为既有 WarVK 强制 Ambient=white |
| x64-fixed-function-gpu-20261029-r3 | 本地纹理变换预期失败：用错输入分量的平移预期；改为确定性的 y 缩放验证。该次仍未到远端渲染 |

同时只读审查发现 host FF 适配层把公共 D3DTSS 编号直接用作内部数组下标；
已改成调用原 RemapTextureStageStateType，不能把 wire 纯往返通过算成该接线已验证。
最终 FF 宿主为
`build64_render_host_fixed_function_20261029_r3/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe`：
5,840,896 B，SHA-256
`593E9494A756228CC9887BAC595DAB17940DD311356A23FCB5D034D6988D132D`。
`x64-fixed-function-build-20261029-r3` 构建、no-work 与受保护产物保全通过。

`x64-fixed-function-gpu-20261029-r4/receipt.json`：

- 六场景全部通过，restricted-Medium 非输入 Desktop、输入/副本/raw 哈希保全；
- x86/x64 CPU 各 **1,149,647** 检查；包含原 FVF 副本差分、定宽 codec、
  截断/NaN/无效状态/半空身份及失败不发布；
- 四个渲染场景每个 **9 Clear + 11 顶点/固定功能画面，327,680 通道**；
  原本地前后两轮、远端同步、远端开启既有 grouped 配置，完整像素哈希全部为
  `4ac8ed27daf1e5ae`；
- 覆盖紧凑 FVF、FF/SM3 两方向混用、无纹理 diffuse、世界缩放、矩阵乘法顺序、
  材质/方向光启停、纹理坐标缩放、POSITIONT 忽略世界矩阵与光照；
- 两个远端渲染场景 VVL 1.4.365 六类检查实际激活，独立回调
  **warning=0 / error=0**；真实 GPU submitted/completed/retired 相等，
  所有作者资源归零、宿主自然退出与 Job 结算；
- grouped 配置仍不表示 FF 已批处理：本轮 FF 有独立同步事务，
  未测游戏性能、帧时间收益或内存。

上述 r4 使用当时参考 DLL；后续共享生产源码的 fresh x86 构建与完整配置回归见下文。
原 census 接线/严格解析 Python 定向 **44/44** 通过。

### 后续故障测试发现的真实缺口与重验

fresh x86 的 r1 构建已通过；以它作参考，GPU r6 的四轮 20 画面对照仍一致。
但新增的真实 IPC 反例/恢复测试在连续绘制后出现白色纹理，故整个 r6 为失败，
不能沿用短场景结果宣称固定功能已完整验收。r5 则是测试编译时 NOMINMAX 重定义，
已加正常 include guard；两个红回执均保留。

检查发现 host 只更新 FF key UBO，优化 pipeline 的 SpecFF* 常量仍为零。
原编译器在优化 pipeline 就绪后会使用专门化常量，这与快链 UBO 路径不同。
已将原 UpdateFixedFunctionPS 的打包段提到生产共用 helper，两种 owner 同用；
host 同步维护 UBO 与 pipeline 常量，在相同 SM3 批次在途时不重写共享常量。
不是通过禁用优化管线规避问题。新增连续正常绘制、20 项非法事务后的恢复、
强制优化管线配方以及双位宽原实现差分；在实际结果落盘前以上仍为候选。

后续 r10 已用更新后的原生 DLL 完成重验：四轮全部 20 画面相同，双位宽专门化
差分各 200,000 检查，36 次正常连续绘制 + 20 次非法请求及其后恢复均通过，
VVL 六类检查实际激活且 warning/error 为 0。强制优化模式现在同时校验两端
实际日志的有效配置；r9 的验证层配方曾覆盖远端配置，故 r9 仅是默认远端模式通过，
不能追认为强制优化。r7/r8 为 CPU 夹具引入既有 DXVK 头时的编译告警门错误，保留。

原生 x86 `x86-fixed-function-build-20261029-r2` 构建/no-work 通过；
`x86-fixed-function-regressions-20261029-r1` 的本次配置 **119/119** 通过，
真实受限 Desktop、源码/原始回执/产品及玩家 DLL 保全通过。数量以本配置为准，
不继承旧诊断配置的测试总数。DLL 为 24,420,352 B，
`ED72F52E6466C4E80565A55444912390F494810A7D898690C72BDD1B18B3AFB7`。

### 多级 BGRA8 纹理接线（隔离 GPU 对照通过）

已加入 BGRA8 managed 2D 的独立子资源布局、levels=0 全链、非整幂尺寸、
局部/只读 LockRect 与逐纹理 LOD；尺寸最多 1024，每个上传事务最多一个完整
4 MiB level。仍不支持压缩纹理、RT 纹理、GetSurfaceLevel 和完整 sampler。

调用点保留原生 pitch，wire 只发紧密行。宿主每连接只有一份带资源代际与递增
token 的未提交 patch；连续块完整后才交给原 DxvkContext。短包、重复偏移、
陈旧代际或不完整 commit 不改变 GPU 图像；abort/destroy 清掉 patch。首次局部
写入会用代理已初始化的本地 backing 补齐该层，不允许未初始化 mip 被采样。
这批仍有 x86 managed backing 与上传复制，不能称为已经获得内存节省。

`x64-texture-levels-build-20261029-r2` fresh 构建/no-work 通过；宿主为
5,853,184 B / `089A0BAA002030AE7F50072D200A4DCFEEC9220A96025D75157F71FD218BA1D5`。
同源的 `x64-texture-levels-gpu-20261029-r2`（强制优化管线）及 `r3`（默认管线）均通过：

- 两宽 wire CPU 各 1,150,226 检查，原专门化算法差分各 200,000 检查；
- 本地前后、远端同步/grouped 四场景各 31 张图 / 507,904 通道，
  整图哈希均为 `8c589e78d406e5ae`；实际覆盖 257×129 的 9 个 mip、局部更新与 LOD；
- 每个远端对照 23 draw，原 GPU submitted/completed/retired 相等、作者资源归零；
- 真实 IPC 故障门含 20 个 FF 反例与 10 个纹理事务反例，共 60 个合法 draw，
  终态计数、拒绝后恢复、取消与销毁重建通过；
- VVL365 六类检查实际启用，独立回调 warning/error 为零；受限 Desktop、自然退出、
  原始输出和受保护文件保全通过。没有测游戏/Storm 内存。

GPU r1 的代理编译失败（DWORD/uint32_t 的 min 推导、ReadSelf 声明顺序）保留；
修复后 fresh host r2 与上述两门绑定同一源码，不追认失败身份。

### 真实格式与表面接口（隔离 GPU 对照通过）

下一批沿同一事务/资源所有者扩展到 16 个线性、亮度/Alpha、16 位及 DXT1–5 格式。
宿主先通过原 D3D9Adapter 的物理格式检查，再使用原 ConvertFormatUnfixed 映射和 swizzle；
块压缩 pitch、末级 mip 与边缘局部块域必须同格式同行。新增 GetSurfaceLevel / GetContainer，
表面独立公共身份借用纹理私有寿命，逐 mip 共享 LockRect 状态。
这批独立构建 `x64-texture-formats-build-20261029-r1` 通过：宿主为
5,857,280 B / `AF4AE576425620E61D8DB95378C217929033E648878ACEA7DAE666F6B1D0FA9E`。
`x64-texture-formats-gpu-20261029-r2/r3` 分别验证强制优化和默认管线，均通过：

- 双位宽 wire 各 1,171,067、专门化各 200,000 检查；
- 四个对照各 207 张完整图 / 3,391,488 通道，哈希均为 `cb4b8fea1843a5ae`；
- 新增 16 格式、每个完整 8 层至 1×1、局部更新/邻域保留、304 项表面身份与保活检查；
- 20 FF + 17 纹理事务反例、各自合法恢复；raw 宿主 61 draw / 37 拒绝，像素失败 0；
- VVL365 实际六类激活、独立 warning/error=0，GPU/use 退休、自然退出及保全通过。

该门 r1 的测试链接缺少 IID_IUnknown 定义，改为 __uuidof(IUnknown) 后重跑，红回执保留。
原资源寿命回归 r1 的实际 COM 轮完成，但旧读方仍期待 2244 次消息；新的纹理事务为
Begin/Chunk/Commit，每纹理增加 2 次纯 CPU 协议操作，96 轮实际 2436 次，GPU 后端调用
2242 次/提交 673 次保持不变。已按这条派生关系更新精确断言，不改成宽松下限。
`x64-texture-formats-lifetime-20261029-r2` 已重跑通过：96 轮 / 576 资源、256 Query、
319,488 像素检查；接受 2436、后端调用 2242、提交 673，原始输出、源与玩家 DLL 保全。

该身份仍不覆盖 managed 非块对齐压缩锁域与完整 sampler；下一批结果见下。
dynamic/default/systemmem、autogen、cube/volume、render-target 纹理与有限 1024 尺寸限制仍待处理。

### 采样器与压缩锁域（首轮 GPU 对照通过）

两端共用原 BindSampler 的 key 构造，不复制新的寻址/过滤算法。代理保留 API 值，
连续 SetSamplerState 在本地合并；draw 前仅在值改变时发送一条有序消息。宿主记录
draw 时绑定原 DXVK sampler/view；既有命令的描述符仍由原资源跟踪保活。
16 格式纹理按原映射补 sRGB view；managed BC 锁按原语义将非对齐矩形扩到触及的块，
传输仍用对齐后的精确区域，末级边缘不越界。

新 x64 宿主 `x64-sampler-build-20261029-r1` 为 5,857,280 B /
`E7EFBD5E846C61A9927F7D5D57334DCCDFCCC79BD3D13A1D4F8C5A100553BF32`；
本地共享代码构建 `x86-sampler-build-20261029-r1` 为 24,420,352 B /
`26B411E99AAEEB2A53F064E1D221202915CE995C95742C1B9891659FD4C55A69`。
构建/no-work/保全通过。`x64-sampler-gpu-20261029-r3` 强制优化管线通过：

- 双位宽 wire 各 1,180,900，原专门化和原 sampler key 各 200,000 差分检查；
- 四个对照各 225 图 / 3,686,400 通道，完整实际图像哈希均 `ae745aabca6905ae`；
- 18 个新采样用例覆盖寻址、过滤、mip/LOD、各向异性开启与 sRGB；各向异性用例
  是均匀 mip 的等轴导数，不外推斜面质量；
- 20 FF + 17 纹理 + 11 sampler 非法事务及恢复；raw 宿主 72 draw / 48 拒绝；
- VVL365 六类实际激活、独立 warning/error=0，GPU/use 退休、自然退出及保全通过。

保留 r1 测试编译的缩进错误。r2 在原生参考端发现线性采样理论值 127.5 的量化为 127；
只对两个理论半整数 RGB 用例允许 1 LSB，Alpha 与其余用例仍精确判定，四端实际整图
哈希仍要求完全相等。不是放宽远端与本地差异。
随后默认管线 `x64-sampler-gpu-20261029-r4` 同样通过；
`x86-sampler-regressions-20261029-r1` 实际 119/119；
`x64-sampler-lifetime-20261029-r1` 96 轮、576 资源、256 Query、319,488 像素和原 raw
故障恢复通过。96 轮相同 sampler 设置只在首 draw 增加 1 次消息，接受 2437 / 后端调用
2243，GPU 提交仍 673。以上均为受限隔离窗口，不是 Warcraft 完整接入或内存测量。

## Alpha / fog / scissor / separate blend 实际对照

`x64-effects-build-20261029-r2` 宿主为 5,861,376 B /
`5BCCD41A7338DAACADCFEEA671DB19D4691845688519F94100484B20F6477DBF`。
新增 76 B 按值状态块，完整预检后一次发布；使用原 DXVK FF shader、push constants、
blend 和 scissor。变化的专门化 UBO 通过原 buffer storage rename 保留在途旧版本，
不是原地覆盖，也未给 setter 增加 GPU wait。

`x64-effects-gpu-20261029-r3/r4` 强制优化/默认管线均通过：四端各 269 张完整图 /
4,407,296 通道，实际图像哈希完全相等。新增 44 图覆盖八种 Alpha 比较及阈值两侧、
负/空/倒置 scissor、两种 Clear 交集、独立 alpha blend、table/vertex/range fog，
以及不逐 draw 读回的 SM3 状态变化。各远端 274 draws。两宽 wire 各 1,453,405 检查。
原始 IPC 的 60 个非法请求与恢复通过（新增 12 个 effects 末字段失败，不得部分发布），
raw host 84 draws / 60 rejected / 资源归零；实际 VVL365 无 warning/error，自然退出。

保留红门 `x64-effects-gpu-20261029-r1/r2`：native 第 25 张 effects 图首先失败，原因是
测试初始化 Clear 仍开着 scissor，外部像素合法保留旧红色。背景初始化改成先关裁剪；
同时补出了代理 Clear 原先忽略 scissor 的真实遗漏，增加独立交集图和 262,144 像素
CPU 交集判据。未放宽实际四端图像相等条件。`x64-effects-lifetime-20261029-r1`
随后通过原 96 轮 / 576 资源 / 256 Query / 319,488 像素和 raw 故障恢复门，
source/reference 保持，资源归零和自然退出。
共享 native 源码未变，继续绑定 26B411E9 的 119/119 记录，不转移到别的 DLL。

## 八纹理 stage：默认/优化 GPU 对照通过

新 `SetTextureStages` 为 448 B 完整事务：stage 1..7 的 `{id,generation}` 和各自 sampler，
stage 0 保留既有 wire。x86 私有绑定保活 COM 对象；host 先完整校验表，再发布 sampler/
绑定，并在每个 draw 重新校验代际、上传完成和在途上传所有者。销毁再重用编号不会自动
复活旧绑定。没有向 x64 传 C++ 指针，也未改变 GPU 退休所有者。

原 FF key 接受八位真实绑定掩码，原 DXSO 支持已声明的 2D s0..s7；cube/volume/vertex
sampler 仍拒绝。各 stage 分别选原 sampler key / linear-sRGB view；shader、sampler、
image 都由原 DXVK 跟踪。离散纹理资源槽从 4 扩为 16 供八纹理同时存在，逻辑句柄表仍
64；这是开发协议容量，不是增大 Warcraft 的 Arena 或快照预算。重复状态不发额外消息。

新增 17 图实际比较八层 FF 累加、八个 SM3 sampler、高 stage 地址模式/sRGB、私有
引用保活与公开接口恢复；raw IPC 检查尾字段错误不能发布有效红色纹理前缀、销毁/
重用后旧代际仍拒绝，显式重绑新代际后恢复蓝色。

`x64-multitexture-build-20261029-r2` 宿主为 5,869,568 B /
`C68506CF75DC9505D246517502C2DA76BA9E0505DAF59558857213FE204313D4`。
`x64-multitexture-gpu-20261029-r3/r4` 强制优化与默认两轮通过：四端各 286 图 /
4,685,824 通道、哈希 `2fb80fc6e533b82e` 完全一致，各远端 291 draws。
两位宽 wire 各 1,454,388 检查，shader token 各 6,560，原专门化与 sampler key 差分
各 200,000。raw host 97 draws / 72 rejected / 342 submitted=completed=retired，
资源归零、VVL365 零 warning/error、宿主自然退出。第 7 号绑定参与最终 COM 无环收尾。
`x64-multitexture-lifetime-20261029-r1` 已通过 96 轮 / 576 资源 / 256 Query /
319,488 像素与原 raw 故障恢复；source/reference 保持，资源归零及自然退出。

保留 `x64-multitexture-build-20261029-r1` 失败：显式带默认参数构造函数不能作 std::array
的隐式元素初始化；已改成普通零参默认构造委托显式 bool 构造，原默认值不变。
保留 GPU r1/r2 失败：r1 是新 runner 错设 shader token 检查量 >10,000，实际确定性
套件为 6,560/0；改为精确计数。r2 的 native 已完成新增图像，随后夹具提前释放所有
纹理，违反既有 Teardown 必须保留一个公共资源引用的前提；现将第 7 纹理交给原
Teardown，保持私有绑定，验证纹理保活设备和最终无环释放。未削弱退休/退出要求。

这批没有性能 A/B，FF draw 仍同步，x86 managed texture backing 也仍存在。448 B
状态只在变化时发出，无逐 setter RPC、新全局锁或 GPU wait；这些源码性质不等于
真实游戏性能接受。八纹理上限、64×64 target 和有限资源表仍是开发约束。

## 未关闭的完整接入门

当前仍是有限 64×64 开发 profile。Game.dll 保护仍在；实际游戏尚未通过该代理进图。
下一块必须覆盖：

1. 更多真实纹理格式/表面接口的组合验收，当前 16 格式 mip/分块上传已通过有限测试；
2. 八纹理 stage 之外的更多 sampler/状态组合、完整 caps、拓扑及 UP 路径；
3. 真实 RT/DS/StretchRect、Reset/换图与游戏正常退出（有限 profile StateBlock 已通过后续门）；
4. WarVK 原生光照改写及其增强 pass、Game/JASS/语义数据交接；
5. 同图同相机的全场景等价、完整帧耗时及尾延迟。

本地 SetMaterial 会在 WarVK pipeline 存在时强制 Ambient 为白，SetLight(0) 也有太阳改写。
当前 FF 对照显式使用白 Ambient 和灯 7，验证基础 FF，不等于这些 WarVK 行为已迁移。
不得为进图解除保护、虚报 caps 或省略效果后，把更少工作量当成内存/性能收益。

后续 StateBlock 以原 `d3d9_stateblock.cpp/.h` 和设备实现为语义源：Begin/End 记录期
常规 setter 不得修改当前 draw，getters 仍读取当前状态；记录期拒绝创建/Capture/Apply；
ALL/PIXEL/VERTEX 的捕获集合不同（纹理绑定仅 ALL，材质/变换/viewport/scissor 亦仅
ALL）。原 Capture 会保留旧 stream offset，不能简单复制整个 Device 状态；light
定义和启用变更也有独立规则。使用当前 COM 私有保活和原 GPU 退休，不另造资源寿命。
以上有限 profile 的实现已通过默认/强制优化 GPU 门，另有原 MultiplyTransform 不经
recorder 的特殊规则及无纹理 shader 接线修复。最新身份、失败和验收范围以
[StateBlock 专项回执](2026-10-29-x64-stateblock-integration.md)为准；不外推为完整游戏接入。

## 用户要求的后续内存账

只在完整接入门通过后执行。同一地图/相机/配置/时段的 OFF/ON 对照应分别报告：
Storm requested/live/reserved 高水位与最高地址（地址与字节量分开），游戏进程 private
commit / working set / VA，以及 x64 宿主和 GPU 账。迁出的占用与双进程总开销分列。
场景、截图、DLL/host hash、实际开关和正常退出必须一起绑定；不使用旧 TLS A/B 追认。
