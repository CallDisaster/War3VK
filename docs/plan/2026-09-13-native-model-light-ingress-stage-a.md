# 原生模型点光实施 A：真实采样入口与默认关闭的帧内通道

状态：SOURCE / OFFLINE VERIFIED。真实 Hook 调用点已接入诊断帧通道；
**不是增强照明已生效，也不是点阴影已启用**。无产品 DLL 构建、部署、游戏或视觉接受。

## 实现范围

- `Hook_RuntimePoseUpdate`：复用已安装的 `0x12F0A0` 姿态/世界矩阵更新 Hook，原函数只调用
  原有次数，原参数/返回值不变。原函数前记录帧域 ticket 与 modelResource，返回后再次核对。
  仅在 WorldFramePrepare 未结束或 WorldRenderScene active 时允许记录；perf-only 分支仍直接
  原样返回。没有新增 Detour 或根据函数名猜调用约定。
- `NativeModelLightBridge`（IDA 纠错版）：从 **runtime CModelComplex 本身 +0xB4** 读取
  capacity/count/data（三字段，count/data 为 +0xB8/+0xBC）；不从 runtime+0x9C 指向的共享
  CModelData 读灯数组。前文早期“运行时 CModelData 有实例灯数组”的解释已由汇编推翻。
  新 layout reader 检查两个精确 CModelComplex vtable、flags.bit4、共享 backing 前后身份、
  数组容量/16 灯上限/地址溢出，再复制 44-byte CGxuLight。创建/克隆/求值/析构链交叉证明
  实例所有权。初始化核验 builder 传入 &instance[B4] 与 node_type=1 读数组的两段指令；
  这是局部布局准入，**不是完整 Game.dll 内容身份或画面正确性证明**。
- `NativeModelLightFrame`：128 条固定容量、帧域更新与 map reset 清空；键为 runtime/modelResource/
  nativeLight/outputSlot 的完整组合。相同身份更新，包括 enabled=0；同帧指针/slot 换身份、
  多线程混写、溢出均 fault。旧 ticket、seal 后写入被拒绝，返回独立值快照。
- 观察最多接纳每帧 4096 次模型入口，达到上限会置 fault，而非将截断结果称为完整清单。
  只在最初 4 个有数据帧和后续每 256 帧记摘要；每次摘要只显示前 4 条示例，不是全量导出。
  计数 stored/updated/named 属于采样路径，不可当作实际呈现/复用人口。
- `War3Renderer::BeginFrame/ResetMapSession` 接逐帧清理及地图重置。不会强制开启额外渲染、
  semantic producer 或 GPU pass；若正常跟踪路径不调用 BeginFrame，观察不会获得有效 ticket。
- `war3_native_model_light_policy.h`：与已冻结 JSON 一致的 11 个资源版本。匹配完整路径、
  SHA-256、文件大小、MDX ObjectID；仅允许斜杠/ASCII 大小写规范化，不允许目录前缀/模糊搜索。
  返回的是 trial candidate，不是渲染权限；sampler 的 outputSlot 不冒充 ObjectID。

## 开关

- 新的独立 Meson 选项 `warvk_native_model_lights_dev=false`，默认关闭。
- 只有编译为 true 且环境 `DXVK_WAR3_NATIVE_MODEL_LIGHT_OBSERVE=1` 精确成立才采样。
- 专用宏 `WARVK_NATIVE_MODEL_LIGHTS_DEV`，不借用或改变旧 Shadow observer、V3 或其他实验开关。
- 模型帧快照中 `lightingAuthorized=false`、`shadowAuthorized=false`；没有 AddPointLight 或
  shadow receiver 发布调用。没有修改 War3LightManager、shader、JAPI 或渲染设置。
- 不建议用户在当前 DLL 上仅设置环境变量就期待效果：本轮未重建任何产品 DLL。

## 新确认的后续阻断项

IDA 最终纠错证据保存在 `AutoTest/artifacts/native_model_light_ida_20260913/`：
`0x6F12F0A0` 是姿态/StagePreset 入口，`0x6F1322B0` 是按模板逐项克隆 CGxuLight，
`0x6F13D9C0` 从解析记录初始化一盏模板灯，`0x6F77F2D0` 更新克隆出的实例灯。
`CGxuLight +0x28` 是原生 priority-queue selection score，现已改名 `nativeSelectionScore`，
不能当作 MDX attenuationEnd 或 WarVK 半径。完整最终结论见
[IDA 纠错与加载链证据](../research/2026-09-13-native-model-light-ida-findings.md)。

主树 `ModelRegistry::recordSpriteModelPath` 在 `src` 中只有声明和定义，没有产品调用点。
因此 record 里的 modelPath 不能视为已闭合来源。本候选允许 path unknown 并明确计数，
绝不把空路径/原生裸指针猜成白名单路径；不在渲染热路径通过文件重载伪造“当前已加载内容”。

下一阶段必须补真正的模型加载 producer，将**实际加载的 bytes/SHA、资源代际、实例与节点**
关联；不能只读取同名磁盘文件，因为地图/locale/外部覆盖可能不同。还需闭合 MDX 衰减的
运行时映射、世界坐标域和对象删除可见性。当前仅做 snapshot 诊断，没有借帧内原生
指针的可读性来授权跨帧灯光。

首次无阴影照明试验还须处理原生固定功能重复贡献。此项没有凭经验关闭原生照明，避免改变
现有画面。以上门完成后才将验证后的灯光值接入 War3LightManager 的独立预算/所有权通道，
再按白名单申请点阴影消费；当前 A 阶段不应被包装成已完成整个接入请求。

## 离线验证

本节先记录 A 初版历史检查。其“通过语法”不代表 native 偏移正确；后续 IDA 发现偏移错误，
现已修正。纠错版通过 260 条 Win32 断言（新增 18 条实例/共享资源布局反例）、21 项 Python
定向门、实际 TU 开关 0/1 共 4 次语法与 4 次预处理检查，最新收据在
`AutoTest/artifacts/native_model_light_ida_20260913/validation/`。没有产品 DLL 构建或实机验收。

- 独立 i386 C++ runnable：242 个断言通过，覆盖正常、inactive 更新、帧/地图 reset、迟到
  ticket、seal、线程冲突、指针/slot 身份冲突、容量、有限性、非法字段，以及 11 个资源版本的
  正例与 path/SHA/size/ObjectID 错配。
- 新 static：6/6；既有模型解析 tests：13/13。均为定向检查，不称全量项目回归。
- 两个真实改动 TU（war3_model_hook.cpp、war3_renderer.cpp）在宏 0/1 下共 4 次
  `-fsyntax-only` 和 4 次预处理检查通过；宏 0 的预处理文本没有 NativeModelLightBridge。
- bridge 头在宏 0/1 下独立语法检查通过。未生成这些 TU 的 object，未运行 Ninja/链接 DLL。
- 所有编译检查父/子进程使用 Below Normal，顺序执行；没有并发编译或游戏/GPU行为。
- 收据：`AutoTest/artifacts/native_model_light_ingress_20260913/`，包括 runnable 及
  `syntax/validation.json`、4 份编译日志。现有 build32 产品没有被替换。

## 源码复现入口

工具 `AutoTest/validate_native_model_light_syntax.py` 只从既有 compile_commands 提取 include/define，
固定编译器、固定两个 TU；不执行存储的 build command、不产生依赖文件和 object，也不配置 Meson。
新值类型 runnable 已登记为 `war3_native_model_light_frame_test`，将来的产品构建仍需独立核验
实际 fan-out、完整性及现场权限，不能拿本轮语法检查替代 DLL 构建。
