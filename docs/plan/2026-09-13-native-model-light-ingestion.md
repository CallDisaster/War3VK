# 原版模型点光接入：资源清单、候选白名单与未闭合边界

状态：资源提取/灯光清单完成；点阴影候选名单完成；后续实施 A 已接真实原生采样到诊断帧通道，
但增强照明/点阴影消费 **未启用**。见 [实施 A 收据](2026-09-13-native-model-light-ingress-stage-a.md)。
下文记录资源首轮，彼时未修改 C++；后续实施不追认为产品接受或新的稳定 DLL。

## 1. 输入与可复现输出

主树 `codex/native-shadow-stable-baseline-20260830` / `88089cdf90f728e85b91bf45d75002348574b665`。
保留既有 AutoTest 删除、其他 dirty 源码和子模块，不借用参考工作树的构建或运行证明。

输入仅为 `E:/Work/Warcraft III` 的四个 MPQ，源文件只读打开，处理前后 SHA 一致：

| MPQ | 枚举条目 | 模型文件 | SHA-256 |
| --- | ---: | ---: | --- |
| war3.mpq | 10,684 | 2,048 | `0516A94A2B31DDBF63A305EE59C95E602EEE817C1E7F4EB6D1968F522C74799C` |
| War3x.mpq | 5,843 | 1,272 | `511889AF46359E9C01DD421C1D61818F0D5EA320F016CD281EF22E68D8BBD5DA` |
| War3xLocal.mpq | 1,135 | 0 | `973F32A69D4F4587EB386B85CF4A672BE2E86E78E06307A4023B9D695094F86E` |
| War3Patch.mpq | 770 | 61 | `873825F51EA7213E6A62A40F52967036D6F980411D1F7D76172FEBE12723B53F` |

工具 `AutoTest/extract_native_light_models.ps1` 使用既有
`E:/Work/w3x2lni_enUS_v2.7.2/bin/stormlib.dll` 的 32 位只读 API。
所有条目都检查文件头，提取 MDLX 或 mdl/mdx 名称，不只搜索 Light/Torch 关键词。
拒绝输出目录已存在、路径穿越、超限模型、短读和非正常枚举终止；不调用 archive compact/flush/write。
每个包/locale 单独保存，CreateNew 禁止覆盖。四包未命名条目都是 0。
API 依据：[StormLib 原作者头文件](https://github.com/ladislav-zezula/StormLib/blob/master/src/StormLib.h)。

原字节模型仅保存在 ignored 目录：
`AutoTest/artifacts/native_model_lights_20260913/<archive>/locale_0/...`。
共 **3,381 份，131,859,292 bytes，按模型路径去重 3,264 个**；没有提取纹理、声音或其他资源。
不提交/分发原游戏资产。

输出文件：

- `extraction.json`：2,083,374 bytes，逐文件 size/SHA、源 MPQ、locale 和相对路径。
- `analysis_v2.json`：4,283,152 bytes，SHA
  `16B0C0897F9B94D77DD6CA9237C294BAFCCB35177149658D9A66BB80DB111BA2`。
- `model-light-inventory.md`：67,972 bytes，可阅读的 168 个点光模型路径清单。
- `point-shadow-review-whitelist.json`：6,067 bytes，审查候选名单。
- `analysis.json` 保留为最初分组结果；v2 将 Cinematic 单独排除出常驻装饰候选，节点解析数据未改写。

这不是 game mount 的最终覆盖解析：运行时地图导入、外部包、locale 与资源覆盖都可能改变实际模型。
不能先把全部同名路径合并并声称已经选中了当前游戏内容。

## 2. Python 分析结果与覆盖边界

`AutoTest/analyze_native_model_lights.py` 验证每个提取文件 size/SHA，并验证 MDLX 顶层长度、
唯一 chunk、MDX800、Light/node/track 边界、有限数、父索引、pivot、全局序列和关键帧顺序。
保留静态属性、动画值/插值/tangent、全局序列和 SEQS，不从 static=0 推断灯光不存在。
这是灯光结构检查，不是模型每个 geoset/material 的完整校验器，也没有评价运行中的骨骼姿态。

- 3,381/3,381 解析成功，错误 0，全部 VERS800。
- 204 份模型有任意类型灯光。
- 179 份模型有 Omni，按路径去重 **168 个**。
- 按包内版本合计：235 个 Omni、25 个 Directional、2 个 Ambient 节点。
- 179 份 Omni 模型分类：常驻场景装饰 19、建筑 54、单位/单位特效 12、短暂特效 56、
  Cinematic 9、头像/UI/环境等 27、强度/范围待解释 2。

静态类别只决定审查优先级，不能授权灯光或阴影。建筑灯可能只服务 Birth/Death，
不能见到建筑有 LITE 就让 Stand 永远发光；动画最大关键帧也不是实际当前强度或 Hermite 全曲线界。

## 3. 价值筛选与白名单

版本化候选名单：
`docs/research/2026-09-13-native-point-shadow-review-whitelist.json`。
它不是运行时配置；`runtimeAuthorized=false`，所有未列举资源默认不得自动开点阴影。

第一批选 **9 个路径、11 个包内版本**，全是单 Omni、无父节点的固定场景光，
具有明确照明/近场遮挡价值：

1. Cityscape LanternPost。
2. LordaeronSummer TorchHuman（基础包/资料片两个版本）。
3. LordaeronSummer TorchHumanOmni。
4. LordaeronSummer LanternPost0。
5. LordaeronSummer LanternPost1。
6. City_LowWall_TallEndCapWithLantern。
7. LordaeronSummer brazierOmni（两个版本）。
8. Village_Lightpost。
9. Ruins Firepot。

每条保存完整路径、包、locale、模型 size/SHA、Light ObjectID/名称和选择理由。
TorchHuman 静态强度由 10 变为 4，brazier 的动画强度也随包变化，证明不能只按名称套一组常量。

弹道、爆炸、建筑生成/死亡效果：优先仅照明，不默认加入点阴影。
头像、菜单、昼夜 Directional/Ambient、电影专用模型：不得作为普通世界点光自动入库。
无 LITE 的火焰/光晕不按名字猜灯；多灯魔法桥、发光蘑菇、火山等放后续美术/预算审查。

建议最初自动点阴影最多 2 盏，**共享**当前总点阴影预算而非额外添加；这是待验收预算提案。
不挤掉地图作者手工 JAPI 灯，不把 9 个模型种类理解成可同时无上限放置 9 盏。

## 4. 为什么运行时接入没有伪装为完成

对本机 `Game.dll`（13,187,048 bytes，SHA
`E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A`）
用既有 LLVM objdump 只读复核了下列区间。没有注入、修改该 DLL 或启动进程。

- `0x6F77F2D0`：fastcall，ECX 为求值上下文、EDX 为 node，另一个栈参数，`ret 4`。
  从 context+0x48 -> bundle+0x14 -> handle+0x08 -> [node+0xAC] 取得 CGxuLight；
  原生自己持有/释放引用。节点源记录索引与 Light ObjectID **不能直接当作相同编号**。
- `0x6F77F250`：求值并写 CGxuLight+0x20/+0x1C 的直接/环境强度；不能只读离线常量。
- `0x6F77EFE0`：评价并写 packed color。
- `0x6F77F1D0` 与 `0x6F1AAAF0`：方向/位置变换；后者会修改输入向量，已有文档的 scratch/world
  构建链是定位依据，但仍需把采样点与具体模型实例、最终消费坐标域闭合。
- `0x6F12F0A0` 的现有 Hook_RuntimePoseUpdate 是可能的模型归属 scope，但包括嵌套子模型。
  仅加一个 TLS current-model 指针不足以证明归属、头像排除及递归恢复。
- CLight/COmniLight 环境对象和模型 node_type=1 都能接触 CGxuLight，前者不是全量模型 Light
  的唯一入口。原结构 +0x28 仍不能凭名 `maxDistanceOrRange` 当作 MDX attenuationEnd。

因此没有新增猜测 Hook、没有把 CGxuLight 的不完整字段塞进 AddPointLight，
也没有制造一个没有真实 producer 的“已接通”接口。**原生点光目前仍未自动进入 WarVK。**

## 5. 下一实现边界

1. 精确绑定已挂载模型内容、runtime instance、node 源记录与输出 light；白名单路径只作查找提示。
2. 在原生动画完成后的有效世界 scope 取值；嵌套、异常、界面、离屏更新和未评价灯分别处理，
   不猜可见性，不重新执行全套模型动画。
3. 将位置、颜色、强度、范围、启停复制成受 map/device/frame 域约束的值，不跨帧保存可解引用
   的游戏裸指针；帧尾/换图/销毁撤销灯，手工灯保持独立所有权。
4. 先不带阴影，标定旧强度/衰减到当前 Lambert 的映射；证明原生照明没有重复贡献。
5. 仅对上面的 exact 模型+节点且内容证据匹配者开放点阴影试验；未知、超预算、失配安全回退。
6. 两个同模型实例、父子附件、不同动画、头像、删除、换图、密集特效是必须的运行门。
   CPU/GPU/完整帧墙钟和玩家视觉通过前，不写成稳定收益。

## 6. 验证

- 原始提取遍历完成、零未命名；四份源 MPQ 前后 SHA 不变。
- Python 3,381 个模型逐份 size/SHA 与灯光解析通过。
- 新定向 unittest 13/13，通过正常/缺灯/动画/static=0、其他灯类型、全局序列、Hermite、
  畸形长度/未知版本/重复字段/非有限值/非法索引/时间顺序/路径逃逸/身份失配与默认拒绝门。
- py_compile、PowerShell 语法与 diff 空白检查；不冒充 C++/Win32/实机回归。
- 没有 Ninja、WarVK 构建、部署或游戏/GPU动作；根 CHANGELOG 保持不变。
