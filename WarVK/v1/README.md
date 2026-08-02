# WarVK JAPI v1（DXVK 内置版）

这套 JAPI 的唯一运行时是本目录所属的 d3d9.dll。地图不需要、也不应再编译或加载
war3map.dll 才能使用公开接口。代理 DLL 启动后，DXVK 在游戏建立 JASS Native 表时
接管三个低频 stock carrier：

- Preloader(string)：无返回值的 setter/command；
- GetLocalizedHotkey(string)：boolean、integer 和 WarVK managed id；
- GetLocalizedString(string)：string 与 real 的文本形式。

非 warvk: 调用始终转发给原 Native。正式公共请求必须使用
warvk:v1;<command>[;<typed-arg>...]；错误的版本、carrier、类型、参数数量、非有限
浮点或超限输入都会在 DXVK 内 fail-closed，不会落回游戏 Native。旧的无版本
warvk:ping / warvk:cmd: 仅保留给项目 AutoTest，不属于公共 JAPI。

## 地图侧文件

最简使用直接导入 jass/warvk_v1_complete.j（公共常量 + 55 个 wrapper）。
如果编辑器采用模块化拼接，则按顺序导入：

1. jass/warvk_v1_constants.j（可选，但推荐）；
2. jass/warvk_v1.j（55 个正式 WarVK... wrapper）；
3. jass/warvk_v1_smoke_test.j（只在验收候选地图中导入，发布地图可删除）。

GUI 编辑器资源位于 editor/：TriggerData.txt、TriggerStrings.txt 与
editor/mapui/{action,call,define}.txt。manifest/warvk_v1.json 是协议和 wrapper
的单一清单来源；tests/protocol_vectors.json 用于实现一致性测试。

## 当前真实能力

WarVKGetFeatureFlags() 在渲染管线就绪后返回十进制 7687（0x1E07）：

- 太阳、CSM；
- 点光与点阴影；
- 闪电；
- WarVK managed object 查询；
- 本地视觉时间与帧/绘制统计。

完整清单中的体积光、outline、bloom、postfx、AA 与 day/night wrapper 也已交付，
但当前 DXVK 后端没有完整对应字段，因此这些能力位保持关闭，调用返回
WARVK_ERROR_UNSUPPORTED_FEATURE。这不是静默 no-op。

点阴影当前仍采用渲染器全局 cube-map 分辨率和 bias；所以
WarVKSetPointLightShadowConfig(lightId, resolution, bias) 会先验证 lightId，
再更新本局共享的点阴影配置。公开签名保留 light id，以便以后增加逐灯配置时不破坏
协议。

## 生命周期与 ID

点光和闪电使用同一个正数 WarVK ID 空间，绝不暴露游戏 handle、内存地址或 Vulkan
句柄。WarVKGetManagedObjectType 返回 WARVK_OBJECT_POINT_LIGHT 或
WARVK_OBJECT_LIGHTNING。地图/JASS VM 重建时，DXVK 只销毁该 JAPI 创建的对象，
不会清空渲染器或其他调试系统创建的资源。

## 验收建议

先在旁路候选地图调用 WarVKBeginPointLightSmokeTest(x, y, z) 并保存其返回 id。至少
跨过一个渲染帧并截图后，再调用 WarVKFinishPointLightSmokeTest(id)，确认：

- version/protocol/feature flags 有返回；
- create 返回正数 id；
- managed count 增加；
- 点光和点阴影可见；
- destroy 后 count 恢复；
- WarVKGetLastErrorCode() 为 0。

所有查询都只有本机视觉语义。严禁把其返回值用于多人同步玩法分支、单位命令、
随机数种子或任何同步状态判断。
