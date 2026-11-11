# 原生渲染输入采集第一条链：实施与验收边界

日期：2026-09-30。主树 `dxvk`，HEAD `a2054a9776e40edc3f01f6f390c06037965b8ef2`。用户要求补数据采集，本轮实施 WarVK 内部采集链，不实施共享核心 B、不修改渲染策略。

## 已实现

1. `NativeDrawMaterialFacts` 是 344 B 定长、按值拥有的 CPU 观察。记录 17 个 render state、10 个 stage-0 state、13 个 sampler-0 state、D3DMATERIAL9 的 17 个浮点原始位、TEXTURE0 变换的 16 个原始位；携带 part/layer、map/device epoch、原 ShadowProducerSerial 和 shader 存在位。
2. device helper 的第一条路径检查既有 raw-input 子门；关时不读这些额外设备状态。它只读 draw 入口已有设备锁保护的 CPU 状态，不读游戏裸指针、不查 native arena、不加扫描/锁/日志/GPU 读回。sampler 按公开 ID 索引；texture stage 必须通过 `RemapTextureStageStateType`；矩阵按 DXVK `data[0..3]` 的 x/y/z/w 存储原样保留，不额外承诺数学 row-major。
3. 实际 native entry 替换几何时同时携带该 entry 的观察；exact alpha 元数据最后替换 alpha/UV/texture 时同时替换观察。采集不重新选择一份“更完整”的旧 palette 或材质。
4. 观察随原有 entry/caster/payload owner、跨图退休和 reset 迁移，无新缓存或生命周期系统。Stage13/S1 跨帧复制保留旧 sourceFrame，不能用重新上传几何、lastSeen 或 batch frame 伪造新材质。
5. 既有 `InputCapture` 在 prepared shadow replay 点按最终 draw 与 batch 的地图/设备身份、part/layer 验证观察。无身份或不一致时保持 Unknown，并逐批报告 `nativeMaterialIdentityRejected`；不为诊断填充 shadow 身份、改变 caster 准入。保留 NaN/Inf 的原始位，严格读方拒绝它，不偷偷改成 0、白色或 null。
6. 单独记录 56 B `FinalShadowMaterialFacts`：alpha/ref、blend/depth/additive、UV 格式/范围及 prepared texture view/sampler/slot。它是 prepared replay 材料，不是未经修改的原生材质，也不证明 GPU 执行；后续 fallback 或实际 draw 关联仍以既有命令事件为准。
7. `.inputs.json` 升为 schema 3，新增每 draw 的严格 `renderInputEvidence`。实际生产 encoder 与 Python reader 共用往返夹具。schema 1/2 继续读，不补造旧文件不存在的观察。CPU event wire 与共享 A2 格式未改。

## Unknown 与缺失是正常结果，不是默认白色

- fast/native entry 虽可能有源数据，最终 caster 没有可靠 part/layer 时不回填渲染身份；upper/compat/general legacy 同理。attachment legacy 原本带 exact 身份时才可有观察。
- `available` 仅表示这些 CPU 字节曾被观察，不是当前帧新鲜性、实例存活、原生分配所有权或消费许可。sourceFrame 为 `ShadowProducerSerial`，与 event/batch render 帧域分开，不做异域相等判断。
- device `SetMaterial` 已强制修改 Ambient。shaderMask 仅“有无 shader”，没有保存原 shader 代码/常量；stage/sampler 仅 0 号。这些值不能冒充完整 MDX/GEOA、材质动画或原画面最终 RGBA。
- 每个 schema-3 block 固定列出六项缺失：原生实例生命周期、原 MDX geoset 对应、求值 GEOA alpha/RGB、动画时钟、纹理像素、全场景 pass 覆盖。`sharedRendererReady=false` 固定。
- 此 provider 仍是有界 Stage11 shadow 样本：每 slot 至多 128 draws/256 KiB、优先两个既有异常形状后采其他 Stage11；eligible/omitted 原有计数保留。没有全场景 Forward 输入/粒子/条带/UI/战争迷雾，也没有自动 renderer 接管。

## 容量与成本

- 无新 GPU 历史环或 per-event 堆分配。但 entry/caster 的固定 POD 确实增大了，关闭开关不能消除此复制/驻留成本。
- `MaxDrawMetadataBytes` 1088 → 1472，生产 `sizeof(Draw)<=MaxDrawMetadataBytes` 继续阻止失账。两种观察合计 400 B；metadata 上限采用保守 round-up，不称其为实际 Draw 的精确大小。
- raw inputs 的完整 CPU payload 准入上限 220.5 → 247.5 MiB；内部 224-slot profile 85.75 → 96.25 MiB。这是诊断容量账户的增加，不是提高 Arena/Stage11 显存预算；只有启用 raw-input 历史才按既有准入申请。
- Win32 helper 宿主测试各执行 100,000 次开/关路径：测量窗口 `new` 次数 0，位校验通过。一次独立复跑 enabled 85.443 ns/op、disabled poison-view 51.790 ns/op，包含 noinline 包装和校验开销。它不是游戏帧耗时、不保证其他机器、不代替完整帧 p95/p99 与长期内存测量。

## 本次验证

- 独立本树 `build32_clean_diag`：BelowNormal、`-j2`，exact `src/d3d9/d3d9.dll` 编译 exit 0，最后 exact dry-run no-work。未使用旧 build32 发布构建，未构建其它 DLL/默认 all。
- 定向 Meson **8/8**：POD、生产 encoder、encoder/strict-reader 往返、严格块合同、manifest 集成、CPU 成本、两种既有 recorder memory 生产测试。
- POD **1296** 检查、encoder **262** 检查、recorder memory **46+87** 检查；生产 encoder/reader 往返 **7** Python 用例。
- 严格块 **57**、manifest **45**、接线静态 **27**；另受影响 memory/alpha metadata/replay/skin/current-UP **75** 个 Python 用例通过。不同类别不混成“全库通过”。
- 六个相关 Python 文件 `py_compile` 与 `git diff --check` 通过。开工 93 → 收口 111 个 status 路径：仅增加 7 个此前干净的改动路径 + 11 个新文件；开工 92 个 hashable dirty 文件中，仅本批四个重叠获准文件（device.cpp/.h、d3d9/meson.build、开发日志）变化，其余 88 个逐 SHA 不变、无文件缺失。HEAD 与 StormBreaker HEAD/status 不变；本批共触及 22 个文件。
- 编译完成时游戏/编辑器/编译进程查询为 0；最后再次收口发现用户目录的 `worldeditydwe.exe` PID 45456 已于 21:46:48 启动，因此最终是编译/游戏进程 0、编辑器 1，不冒称全相关进程为 0。本轮未启动/关闭该编辑器、未部署/启动游戏/GPU；编译资源已释放。
- DLL PE32/pei-i386/i386，**36,551,002 B**，SHA-256 **24C90B03AE1052882217BC24BA11990BB7B7F1E0784828D153587FDFDFB082FE**。仅本地源码候选，未做 release-clean 包审计，不替代既有 RC 身份。

## 后续顺序

1. 先在既有 raw-input 配方下核 Stage11 对象的 observed/unknown/identityRejected 覆盖，并与原生画面/实际 command 做同对象关联；当前无本次游戏或 GPU 证明。
2. 下一采集层补原 geoset/group remap、稳定实例生命期和动画/GEOA 求值来源。没有来源证明不读猜测偏移，不把 override-only 时间或模型资源 generation 冒充实例生命周期。
3. 扩展主画面 pass/多层纹理/程序状态与受限纹理资产交接；原始模型与纹理不在本轮重新打包。
4. 上游共享仓库到位后才按已批准 A1/A2/A3 签发适配输入，独立完成组合矩阵与截图差分；本批不授权共享后端接管或产品发布。
