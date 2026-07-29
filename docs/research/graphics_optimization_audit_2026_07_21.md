# War3 图形系统优化质量审查报告（7 路并行取证，原始全文）

> 生成：2026-07-21，7 个并行审查代理对点光源/点阴影/世界光阴影(CSM)/体积雾体积光
> 相关实现逐文件取证。所有问题附 `文件:行号` 证据。行号对应审查时的工作区版本，
> 后续整改可能使行号漂移，以函数名/代码摘录为准。
> 已完成整改项见 AGENTS.md 清单 ✅ 标注；本文档保留全量发现供接手者查阅。


---

# 审查 1/7：体积雾/体积光 CPU 驱动层

> 范围：体积雾/体积光 CPU 驱动层:完整阅读 src/d3d9/d3d9_war3_volumetric_light.cpp(约 1700 行)与 src/d3d9/d3d9_war3_volumetric_light.h。重点:每帧常量/UBO 更新路径、ray-marching 参数计算、CSM blend/interval 计算是否在 CPU 侧有冗余、render target/barrier 管理、与 post-FX 的耦合开销、配置热路径(env/setting 查询)是否每帧重复解析。

已完成全部 1700 行源文件、头文件及相关接口（`War3ShadowReceiverPass` 快照获取、`War3LightManager` 帧快照、配置默认值）的取证阅读。以下为最终审查报告。

---

# 体积雾/体积光 CPU 驱动层优化审查报告

## 【实现概述】

`War3VolumetricLightPass` 是一个 `BeforeUi` 插入点的全屏后处理 pass：每帧 `Run()` 依次执行早退门（开关/强度/密度/采样数/MSAA/深度代数）→ 点光源帧快照获取与 top-K 选择（≤2 盏）→ 分辨率除数与 ray-segment 预算钳制 → `ensureResources`（按 extent/format 缓存的三张图像：color copy、depth copy、低分辨率 effect RT）→ 全屏 color/depth copy → `drawVolumetricLight`（填充 496B CSM UBO + 800B 点光 UBO，device-local buffer 经 transfer barrier 包裹的 `cmdUpdateBuffer` 上传，低分辨率 ray-march draw）→ `compositeVolumetricLight`（全分辨率 depth-guided 合成回 color）。阴影来源为体积太阳 ortho 优先、相机 CSM 回退、点阴影 cube array 可选；pipeline 按 (format, samples) 缓存于 `unordered_map`。整体结构清晰，正确性防御（fail-soft、constexpr debug 开关、静态 ABI assert）做得相当充分。

## 【优化质量问题清单】（按严重程度排序）

### P1 — 点光 ROI 优化是死代码：分支条件永不可达（GPU，高）

`src/d3d9/d3d9_war3_volumetric_light.cpp:1651`
```cpp
if (!hasSunVolume && hasPointVolume && resolutionDivisor == 2u) {
```
但同文件 `:1618-1621` 与 `:1635-1638` 已将除数钳制并只增不减到 `[4,8]`：
```cpp
uint32_t resolutionDivisor = std::clamp<uint32_t>(
    settings.resolutionDivisor, kVolumetricMinResolutionDivisor,  // = 4 (line 27)
    kVolumetricMaxResolutionDivisor);                             // = 8 (line 28)
```
`d3d9_war3_settings.h:234-235` 注释亦明确 "TDR 合同硬下限 divisor=4"。因此 `resolutionDivisor == 2u` 恒为 false，":1652-1671" 的 sphere-region 计算、`UnionRegion`、`HalfResRegionRect`、`EffectRegionToCompositeRect`（:159-200，约 40 行精心写的映射代码）**永远不会执行**。结果：纯点光帧仍跑全屏 1/4 分辨率 ray-march + 全屏 composite——这正是审查目标第 6 条"看起来优化了但路径上仍是全量"的死角。

### P2 — 全屏 color/depth copy 无区域裁剪，且与 ROI 设计脱节（GPU，高）

`:1676-1677` 每帧无条件 `copyColor(ctx, input.colorView); copyDepth(ctx, input.depthView);`。copy 范围在 `:723` / `:793` 写死为整图：
```cpp
copyRegion.extent = srcView->image()->info().extent;
```
两个函数均不接收 scissor/region 参数。1280×720 下每帧约 3.7 MB color + 3.7 MB depth 的拷贝带宽，外加每路 2 组 pipeline barrier（共 4 组）。即使 P1 的 ROI 被修复，copy 仍会全屏执行，区域收缩收益无法兑现。color copy 本身因 composite 读写同一 attachment（反馈环）而难以彻底消除（见改进方向），但 copy 范围完全可以收缩到 composite 区域。

### P3 — 单缓冲 device-local UBO 的每帧 WAR barrier 链（GPU pipeline，中）

`:1209-1245`：两个 UBO（`m_csmUniformBuffer`、`m_lightBuffer`，构造时 `:335-354` 为单个 device-local buffer）每帧执行
```
FRAGMENT_SHADER/UNIFORM_READ → TRANSFER/TRANSFER_WRITE  (barrier 1)
cmdUpdateBuffer ×2                                       (:1234-1237)
TRANSFER/TRANSFER_WRITE → FRAGMENT_SHADER/UNIFORM_READ  (barrier 2)
```
由于缓冲是**单份**的，barrier 1 必须与**上一帧** volumetric draw 的 uniform read 排序——这是真实的跨帧 WAR hazard，每帧在 effect draw 前引入一次 fragment→transfer 的管线排空。`getSliceInfo(0u, …)`（:1136, :1206）也表明始终使用 offset 0 的同一段。

### P4 — 帧内重复计算与重复快照查询（CPU/可维护性，中）

- 太阳强度/颜色 sanitize 与门控在 `Run()`（:1517-1531）与 `drawVolumetricLight()`（:844-865）各算一遍，逻辑近似但不完全同源（一个用 `>= minSunIntensity`，一个用 `>=` 后取 0），存在发散风险；
- `SanitizeVolumetricPointPosition` 对同一盏灯最多算 3 次：选择（:265）、Run ROI 循环（:1659）、UBO 填充（:1168）；
- `GetVolumetricShadowSnapshot` 在 `requireCsmSnapshot && hasSunVolume` 时每帧调用 2 次：Run 的 probe（:1602）与 draw 内正式获取（:877）。getter 本身便宜（`d3d9_war3_shadow_resources.cpp:58-83`，纯成员读），但 `War3CsmData`（4×Matrix4+）按值拷贝了两份。

### P5 — 点光 UBO 无条件全量上传（GPU/CPU，低-中）

`:1236-1237` 无论 `lightUbo.count` 是否为 0，每帧都 `cmdUpdateBuffer` 完整 800B `VolumetricPointLightUniform` 并附带两组 buffer barrier。而 `kVolumetricMaxPointLights = 2`（:31），ABI 中 `lights[16]`（:96）实际最多用 2 项——768B 中有 704B 恒为 0。纯太阳帧的上传与 barrier 纯属浪费。

### P6 — barrier 提交粒度过碎（CPU/驱动调用，低）

`drawVolumetricLight` 内一次帧提交包含 5 次独立 `cmdPipelineBarrier`：effect image toWrite（:1004）、UBO pre（:1232）、UBO post（:1245）、effect toRead（:1373）、外加 copyColor/copyDepth 各 2 组（:718/:749, :788/:819）。effect toWrite 与 UBO pre-barrier 之间没有任何命令，完全可合并为一次依赖提交（1 image + 2 buffer barrier）。

### P7 — Run 的 CSM probe 门与 draw 内判定不一致（可维护性/逻辑，低）

`:1583-1616` 的 probe 只查询相机 CSM 快照，不查询体积太阳 primary 快照；而 draw 内（:882-893）明确 "体积 ortho 优先"。当 volume-sun 已发布而相机 CSM 恰好缺失、且无点光时，Run 会提前 return，draw 内本可走 primary 路径。属于冗余且略过保守的重复门。

### 正面确认（已取证，非问题）

- 调试开关为 `inline constexpr bool`（`war3/core/war3_internal_test_config.h:1042-1046`），无每帧 env 解析；`input.settings` 为上游帧快照，本文件无配置热路径重复解析；
- `HasActiveLights()` 为 atomic 快路径（`d3d9_war3_light.h:191-193`），`GetFrameSnapshot` 同帧缓存命中后仅一次固定大小拷贝（`:229-238`）；
- 点光选择为固定数组插入排序、无堆分配（:295-312）；ray-segment 预算 4M 硬上限 + 除数自适应（:1635-1643）是真实生效的 TDR 防线；
- pipeline、effect/depth/color 资源均按 key/extent 缓存，关闭路径在创建任何资源前 return（:1482-1499）；插入排序 tie-break、fail-lit fallback cube（:598-684，仅首次 clear 一次）均正确。

## 【改进方向】

1. **修复或移除 ROI 死代码（GPU，风险中）**：若 TDR 合同允许纯点光帧使用除数 2（点光 ray 距离短、sample 预算可控），将 ROI 条件改为与预算联动（例如 `!hasSunVolume && hasPointVolume && minimumRaySegments(halfResExtent) <= budget`），并在 `effectExtent` 计算处放行除数 2；若产品决定永不放行，则删除 `:159-200`、`:1644-1671` 死代码以免误导。预期：纯点光场景 fragment 工作量从全屏 1/4 分辨率收缩到灯覆盖区域（通常 <10% 屏幕）。
2. **copy 范围参数化（GPU，风险低）**：给 `copyColor/copyDepth` 增加 `VkRect2D`/region 参数，copy 区域取 composite scissor 外接矩形（含 1 texel 膨胀，与 `EffectRegionToCompositeRect` 同源）。配合改进 1 后，点光-only 帧的 copy 带宽同步收缩一个数量级以上。太阳帧维持全屏不变。
3. **UBO 双缓冲/环形 slice（GPU 管线，风险低-中）**：两个 UBO 各做 2 份轮换（frame parity 选择 slice 与 descriptor offset），pre-barrier 的 srcStage 即可从 `FRAGMENT_SHADER/UNIFORM_READ` 降为 `NONE`（首用后仍为 TRANSFER→TRANSFER），消除与上一帧 draw 的 WAR 排序。风险：descriptor offset 需进 `DxvkDescriptorWrite.buffer`，本代码已用 `DxvkBufferSliceInfo`，改动局部。
4. **提取帧级 PreparedParams 结构（CPU/可维护性，风险低）**：`Run()` 内一次性计算 sanitized sun color/intensity、sunDir/worldUp 归一化、CSM/volume-sun 快照与点光 sanitize 结果，以 const 引用传给 `drawVolumetricLight`，消除 P4 的重复计算与双份 `War3CsmData` 拷贝，同时让两处门控强制同源。顺带将 Run 的 probe（:1583-1616）与 draw 内判定合并为一处，覆盖 volume-sun primary。
5. **点光 UBO 条件上传 + ABI 瘦身（GPU/CPU，风险低）**：`lightUbo.count == 0 && 上帧也为 0` 时跳过 `cmdUpdateBuffer`（barrier 2 的 srcStage 相应改为 NONE）；ABI 允许时将 `lights[16]` 缩为 `lights[4]`（800B→352B），同步 GLSL 与 static_assert。
6. **合并相邻 barrier 提交（CPU，风险极低）**：将 effect toWrite（:1004）与 UBO pre-barrier（:1209-1232）合并为一次 `cmdPipelineBarrier`（1 image + 2 buffer），每帧省一次驱动调用；copy 前后两组 barrier 本已合并，无需动。
7. **composite 颜色回读的长线替代（GPU/内存，风险高，仅记录）**：color copy 的根本原因是 composite 读写同一 attachment。若未来接受双 draw（先乘性 extinction 后加性 scatter，或用独立 accumulation RT 再 blit 回）可去掉全屏 color copy，但多一次全屏 pass，通常得不偿失；仅在带宽敏感平台上值得 A/B。

## 【总体评价】

这是一份**正确性工程显著优于性能工程**的实现：fail-soft 合同、静态 ABI 断言、constexpr 调试面、atomic 快路径、帧级快照缓存、无分配排序与 ray-segment 硬预算都做得严谨且经过思考，CPU 侧没有锁滥用、字符串查找、O(n²) 或每帧堆分配等常见病灶，早退门也确实挡在资源创建之前。但 GPU 侧存在两处实质性损失：其一，精心实现的点光 ROI（约 40 行区域映射代码）因除数硬下限 4 与 `== 2u` 条件矛盾而**整体成为死代码**，纯点光帧实际仍付全屏低分辨率 march + 全屏 composite 的全量成本；其二，每帧两张全屏 copy 无区域参数、单缓冲 UBO 的跨帧 WAR barrier 链，属于"标准但可收敛"的固定开销。修复 P1+P2 是本会话范围内收益/风险比最高的一步；P3-P6 为稳健的常规收敛。建议优先处理 P1（决定放行还是删除），其余按序跟进，且任何 ROI 改动需先用真实点光场景验证 TDR 预算与奇数尺寸光栅语义。

（本报告仅基于实际阅读的 `d3d9_war3_volumetric_light.{h,cpp}`、`d3d9_war3_shadow_resources.cpp` 快照段、`d3d9_war3_light.h`、`d3d9_war3_settings.h` 与 `war3_internal_test_config.h`；未修改任何文件。）


---

# 审查 2/7：体积光着色器 GPU 侧

> 范围：体积光着色器 GPU 侧:war3_volumetric_light.frag 的 GLSL 源码内嵌在 src/d3d9/d3d9_war3_volumetric_light.cpp 中(以字符串形式),另有 composite/后处理相关 shader 可能内嵌在 src/d3d9/d3d9_war3_pipeline.cpp 或 src/d3d9/d3d9_war3_shadow.cpp,请定位并阅读。重点:ray marching 段数与早退逻辑、每段 CSM fetch 次数(普通 8 次/seam 16 次上限)、动态分支与 divergence、可移到 CPU 预计算的量、寄存器压力、alpha edge dead zone 与双线性路径的效率。

取证完毕，以下为最终审查报告。

---

# 体积光着色器 GPU 侧优化质量审查报告

## 【实现概述】

体积光系统由两个全屏 pass 构成：低分辨率（1/4～1/8）ray-march 散射 pass(`war3_volumetric_light.frag`，内嵌 SPIR-V 经 `subprojects/war3fx/shaders/` 编译为头文件，由 `src/d3d9/d3d9_war3_volumetric_light.cpp` 驱动）和全分辨率深度感知双边 upsample composite pass(`war3_volumetric_composite.frag`)。散射 shader 每像素做 4～16 段中点求积，每段最多 8 个纵向 CSM probe(cascade 接缝处二次 fetch，即"普通 8 / seam 16"上限），太阳路径用仿射提升（每像素 8 次矩阵变换换每 probe 两次 vec4 FMA)，点光路径用 ray-sphere 区间预裁剪、每重叠段每灯 2 次 cube 采样；CSM 采样为手工 2x2 双线性 PCF（体积太阳 3x3)。CPU 侧（`War3VolumetricLightPass::Run`）有完整的多级早退链（强度/密度门 → 太阳+点光双门 → CSM 快照 probe → ray-segment 预算门 → 资源创建），再执行 color/depth 全图 copy、双 UBO 更新、散射 draw 和 composite draw。composite 用相对散射/透过率对比度做边缘距离度量，2.5% 死区内退化为双线性。

## 【优化质量问题清单】（按严重程度排序）

### P1 — 点光 ROI 优化是死代码："看起来优化了但永不生效"
- `src/d3d9/d3d9_war3_volumetric_light.cpp:27-28` 定义 `kVolumetricMinResolutionDivisor = 4`，`:1618-1621` 将 `resolutionDivisor` clamp 到 `[4,8]`,`:1635-1638` 的预算循环只会**增大**它；而 `:1651` 的 ROI 分支条件是 `resolutionDivisor == 2u`——恒为 false。所有设置入口同样 clamp 到 `[4,8]`(`d3d9_war3_pipeline.cpp:636`、`war3_shader_api.cpp:891-892`)。
- 后果：`:1651-1671` 的点光球体屏幕区域 union、effect/composite scissor 收窄从未执行；`UnionRegion/HalfResRegionRect/EffectRegionToCompositeRect`(`:135-200`）三个函数及 composite 中 `IsFullRect` 的 `LOAD` 分支（`:1425`）全部为死路径。点光-only 帧仍付出全屏散射 + 全屏 composite + 全图 color/depth copy。

### P2 — 动态索引局部数组，存在 scratch spill 高风险（需验证）
- `war3_volumetric_light.frag:583-603` 预计算 `csmClipOrigin[4]`、`csmClipSlope[4]`(8 个 vec4 = 32 寄存器），但 `sampleCsmCascade`(`:267-275`）用**运行时**索引 `clipOrigin[cascadeIndex]` 访问：`c0` 由 `:367-373` 的 view-depth 循环动态选出，blend 取 `c0+1`(`:418-420`),fallback 循环取 `c0+1..cascadeCount-1`(`:385-397`)。
- GLSL/SPIR-V 中被动态索引的局部数组通常会降级到 local memory(scratch)，每线程 512B scratch 会同时牺牲占用率和带宽——这恰是这段代码注释（`:579-582`）声称要避免的代价。是否真正 spill 取决于编译器，建议用 SPIR-V 反射或驱动查询确认 scratch 用量。

### P3 — 手工 2x2/3x3 PCF，每次可见性查询 4～9 次 texelFetch
- `war3_volumetric_light.frag:194-252`:2x2 路径 4 次 `texelFetch`，体积太阳 3x3 路径最多 9 次。而 CSM 图集和体积太阳 ortho 均为 `VK_FORMAT_D32_SFLOAT` 深度附件（`d3d9_war3_shadow.cpp:1556`、`d3d9_war3_shadow_resources.cpp:155`)——格式上完全支持 `sampler2DArrayShadow` 硬件 PCF，一条采样指令即可完成 2x2 比较滤波。按每段 8 probe × seam 双 fetch 计，CSM 相关 fetch 可从每段最多 64 texelFetch 降到 16 次采样指令。

### P4 — 每帧全图 color/depth copy + 8 个 image barrier，未按实际使用区域收窄
- `src/d3d9/d3d9_war3_volumetric_light.cpp:686-823`:`copyColor` 与 `copyDepth` 各自执行 2 次 barrier + copy + 2 次 barrier，共 4 次 `cmdPipelineBarrier` 调用、8 个 image barrier;copy 范围是整幅图像（`:723`、`:793`),barrier 的 subresourceRange 也是整幅。即使 P1 的 ROI 修复后，这两个 copy 也仍是全帧。两次 copy 的前置/后置 barrier 可以分别合并为 1 次 dependency 调用（4→2 次）。

### P5 — UBO 单缓冲复用，每帧两次 cmdUpdateBuffer + WAR barrier 对
- `src/d3d9/d3d9_volumetric_light.cpp`(`d3d9_war3_volumetric_light.cpp:335-354`、`:1209-1245`):CSM(496B）与点光（800B）两个 DEVICE_LOCAL buffer 终生只有一份，每帧走 uniform-read → transfer-write → uniform-read 的 barrier 对，构成帧间 WAR 串行点。另外 `VolumetricPointLightUniform` 按 16 盏灯分配/上传（800B)，但 CPU 端 `kVolumetricMaxPointLights = 2`(`:31`)、shader 端 `min(lights.u_count, 2u)`(`frag:489`)——实际有效载荷最多 32+2×48=128B，每帧多拷约 6 倍。

### P6 — CSM 快照重复查询
- `d3d9_war3_volumetric_light.cpp:1602`(`requireCsmSnapshot` probe）与 `:877`(`drawVolumetricLight` 内）每帧各调一次 `GetVolumetricShadowSnapshot`，每次拷贝整个 `War3CsmData`(4 组矩阵+参数，`d3d9_war3_shadow_resources.cpp:58-83`),probe 结果被直接丢弃。

### P7 — shader 微效率
- `hgPhase` 用 `pow(denom, 1.5)`(`frag:436`)，太阳路径每像素 1 次、点光路径每段每灯 1 次（上限 32 次/像素）;`denom ≥ 1e-3` 恒正，可等价改写为 `denom * sqrt(denom)`。
- `frag:593-603` 无条件 4 层 × 2 次 vec4 矩阵变换；体积太阳只用 2 层（`:1035-1037`)、无 CSM 时整组写零。
- `frag:824-825` 每段 probe 数下限为 2，对很短的有效段（`acceptedLength` 远小于 `directionalProbeSpacing`）存在过度采样。
- `frag:7` 声明的 `s_color` 在全 shader 中从未被采样（grep 仅命中声明行）,CPU 侧仍为其准备描述符（`d3d9_war3_volumetric_light.cpp:1249`)。
- `m_colorCopy`/`m_depthCopy` 的 image usage 含 `TRANSFER_SRC`(`:469-470`、`:513-514`）但实际从未作为 copy 源；多余 usage 位可能影响部分驱动的压缩布局选择。
- composite 的 `fullSize == effectSize` 快路径（`war3_volumetric_composite.frag:70-74`）在 divisor≥4 下同样永不触发（无害死路径）。

## 【改进方向】

1. **修复或删除 ROI 路径（GPU + CPU 带宽，风险低）**：两选一——(a) 点光-only 帧允许 `resolutionDivisor = 2`（需要复核 `:1651` 注释中"ceil-half contract shared with A1"的奇数尺寸光栅语义，这正是当初加 min=4 的原因）;(b) 若决定不做 ROI，删除 `:1651-1671` 与 `:135-200` 三个死函数，避免维护者误以为该优化存在。预期收益：点光-only 夜间场景散射像素减少大半。
2. **消除动态索引 spill 风险（GPU，风险中）**：先用 SPIR-V/驱动确认 scratch 用量；若证实，把 `sampleCsmCascade` 改为接收按值传入的 origin/slope，选择处用常量索引链（`c0==0 ? clipOrigin[0] : c0==1 ? clipOrigin[1] : …`，编译器生成 select，不进 scratch)，或直接对 UBO `csm.u_lightViewProj[idx]` 动态索引（UBO 动态索引不产生 scratch，代价是每 probe 一次矩阵乘）。注意保留 `:579-582` 提升仿射变换的原始收益。
3. **评估硬件 PCF(GPU，风险中）**：为体积光单独建 compare sampler(`setCompareOp`)+ shadow 采样指令，替换 `sampleShadowVisibility2x2` 的 4/9 次 texelFetch;CSM 与体积太阳均为 D32 深度格式，无障碍。风险：改动 sampler heap 索引契约与着色器 ABI，需要同步离线合同测试；3x3 softRadius 路径仍需手工（可用一次 hardware-PCF + 少量偏移采样近似）。
4. **合并 copy barrier 并按区域收窄（GPU + CPU，风险低）**:copyColor/copyDepth 的前置 barrier 合并为一个 dependency(4 image barrier 一次提交），后置同理；P1 修复后 copy region 用 compositeScissor。每帧省 2 次 `cmdPipelineBarrier` 调用和若干全图转换。
5. **UBO 改环形 slice 或按需上传（CPU + 同步，风险低）**：两个 UBO 改为每帧从环形 arena 取 slice（去掉 barrier 对与 WAR 链），或至少 `cmdUpdateBuffer` 只上传 `32 + count*48` 字节；同时把 `VolumetricPointLightUniform::lights` 从 16 缩到 2(ABI 与 shader 同步改，static_assert 已兜底）。
6. **快照查询去重（CPU，风险极低）**:Run() probe 成功后把 `War3CsmData` 透传给 `drawVolumetricLight`，省一次全量拷贝。
7. **shader 微优化（GPU，风险极低）**:`pow(denom,1.5)` → `denom*sqrt(denom)`;clip origin/slope 预计算循环上限改为实际层数；段长显著小于 probe 间距时允许 probe 下限 1；删除未采样的 `s_color` binding（同步收缩 pipeline layout 与 descriptors)；去掉 color/depth copy 的 `TRANSFER_SRC` usage。

## 【总体评价】

该模块的**工程化防护质量明显高于典型游戏后处理代码**:CPU 侧早退链与 shader 的 `emitNoEffect` 门一一对应且先于任何资源/copy 工作；ray-segment 预算有 CPU+GPU 双重 clamp 且明确声明"外部设置不能恢复旧的无界循环";adaptiveSampleCount 修正了 shadow strength 误伤采样数的历史 bug；段内 probe 从 max 改平均、cascade 接缝 blend true-occlusion、点光 ray-sphere 区间预裁剪等，都是方向正确且已落地的结构性优化；composite 的死区+相对对比度设计用无分支方式保住了双线性快路径。但存在两类值得警惕的问题：一是 **P1 这种"注释里写着优化、实际被 clamp 条件永远关闭"的死角**(ROI 与 composite 快路径各一处），说明优化意图与守门常量之间缺少交叉验证测试；二是 **P2/P3 的底层取舍未经测量验证**——32 个寄存器的仿射提升是否换来 scratch spill、手工 PCF 是否落后于硬件 PCF，都应以一次 SPIR-V 反射 + 实机 profiling 定案。在 P1 修复、P2 验证之前，该 pass 的点光场景开销仍接近全量路径，不宜按"已做 ROI 优化"估算其成本。


---

# 审查 3/7：CSM 世界阴影顶层管理

> 范围：CSM 世界阴影顶层管理:完整阅读 src/d3d9/d3d9_war3_shadow.cpp(约 7314 行)与 src/d3d9/d3d9_war3_shadow.h,以及 src/d3d9/d3d9_war3_shadow_resources.cpp。重点:每帧 cascade split/frustum/receiver sphere 计算开销、SubmitDrawCap 512/DirectRecordCap 256 的批处理路径、shadow map 分配与格式、级联间冗余 raster、culling 效率、点阴影(如有 cubemap/paraboloid)在该文件中的路径。

所有关键行号引用均已逐一核实。阅读与取证工作完成，以下是最终审查报告。

---

# War3 DXVK 世界阴影（CSM）顶层管理 —— 只读优化质量审查报告

**审查范围**：`src/d3d9/d3d9_war3_shadow.h`（818 行）、`src/d3d9/d3d9_war3_shadow.cpp`（7314 行）、`src/d3d9/d3d9_war3_shadow_resources.cpp`（1109 行）、`src/d3d9/d3d9_war3_csm.cpp`（714 行），全部通读；并对 `d3d9_war3_pipeline.h`、`d3d9_war3_scene.h`、`dxvk_cmdlist.cpp`、`war3/core/war3_internal_test_config.h` 做了交叉取证。未修改任何文件。

---

## 【实现概述】

`War3ShadowReceiverPass` 是世界阴影的顶层管理者，挂在管线 `BeforeUi` 插入点，每帧 `Run()` 依次执行：

1. **输入校验与设置解析**——校验相机/视口合法性（允许视口小至全幅 25% 面积），解析 env 覆盖项，计算日夜循环与太阳方向。
2. **CSM 计算**——`War3CsmCalculator::Compute`（`d3d9_war3_csm.cpp`）产出 4 级联的 lightViewProj、split、texel 吸附等。
3. **自适应复用决策**——按 caster 数量动态调整 shadow map 更新周期，叠加 dynamic pose 签名、太阳方向/强度漂移、语义身份 churn、瞬态空 replay 等多重 hold-last-good 门（6308-6397），命中则整帧跳过重画。
4. **renderShadowMap**（2290-3367）——构建/复用 replay 指针列表 → prepare（逐 draw 填充 `m_shadowPreparedScratch`）→ 按 pipelineHash/alphaImageView/positionBuffer/indexBuffer 排序做状态去重（2670-2685）→ 4 个级联 render pass，循环内做 VB/IB dirty-check（2931-2989）与级联剔除（2578-2630）；之后可选最多 4 个 terrain caster mask pass（R8，3045-3266）。
5. **renderVolumeSunShadow**（2130-2277）——体积太阳开启时，通过成员交换复用 `renderShadowMap` 再画 1-2 层独立 ortho 阴影（Run 中 6494-6510）。
6. **renderPointShadow**——点阴影 cube array；CPU 侧剔除/签名计划经 `std::async` worker 与主线程重叠（3541-3577 发起，6429-6438 汇合），GPU 侧逐 face 渲染（最多 4 灯 × 6 面，4386-4646）。
7. **拷贝与全屏合成**——color/depth 拷贝 → motion vector（RG16F）+ shadow visibility（R8，ShadowTAA 输入）→ `drawReceiver` 全屏合成 → outline。
8. **发布与簿记**——`resources.cpp` 用 ensure* 惰性重建全部资源，矩阵 SSBO 按 frameNumber+sceneKey 去重（735-742，12 槽 ring）；reconciliation → stats 双套计数在帧尾发布（5130-5299、7138 附近）。

已确认的良性机制：`thread_local ReplayDrawsCache` 同帧去重（810-944）；scratch 容器跨帧复用（头文件 367-370）；CSM 画循环状态去重与 dirty-check；矩阵 SSBO 按帧去重；点阴影 worker 重叠；自适应更新节奏 + 多重 hold 门；采样器 LOD bias 量化缓存（1240-1270）；多数 env flag 已静态缓存。

---

## 【优化质量问题清单】

按严重度排序，每条附文件：行号与代码摘录。收益类别标注：CPU / GPU / 内存 / 可维护性 / 诊断。

### P1（高开销、结构性）

**P1-1　点阴影 worker 每帧深拷贝整个帧场景（CPU，极重）**
`d3d9_war3_shadow.cpp:3544` `const War3PipelineInput inputCopy = input;` —— `War3PipelineInput` 按值持有 `War3FrameScene scene`（`d3d9_war3_pipeline.h:36`），包含 caster 大结构体数组（每个含 ~5 个 `Rc<>` 原子引用计数字段）+ `palettes` 哈希表（每条 palette 为 256×Matrix4 ≈ 16.4 KB，`d3d9_war3_scene.h:47-50`）。3549 行 lambda `[this, inputCopy, lightSnapshot, draws]()` 又把 `inputCopy` **再拷贝一份**进入闭包。而 `preparePointShadowCpuPlan` 实际只读 `input.settings`、`shadowStats` 三个字段与 palettes 哈希、`frameSerial`；replay 指针已由 `draws` 独立捕获。每帧两次全场景深拷贝（含大量原子增减与可能 MB 级 palette 数据）纯属浪费。

**P1-2　体积太阳路径每帧全量重画且无自适应复用保护（GPU+CPU）**
`renderVolumeSunShadow`（2130-2277）调用 `renderShadowMap`（2238），后者每次都重跑 prepare + 排序 + 剔除全套流程；而 Run 中体积块（6494-6510）的 `wantVolumeSun`（6498-6501）**不含 `reuseLastShadowMap` 条件**——主 CSM 命中自适应复用的帧，体积阴影仍全量重画。叠加 P1-3 的级联豁免，体积路径（只用 1-2 层）的非地形 caster 剔除率为零。

**P1-3　C0/C1 级联对非地形 caster 永不剔除（GPU，最大光栅乘数）**
`d3d9_war3_shadow.cpp:2596-2597`：`if ((cascadeIdx < 2u || s_disableFarCascadeCull) && !terrainDraw) return true;` —— 所有非地形 caster 每帧至少在 C0、C1 各光栅化一次（开体积太阳时更多）。2590-2592 注释说明这是 RTS 俯视镜头下保近处阴影完整的刻意设计，且已有 `DXVK_WAR3_CSM_DISABLE_FAR_CASCADE_CULL` A/B 开关；但它是当前阴影 GPU 成本的最大乘数，值得在改进方向中给出分级方案。地形在 `kShadowCascadeCullTerrainWithBounds=true`（默认 true，`war3_internal_test_config.h:988`）时正常走包围球剔除。

### P2（中等开销或正确性/诊断影响）

**P2-4　GpuSkinDirect 评估在 (级联×draw) / (面×draw) 内层重复执行（CPU）**
`d3d9_war3_shadow.cpp:2797-2798`（CSM 级联循环内）与 `4476-4477`（点阴影 face 循环内）逐 draw 调用 `EvaluateShadowGpuSkinDirectInput`（约 40 项检查，含 `buffer()->info()` 查询）。评估结果与 cascadeIdx/face 完全无关，同一 draw 在一帧内被重复评估最多 4 次（CSM）或 24 次（点阴影）。应上提到 prepare 循环按 draw 评估一次。

**P2-5　点阴影 draw 循环完全没有状态去重（CPU）**
点阴影渲染循环中：`cmdBindPipeline` 每 draw 无条件调用（4488-4489）、`cmdBindVertexBuffers` 无条件（4601）、`cmdBindIndexBuffer2` 无条件（4612-4613）、`getShadowCasterPipeline` 按 (face×draw) 重复哈希+查表（4472）。定向 CSM 路径已有排序+dirty-check 去重（2670-2685、2931-2989），点阴影路径同等工作没做。

**P2-6　GpuSkinDirect 后的"清除重绑"使 direct draw 描述符开销翻倍（CPU）**
CSM（3010-3029）与点阴影（4620-4639）在每个 gpuSkinDirect draw 完成后，都复制 descriptors/pc、清除私有 flag、再次 `bindResources`。已验证 `bindResourcesLegacy` 每次调用都从 pool **新分配** descriptor set 并全量更新（`dxvk_cmdlist.cpp:515`），且循环内每个 draw 本就完整重建自己的 descriptors+pc（CSM 2865-2884 区域）。3011-3012 注释表明其意图是"后续复用兼容 layout 时不能继承上一 draw 的输入租约"——在当前"每 draw 无条件重建"前提下该清除冗余，但它同时是租约安全防线兼计数器语义（3026-3028、4636-4638），删除前必须确认重建不变式并复跑 crash-gate。

**P2-7　体积光开启时发布的每帧 shadow map 统计被体积 pass 覆盖（诊断正确性）**
`renderShadowMap` 入口重置全部 reconciliation 计数（2295-2313）并自增 `m_shadowMapRenderSerial`（3365）。Run 顺序为：主渲染 6446 → 体积渲染 6505 → 帧尾发布 7138 附近。因此体积太阳开启时，对外发布的 `shadowMapDrawnCasters`、各级联 drawn/culled、prepared 等"每帧"数字实际是**体积 pass（1-2 层）**的值而非主 CSM（4 层）；render serial 每帧双增。任何依赖这些计数做回归判断的 crash-gate/报表都会读错。

**P2-8　全屏 pass 与拷贝一律用整幅 extent 而非相机视口（GPU/CPU）**
输入校验允许视口小至全幅 25% 面积（516 附近），但：`drawReceiver` renderArea/viewport/scissor 恒为整幅（4911-4913、4919-4932）；`renderMotionVectors`（1824-1826）、`renderShadowVisibility`（1997-1999）、`copyColor`（resources 997）、`copyDepth`（resources 1072）同样整幅。小视口场景最多浪费 ~4× 拷贝与全屏光栅带宽。

**P2-20　`std::async(std::launch::async)` 每帧新建线程（CPU）**
`d3d9_war3_shadow.cpp:3547`——MSVC 实现下每次调用都新建并销毁一个 OS 线程。点阴影激活期间每帧一次线程创建/销毁，应改为持久 worker + 条件变量。

### P3（轻量，但量大或值得收口）

- **P3-9（CPU）**：每帧两次 `EnvIntOverride`（5661-5662 `DXVK_WAR3_SHADOW_DEBUG`、5674-5677 `DXVK_WAR3_POINT_SHADOW_DEBUG_LIGHT`），其实现（376-378）每次 `env::getEnvVar` = getenv + `std::string` 分配。其他 flag 均已静态缓存，这两个应照此办理。
- **P3-10（CPU）**：`selectStableWorldUp` 每次调用都计算 `inverse(camera.view)`+打分（csm 140-148），但 `s_forcedUp` 静态初值为 1（153），171-172 行直接强制 zUp，打分结果完全被丢弃；且最终结果只在首次 init 使用（176-178）。155-165 的 env 读取分支（`s_forcedUp == -2`）永不可达，是死代码。另外 `invProj * ndcCorners[i]` 与级联无关却在级联循环内重算（csm 341-343）；Run 路径还有重复 `inverse(view)`。
- **P3-11（CPU）**：每个阴影 draw 前后 `War3RenderState::SetTlsBatchHandle` 写/清（2993-2999、3002-3007）。grep 证实其读取方仅在 `d3d9_device.cpp` 的 D3D9 draw 路径（22768/31199/33172），`ctx->cmdDraw*` 录制路径不经过——此路径下为死开销。
- **P3-12（CPU）**：`BuildShadowReplayDraws` 按值返回 `cache.draws`（825、859、867、943），cache 命中时仍每次拷贝 N 个指针；每帧调用 2-3 次。返回 `const&` 即可（thread_local 寿命足够）。
- **P3-13（内存/稳定性）**：`ResolveAdaptiveShadowResolution`（979-1000，`kShadowAdaptiveResolutionEnabled` 默认 true，config 931）无迟滞；geometryWork 在阈值附近振荡会反复重建 shadow map + mask + 8 个 view，并使 `m_hasCompleteShadowMap` 失效（6025-6038、6411），引发资源抖动与 hold 门失效。
- **P3-14（GPU）**：render pass 拆分偏多：4 个级联 pass + 最多 4 个 terrain mask pass（depth LOAD，3045-3266）+ 点阴影逐 face begin/end 最多 24 次（4386-4646）。terrain mask 可并入主 pass 做 MRT（须保持"地形最后、depth test 不写深度"语义）；motion vector + visibility 也可合并或并入 receiver。
- **P3-15（GPU/barrier）**：每帧约 25-30 次 `cmdPipelineBarrier`；阴影 UBO 更新 barrier（6979-7003）与 light+pointShadow UBO 批次（4862-4894）分离；copyColor/copyDepth 各 2×2 图像 barrier。存在合并空间。
- **P3-16（内存）**：D32_SFLOAT 遍地：CSM 4×2048²×4B = 64 MB（resources 610）、点阴影 cube 预算 96 MB（resources 882，注释 3579-3582）、体积太阳 2×2048²×4B = 32 MB（resources 155 附近）。可选 D16/D24 档位减半，有精度/闪烁风险需实图验证。
- **P3-17（CPU）**：点阴影 content signature 对每 draw 混合 ~70 字段（3739-3796，`mixMatrix` 16 float 在 3794），但 `hasDynamicCaster` 几乎恒真时强制全 face 更新（3820-3828），签名混合结果形同虚设；可用 stats 已知字段提前跳过。
- **P3-18（CPU）**：`m_shadowPreparedScratch.clear()+resize(casterCount)`（2437-2438）值初始化全部条目，每次 `renderShadowMap` 调用一次；体积光帧 ×2。
- **P3-19（可维护性）**：reconciliation → stats 双套簿记每帧约 180 次字段写（5130-5299）；6496-6497 疑似冗余防御（`m_csmData` 在 6399-6401 已赋值为同一 `newCsm`，cascadeCount==0 时再赋同值是无操作）。

---

## 【改进方向】

与问题清单一一对应，标注收益类别与风险等级。

1. **（对应 P1-1，CPU，风险低）** 点阴影 worker 改为捕获小 POD：`settings` 值拷贝 + `shadowStats` 所需 3 个字段 + palettes 哈希 + `frameSerial`；replay 指针维持现有 scope-exit 守卫模式。同时把 `std::async` 换成持久 worker（合并 P2-20）。完成后复跑点阴影 crash-gate。
2. **（对应 P1-2，GPU+CPU，风险中）** 体积太阳路径：① `wantVolumeSun` 纳入 `reuseLastShadowMap` 与自适应更新节奏；② prepare/排序结果按帧缓存，供主 CSM 与体积两次 `renderShadowMap` 共享；③ 体积专用的 1-2 层允许 `intersectsCascade` 真实生效（绕开 cascadeIdx<2 豁免）。剔除错误会丢体积阴影柱，必须实图对比验收。
3. **（对应 P1-3，GPU，风险高）** C0/C1 引入保守剔除分级：至少做"包围球在级联 frustum 外才剔"的保守球测试（现有代码即此逻辑，只是被豁免），或按 ObjectKind 降采样。用既有 `DXVK_WAR3_CSM_DISABLE_FAR_CASCADE_CULL` 开关做 A/B 对照，重点看高镜头树影/单位是否漏杀。
4. **（对应 P2-4/P2-5，CPU，风险低-中）** `EvaluateShadowGpuSkinDirectInput` 上提到 prepare 循环按 draw 评估一次并存入 scratch；点阴影路径补齐与定向路径同级的 pipeline/VB/IB dirty-check 去重（复用现有排序键即可）。
5. **（对应 P2-6，CPU，风险中）** 删除 direct draw 后的清除重绑，改为在源码注释/断言中固化"每 draw 无条件重建 descriptors"不变式；或保守地改为下一次 bind 前的覆盖而非事后清除。必须复跑 gpu-skin crash-gate 验证租约不串扰。
6. **（对应 P2-7，诊断，风险低）** `renderShadowMap` 的计数重置改为按调用方标签（Main/Volume）分桶累加，或体积路径使用独立计数器；render serial 区分语义。
7. **（对应 P2-8，GPU/CPU，风险低）** copyColor/copyDepth/drawReceiver/motion/visibility 全部裁剪到相机视口矩形；注意验证视口偏移与 D32 拷贝对齐。
8. **（对应 P3-9~12、17、18，CPU，风险低）** 两个 debug env 静态化；`selectStableWorldUp` 早退（s_forcedUp==1 直接返回 zUp，删除死分支）；`invProj*ndcCorners` 移出级联循环；删除阴影路径的 `SetTlsBatchHandle` 写清；`BuildShadowReplayDraws` 返回 const 引用；点阴影签名在 `hasDynamicCaster` 时跳过混合；scratch 改为按需 `resize` + 逐槽赋值或改为 `resize_uninitialized` 语义。
9. **（对应 P3-13，内存/稳定性，风险低）** 自适应分辨率加迟滞/粘性量化（如进入降档需连续 N 帧超阈、升档需连续 M 帧低于回滞阈值）。
10. **（对应 P3-14/15，GPU，风险中）** terrain mask 并入主级联 pass 做 MRT；motion+visibility 合并；UBO barrier 合并为一次。逐一做 crash-gate。
11. **（对应 P3-16，内存/GPU，风险中）** 增加 D16/D24 可选深度格式档位，实图验证阴影精度与闪烁。
12. **（对应 P3-19，可维护性，风险低）** reconciliation → stats 改为结构体整块赋值或宏生成；删除 6496-6497 冗余行。

**建议优先级**：1、6、8 先行（低风险、纯 CPU/诊断收益且能净化 crash-gate 读数）→ 4、5、7、9 → 2、3、10、11（需实图/ABBA 门）。任何涉及剔除与租约语义的改动（2、3、5）都应先过隔离 crash-gate 再谈性能数字。

---

## 【总体评价】

这是一套**设计成熟度很高**的阴影顶层管理代码：发布路径 fail-closed（任何一步失败都安全失效而非发布脏状态）、自适应复用与多重 hold-last-good 门体系完整、worker 重叠、thread_local 缓存、排序去重、SSBO 按帧去重、采样器量化缓存等机制表明作者对每帧 CPU/GPU 成本有系统性意识；簿记与 reconciliation 的覆盖率在同类代码中属于上游水平。

当前的主要浪费集中在**"安全冗余型重复劳动"**：点阴影 worker 为读几个字段而每帧两次深拷贝整个帧场景（P1-1）；体积太阳路径借"成员交换复用 renderShadowMap"之名行全量重画之实，且不受自适应复用门保护（P1-2）；direct draw 的清除重绑把描述符分配/更新做两遍（P2-6）；GpuSkinDirect 评估与管线查询在 (级联/面 × draw) 内层重复（P2-4/P2-5）。这些都不是算法错误，而是防御性写法叠加演进后产生的冗余，多数可以低风险收敛。GPU 侧最大的单一乘数是 C0/C1 零剔除（P1-3）——这是刻意设计，建议以保守球剔除 + 既有 A/B 开关对照的方式渐进收口，不宜一步放开。

另外两条会影响"看数字"的问题值得优先修：体积光开启时发布的 shadow map 统计被体积 pass 覆盖（P2-7），会让依赖计数的 crash-gate/报表误读；`EnvIntOverride` 每帧 getenv（P3-9）与每帧线程创建（P2-20）则是最便宜、最无争议的速赢项。

**审查限制声明**：receiver/caster 的 GLSL 源码不在本次范围（只见到嵌入 SPIR-V blob，着色器内部效率未评）；`renderUnitOutline` 位于 `d3d9_war3_shadow_outline.cpp`，超出指定范围未评；涉及的默认值已核实（`kShadowAdaptiveResolutionEnabled=true`、`kShadowCascadeCullTerrainWithBounds=true`、`kShadowReplayCasterCapEnabled=false`，见 `war3_internal_test_config.h:931/988/1024`）。


---

# 审查 4/7：阴影渲染核心与运行时合同

> 范围：阴影渲染核心与运行时合同:完整阅读 src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp(约 9492 行)与 src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp(约 4085 行)及对应头文件。重点:capture/commit/replay 每帧的验证与记账开销、receipt/fence 机制成本、per-draw 状态比较、容器重建 vs 复用、诊断计数器在 release 路径上的成本。

两个文件及对应头文件已全部读完（renderer_core.cpp 9492 行 + runtime_contract.cpp 4085 行 + 两个头文件），关键声明均已用 grep 复核。以下为最终审查报告。

---

# 图形优化审查报告：war3_shadow_renderer_core.cpp / war3_shadow_runtime_contract.cpp

## 【实现概述】

这两个文件构成 War3 阴影管线的「语义合同层 + 提交帧构建器」：`war3_shadow_runtime_contract.cpp` 负责从游戏 registry/缓存采集每帧的 ShadowFrameManifest、模型资源库（几何/材质深拷贝快照）、姿态库（matrix palette）与 attachment 刚性身份，并提供 reader 侧的快照接口；`war3_shadow_renderer_core.cpp` 的 `ShadowValidationRuntime` 以分块增量方式（每块 8 条记录 / 2000µs 预算，`renderer_core.cpp:9270-9280`）把 manifest 记录解析成 ShadowSubmissionFrame 并提交上层后端。整体设计意识是好的：指针哈希索引、thread_local 缓存、分块预算、preview cap、dedup 集合等机制随处可见。但热路径上仍存在大量整记录深拷贝、全容器线性扫描、逐字节哈希和无开关的逐记录计时/统计，使得「采集—解析—提交」三段各自的实际复杂度远高于名义上的 O(N)。

## 【优化质量问题清单】（按严重度排序）

**P1 — attachment 快照链路每次调用全量重建 + 多级 O(A×(R+M)) 扫描**
`war3_shadow_runtime_contract.cpp:3995-4032`（snapshotAttachments）与 `4039-4083`（snapshotBundleShared）每次调用都经 `BuildLiveAttachmentStore`（`2890-2911`）对每条 attachment 跑 `RepairAttachmentRigidIdentity`（`2763-2888`：数十次 registry 查找 + `2716/2824` `getAllObjects()` 整 vector 拷贝 + `2280-2316` `shadowRegistry.snapshot()` 拷贝与 manifest O(N) 扫描 + `2533-2589` `FindManifestAttachmentIdentityCandidate` O(M) 线性候选扫描）和 `PopulateAttachmentChildSemanticKey`（`2048-2267`）。`requestLatestFrameBuild()`（`renderer_core.cpp:9069-9077`）每帧至少调用一次 bundle 快照，reader 侧另有调用方 → 每帧 O(A×(R+M))。叠加 `ShouldPreferLiveAttachments`（`1824-1839`）两次 O(A) 计数。

**P2 — find 重载默认整记录拷贝，热路径大量命中**
`war3_shadow_runtime_contract.cpp:3031-3106` 的 `findByRuntimeGeoset/findByRuntimeGeosetData/findByRuntimeModel/findByModelResource(void*, Record& out)` 用 `out = *record` 拷贝含 positions/normals/indices/uvLayers 等 7+ 个 vector 的几何记录；`ShadowPoseStore::findByRuntimeModel/findBySceneNode/findByUnitPtr`（`3134-3162`）拷贝含最多 256 个 Matrix4（约 16KB）的 matrixPalette。指针版重载已存在却很少被用，拷贝版调用点包括：`renderer_core.cpp:2784/2789/2833/2838/2862`（pose 查找）、`7135-7141`（attachment 补充循环 0..128，每轮 2 次几何整记录拷贝）、`runtime_contract.cpp:886`（HasSnapshotPoseForRuntimeAlias）。这是典型的「为调用方方便牺牲性能」的接口设计失误。

**P3 — buildFrameChunk 的 attachment 补充段是 O(A×R) 全扫描**
`renderer_core.cpp:7024-7164`：每个 attachment 先 128×2 次带整记录拷贝的哈希查找（P2），失败后 `7145-7159` 直接全量线扫 `resources.records()` 做 modelResource/modelKey 匹配。cap 16/128 只限制 resolved 数量，不限制扫描成本。

**P4 — manifest 采集段多重 O(S×G×M) 且预算帽在扫描之后**
`war3_shadow_runtime_contract.cpp:1465-1675` AppendRootUnitSupplementRecords：5 个 registry 全量 snapshot（`1514/1524/1536`），每个 seed 的 `TryMergeRootUnitSeedFromRegistries` 约 15+ 次查找 × 3 个 alias，且 `1631` `ManifestHasRootUnitSupplementRecord` 对每个 geoset 做 O(manifest) 线性查重。96/8/64 预算帽限制的是产出，扫描在帽前已发生。

**P5 — preview cap 路径每记录重复昂贵查找 + O(P²) 去重**
`renderer_core.cpp:8971-9055` MaybeCapPreviewManifest：每记录先 `ApplyPreviewCapResourceHints`（`9001`）再 `ScorePreviewCapRecord`（`9003`），两者各自调用 `TryFindPreviewCapResource`（`8614-8690`，最多 6 次 store 查找 + 1 次 `findRuntimeModelOwner` 缓存穿透）——同一记录查两遍。`PreviewCapPoseAvailability`（`8859-8888`）用 `pushUnique`+`std::find` 构建 6 个 vector，姿态数为 P 时是 6×O(P²)。

**P6 — pose-only 发布每次深拷贝整个 manifest**
`war3_shadow_runtime_contract.cpp:3890-3960` capturePoseOnlyLiveState 在 `3942` `make_shared<ShadowFrameManifest>(*manifestPtr)` 深拷贝全部记录，只为改 frameSerial/publishRevision 两个标量。虽有意的注释（`3944-3947`）说明不触发 rebuild，但拷贝本身每 pose tick 都在发生。

**P7 — resource store 每 revision 全量重建，构建期 O(G×N)**
`war3_shadow_runtime_contract.cpp:3675-3696` buildResourceStore：`ConvertGeoset`（`635-663`）深拷贝每个 geoset 的全部几何 vector；`bindRuntimeModelAlias` 存在 O(N) 兜底扫描（`3001-3028`）。有 30 帧冷却复用（`3702-3716`），但 scene-submission 路径 `allowCooldownReuse=false`（`3706-3707`）使其形同虚设。

**P8 — release 路径无编译期开关的逐记录计时与近百项统计**
已用 grep 确认整个 `renderer_core.cpp`（9492 行）**没有任何 `#if/#ifdef`**。`ScopedMaxUs`（`2626-2642`）、`ResolvePhaseTimer`（`7239-7250`）、`6988-6995` 的逐记录 chrono、buildFrameChunk 内每记录的预算检查，合计每条 manifest 记录 12+ 次 `steady_clock::now()`；ShadowResolveStats 近百个计数器无条件更新。分块增量设计（`9270-9280`，8 记录/2000µs）本身正确，但计时粒度过细，开销与被测对象同量级。

**P9 — 同一记录的 material signature / layer contract 重复解析**
`renderer_core.cpp:7356/7382/7407/7522/7567/8396` 多处对同一 record 重复调用 BuildShadowMaterialSignature（每次 FNV 哈希十余字段，且 `1531-1538` 可能触发 `IsReadableRange(layerState)` + memcpy 探测）；`TryResolveMeshLayerBindingContract`（`1858-2055`，一长串 SafeRead）在 finalizeResolvedPacket、`6333`（TryConvertUpperLayerResolvedItem）、`1546-1553`（BuildShadowMaterialSignatureForRenderable）重复执行。结果应缓存于 record/recordIndex。

**P10 — palette 槽位缓存 4096 项线性扫描 + 伪帧号语义错误**
`renderer_core.cpp:696-731`：每个蒙皮 draw（`6408` TryBuildRuntimeGroupPalette 调用）对 thread_local 4096 项数组做线性查找。且 `707/723` 把 `g_paletteSlotCacheHitCount`（命中计数器）当作帧号写入 `lastUpdateFrame`——语义错误（当前无害是因为淘汰靠 round-robin 索引而非该字段），但说明该缓存缺少真正的失效策略。应改为指针哈希索引。

**P11 — 静态网格数据查找三轮全表扫描 + 无界缓存**
`renderer_core.cpp:515-618` TryFindStaticMeshDataResource：primitive 序列比较 O(P²) + `MeshPrimaryStreamMatchesResource` 8 采样点比较，三轮扫全部 records。缓存（`327-336`）key 仅含 resourceRecordCount/streamPtr/stride/meshIndex，且为 static `unordered_map` 永不清理，长跑无界增长。

**P12 — 分块发布会每块深拷贝累计的部分帧，快照接口保留深拷贝旧版**
`renderer_core.cpp:9288-9296`：每个产生 draw 的构建块都 `make_shared<ShadowSubmissionFrame>(buildWork->frame)` 拷贝**截至当前的整个 draws 数组**——N 条记录的帧约 N/8 个块，总拷贝量 O(N²/16) 个 packet（每个 packet 内含多个 vector/shared_ptr）。`snapshotFrame()`（`9464-9467`）同样整帧深拷贝，shared_ptr 版 `snapshotFrameShared()`（`9469-9473`）已存在但旧接口未退役。`runtime_contract.cpp:3962-3986` 的 snapshotManifest/snapshotPoses/snapshotResources 亦同。

**P13 — 逐字节 FNV 哈希 12KB palette**
`renderer_core.cpp:733-742` HashMatrixPalette 对最多 256×48B 逐字节循环，每 packet 至少一次（`7818-7821/8382-8385/6345-6348`）；`runtime_contract.cpp:693-699` HashBytesFnv64Append 同样逐字节，且 `731-735` 在 capture 期对每个矩阵逐次调用。8 字节步进可降 8 倍指令数。

**P14 — 其余点状问题**
- `renderer_core.cpp:6414` tryEngineDirectPosePalette 每个蒙皮 draw 调一次 `GetModuleHandleA("Game.dll")`，应 static 缓存（句柄进程生命周期内不变）。
- `runtime_contract.cpp:372-409` DemandFillVisibleUnitGeosetBindings：`capturedThisFrame>=64` 用 `continue` 而非 `break`（空转扫完全表）；`407-408` 第二遍对全部 record 无差别 Backfill。
- `renderer_core.cpp:1706-1856` CollectDynamicAuxStreamCandidates 每次调用堆分配重建；`4846-4857` MakePrioritizedCompactRemapTables 全量拷贝+stable_sort，在 `4641/5678/5903` 三处分别重跑；`5691-5781` 四重循环（候选×stride×offset×bias×64 采样×tuple 解码）——仅非 scene-submission/救援路径，scene 路径 `8133` kPreferCanonicalGroupPaletteOnly 会跳过，属可接受的冷门路径但仍浪费。
- `renderer_core.cpp:7174-7199` submitFrameLimited 每 draw 4 次虚调用（ensureGeometry/ensurePalette/ensureMaterial/submitDraw），无批量或句柄预取。

**值得肯定的正确优化**（不应在重构中回退）：`runtime_contract.cpp:184-247` 的 256 项 thread_local geoset 解析缓存；`61-75` CachedPointerBool 4096 槽指针哈希；`537-605`/`3569-3628` 的 64 阈值 hash 索引与拷贝去重；`3507-3552` sameFrame 双层早退；`990-1082` SupplementPosesFromLiveCModels 的 768 预算 + 64 冷扫；`renderer_core.cpp:2686-2702` scene 路径 pose 查找提前截断；`8516-8558` 的 revision-grace 帧优选与 `8447-8460` 的 miss-only 预览帧抢占（`9130-9150`）是有真实并发考量的设计。

## 【改进方向】

1. **消灭 find 拷贝重载（CPU + 内存，风险低）**：删除或 deprecated `runtime_contract.cpp:3031-3162` 的 `Record& out` 版本，全部改指针版；调用点机械替换。唯一风险是某些调用方确实需要私有副本（如 `8650-8655` 的 ownerResource 模式），这些保留显式拷贝并加注释。预期是清单中收益/风险比最高的一项。
2. **attachment 快照增量化（CPU，风险中）**：`BuildLiveAttachmentStore` 改为按 registry/manifest 版本号缓存上次结果，仅对版本变化的 attachment 重跑 Repair/Populate；`getAllObjects()`/`snapshot()` 返回 const 引用或 span 而非 vector 拷贝。风险在于身份修复逻辑依赖全局状态，缓存键必须覆盖所有输入版本，需先用现有 crash-gate/lifecycle 门验证。
3. **给 resources.records() 建 modelResource/modelKey 二级哈希索引（CPU，风险低）**：消除 `renderer_core.cpp:7145-7159`、`runtime_contract.cpp:1631`、`2533-2589` 的线性兜底扫描；store 构建时一并维护，成本摊入已有的全量重建。
4. **统计与计时编译期/运行期双开关（CPU，风险低）**：给 ScopedMaxUs/ResolvePhaseTimer/逐记录 chrono 包 `constexpr bool kShadowResolveProfiling`（默认 false），ShadowResolveStats 保留计数但把每记录 chrono 降为每块一次。零行为风险，纯预编译改动。
5. **signature/contract 结果按 recordIndex 缓存（CPU，风险低-中）**：在 buildFrameChunk 作用域内建 `unordered_map<uint32_t, Signature/Contract>`，替换 P9 的重复解析；注意 layerState 指针失效场景需随 frameSerial 一起作 key。
6. **preview cap 单遍化（CPU，风险低）**：`MaybeCapPreviewManifest` 每记录只跑一次 `TryFindPreviewCapResource`，把结果同时喂给 hints 与 score；`PreviewCapPoseAvailability` 的 6 个 vector 改 `unordered_set<void*>`（P²→P）。
7. **部分帧发布改「追加游标」而非整帧拷贝（CPU + 内存，风险中）**：`m_lastRenderableFrame` 维护 shared 帧 + atomic 的 publishedDrawCount，发布即推进游标；完成时 move 一次。需处理 `ShouldPreferRenderableSubmissionFrame` 的并发读一致性。
8. **哈希改 8 字节步进 + palette 槽位缓存改哈希索引（CPU，风险低）**：两处 FNV 机械重写；槽位缓存用 `renderablePart` 指针哈希 512 桶。顺带把 `lastUpdateFrame` 改接真实 frameSerial 或删除该字段。
9. **静态网格查找缓存加容量上限与失效（内存，风险低）**：`renderer_core.cpp:327-336` 的 static map 加 LRU 或随 resourceStore revision 清空。
10. **退役深拷贝快照旧接口（可维护性，风险中）**：`snapshotFrame()`/`snapshotManifest()` 等整拷贝版本标记 deprecated 并迁移调用方到 shared_ptr 版；涉及本文件外调用方，需先全局搜索引用。

## 【总体评价】

这是一份「框架设计优秀、微观执行粗糙」的优化代码：分块增量构建、preview 预算、miss-only 抢占、revision-grace 帧优选等机制说明作者对渲染线程的实时性约束有正确理解，指针哈希索引和 thread_local 缓存也用在了不少对的地方；但热路径上系统性地存在整记录深拷贝、全容器线性扫描、逐字节哈希和无开关逐记录计时这四类反模式，且多数集中在「采集→解析→提交」三段各自的兜底/补充/预览分支里，使名义 O(N) 的管线在 attachment 多、pose 多、预览记录多的真实场景退化为 O(N²) 甚至 O(N×M)。这些问题彼此独立、可逐项修复，优先级建议按 P1/P2（attachment 快照链 + find 拷贝）→ P3/P5/P12（构建期放大）→ P8/P9/P10/P13（逐记录常数）推进；全部属于 CPU/内存侧收益，不改变任何 GPU 行为与对外 ABI，修复风险整体可控，但每一项都应按项目既定流程先过隔离 crash-gate 再做 ABBA 验证。


---

# 审查 5/7：阴影 runtime bridge 与语义捕获

> 范围：阴影 runtime bridge 与语义捕获:完整阅读 src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp(约 8030 行)及 war3_shadow_runtime_bridge.h(约 2227 行),必要时查阅 war3/render/war3_current_draw_contract.cpp。重点:semantic capture 的 per-draw 开销、direct/replay 分流、与主 render lane 的同步点、状态快照/restore 的拷贝量、热点 map 查找。

# 阴影 runtime bridge 与语义捕获 —— 优化质量审查报告

## 【实现概述】

该范围由三层构成：(1) `war3_shadow_runtime_bridge.cpp` 是诊断/注册表中转层——接收 `NoteShadowRuntime*` 系列钩子事件写入 4 个全局注册表（`ShadowObjectRegistry`、`ModelInstanceRegistry`、`PoseRegistry`、`RenderObjectRegistry`)，并每帧汇总 `War3ShadowCaptureStats`（约 663 行纯计数器字段）、cadence 环形样本与一个 3800 行的巨型 Summary 查询函数；(2) `war3_current_draw_contract.cpp` 通过 `Hook_RenderQueueUpdateItemWorldMatrix` 在游戏线程内做 per-draw 捕获，写入 thread_local 直接映射缓存（4096 槽 contract + 512 槽 12KB palette snapshot）及一组全局 mutex 保护 map，供 submit 端查询；(3) direct/replay 分流本身在 shadow renderer 侧，bridge 只承担统计透传。整体上存在大量历史优化痕迹（Phase 7.26 快照索引、7.81 TLS 去重复用、7.97/7.99 demand-fill 预算与解析缓存、7.49 probe 默认关闭），但热点路径上仍残留几处重量级开销与一个"声称已优化但实际未生效"的锁死角。

## 【优化质量问题清单】（按严重程度排序）

**P0-1 每次 ready publish 零初始化 12KB 栈数组 + 临时 vector 堆分配（CPU，疑似毫秒级/帧）**
`war3_current_draw_contract.cpp:1463`:`std::array<uint8_t, kMaxPaletteMatrices * 48u> trustedPaletteBytes = {};` —— 256×48=12,288 字节在每次通过 palette 校验的 publish 中全量 memset，而该函数自述"每帧约 10K-30K 次"(`:1237-1238`)。即便按几千次 ready publish 估算也是数十 MB/帧 的无效写带宽；且只在 trusted 分支按 `capturedPaletteCount*48` 部分填充，零初始化绝大部分被浪费。同函数 `:1467` `std::vector<Matrix4> trustedPalette;` 每次 trusted 查询产生一次堆分配。

**P0-2 注册表 findBy* 热点全部使用 `std::unique_lock`,Phase 7.83 注释声称 reader 已走 shared_lock——实际未改（CPU/同步，"假优化"死角）**
`war3_shadow_object_registry.cpp:618,630,642,654,666,678` 全部 6 个 find;`war3_model_registry.cpp:1203` 起全部 22 个 find(`findByWorldObjectEntry/findBySceneNode/findByUnitPtr/findBySpritePtr/findByRuntimeModel/findBySourceObject/...`）均为 `std::unique_lock<std::shared_mutex>`，纯读却独占。而头文件注释明确写着"reader 路径每帧 400-3000 次……改 shared_mutex 后 reader 走 shared_lock"(`war3_model_registry.h:254-256`、`war3_shadow_object_registry.h:138`)。共享锁只应用在 `*Count/snapshot` 等次要函数上，真正的热 find 被遗漏，读-读线程（render/CS/控制面）全部互斥串行。

**P0-3 `AugmentShadowSemanticContext` 每 draw 链式最多 ~18 次独占锁 map 查询 + 大记录拷贝（CPU/同步）**
`war3_shadow_runtime_bridge.cpp:7845-8004`:instance registry 顺序 5 查（:7849-7865)、shadow registry 顺序 6 查（:7893-7912)、pose registry 顺序 4 查（:7958-7970)、render object registry 顺序 3 查（:7998-8003)。每次 find 都是独占锁（见 P0-2)，且 `out = it->second` 拷贝含 `std::string modelPath` 的整条 `ShadowObjectRecord`(`war3_shadow_object_registry.h:27-64`，约 250B+，路径超 SSO 即堆分配）。该函数在 `War3BuildShadowSemanticContext`(`d3d9_device.cpp:15792`）与 `war3_shadow_renderer_core.cpp:2234`、`war3_visible_renderables.cpp:1171` 的 per-draw 路径上被调用；`needsRuntimePoseAugment` 在 pose hook 激活且带 runtimeModel 时几乎恒真（:7827-7832)，即蒙皮单位每 draw 必走 PoseRegistry 段。

**P0-4 注册表多镜像写放大：一条记录写 6~9 张 map，全量深拷贝（CPU/内存）**
`war3_shadow_object_registry.cpp:177-222` `storeRecord`:6 次 find merge + 6 次 `m_by*[key] = merged`（每次含 `std::string` 拷贝，modelPath 超 15 字符即 6 次堆分配）。`notePose`(:506)/`noteSpriteFramePose`(:562）按 pose hook 频率触发（每单位每帧）。`ModelInstanceRegistry` 同模式 9 张 map(`war3_model_registry.h:257-265`),`PoseRegistry` 3 张。内存占用 ×N、写入 ×N，且 unordered_map 节点分散，对后续 find 的缓存命中率也不利。

**P1-5 持锁跨注册表嵌套查询，临界区叠加且存在锁序依赖（CPU/可维护性/潜在死锁风险）**
`war3_shadow_object_registry.cpp:478`(`noteModelBinding` 持自家 unique_lock 调 `ModelInstanceRegistry::findBySpritePtr`)、`:524`(`notePose` 持锁调 `ModelRegistry::findByRuntimeModel`)。方向目前一致（Shadow→Model)，但任何反向调用即成 AB-BA 死锁；同时把两个独占锁的持有时间串在一起放大争用窗口。

**P1-6 `QueryCurrentDrawPreparedSlice` miss 路径线性扫描 1024 槽（CPU)**
`war3_current_draw_contract.cpp:1213-1220`：直接映射槽未命中时 `for (const auto& candidate : g_preparedSliceCache)` 全扫找最大 captureSerial。该查询在 per-draw 路径上，miss 时每次 1024 次比较。

**P1-7 `SnapshotPublishedCurrentDrawContracts` 每次全扫 4096 槽 + 排序插入路径 O(n²) 比较器（CPU)**
`war3_current_draw_contract.cpp:2228` 无条件遍历整个 TLS contract cache；每帧至少调用 2 次（`d3d9_device.cpp:19074` 主快照、`:22206` 静态补充快照）。`maxRecords != 0` 分支（`:2204-2225`）用 `std::find_if` + 有序 `insert`,`betterRecord → isPreferred → SnapshotStableIdentityKey/preferredVisibleKey`(:2145-2163）在插入排序中被对同一元素反复求值（FNV 哈希 + TLS map 查询，未命中时还有 registry 查询）;`maxRecords==0` 分支已有 Phase 7.81 哈希去重（好），两条路径质量不一致。

**P1-8 常开的 per-publish 原子诊断（CPU)**
`war3_current_draw_contract.cpp:436-475` `NotePublishedStream1Layout`：每次 publish（带 stream）做 1 次 CAS max-loop + 多次 fetch_add，仅维护 stride 直方图，无 env 门控；加上 publish 主路径其余 ~10 次 atomic RMW(`g_publishAttemptCount`、provenance 三桶、ready/miss 等），按自述 10K-30K 次/帧放大为每帧数十万原子操作。对照组：Phase 7.49 probe 已因同样原因默认关闭（`:1261-1264` 注释自述 1-3ms),stream1 统计属于同类遗漏。

**P2-9 TLS 缓存体量与重置成本（内存）**
`war3_current_draw_contract.cpp:88-97`:`g_paletteSnapshotCache` = 512 × `PaletteSnapshotEntry`（单个 12,304B,:74-81)≈ **6.3 MiB thread_local**，另有 4096 槽 contract cache ≈ 0.5 MiB、1024+2048 槽 slice 缓存；`ResetCurrentDrawContractCache`(:1024-1033）对前两项逐元素 `={}`，相当于一次 6.8MB memset。

**P2-10 状态快照/合并的拷贝量（CPU，可接受但可精简）**
`war3_shadow_runtime_bridge.cpp:3083-3345` `NoteShadowSceneStats`：按值拷贝约 4.8KB 的 `War3ShadowCaptureStats`(`d3d9_war3_scene.h:337` 起 663 行字段）,`hasReceiverDetails` lambda 评估 ~50 字段×2 次（:3087-3142)，占位合并路径再逐字段拷 ~100 项（:3149-3313)。每帧 2~4 次（`d3d9_device.cpp` 9 个调用点，多数为不同早退分支）。绝对成本约微秒级，但写法使任一字段新增都要手工同步三处。

**P2-11 `LookupCurrentDrawContractRecord` 每查询直读 Game.dll frameTag，未用已有的 Cached 版本（CPU)**
`war3_current_draw_contract.cpp:564-568` 调 `TryReadCurrentPaletteFrameTag`(:518 含 `GetModuleHandleA` fallback + 远程读），而 `:530` 的 `CachedCurrentPaletteFrameTag`(per-frame TLS 缓存）就在旁边未被使用。

**P2-12 3800 行巨型 Summary 函数 + 诊断面全量 snapshot(可维护性/低频 CPU)**
`war3_shadow_runtime_bridge.cpp:3711-7542` `QueryShadowRuntimeBridgeSummary` 单函数约 3830 行，内部遍历全部 visible record 并 per-record 做多注册表 find(:3859 起），另有 `AttachmentRigidRegistry::instance().snapshot()` 类全量拷贝。调用频率低（控制面/perf 报告，`war3_perf_monitor.cpp:4673`)，性能影响可控，但函数体积、字段手工展开（`ShadowRuntimeBridgeSummary` 在头文件中占 ~1600 行）是明显的可维护性负担。

**P2-13 submit 端解码每 draw 堆分配与整记录拷贝（CPU)**
`war3_current_draw_contract.cpp:785`(`outPalette.resize` 每次堆分配）、`:1981`(`CurrentDrawContractRecord record = *entry` 整条拷贝后仅改 `known`)。

**P2-14 同一函数内两次获取同一互斥锁（CPU/同步）**
`war3_current_draw_contract.cpp:1387` 与 `:1591` 两次 `lock_guard(g_publishedCurrentDrawMutex)`，中间无依赖释放点，可合并为一个临界区，顺带避免 `ClearPublishedCurrentDrawReadyRecord`(:1401-1423 分支）与主 publish 的重复锁往返。

## 【改进方向】

1. **消除 publish 大数组零初始化**（收益：CPU；风险：低）:`trustedPaletteBytes` 改为 `thread_local` 复用缓冲或 `std::unique_ptr` 懒分配，去掉 `= {}`；只按 `requiredBytes` 写。`trustedPalette` 改 `thread_local std::vector` 复用容量。预期回收大部分 memset/分配开销，纯机械改动。
2. **把全部 findBy* 的 `std::unique_lock` 改回 `std::shared_lock`**（收益：CPU/同步；风险：低-中）：先确认 find 内无惰性写（已核实纯读），逐注册表替换并补一条回归（并发 find + note 压力）。这也是让头文件 Phase 7.83 注释变为事实。
3. **压缩 `AugmentShadowSemanticContext` 链式查询**（收益：CPU；风险：中）：增加单锁多键批量查询接口（一次 shared_lock 内按 worldObjectEntry/sceneNode/unitPtr/handle 顺序 find)，或让调用方先做一次"身份完整"快速判定，完整时整段跳过注册表；`findBy*` 出参可提供 `const&` + 锁内拷贝所需字段，避免整条含 string 记录拷贝。
4. **注册表改单存储 + 二级索引**（收益：内存/CPU；风险：中-高）：记录只存一份（vector 或单 map by canonical key),6~9 个别名 map 只存 `key→index/pointer`;`storeRecord` 从 6 次全量拷贝降为 1 次写 + 5 次指针写。需同步改造 snapshot/count/merge 逻辑，建议先在 `ShadowObjectRegistry` 试点。
5. **消除跨注册表持锁嵌套**（收益：同步/可维护性；风险：低）:`noteModelBinding`/`notePose` 中先释放自家锁或用"先查后写"两阶段：锁外完成 `ModelInstanceRegistry/ModelRegistry` 查询，再取自家锁 merge 写入。
6. **`QueryCurrentDrawPreparedSlice` miss 不再全扫**（收益：CPU；风险：低）：维护一个 per-part 最新槽位小索引（同直接映射思路，key=renderablePart,value=最新 slot),miss 时 O(1) 找到同 part 最新记录。
7. **统一 Snapshot 两条路径到哈希去重 + 预算截断**（收益：CPU；风险：中）:`maxRecords!=0` 分支改用与 `maxRecords==0` 相同的 `unlimitedDedupeKey` 哈希去重，收集后用 `std::partial_sort` 或 nth_element 截断；`isPreferred` 结果按 record 指针 memoize，避免插入排序中重复求值。4096 槽全扫可维护一个"本帧非空槽"计数提前退出。
8. **常开诊断归一档**（收益：CPU；风险：低）:`NotePublishedStream1Layout` 与 publish 主路径的非关键计数收进与 `DXVK_WAR3_PUBLISH_PROBE` 相同的 env 门控或 compile-time `kNativeOptimizationPerfTrackingEnabled` 风格开关，保留默认关闭。
9. **TLS 快照瘦身**（收益：内存；风险：中）:`PaletteSnapshotEntry::bytes` 按实际 matrixCount 上限（如 64×48）或改堆外共享 arena;512 槽 LRU 可降到 256；或在 reset 时用生成号失效代替 6.8MB memset。
10. **Summary/stats 合并机械化**（收益：可维护性；风险：低）：占位合并改用字段表/宏或 `memcpy` 区间拷贝 + 显式例外清单，避免 ~100 行手工逐字段；`War3ShadowCaptureStats` 发布可改为双缓冲指针交换，读端免 4.8KB 拷贝。
11. **小项**:`LookupCurrentDrawContractRecord` 换用 `CachedCurrentPaletteFrameTag`（低险）;submit 解码复用 `thread_local std::vector<Matrix4>`（低险）;`PublishCurrentDrawContract` 两次锁合并（低险）。

## 【总体评价】

该范围的优化意识是明确且有据可查的：VisibleRenderableRegistry 的快照索引与 miss 短路（Phase 7.26)、TLS 去重表复用（7.81)、demand-fill 预算与解析缓存（7.97/7.99)、诊断 probe 默认关闭（7.77）都是方向正确且真正生效的优化；注释里对每次优化的动机与量化依据交代得很清楚，这在同等规模项目里少见。但代码同时呈现"诊断驱动开发"的沉重尾巴：发布路径上仍有一次 12KB 栈清零、每次 publish 十余次常开原子计数、注册表 6~9 镜像写放大，以及最典型的——头文件声称 reader 已走 shared_lock、实际 22+ 个热点 find 仍全部独占锁的"假优化"死角。这些问题单点都不致命，叠加在每帧数千到上万次 draw 的同步 CPU render lane 上，与 AGENTS.md 中"损失位于同步 CPU lane 而非 GPU"的实测结论高度吻合。建议按 P0-1 → P0-2 → P0-4 的顺序做三轮小步收口，每轮沿用项目既有的 crash-gate + ABBA 验证流程，预期可在不改变任何渲染语义的前提下收回可观的每帧 CPU 时间。


---

# 审查 6/7：点光源/点阴影系统全路径

> 范围：点光源系统全路径:跨文件定位并阅读所有点光源/点阴影实现——src/d3d9/d3d9_war3_light.h、src/d3d9/war3_shader_api.cpp、src/d3d9/d3d9_war3_pipeline.cpp、src/d3d9/war3/render/war3_hybrid_ray_tracing.cpp、src/d3d9/war3/ui/war3_imgui.cpp、src/d3d9/d3d9_war3_settings.h 中的点光相关段落,以及体积光 shader 中 ray-sphere 点光积分。重点:点光源数量上限与逐光开销、光照 culling(距离/视锥)是否存在、per-fragment 逐光循环的 GPU 成本、阴影贴图分配策略、UI/设置每帧同步开销。

已完成全路径取证。以下是点光源系统优化质量审查报告。

---

# 点光源/点阴影系统 优化质量审查报告

## 【实现概述】

点光源系统以 `War3LightManager`（`d3d9_war3_light.h`）为唯一写侧，写接口加 mutex、读侧通过帧快照缓存（frame+generation 为键）按值分发 16 灯/4 阴影灯的 canonical 快照；禁用路径只需一次 atomic 读取。下游有三个消费者：ShadowReceiver（直接光照 UBO + cube 点阴影）、VolumetricLight pass（ray-sphere 体积积分，top-2）、war3_shader_api（外部 JASS/脚本消费者，ring buffer 三槽）。点阴影为独立的 cube-array D32 路径：CPU 侧在 worker 线程（std::async，与 CSM 录制重叠）做 range 预过滤 + 90° 方锥 face 剔除 + 内容签名/时序复用判断，GPU 侧按 face updateMask 逐面 dynamic rendering 录制，每灯 6 面、最多 4 灯，资源按 96 MiB 预算与配置容量分配。整体设计上已有大量有意识的优化（atomic 门控、快照复用、CPU 预算变换、逐像素平方距离早退、worker 重叠、预算钳制），默认配置（pointLightsEnabled=false）下确实接近零开销。

## 【优化质量问题清单】（按严重程度排序）

### 1. 中高 — Worker_Prepare 每帧对场景 vector 做两次深拷贝（CPU/内存分配风暴）
`src/d3d9/d3d9_war3_shadow.cpp:3544-3549`：
```cpp
const War3PipelineInput inputCopy = input;   // 第一次深拷贝
m_pointShadowPrepareFuture = std::async(std::launch::async,
    [this, inputCopy, lightSnapshot, draws]() { ... });  // 按值捕获，第二次拷贝
```
`War3PipelineInput.scene` 按值含 `shadowPalettes/shadowInstances/shadowFallbacks/shadowCasters` 四个 `std::vector`（`d3d9_war3_pipeline.h:36`、`d3d9_war3_scene.h:1003-1011`），`War3ShadowCasterDraw` 是含矩阵与多套 buffer 描述的大结构体。点阴影开启时每帧在渲染线程上产生 2 次可达数百 KB～数 MB 的堆分配+拷贝，而 worker 实际只读 `input.settings / scene.shadowStats / scene.shadowPalettes`，replay 数据已通过 `draws` 裸指针共享（寿命由 scope-exit guard 保证，`d3d9_war3_shadow.cpp:6014-6015`）。这条"为了安全而整包拷贝"的代价与系统其他部分极力消除的每帧分配形成直接矛盾。

### 2. 中 — 光照/点阴影 UBO 单缓冲 + 每帧 read→write barrier 序列化（GPU 气泡）
`src/d3d9/d3d9_war3_shadow.cpp:4862-4894`：每帧对同一块 device-local UBO 先 `FRAGMENT_SHADER/UNIFORM_READ → TRANSFER/TRANSFER_WRITE` barrier，再 `cmdUpdateBuffer`，再反向 barrier。由于 buffer 不做帧间分片，write 必须等上一帧 receiver fragment 读取完成，在 GPU 时间线上制造跨帧序列化点。体积光 pass 的 CSM/点光 UBO 是同一模式（`d3d9_war3_volumetric_light.cpp:1206-1225`），每帧共 4 次这样的往返。单块 UBO 只有 ~784 B，完全具备 ring-slice 条件。

### 3. 中 — 点阴影逐 face 独立 render pass，最多 24 个/帧；空 face 也走完整 clear pass（GPU）
`src/d3d9/d3d9_war3_shadow.cpp:4386-4413`：每灯 6 面各自 `cmdBeginRendering`/`cmdEndRendering`（4 灯全开 = 24 个小 pass，1024²）。每次 begin/end 在 tiler 上都是一次 tile 写回/重载；空 caster face 也只靠 `LOAD_OP_CLEAR` 空转一个 pass（`4422-4433` 注释明说"空 face：只 clear"）。layout 转换已合并为一次 toWrite/toRead（`4364-4378`），但 pass 粒度没有合并——未用 layered/multiview，也未对空 face 改用 `vkCmdClearDepthStencilImage`。

### 4. 中低 — shader_api 消费者用相机 (0,0,0) 取快照，击穿帧缓存且排序失真（CPU + 一致性）
`src/d3d9/war3_shader_api.cpp:1515-1517`：
```cpp
dxvk::Vector4 cameraPos(0.0f, 0.0f, 0.0f, 1.0f);
const auto snap = dxvk::War3LightManager::Instance().GetFrameSnapshot(input.frameSerial, cameraPos);
```
快照缓存键含 cameraPos（`d3d9_war3_light.h:230-233`）。渲染路径（`d3d9_war3_shadow.cpp:6111`、`d3d9_war3_volumetric_light.cpp:1570`）用真实相机，而这里固定原点：(a) 若本调用夹在两个渲染消费者之间，会把缓存冲掉造成每帧最多 3 次 O(N log N) 重建，违背"同帧 O(1)"的设计注释；(b) 距离重要度排序按原点计算，外部 API 消费者看到的灯序与渲染器不一致，shadow-prefix 选择可能不同。N≤16 时纯 CPU 代价小，但属于"看起来有缓存、实际被自己的第二个调用方打穿"的典型死角。

### 5. 低 — 时序复用/face 预算模式下每帧 O(casters × ~45) 的全量内容签名（CPU，可选路径）
`src/d3d9/d3d9_war3_shadow.cpp:3728-3797`：仅当 `pointShadowTemporalReuse || maxFacesPerFrame∈[1,5]` 时启用（默认关闭，门控正确），但启用后对每个 replay draw 逐字段 mix ~45 次（含 `mixMatrix` 16 次 float memcpy），千级 caster 时每帧数万次哈希操作。其中大量字段（格式/offset/句柄）在一帧内不变，完全可由上游捕获阶段产出一个帧级签名增量复用。

### 6. 低 — gpuSkinDirect caster 每 draw 双重 descriptor 绑定（CPU 录制开销）
`src/d3d9/d3d9_war3_shadow.cpp:4556-4558` 与 `4620-4639`：direct 路径先 `bindResources`（5 描述符+push constants）draw，随后立即再构造 clearedDescriptors/clearedPc 二次 `bindResources` 清理。每个 gpu-skin caster 在每个更新 face 上付 2 份描述符写入，6 面全更时放大 6 倍。

### 7. 低 — receiver PCF 内 per-pixel per-light `textureSize`（GPU 微开销）
`subprojects/war3fx/shaders/war3_shadow_receiver.frag:486-489`：`samplePointShadowPcf` 内部对 cubeArray 做 `textureSize()` 求分辨率，该值全帧恒定，每次 PCF 调用（每像素×每阴影灯）都重新发射一条 size 查询。16 tap 本身有注释说明是刻意权衡，可不动；size 应上 UBO。

### 8. 低 — 每帧 `std::async(launch::async)` 任务创建（CPU 微开销/抖动）
`src/d3d9/d3d9_war3_shadow.cpp:3547`：每帧新建 async 任务+future，MSVC 下虽走 ConRT 池，仍有任务调度与异常框架开销；一个持久 worker 线程+双槽 plan 更平稳。与其配套的三段几乎逐字重复的 catch 块（`3459-3515`）是纯可维护性问题。

### 9. 提示（生命周期/正确性相邻）— `GetPointLights` 解锁后返回内部 vector 指针
`src/d3d9/war3_shader_api.cpp:1109-1118`：`std::lock_guard` 作用域内取 `lights.data()`，锁释放后把指针交给调用方，CS 线程下一帧 `clear()+push_back`（`1512-1528`）即可让其悬空/读到半写数据。不是热路径开销问题，但该 API 的"零拷贝"是用安全换的，且与快照系统其他地方刻意避免的模式相反。

### 10. 提示 — 死成员与三份重复状态打包
`d3d9_war3_shadow.h:605` 的 `m_pointShadowFaceCasterIndicesScratch` 声明后全工程零使用；`war3_shader_api.cpp:1519-1529` 每帧把快照重打包成第三套 `LightData` 表示（渲染侧已有 `m_pointLightFrameUniform` 与 volumetric UBO 两份），属轻微的三重表示维护负担。

## 【改进方向】

| # | 建议 | 收益类别 | 风险 |
|---|------|---------|------|
| 1 | worker 捕获改为与 `draws` 相同的裸指针/引用方案（`input` 寿命已由 scope-exit guard 覆盖），或把 scene 改为 `shared_ptr<const War3FrameScene>` 一次发布；至少把 lambda 捕获改为 `std::move(inputCopy)` 消掉第二次拷贝 | CPU/内存 | 低；需保持 guard 先行等待的不变量（已在代码中） |
| 2 | 光照/点阴影/体积 CSM UBO 改为 3 槽 ring slice（与 `g_pointLightBuffers` 同思路），按 `frameIndex%3` 选 slice，消除 FRAGMENT→TRANSFER 跨帧 hazard | GPU | 低；DXVK buffer 原生支持 slice，注意 `getSliceInfo` offset 对齐 |
| 3 | 空 caster face 改 `vkCmdClearDepthStencilImage`（在 attachment layout 内合法），非空 face 可考虑按灯合并为单 pass + per-face push constant 重绑（避免 end/begin 配对），长期可评估 multiview 一次渲 6 面 | GPU | 中；multiview 需 shader/布局改动，先做前两项零风险项 |
| 4 | `war3_shader_api.cpp:1515` 改为从 `g_currentContext.camera`（同函数内已解析）取相机位置；若不可得则让快照缓存键去掉 camera 分量、把排序键改为"灯 id + generation"，由消费者各自排序 | CPU/一致性 | 低 |
| 5 | 内容签名改为复用上游捕获阶段的帧级几何签名 + 仅叠加灯/设置字段；或仅 hash `boundsCenter/Radius + worldMatrix + palette hash` 的必要子集 | CPU | 中；签名是跨帧正确性契约，收缩字段必须保持"内容变→签名变"方向 |
| 6 | gpuSkinDirect 的 clear-bind 改为只在 face 内最后一个 direct draw 后执行一次，或把 direct 元数据做成独立描述符槽位避免回写 | CPU | 低-中；需证明后续非 direct draw 不读 direct 槽位 |
| 7 | cube 分辨率（及其倒数）进 PointShadowUniform，删除 shader 内 `textureSize` | GPU | 低 |
| 8 | `std::async` 换持久 worker + 双槽 plan 邮箱；顺手把三段 catch 合并为一个按异常类型取 message 的 lambda | CPU/可维护性 | 中；线程生命周期需挂到 pass 析构 |
| 9 | `GetPointLights` 改为按值拷贝到调用方 buffer（16 灯极小）或返回快照代际号+拷贝，消除悬窗 | 可维护性/正确性 | 低；需改外部调用点 |
| 10 | 删除 `m_pointShadowFaceCasterIndicesScratch`；考虑让 shader_api 直接复用 `War3PointLightFrameSnapshot` 而非第三套 `LightData` | 可维护性 | 低 |

## 【总体评价】

该系统的优化意识在代码库中属于上游水平：默认关闭时零成本（atomic 门控）、快照帧缓存固定大小无堆分配、CPU 预算视图空间位置、shader 内平方距离早退+惰性法线、worker 与 CSM 录制重叠、cube 资源按容量与 96 MiB 预算分配、所有"贵"的可选项（16-tap PCF、软件射线、时序复用、face 预算）都有明确的默认关闭/质量默认标注，且多数优化门控（如内容签名仅在复用模式下才全量哈希）确实生效而非摆设。主要缺口集中在三处真实热路径：worker 每帧对场景 vector 的双份深拷贝（与系统其余部分的零分配原则自相矛盾，是点阴影开启后最确定的 CPU 负增量来源）、光照 UBO 单缓冲造成的逐帧 GPU 序列化气泡、以及逐 face 最多 24 个小 render pass 的粒度问题。次一级是若干"局部正确但整体打穿"的死角：(0,0,0) 相机调用击穿帧快照缓存、per-pixel `textureSize`、每 draw 双份描述符绑定。这些问题的修复都不需要架构变动，按上表 #1/#2/#4 优先实施即可在不动正确性契约的前提下收回大部分可测开销。


---

# 审查 7/7：阴影后端与外围

> 范围：阴影后端与外围:阅读 src/d3d9/war3/shadow/war3_shadow_backend_native_d3d9.cpp(约 1073 行)、src/d3d9/war3/shadow/war3_shadow_backend_dxvk.h、src/d3d9/war3/shadow/war3_shadow_backend.h、src/d3d9/d3d9_war3_shadow_outline.cpp(约 979 行)、src/d3d9/war3/render/war3_upper_layer_shadow.h、src/d3d9/war3/shadow/war3_shadow_alpha_test_payload.h。重点:后端抽象的虚调用/间接层成本、native D3D9 与 dxvk 后端切换开销、outline 与 alpha-test payload 的 per-draw 成本、backend 接口是否造成冗余状态同步。

# 阴影后端与外围 —— 优化质量审查报告

## 【实现概述】

该范围由三层组成：①`IShadowRenderBackend` 虚接口（war3_shadow_backend.h）抽象出 beginFrame/ensureGeometry/ensurePalette/ensureMaterial/submitDraw/endFrame 六个调用，`NativeD3D9Backend` 是真实执行后端，按内容哈希缓存几何/调色板/材质句柄，把每个 draw 深拷贝成 `SubmittedDrawRecord` 后在 `executePreparedDraws` 里逐个用固定管线（rigid 走静态 VB/IB,skinned 走 CPU 蒙皮 + DrawPrimitiveUP）回放；②`DxvkValidationBackend` 是只记录句柄与身份的校验桩；③`d3d9_war3_shadow_outline.cpp` 是 DXVK 侧描边 pass，支持屏幕空间 MRT mask + 全屏边缘检测、几何外扩两种模式，管线按完整 vertex-input key 缓存；④`war3_upper_layer_shadow` 是上层语义→可见物/geoset/pose 的多键解析注册表；⑤alpha-test payload 头定义了 draw-time stash、semantic 消费的 UV/纹理/alphaRef 活绑定结构。整体已有内容键缓存、管线缓存、早退与日志节流等优化意识，但热点路径上存在多处"看起来优化了、实际仍是全量"的死角。

## 【优化质量问题清单】（按严重程度排序）

### 高

**H1. Palette 缓存无界增长，动画单位每帧泄漏一个条目（内存 + CPU)**
`war3_shadow_backend_native_d3d9.cpp:54-66`:`MakePaletteCacheKey` 对 Skinned 路径把 `packet.pose.matrixHash`（逐帧姿态哈希）混入 key;`:865-892` 未命中即插入 `m_paletteHandleByKey`/`m_paletteResources`；而全文件只有 `reset()`(`:696-730`，仅由 `setDevice` 触发）会清这两个 map,`beginFrame`(`:744-759`）不清。动画单位每帧姿态哈希变化 → 每帧每个动画单位新增一条 palette（含 `std::vector<Matrix4> matrices` 拷贝），整场游戏单调增长，哈希表查找/内存同步恶化。刚性移动单位同理（`:69-71` 对 worldTransform 逐分量哈希）。这是典型的"缓存了，但缓存键是易变量"的死角。

**H2. Skinned 几何的静态 VB/IB/blend buffer 建而不用；blend buffer 在任何路径都未被绑定（GPU 内存 + 上传带宽）**
`ensureGeometry`(`:816-845`）对所有 packet 无条件创建 position/index/blend 静态缓冲；但 `DrawPreparedSkinnedRecord`(`:593-684`）只用 record 内 CPU 蒙皮后的 `skinnedPositions` 走 `DrawIndexedPrimitiveUP/DrawPrimitiveUP`，完全不碰 geometry 资源；`DrawPreparedRigidRecord`(`:502-591`）只绑定 `positionBuffer`/`indexBuffer`,`blendBuffer` 全文件无任何绑定点。即：skinned packet 的 VB/IB 创建 + 锁上传 + DEFAULT/MANAGED 池显存是纯浪费，blendBuffer 则是全局死资源。

**H3. submitDraw 对每个 draw 每帧做全量数据深拷贝 + 多次 VirtualQuery,rigid draw 拷贝的数据从不被使用（CPU 热点）**
`:956-972`:`submitDraw` 无条件执行 `ResolvePositionStream`/`ResolveIndexStream`；对 skinned 再拷贝 `vertexGroupIndices`/`explicitBlendWeights`/`explicitBlendIndices`/`runtimeGroupPalette` 四个 vector。而 rigid 路径 `DrawPreparedRigidRecord` 只读 record 的 `worldTransform`——positions/indices 拷贝后从不使用。且 `ensureGeometry` 缓存命中时已经拷过一次，等于同一数据每帧每 draw 拷两遍。`TryCopyPacketVector`/`IsPacketVectorReadable`(`:96-138`）内部每个 vector 调 `IsReadableRange`——其头文件自述"内部会频繁调用 VirtualQuery，在对象很多时开销很大"(war3_memory.h:41-44)，即每个 draw 多达 5~6 次内核态查询。`SubmittedDrawRecord` 持有 6 个 `std::vector`(native_d3d9.h:118-138),`m_submittedDraws.clear()` 每帧释放全部内层堆缓冲，下一帧重新分配，呈 draw 数 × 6 的堆抖动。

**H4. CPU 蒙皮逐 draw 全量重算 + 每次新建堆 vector + UP 路径再被驱动拷贝一次（CPU + GPU 内存带宽）**
`DrawPreparedSkinnedRecord:604` 每次执行新建 `std::vector<float> skinnedPositions`;`PrepareSkinnedWorldPositions:454-489` 对每顶点做最多 4 次矩阵乘加；随后 `DrawIndexedPrimitiveUP`(`:659-663`）让驱动再整体拷一份。同一蒙皮模型每帧（乃至每级联）从零重算，无帧间/级联间复用，也无持久 scratch buffer。与 H2 对照：蒙皮结果没有写回任何 VB,GPU 侧零复用。

### 中

**M1. 每次 executePreparedDraws 创建 D3DSBT_ALL 状态块（CPU)**
`:1015-1016` + `CaptureStateBlock:401-421`:`CreateStateBlock(D3DSBT_ALL)` + `Capture()` 是 D3D9 里最重的调用之一（捕获全部 render state/texture stage/sampler/stream)，在 DXVK 翻译层下成本进一步放大。实际被改动的状态只有约 10 个 render state + FVF + 两个 shader + texture0 + stream/indices + WORLD 变换（`:531-543`)。

**M2. 每个 draw 重复设置完全相同的 10+ 个状态（CPU)**
`:531-543` 与 `:638-651`：每个 draw 都执行 `SetVertexShader(nullptr)`、`SetPixelShader(nullptr)`、8 个 `SetRenderState`、`SetFVF`——同一批次内这些值恒定，本可在 `executePreparedDraws` 开头设一次。D3D9 运行时可滤掉部分冗余，但调用本身（虚函数进入 DXVK device)仍是 per-draw 开销。

**M3. executePreparedDraws 中冗余的 O(n) 统计遍历与无效哈希查找（CPU)**
`:989-996`：遍历全部 submitted draws 重数 rigid/skinned，而 `m_submittedRigidDrawCount`/`m_submittedSkinnedDrawCount` 在 submit 时已维护（`:976-979`);`:1023` 对 skinned record 也执行 `m_geometryResources.find`，结果在 skinned 分支完全不用。

**M4. 上层解析的"每 4096 次 miss 做一次 6 键复查 + printf"(CPU 毛刺）**
`war3_upper_layer_shadow.cpp:273-308`、`:336-360`:miss 计数 `% 4096 == 0` 时对同一对象再做 6 次 registry 查询并 `war3dbg::Print`。大场景 miss 频繁时，每 4096 次就有一拍远超正常路径的延迟毛刺；且正常路径本身是最多 6 次顺序哈希探测（`:14-39`)，前序键命中才短路。

**M5. resolved items 快照深拷贝调色板（CPU + 内存）**
`war3_upper_layer_shadow.cpp:450`:`m_resolvedItems.push_back(item)` 拷贝整个 item（含 `std::vector<Matrix4> runtimeGroupPalette`,war3_upper_layer_shadow.h:23);`snapshotResolvedItems()`(`:419-422`）返回时再整体深拷贝一次。`beginFrame` 每帧 clear 三个容器（`:251-256`),unordered_set 节点内存随之释放。

**M6. Outline 的双遍 IsOutlineHandle 与 per-draw TLS 写（CPU)**
`d3d9_war3_shadow_outline.cpp:53-61`/`533-538` 先全量数一遍 `IsOutlineHandle`（哈希查找），绘制循环 `168-172`/`643-647` 再查一遍，每个 caster 每帧 2 次；`SetTlsBatchHandle(draw.batchHandle)` → draw → `SetTlsBatchHandle(0)`(`:325-337`、`:946-958`）每 draw 两次 TLS 写，而 TLS 值只在 `OnDraw` 过滤语义中需要，批量设置一次即可（同批 handle 不变时）。

**M7. 两张 mask 分两次 barrier，可合并为一次 dependency(GPU/CPU 小幅）**
`:370-371`:`transitionMaskToRead` 对 Visible/All 两张同阶段同参数的 mask 各发一个 `cmdPipelineBarrier`（各含 1 个 image barrier)，可合成一个 `VkDependencyInfo` 带 2 个 barrier，少一次命令记录与提交开销。

### 低

**L1. Native 后端的 material 句柄体系是记账型死重（CPU/可维护性）**
`:86-93`、`:896-915`：每 draw 计算 material 哈希、查/建 map，但两个 draw 路径（`:531-543`、`:638-651`）都强制固定状态、从不读 `MaterialResource.signature`。除校验外无消费方。

**L2. 校验后端 per-draw 4 个 unordered_set 插入（CPU，仅校验路径）**
`war3_shadow_backend_dxvk.h:66-69`:handles/worldObjectEntries/sceneNodes/runtimeModels 四个 set，每帧重建，节点分配频繁；作为校验桩可接受，但应注意它不在产品路径上造成混淆性开销。

**L3. 屏幕空间 outline mask 与场景同分辨率（GPU，待核实）**
`:93-98` mask 资源按 `colorView` 全尺寸创建（`ensureOutlineMaskResources` 不在本文件内，格式/降采样能力未核实）；描边 mask 通常可半分辨率，若当前为全 res R8×2 + 全屏边缘检测，存在明显降分辨空间。

## 【改进方向】

| # | 建议 | 预期收益 | 风险 |
|---|------|---------|------|
| 1 | 针对 H1:palette 缓存改为按帧分代（frame-serial 双缓冲），或 key 改用稳定身份（modelKey+geoset）而 value 每帧覆写矩阵；至少每 N 帧淘汰 frameSerial 过旧条目 | 内存 + CPU | 中：需保证同帧内同 key 一致性，淘汰不能误杀在飞引用 |
| 2 | 针对 H2:`ensureGeometry` 对 `ShadowDrawPath::Skinned` 跳过 VB/IB/blend 创建；直接删除 blendBuffer 字段（两路径均无消费者） | GPU 内存 + 上传带宽 + CPU | 低：纯删除死代码，需离线合同确认无外部引用 |
| 3 | 针对 H3/H4:rigid record 只存 handle + worldTransform（不拷 positions/indices);skinned 数据改为 geometry 缓存内一次性快照、record 只持引用；`SubmittedDrawRecord` 改用帧级 arena/池化 vector 复用 | CPU（显著，与 draw 数成正比） | 中：需注意 native 指针寿命证明（当前拷贝本意是防悬空，可保留 snapshot 但只在 ensure 未命中时做一次） |
| 4 | 针对 H4:skinned 蒙皮结果写入每帧复用的 ring VB(SetStreamSource + DrawIndexedPrimitive 替代 UP)，或至少 scratch vector 持久化；多 cascade 间共享蒙皮结果 | CPU + 驱动拷贝带宽 | 中：ring 需要 fence/尺寸管理 |
| 5 | 针对 war3_memory：热路径拷贝统一换 `IsReadableRangeFast`（已存在）或在 ensureGeometry 单次校验后记录"已验证"标记，submit 阶段免查 | CPU | 低：Fast 版本跨 region 返回 false，需对超界 fallback |
| 6 | 针对 M1/M2:stateblock 降级为手工保存/恢复被触的 ~12 项状态；批次级状态设置上提到 execute 开头，draw 内只保留 per-draw 的 SetTransform/SetStreamSource/SetIndices | CPU | 低：需枚举完整被改状态清单，漏一项会造成状态泄漏 |
| 7 | 针对 M3：删除重复统计循环，直接复用 submit 计数；skinned 分支去掉 geometry find | CPU | 极低 |
| 8 | 针对 M4/M5：诊断复查改为仅在显式 diag 开关下启用；`snapshotResolvedItems` 提供只读视图或按需导出；`m_resolvedItems` 存索引/指针而非深拷贝 | CPU 毛刺 + 内存 | 低：诊断信息量减少 |
| 9 | 针对 M6/M7:outline 绘制循环内一次遍历同时计数（首命中前不开 render pass 的方案：先遍历收集命中指针到小型持久 vector);TLS 按 handle 变化才写；两个 image barrier 合并 | CPU/GPU 小幅 | 低 |
| 10 | 针对 L1/L3：删除 native 后端 material map（或仅在 validation build 保留）；核实 mask 分辨率，如全 res 则降半 + 边缘检测上采样 | 可维护性 + GPU | 低-中 |

## 【总体评价】

该范围的优化"骨架"是对的：内容键几何缓存、完整 vertex-input 键的管线缓存、unitCount==0 早退、override 跳过、日志 once-flag 节流，这些都真实生效。但热点执行层存在四处实质性与优化意图相悖的全量开销——palette 缓存以易变姿态哈希为键导致无界增长（缓存反而成为泄漏源）、skinned 几何静态缓冲建而不用且 blend buffer 全局死资源、submit 阶段每 draw 全量深拷贝 + 多次 VirtualQuery(rigid 拷了根本不读的数据）、CPU 蒙皮逐 draw 重算 + 堆分配 + UP 驱动拷贝。这些问题集中在 native 后端的 submit/execute 路径，量级与 draw 数成正比，恰是 AGENTS.md 中反复出现的"CPU 侧负增量"可疑贡献者之一。建议优先执行改进表中 #1~#5（均为该文件内部改动、正确性风险可控），再做状态块与 outline 的小幅收尾；alpha-test payload 的 stash/TTL 消费端成本因实现不在本次范围内，未能取证，建议后续单独核实其每帧 stash 的 buffer 快照开销。
