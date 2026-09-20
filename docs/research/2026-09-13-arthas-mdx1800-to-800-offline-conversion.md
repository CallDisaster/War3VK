# Arthas MDX1800 → MDX800：Base Color 离线转换

2026-09-13。状态：OFFLINE_CANDIDATE；未部署到地图、未启动编辑器/游戏、未作玩家或性能接受。
本轮不修改引擎、地图、玩法或共享构建；原始美术资产不进入开源仓库。

## 输入与隔离输出

输入：`C:/Users/Administrator/Desktop/arthaslordaeron/arthas_blackhood.mdx`，
2,092,768 bytes，SHA-256 `338bb4e002616d477c2623df974a992531acd920ee97362421b549bace327eca`。
格式实测 VERS 1800；目录另外含 diffuse、normal、emissive、ORM 四张 DDS。
只消费 diffuse；2048×2048，RGBA alpha 全为 255，SHA
`a1e7c2cf8860c860b40eb6030d330a93e2b22ecb8663e46f10198b615e98fa7b`。

脚本与锁定依赖放在同目录 `_conversion_work/`，交付目录 `converted_127a/`。
第三方依赖为 MIT 的 war3-model 4.0.1、mdx-m3-viewer 5.12.0，Pillow 12.1.0；
只在隔离工具目录 npm install --ignore-scripts，没有改项目依赖或运行第三方安装脚本。

## 现有理论及本样本适配

基础为 [模型系统研究](2026-09-13-model-system-geometry-skin-cache-overview.md) 的经典矩阵组平均；
以及 [兼容性调查](2026-09-13-mdx-model-lights-and-reforged-compatibility.md) 的不能只改版本号原则。
本次只读 IDA 复核 0x6F12E200（0x3F2 bytes）和 0x6F12E600（0x7A bytes）。
前者的通用分支按提供的索引逐项求和再除以组长，后者按 MTGC/MATS 顺序推进；
不是假设最多四根骨，也不能把图形浏览器的四槽 shader 限制当成原生 CPU 合同。

通用 parser 会把此样本 SKIN 当 uint8 然后扫描到 UVAS，从而表面加载成功、实际权重错误。
本轮独立按 GEOS 包含长度及 VRTX/NRMS/PTYP/PCNT/PVTX/GNDX/MTGC/MATS、扩展头、
extents、TANG、SKIN、UVAS/UVBS 顺序验证准确消耗。本样本 SKIN count 是元素数，
物理宽度为 uint16：每顶点 4 索引 + 4 权重。验证 9,731 顶点每个分量不超过 255、
权重和全为 255、所有非零权重引用有效 Bone；这个发现只授权本样本，不宣称完整支持所有 1800。

设原整数权重 `w_i`，总和 255；先合并同骨重复权重，令 `g = gcd(w_i)`。
将每根骨索引重复 `w_i/g` 次，矩阵组长 `N=255/g`。经典平均给出：

`M_group = (sum_i (w_i/g) M_i) / (255/g) = sum_i (w_i/255) M_i`。

这保留任意姿态的矩阵线性组合，而不是只保留主骨或等分原四骨。
权重比例通过整数交叉乘法逐顶点验收；浮点逐项累加仍不保证与 HD shader 逐位相同。
GNDX 8 位，每个新 geoset 最多 240 组；按三角形保持绕序拆分，跨分片复制边界顶点。
原 2 geoset → 10，9,731 → 9,912 顶点；14,456 三角形不变，法线、UV 与位置逐值相同。

代价：2,159 个矩阵组、445,669 个索引项，单组最长 255。源码证明数学可表达，
不等于所有第三方编辑器都支持、不等于原生加载验收或可忽略的运行开销。
不得自动 dedupe 重复骨索引，也不宣称性能收益。

127 骨骼、15 动画、8 附件点保留；MODL/SEQS/BONE/ATCH/PIVT/EVTS/CLID 原块按字节保留。
8 个 CORN 发射器不兼容 1.27a，改为同 ID/父链/pivot/变换动画的无效果 Helper；
153 节点都存在且父链无环。移除 CORN/BPOS/TANG/SKIN/HD 材质，输出旧版材质单层 Base Color。
没有伪造原有光效、独立头像、PBR 或阵营色，也没有把 BPOS 当成必须重烘焙的额外运行时矩阵。

## Base Color 编码的判退与修订

最初普通三分量 JPEG 的 BLP 能由通用图像库读出，但 Warcraft 专用解码器输出通道步长不匹配；
模型预览出现杂色。这是无效中间候选，不能用“尺寸正确”称为材质验收通过。
最终用非标准 BLP JPEG 的四个独立 BGRA 分量，移除 CMYK/YCbCr 色彩变换，
BLP1 JPEG quality95、无色度降采样、2048→1 共 12 mip。两套 Warcraft 解码器 RGBA 全一致，
每 mip alpha 全255，与解码后原 DDS 的 RGB PSNR 为 44.85998 dB。没有生成、融合或烘焙其他贴图。

## 验证、产物与边界

- 两个独立 parser 都回读 MDX 和 MDL 为800；只含经典块；几何、索引、UV、组和纹理路径校验通过。
- 105 姿态覆盖所有15动画，使用 war3-model 节点求值后的原 weighted skin 与转换 matrix-group 比较。
  double 最大位置差 1.148e-13，float32 逐项累加模拟最大差 0.00146954 模型单位；
  使用的是离线动画求值，不是假装在 Blizzard HD/1.27a 客户端完成了 A/B。
- Playwright 驱动本地 WebGL 的全 N 项软件蒙皮，所有15动画中点 GL error=0，console error/warning=0。
  已看待机正面/侧面、攻击三分之四、施法全身；未见明显爆点、错纹理、断肢或分片错位。
  四张图、版本、结论和剩余门见交付目录 `qa_views/REVIEW.md`，无重复图片/大载荷存入文档。
- MDX：3,574,248 bytes，SHA `807235d84fc1828599ae36f06b0948860f655e0903d327377a17ee80d205a0de`。
- MDL：6,698,194 bytes，SHA `42c3e96fb7367ad873f445c470e185b4b65f3b9addcd7c5910c80d4b9bfb91aa`。
- BLP：5,793,311 bytes，SHA `da3de102f9e24852d6c93104e3d18c580ef44adb3e6a219c279182e2e222236c`。
- 未验证：1.27a 原生加载/选择/碰撞/附件/动作触发、真实照明视觉、大量单位 CPU 成本、第三方编辑器限制。
  不更新稳定 CHANGELOG、不构建/部署 DLL，全部既有用户 dirty 状态保留。

## 一手资料

- [GhostWolf Geoset parser/writer](https://github.com/flowtsohg/mdx-m3-viewer/blob/master/src/parsers/mdlx/geoset.ts)：
  经典 GNDX/MTGC/MATS 与新版 TANG/SKIN 的布局边界。
- [war3-model 原作者实现](https://github.com/4eb0da/war3-model)：模型解析、导出、节点动画和软件蒙皮。
- [HD skin vertex shader](https://github.com/4eb0da/war3-model/blob/master/renderer/shaders/webgl/hdHardwareSkinningNew.vs.glsl)：
  相同节点变换的加权矩阵消费；不是 Blizzard 官方 shader。
- [BLP 专用 JPEG decoder](https://github.com/flowtsohg/mdx-m3-viewer/blob/master/src/parsers/blp/jpg.js)：
  BLP 非标准多分量通道；最终使用该库和 war3-model 两个 decoder 校验，不以普通 JPEG 解码代替。

图形算法依据仅用于这个外部模型的离线资产转换，没有授权任何 WarVK Release 默认变化。
