# 原生模型点光：IDA 最终纠错与加载链证据

日期：2026-09-13。来源：用户已打开 IDA 的本机 MCP `127.0.0.1:13337/mcp`。
输入 `E:/Work/War3/Game.dll`，image base `0x6F000000`，x86，自动分析完成。
输入 SHA-256 `E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A`，
13,187,048 bytes，与 `E:/Work/Warcraft III/Game.dll` 相同。

本轮不修改 IDA 类型/名称/注释/二进制，不调用保存 IDB、调试、游戏启动或部署。
反编译缓存可能随正常分析变化；不声称 IDB 文件逐字节不变。
源码变更仅纠正本线程默认关闭的采样候选，不开启增强照明或阴影。

## 1. 必须撤回的旧解释

**`runtime+0x9C → CModelData+0xB8/+0xBC` 不是实例灯读取路径。**
上一阶段通过语法与值类型测试，但没有测试 native 地址所有权；它仍然可以从错误地址读数据。
本轮前半程对同名 `CModelData` 的口头解释也不正确，以下以调用寄存器及汇编为准。

| 对象 | 已证明字段/作用 | 一手证据 |
| --- | --- | --- |
| 实际 CModelComplex / runtime | +0xB4 capacity、+0xB8 count、+0xBC CGxuLight** | 1322B0 ctor-copy、12E900 builder、1307B0 dtor |
| runtime+0x9C | 引用计数共享 backing，实际 CModelData；不可作为实例灯表基址 | 127610、121B00、130CD0、04F200 |
| 缓存中的模型模板 | 同样具有模型实例布局，供 12A5C0 克隆；不是上述 shared backing | 128140/1260C0/13DAA0、12A5C0 |
| 已解析 MDL/MDX document | +0x30E0 light count、+0x30E4 376-byte Light records | 8319F0、13DAA0 |
| CGxuLight +0x28 | 每次选灯更新的估算贡献分数，不是范围 | 0CC650 写入、0CC0E0 优先队列比较 |

现有公共 `war3_native_renderer.h` 对 CModelData 的历史大布局不作为本桥授权来源。本轮不扩散修改
那些可能被其他路径依赖的结构；为当前候选建立独立、明确版本边界的读取器。

## 2. 实例灯所有权四链闭合

1. `0x6F12A5C0`：输入模板在 EDI，新分配对象在 ESI，构造为
   `TAllocatedHandleObjectLeaf<CModelComplex,128>` 后，ECX=新对象、stack arg=模板，调用
   `0x6F130D90`。说明返回的 runtime 和模板不同；不能把反编译不完整的函数参数显示当证明。
2. `0x6F130D90 → 0x6F1322B0`：ECX=目标 runtime、stack arg=模板；目标 `+0xB4` 分配数组，
   每一槽 `0CC450` 新建 CGxuLight，复制模板对应灯的 +0..+0x20 字段。目标 +0xBC 数组不是共享表。
3. `0x6F12F0A0 → 0x6F12E900`：ECX=runtime，EDX=runtime+0x9C backing。
   builder 在 `0x6F12E9CB` 将 **&runtime[0xB4]** 放入输出 bundle+0x14；
   `0x6F77F2D0` 通过该 bundle 的 array+8 与 node+0xAC 输出槽写到实例灯。
4. `0x6F1307B0`：按 runtime+0xB8 次数遍历 runtime+0xBC 指针数组，调用 `0CC530` 从全局
   灯表移除/释放各灯，最后释放数组。与 clone/build 的相同 owner 一致。

`runtime+0x9C` 的来源：`127610` 创建 HMODELDATA，`121B00` 构造共享 backing，只显示
+0..+0x78 的初始化；`130CD0` 通过 `04F200` 引用计数保留它，`04F1E0` 为恒等返回，不存在
“自动解引用到另一个灯光对象”的隐藏变换。

## 3. 灯光数据与索引

- `831740` 读每个 MDX Light：type 写解析记录+0xA4；attenuationStart/End 写 +0xC0/+0xDC；
  颜色、强度、ambient 及 KLA* 轨道均存在。不要把 binary RGB 与 packed byte 顺序直接等同。
- `8319F0` 追加 0x178-byte Light 记录，`81D900/81DD60` 建立并修正总对象表。
  文件 ObjectID、Light ordinal、总对象表索引、graph sourceRecordIndex、outputSlot 仍需分别记录。
  不能用 WhiteList ObjectID 直接索引实例灯数组。
- `13D9C0` 将解析 Light 转为一个模板 CGxuLight：type=0 设 positional=1，填 packed colors
  与直接/环境强度，并初始化 enabled=0。该函数没有将两个 attenuation 字段写进 CGxuLight。
- `77F2D0` 在实例求值中更新 enabled、调用 `77EFE0`/`77F250` 处理动画颜色/强度，再处理位置。
  可见性条件含缓存/依赖分支；未进入当前函数不代表灯应永久保留。
- `77DB20 case 1` 先对 scratch 做 pivot 补偿，再调用 `77F2D0`；因此不能简单把原始 pivot
  加一次实例平移作为最终灯位置。已求值结果是可用采样对象，但投影/世界坐标等价仍需运行核验。

## 4. 原生最终照明路径与性能解释

`0CC650` 对位置分桶、按变化代更新候选，将
`directIntensity / (1 + a1*d + a2*d*d) + ambientIntensity` 写到 CGxuLight+0x28 后，
由 `0CC0E0` 排序，选择有限灯槽，交给 `0E3410 → CGxDevice vslot+0x84 → 0E5690`。
这证明 +0x28 是 **selectionScore**，构造 +INF 是初值，不是无限照明半径。

D3D9 backend 的实际路径是 `0x6F0EFB60 → 0x6F0EF2D0`：

- 填 0x68-byte D3DLIGHT9，positional 对应 POINT=1，非 positional 对应 DIRECTIONAL=3。
- Range 写常量 `0x461C4000`，即 **10000.0f**；Attenuation0=1，Attenuation1/2 从设备
  +0x36C/+0x370 读取。它们是统一设备参数，不是从每盏 CGxuLight 读两段 MDX 衰减。
- 0EF2D0 的 device COM vtable+0xCC 是 SetLight，+0xD4 是 LightEnable；与本地
  `include/native/directx/d3d9.h` 槽位定义及复制结构大小交叉一致。
- `0x6F0EAD80` 是 OpenGL glLight* 对应实现，虽然早期取证文件误命名为 `d3d_light_commit.json`，
  **不得把该文件当 D3D9 证据**。保留原始收据不重命名，D3D9 以 `d3d9_light_flush.json` 为准。

因此原生具有选灯、状态更新和驱动提交成本，但此处没有测量毫秒/FPS。把 WarVK 点光再叠上去
会存在重复贡献风险，不能为替换而不加区别地关闭全局 SetLight（会影响太阳/环境和其他灯）。

## 5. 真实加载字节与模型路径：已定位，但未接入

- `129750` 将请求路径复制到 260-byte buffer，删除最后四字符的扩展部分，再 Storm_590 hash。
  这是缓存 base key，**不是实际打开的完整路径，更不是 SHA**。
- `128270` 查 compiled model cache；miss 又查 parsed/preload cache；仍 miss 时走
  `81A5F0 → 81AB30`。成功后 cache node+0x14 存 base path，+0x18 存模板 HMODEL。
- `81AB30` 可能先将 `.mdl` 换 `.mdx`，再退请求路径或候选路径；文件读取经 `048C10`，既能命中
  FileCache，也能调用 Storm ordinal 279。成功拿到实际 bytes/size 后构造 MsgBuffer，调用
  `819A80(MsgBuffer*, size, parsedDocument*, diagnosticSink*)`。
- 在 `81AC31..81AC5E`，MsgBuffer+0x10 为这次实际 bytes，EDX 为 size，stack arg0 为同一个
  parsed document。该边界适合未来提取/核 SHA；须尊重 buffer 在 `049800` 释放之前的生命周期。
- `12A370 → 1261D0 → 1260C0` 从同一 parsed document 构造模板；随后 `12A5C0/130D90`
  生成 runtime。冷热 cache、预加载和嵌套子模型都会影响次序，禁止 last-path TLS 手递手猜匹配。

后续方案：加载 bytes receipt 按 parsed-document 精确关联，记录实际成功候选路径和 SHA；
资源装配时转为模板代际证据，克隆时按 exact template pointer+generation 关联 runtime。
未捕获到旧 preload/cache identity 就拒绝，而不是重新加载同名磁盘文件补造证明。

## 6. 另一个默认关闭的既有风险

主树 `kResolveRuntimeModelFromHandleRva=0x12A3C0` 对应函数由 ECX/EDX/一个栈参数传入
请求路径、选项、诊断 sink，ret 4；它不是 `__stdcall(void* handlePtr)`。
现有同名 typedef/Hook 与这个 ABI 不符。`kShadowRuntimeModelBootstrapResolveHookEnabled=false`
当前编译期关闭，所以没有据此声称当前游戏正在因它崩溃；**后续不能直接启用它来补路径**。
本轮不修改该历史 Hook，不扩展这次采样修正范围。另行修订需要准确签名、调用约定验证与
返回对象语义的测试，不能只改函数名或把诊断 sink 当 creator handle。

## 7. 本轮源码修正与验证

新增独立 `war3_native_model_light_layout.h`，只接收 uint32 地址与 fault-safe reader：
精确 CModelComplex vptr 两种、flags.bit4、backing identity、实例数组 capacity/count/data、
16 灯上限、整数边界。所有 raw reads 属于已观察 runtime，不追共享 backing 的灯字段。
候选字段改名 nativeSelectionScore；照明和阴影授权继续恒 false。

新 C++ 测试故意令共享 backing 内有一个“合法但不同”的假表，要求真正读取实例表；覆盖
plain/unknown vptr、flags 不符、backing 替换、count/capacity 不符、零灯、坏地址/溢出/失败读取。

- Win32 runnable **260 条断言通过**（242 原断言 + 18 个布局反例）。
- 相关 Python **21/21**（原模型 13 + ingress static 8）。
- 两个实际修改 TU 宏0/1语法与预处理共4/4，通过；独立 bridge 头语法通过。
- `git diff --check` / Python 编译检查通过。没有 Ninja、产品 DLL 链接、部署或游戏运行。
- 所有新 query 原始 JSON、语法及 runnable 收据位于 ignored
  `AutoTest/artifacts/native_model_light_ida_20260913/`；旧 MPQ/model/白名单证据未重写。

此修正只能称为 **原生采样地址纠错**。它没有完成模型身份捕获、无重复增强照明、点阴影
白名单消费、游戏运行完整性或视觉性能验收；不宣称已经开启任何原生点光增强效果。
