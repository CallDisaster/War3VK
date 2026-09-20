# 模型系统现状：几何、蒙皮、参数与缓存口径

日期：2026-09-13。本轮是源码/日志审计、定向 IDA 研究与经用户要求的数据库注释写回。
没有 C++/shader 改动、产品构建、部署或游戏测试；不晋升稳定版本。

## 1. 对当前能力的准确描述

WarVK 已有运行时对象、几何、姿态和当前 draw 的桥接系统，能为阴影建立带所有权的输入。
但它还不是“完整 MDX 资产语义系统”：拿到 runtimeModel/geoset 指针、顶点和本帧矩阵，
不等于拥有实际加载文件的内容身份、所有动画节点、灯光、材质曲线和完整模型生命周期。

源码主树：`codex/native-shadow-stable-baseline-20260830` / `88089cdf90f728e85b91bf45d75002348574b665`。
日志解释另核对同级 `dxvk-native-shadow-stage13-current-20260830` 参考树 `a72c700...` 的对应
生产者与 exporter；运行报告本身绑定 A570 DLL，不把当前主树等同于该运行产物。

## 2. 对象与多边形怎么拿到

### 2.1 上层语义链

`WorldObjectEntry / CUnit / CSprite -> SceneNode -> RenderablePart + layer -> runtime model / geoset`。

- WorldObjectEntry 的渲染 Hook 建立有界当前对象上下文；VisibleRenderableRegistry/ShadowObject
  记录可见部件、layer、对象身份、句柄/rawcode 与模型关系。不是仅看 D3D9 draw 次数猜对象。
- 模型资源入口/可见资源绑定可到 CGeoset/CGeosetData。`ShadowModelResourceCache` 记录/按需
  填充 position、normal、UV、UINT16 index、primitive/material slot，以及 vertex group、
  matrix group sizes、bone indices。顶点/索引/primitive 组合构成三角形，不从屏幕图像还原多边形。
- 资源缓存发布完整不可变 payload，并附 map epoch、单调 generation；仅有地址/记录不算 ready。
  它证明缓存内容身份，不自动证明此刻 Game.dll 内存或每个 draw 的来源正确。

代码入口：

- `src/d3d9/war3/hooks/war3_hook_render_identity.cpp`：WorldObjectEntry_Render。
- `src/d3d9/war3/render/war3_visible_renderables.cpp`：part/layer 索引与可见记录。
- `src/d3d9/war3/model/war3_model_resource_cache.cpp:442`：CaptureGeosetRecord；
  `:1287` 的创建路径先登记 header，ready 后复用，不能把所有 header 记录当完整几何。
- `src/d3d9/war3/model/war3_model_resource_cache.h`：完整 payload、ready/generation 和 source-authority 边界。

### 2.2 当前 draw 的精确绑定

`CurrentDrawContract` 将 renderablePart、layer、mesh payload、可见帧、VB/IB/UV 范围和本次
调色板绑定起来；缺身份时查可见记录补齐。D3D9 draw-time 入口掌握实际 buffer、offset、stride、
index domain、primitive 和变换状态。静态模型文件不能代替它，因为动态 geoset、附件、裁剪索引
和不同 layer 的实际 draw 会变化。

主路径与兼容路径并存：上层 semantic/CurrentDraw 构建对象阴影，部分地形或未覆盖路径仍用
受门控的 capture/freeze/replay。不能说现在所有对象都是“从 MDX 一次解析后直接画”，
也不能说全部仍靠旧 D3D9 抓帧。

最终安全输入进入自有快照/Arena 或已证明的 persistent geometry，再发布 canonical caster，
CSM/receiver 使用已发布结果。复制/冻结是在保护异步 GPU 使用期，不应按“有缓存”就删除。

## 3. 蒙皮的三层必须分开

### 原生动画与矩阵组

经典模型的 vertex group 指向矩阵组，组内列出骨骼索引。原生动画先求当前骨骼变换，再由
`CGeosetData_BuildGroupBlendedPalette (0x6F12E600)` / `CMatrixGroup_BlendOutputMatrix
(0x6F12E200)` 生成每组 48-byte 3x4 变换：一骨直接拷贝，两/三骨取平均，通用多骨累加后平均。
这是本轮新核对的经典 matrix-group 路径，不外推为 Reforged 的任意权重蒙皮格式。

### WarVK 阴影中的蒙皮

- 以当前 draw 的 group slots/完整矩阵快照表达姿态，静态 position/index/组映射可以缓存，
  姿态会随动画和实例变化；同一模型两单位不能共享一个“上帧姿态”。
- 阴影顶点 shader 支持根据 group/blend indices 和 palette 变换顶点；已得到原生变形后 VB
  的路径可以直接投影，不应再次蒙皮。
- `war3_current_draw_contract.cpp:1295` 解码当前 contract 的 captured palette；
  `subprojects/war3fx/shaders/war3_shadow_caster_vert.vert:133` 区分直接变形输入、刚性变换与
  混合变换。VS-B1 的 immutable+palette 实验分支不代表当前全部运行都启用了它。
- 当前主树重型 SpriteFrame pose hooks 默认关闭，matrix publisher hooks 也并非全部默认开启；
  不能把源码里所有可用 Hook 同时画成正在运行的链。按当前可见 contract 取值是重要路径。

### 替代原生主画面 CPU 蒙皮

`war3/gpu_skin/` 另有 native bridge、资源、compute/VS、输入能力证明、bypass 与回退控制。
这套替代系统是否运行，必须查看 `gpuSkinSnapshot.mode/hooksEnabled`，不能由阴影中出现
Skinned packet 推断。最近详细报告这两项是 **disabled / false**，没有原生 kernel bypass。
因此“我们支持 GPU 阴影蒙皮”和“主画面 CPU 蒙皮已经全部省掉”是两件事。

## 4. 最近报告中的缓存数据

输入：`E:/Work/Warcraft III/WarVK/Log/war3_perf_report_2026_09_12_15_42_28.html`，
4,802,693 bytes，SHA `25ED941B2295741911BE85D39932A734CC6A06ACF452C17D428C93755A831ACB`。
DLL `A5701AF3F3724683E559B4F142F7E45DD0EFCA8F6B3DF31C26842EBC10675A7A`，
full_default/detail，3600 帧/37.107s，100.709 FPS。它是最近找到的详细录制，不是当前实时测量。

递归重复键拒绝解析，通过唯一 const-data 根与 workload 列/行完整性核对。
`framesObserved=22779` 和 3600 条 workloadSeries 不同域；累计字段不可直接除以3600。

| 层 | 证据 | 能说什么/不能说什么 |
| --- | --- | --- |
| 可见记录查询 | 导出累计成功5,679,558，sole fallback=0，miss=1299，成功率99.9771% | 身份/部件查找可用率高；计数含内部回退扫描，不是纯哈希命中率，更不是几何免复制率 |
| 有代际证明的 stream 复用 | 3600帧 position=25,200，index=25,200，均7次/帧 | 确有这两类复用；缺对应eligible/miss分母，不能声称全模型缓存率 |
| draw-time capture | 3600帧440,966次，122.49次/帧 | 仍有当前 draw 工作；同帧dedup miss为26次/帧，但同类hit未在此series导出，不能算命中率 |
| 同帧 freeze catalog | 聚合91,116次hit，hitBytes=2,243,320,034 | 已有重复冻结复用证据；没有同口径查询分母，不能给百分比或归为3600帧带宽收益 |
| 旧VB consume分支 | hit=0、miss=0 | N/A，没有可用命中率；不能说0%或100% |
| Stage13专门持久缓存 | 此3600帧hit=0 | 本场景未显示该条路径收益，不等于所有persistent机制失效 |
| GPU蒙皮替代 | disabled，hooks=false | 未运行；不是缓存命中率0% |

对象身份同帧dedup两个统计各有 attempts=5881、hits=0，拒绝以 incomplete/crossFrame为主。
这是另一条特定复用优化没有命中的证据，不是可见对象整体查找失败。

### 不可靠命名不能转成“100%缓存率”

参考树 `war3_perf_monitor.cpp:9847` 附近实际输出：

- `modelRegistryHit=55` 来源 **modelRegistryCount**，`modelRegistryMiss` 写常量0。
- `modelReuseCount=584` 来源 **instanceRegistryCount**。
- `poseCacheHit/Miss` 均写常量0。
- geoset记录1843，ready不可变payload记录7：是填充状态统计，不是命中率或画面覆盖率。

因此当前没有可信的单一“全模型／全蒙皮缓存率”。本轮只澄清，不修改冻结报告或 exporter，
不拿这些假分母制造100%或0%收益结论。缓存观测规范本身仍需改进。

## 5. 下一步应有的模型参数系统（设计建议，未实现）

| 层 | 身份和数据 | 生命周期/消费者 |
| --- | --- | --- |
| AssetDefinition | 实际加载bytes SHA、resolved路径；geoset拓扑、材质层、纹理、骨骼/组映射、Light/Attachment/Emitter和动画定义 | 内容版本级不可变；加载缓存、渲染、照明共用 |
| RenderInstance | runtime generation、asset关联、map/device域、world transform、动画时间、当前palette、light enabled/color/position | 对象创建/销毁/换图；各实例不能共享动态状态 |
| DrawContract | 当前part/layer、VB/IB/UV精确范围、pose/frame/revision、透明裁剪与渲染stage | 当前draw证明；兼容原生动态修改 |
| RenderSnapshot | 消费者自有值/资源引用、预算、最后使用fence | 渲染所有者发布/退役；不让裸游戏指针异步逃逸 |

优先建立统一的模型资产描述与实例参数快照，而不是给每种灯/每个渲染特性单独再加一套
扫描器。模型路径只用于查找，实际内容/代际授权；snapshot读的是游戏已评价后的结果，不
在每个draw里重读MPQ或重新运行整套动画。灯光是第一项参数消费者，后续可扩材质、附件和发射器。

元数据状态应明确 Unknown/Observed/Complete/Rejected；按参数种类标完整性，不能拿
“几何已ready”自动授权灯光、透明材质或骨骼。旧预加载缓存缺bytes证据时仍保持未知。

每层单独记录 attempts/hits/misses/invalidation/reject、entries/capacity、实际省去的bytes
与CPU/GPU耗时；统一报告窗口，并保留累积与last-frame标识。API设计完成不等于性能收益成立。

## 6. IDA 写回收据

- 输入Game.dll `E04D1716...D0C3A`，image base 6F000000，x86。
- 对 **25个函数重命名、28个函数追加研究注释、5个指令点追加说明**；保留旧注释和旧名称。
- 全部读回验证，保存 `E:/Work/War3/Game.dll.i64`。随后刷新对应反编译缓存并重新反编译
  1322B0，确认新函数名与 owner 注释已出现在反编译文本，不只是脚本报告成功。
- 不修改函数原型/全局结构类型、不修补bytes，不把估计字段强套到整个数据库。
- 写回前备份：`AutoTest/artifacts/native_model_system_ida_20260913/writeback_v3/before-writeback.i64`，
  253,811,440 bytes / SHA `FF530FA0C87EE22E7F1C74D4E49E31CCF46586705339489ACD7E5D88B99EBD74`。
- `before.json` / `plan.json` / `after.json` / `verified.json` 保留准确地址、旧名/旧注释、
  新内容和验证结果。首次接口参数与stdout回显兼容错误均在正式修改前失败；没有覆盖原备份。
- 新工具 `annotate_native_model_system_ida.py` 仅提供固定清单prepare/apply/verify。
  生效前先校验数据库SHA及前检元数据不变；重名/并发改动即拒绝，旧证据禁止覆盖。

相关可读证据：原生灯深入证据 `2026-09-13-native-model-light-ida-findings.md`；
本轮MDX/几何/骨骼新查询及缓存计算在 ignored `AutoTest/artifacts/native_model_system_ida_20260913/`。
本轮产物是研究与维护信息，不是新DLL、不改变游戏画面或性能。
