# MDX 模型灯光与 Reforged / Classic HD 兼容性调查

日期：2026-09-13。状态：EVIDENCE_ONLY，未实现、未构建、未部署、未作视觉或性能接受。

## 1. 范围与结论

用户要求先查 Hive Workshop 的新版模型规则，尤其灯光；用户目前没有 Reforged 安装或导出资源。
本轮检查公开的一手格式研究/解析器、Hive 站方规则，以及本地经典模型和 WarVK 源码。
没有下载模型、登录 Battle.net、创建游戏 artifact 或启动游戏。

源码范围为主树 `Core/Base/Graphics/dxvk`，分支
`codex/native-shadow-stable-baseline-20260830`，HEAD
`88089cdf90f728e85b91bf45d75002348574b665`；不能把本树观察冒充旁边参考工作树的当前实现。
开工已有 270 条普通 short-status 记录，展开 untracked 文件后为 275 条，包含 258 个既有删除。
保留这些外部改动；本轮只新增本文并追加开发日志，不修改根 CHANGELOG。

结论：模型原生点光有明确数据来源，而且旧 MDX800 已支持。当前优先研究缺口是将已有模型灯光
接入 WarVK 增强点光，而不是假定必须先更换整套 Reforged 几何。新版灯光元数据可以作为后续
补充来源，但没有本机 Reforged 样本，不能声称已经查明哪些官方 Classic HD 模型增加了灯光。

## 2. Hive 与格式变化

- 经典模型已有 LITE / Light 节点，含类型、颜色、强度、衰减起止、环境项和动画；光晕贴片、
  粒子、材质自发光不等于真正照亮其他表面的灯光节点。
  来源：[GhostWolf 原始 MDX 格式研究](https://www.hiveworkshop.com/threads/mdx-specifications.240487/)、
  [Light 解析器](https://github.com/flowtsohg/mdx-m3-viewer/blob/master/src/parsers/mdlx/light.ts)。
- Blizzard 2.0 明确把 Classic HD 与 Reforged 资产作为可选组合；Classic HD 不是 1.27a 旧 SD
  资源的同义词。官方同时调整了 tone mapping 和 ambient，不能把画面差异全部归因于点光。
  来源：[Blizzard 2.0 patch notes](https://news.blizzard.com/en-gb/article/24167122/warcraft-iii-reforged-patch-notes-patch-2-0-0)。
- Hive 站方 2026-07-01 澄清：SD 分类指现代游戏的 SD 图形模式，不要求兼容旧客户端；当前
  投稿规则也明确允许 SD 模型使用 DDS、即使破坏 legacy 兼容。标签不是 1.27a 准入证明。
  来源：[Ralle 的澄清，#28](https://www.hiveworkshop.com/threads/universal-local-fog-glow-shadow-as-a-prop.358900/)、
  [现行投稿规则](https://www.hiveworkshop.com/threads/resource-submission-rules-models-skins-icons-spells-systems.237333/)。
- 规则的 HD 小节要求配套 diffuse/normal/emissive/ORM 材质；这是材质准备要求，不是要求每个
  模型必须有点光。该小节仍使用 mdx1000 措辞，不应据此推定新版二进制布局永远是 1000。
  来源同上投稿规则。原资源的使用、修改和分发权限仍需逐资源确认，Hive 标签不是通用授权。
- Retera 在 2025-08-31 明确提到新版给经典 Light Emitter 增加 ShadowIntensity。
  来源：[Warsmash 作者说明，#110](https://www.hiveworkshop.com/threads/warsmash-mod-engine-alpha.331765/page-3)。

### 新灯光字段的可核对依据

WhiteoutLib 原作者的文档和实际 parser 均在 `version >= 1200` 时，于 `ambientIntensity` 后读取
一个 `float shadowIntensity`，之后才读取动画。检查 revision：
`38d279c2a6d8d439377959c56fe4f1eb9ab12fa6`。
这是第三方逆向实现的一手证据，不是本轮对 Blizzard 客户端行为的实机验证。

来源：[格式 LITE 小节](https://github.com/FernandoS27/WhiteoutLib/blob/38d279c2a6d8d439377959c56fe4f1eb9ab12fa6/docs/MDX_FILE_FORMAT_SPECIFICATION.md#712-lite--lights)、
[parseLight](https://github.com/FernandoS27/WhiteoutLib/blob/38d279c2a6d8d439377959c56fe4f1eb9ab12fa6/src/whiteout/models/mdx/parser.cpp#L822)。

该 parser 的旧版本默认值 0.4 是它的实现选择，不能当作 1.27a 阴影公式或 WarVK 默认值。
本轮未证明 ShadowIntensity 的完整运行时映射或独立动画扩展，也不把它解释为自动授权点阴影。
不能只把 VERS 改成 800：字段长度、材质、纹理及其他块仍可能不兼容。

## 3. 本地模型只读样本

目录：`E:/Mycode/Source/Repos/War3MapReforge/SourceMap/冰冠之陨1.0.232(EBONY_）/resource/war3mapImported`。
以下是项目现有导入/自定义资产，**不是官方 Reforged 或 Classic HD 样本**，也不是全量人口调查。

本轮用只读 PowerShell 按 MDLX chunk 长度遍历，核对 LITE record / node / animation 边界；
所有表中样本为 VERS 800。强度为文件中的原始单位，不是流明，不代表当前游戏已实际渲染到。

| 模型 | bytes | LITE | 衰减起止 | 强度/可见性证据 |
| --- | ---: | --- | --- | --- |
| Firebolt Classic.mdx | 19,064 | 1 个 type 0 Omni | 80 / 200 | static=0；KLAI 10 个关键帧，值 0～300，线性插值 |
| Fireball Major.mdx | 13,346 | 1 个 type 0 Omni | 80 / 200 | static=0；KLAI 4 个关键帧，24/48；KLAV 在 2500 为 0 |
| blood_light.mdx | 4,699 | 1 个 type 0 Omni | 0 / 0 | static=9；KLAV 33 为 1、880 为 0；零衰减不能盲目映射有效半径 |
| [spectacle][light]LightCircle.mdx | 2,423 | 无 | — | 名字含 light 也不等于有灯光节点 |
| [spectacle][light]Glow_white.mdx | 1,580 | 无 | — | 此样本没有 LITE，不能把光晕外观当作点光输入 |

前三个样本的已读灯光轨道 globalSequenceId 均为 -1。尚未按各 SEQS 时间段、父节点动画及实际
播放状态评价 world-space 灯光，因此关键帧范围不能直接等同于当前游戏中的亮度变化。

SHA-256（按上表顺序）：

1. `7E96BC8E74803797485FCB827805B7DDA55319B7B0BEB40F8A1686E2B194A1A4`
2. `A99FB3B9C801DAB08EC311309BB45F25E1026ADFD24E3AFA08CC0BA4E486FFD9`
3. `0BCD650A531D7FADD32BA14CC79D077C68450954C36B0280BCB737B90409F4B6`
4. `0DAA984B1A228DCE574EBF1D734A8CD4A0E247E2DE393D50E90AF919FE521340`
5. `59AC4868F1AC274A14C3A953888C8432E97FA56416B7D755562814369AAF60FE`

## 4. 本树已有能力与接入缺口

- `src/d3d9/d3d9_war3_light.h:36`：当前快照最多 16 点光、4 点阴影；`AddPointLight:63` 本身在
  live 集合达到 16 时拒绝新增。不能假定已有一个接收无限模型灯再自动筛出 16 灯的场景管理器。
- `AddPointLight` 全树调用点来自 pipeline 测试灯、Shader API、visual bridge、JAPI、JASS
  command bridge。此次搜索未找到原生 COmniLight / CGxuLight / LITE 自动进入增强 manager 的路径。
- `src/d3d9/d3d9_device.cpp:17230` 的 SetLight 更新 DXVK 固定功能状态及主方向光覆写，
  并不调用增强点光 manager。不能因此说旧灯光完全没渲染，也不能说已自动进入增强照明。
- `docs/research/war3_render_issues/20_renderqueue_dispatch_layer_reverse/README.md:588` 已记录
  override 灯光数组、`sub_6F77F2D0` 的位置变换和 `COmniLight_SyncGxuLight`。
- `src/d3d9/war3/native/war3_native_renderer.h:785` 有 CGxuLight 结构，其 +0x28 仍标注
  maxDistanceOrRange、构造默认 +INF；本轮不把未闭合字段语义强行当作 MDX attenuationEnd。
  所有地址/布局依据属于 1.27a，不能直接套用 Reforged 客户端。

## 5. 建议路线（设计推论，未获产品接受）

1. **先验证经典模型灯光接入。** 以 Firebolt / Fireball 的正样本和 Glow 的负样本为小集合，
   审计原生完成动画评价后的灯光同步/消费边界，区分世界点光、方向/环境光、头像和 UI。
   如边界可靠，读取同一实例/帧的评价结果，避免每个 draw 重新解析 MDX 或重算整套骨骼。
2. **再做有界自动点光。** 保留父节点/pivot/world transform、动画可见性、死亡与移除语义，
   以 map epoch + 实例 generation 管身份；与人工 JAPI 灯光分别记来源并明确共享预算。
   先无点阴影，确认原生固定功能与增强 pass 不重复加光；增加阴影另设预算和验收门。
3. **拿到 Reforged 后再比资源。** 按准确版本/资源路径成对比较旧 SD、现代 SD/Classic HD、
   Reforged HD；记录是否有 LITE、节点父链、曲线、衰减、新字段。只移植所需元数据时也要做
   节点/动画语义映射，不能按骨骼下标盲拷或仅降低版本号。
4. **独立美术标定。** 旧衰减区间、颜色空间、强度和 ambient 到本地着色器的换算需实测；
   不推断为 inverse-square 或物理光强。没有 LITE 的发光装饰需要作者元数据/手工灯，
   不应单凭贴图亮度或名称自动生成大量动态灯。

下一阶段最低验收：位置跟随、闪烁/爆炸/熄灭、删除和换图无残留、头像不串光、超预算行为、
原生/增强重复贡献、完整帧 CPU/GPU 成本，以及玩家视觉 A/B。格式证明不是性能或画质收益证明。

## 6. 本轮检查边界

进行了公开资料/源码/二进制只读核对和记录；不运行编译、游戏、runnable 或性能测试。
既有 dirty 文件内容保持，唯一既有文本变化为 DEVELOPMENT_CHANGELOG 追加；新文件仅本文。
文本空白与路径/哈希复核属于文档完整性检查，不记为产品回归通过。

## 7. 补充：经典固定功能点光与当前 WarVK 无阴影点光

用户追问两者是否只是同一种照明，以及 WarVK 是否更快。本节仍是源码分析，不是实机测量。

- Microsoft 的 D3D9 固定功能照明合同按顶点计算颜色，支持距离衰减、漫反射、环境项和可选
  高光；可编程 shader 不受该固定功能计算替代。SetLight 不会自动完成遮挡/阴影。
  来源：[Lights and Materials](https://learn.microsoft.com/en-us/windows/win32/direct3d9/lights-and-materials)、
  [Attenuation](https://learn.microsoft.com/en-us/windows/win32/direct3d9/attenuation-and-spotlight-factor)、
  [独立 shadow-map 深度渲染](https://learn.microsoft.com/en-us/windows/uwp/gaming/render-the-shadow-map-to-the-depth-buffer)。
- 本地 `src/d3d9/shaders/d3d9_fixed_function_vert.vert:725` 的 light loop 实现上述兼容路径，
  包含 `1 / (a0 + a1*d + a2*d*d)`、Range、N·L 与材质高光。它本身就是 Vulkan shader；
  不能把 WarVK 内的“原生灯光”与“增强灯光”简单描述为 DX9 硬件对 Vulkan 硬件。
  尚未逐 draw 证明 Warcraft 所有原生灯光都经过 SetLight/这个 shader；现代 Reforged HD 也不在此结论范围。
- `subprojects/war3fx/shaders/war3_shadow_receiver.frag:998` 读已有颜色和深度；`:1527` 起在
  fragment 中遍历最多 16 点光，先按平方距离拒绝范围外像素。首个有效灯才从四邻域深度重建
  点光法线（`:785`），同像素后续灯共用；法线置信度不足时抑制贡献。
- 该点光分支使用范围归一化的自定义衰减和 Lambert，不是完整 PBR 材质光照：
  `x=d/r`，`A=I*(1-x*x)^2/(1+6*x*x)`（范围内）；贡献乘 `max(N·L,0)`、法线置信度、
  0.78 能量尺度、灯色和已有 `col.rgb`。它没有在此读取独立 albedo、normal map、roughness
  或 metalness，也没有点光微表面高光项。不能称为完整 deferred/G-buffer/PBR。
- `src/d3d9/d3d9_war3_shadow.cpp:8182` 提交 receiver 全屏三角形；额外成本涉及既有颜色/深度
  副本与读写、法线重建、灯光循环。若 receiver 已由太阳阴影等功能启用，应区分共享固定成本和
  新增点光的边际成本，不把整个 pass 重复记给每盏灯。
- `d3d9_war3_pipeline.cpp:961` 将 wantsPointLights 与 wantsPointShadow 分开；无阴影点光
  会请求 receiver，但单靠它不会请求 legacy 几何 capture。另有 semantic 强制路径，不能外推
  成“关闭点阴影就保证所有收集路径关闭”。

画质推论：逐像素光斑不受低面数三角形顶点采样限制，但深度法线不等于原始平滑顶点法线或
法线贴图，细轮廓/透明表面有数据限制。两者在没有遮挡项时都不能判断光线是否被墙挡住。

性能推论：经典低面数、少灯场景的逐顶点照明可能比当前屏幕空间方案便宜。原路径主要随被照
顶点、每 draw 活跃灯和提交开销变化；当前方案主要随有效屏幕像素、灯数、覆盖率及共享 pass
成本变化。1920×1080 全覆盖时 16 灯的循环上界约 3318 万次范围测试，不代表同等数量完整
照明计算；早退会减少工作。源码不足以给出实际毫秒/FPS或判定 WarVK 必然更快。

建议测试合同（未执行）：固定同一场景/相机/分辨率，核准同一批灯的身份、位置与动画，分别测
无点光、仅经典点光、仅增强无阴影点光；同时关闭 cube shadow、point contact ray 和体积点光，
防止双重加光。先小样本测增量 CPU/GPU/frame wall-clock，再扩灯数/覆盖率；颜色与衰减应
先作视觉标定，不把不同光强的结果冒充等画质性能对比。
