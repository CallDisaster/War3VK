# WarVK v1.22 双模型点光候选 — 74CC / 2026-09-14

这是供编辑器和玩家验证的候选，不是正式稳定版。DLL 为 PE32/i386，适用当前已核验的
Warcraft III 1.27a。未合入用户明确排除的未完成 Water 分支。

## 使用

1. 完全退出游戏；备份实际运行游戏目录里的原 `d3d9.dll`，再放入本包 DLL。
   本次交付没有自动替换玩家现场，也没有修改任何 MPQ。
2. 将本包 `WarVK` 作为更新后的 `ui/WarVK` 编辑器层使用，保留 action/call/define 和 jass
   目录。地图侧引入 `jass/warvk_init.j`。已有工程更新对应文件，不重复叠加同名函数。
3. 在编辑器导入管理器中导入自己修改的模型，并将场景对象的模型设为该资源。
   例如导入路径是 `war3mapImported\TorchHuman.mdx`，则使用下列 JASS。

```jass
// 在地图初始化时注册；最后一个参数控制点阴影。
call WarVKSetModelPointLightsEnabled("war3mapImported\\TorchHuman.mdx", true, true)
// 同一个模型保留光照、关闭自动点阴影，可用于对照：
call WarVKSetModelPointLightsEnabled("war3mapImported\\TorchHuman.mdx", true, false)
// 取消本规则的接管；游戏原生照明仍保留：
call WarVKSetModelPointLightsEnabled("war3mapImported\\TorchHuman.mdx", false, false)
```

以上三行是不同操作示例，**不要初始化时连续执行三行**。通常只调用第一行。
图形界面动作是“注册模型自动点光与点阴影”，三个参数分别为资源路径、启用、点阴影。
JASS 字符串中的 `\\` 表示一个反斜杠；编辑器资源路径和 GUI 输入使用实际单反斜杠。
不要填写 `C:\Users\...` 这类磁盘路径。

查询：`WarVKIsModelPointLightRegistered(path)` 表示规则已启用；
`WarVKGetModelPointLightCount(path)` 表示当前已加载且绑定的原生灯数量，不是可见灯数或
实际投影灯数。注册本身不会加载模型，模型没有受支持的 Light 节点时不会凭空创建灯。
变更规则后可立即检查 `WarVKGetLastErrorCode()`；规则在地图退出时清理。

## 默认开启与保留内容

- 原生普通帧无消费者同步读回优化、异步截图和录制独立开关。
- 原版模型灯 producer/consumer 及本次自定义模型注册接口，无需额外诊断环境开关。
- 已有退出修正、太阳 API 修正、Froxel High 与局部体积雾候选；效果仍服从地图作者设置，
  不会强制把所有雾和后处理开关全部打开。

如果旧启动器显式设置了 `DXVK_WAR3_NATIVE_MODEL_LIGHTS=0` 或
`DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER=0`，会覆盖本候选的默认开启。

## 必须了解的边界

- 自动原生灯接管/点阴影测试上限由1提高到2盏，显式注册路径优先于内置规则，
  同级按投影中心距离选择。不是所有火把同时投影，更多实例仍需竞争这2个槽。
  作者显式创建的点光优先使用总共4张cube/16灯预算；该总预算本轮不变。
  其它原生灯继续原生照明。关闭某模型点阴影可能让其它内置灯获得空出的槽。
- 修复world渲染结束后合规draw使整帧自动阴影撤回的问题；尾段保留各灯独立颜色，
  不放宽未知shader/资源/身份安全门。新增3个私有颜色附件有带宽成本，未宣称性能提升。
- 支持 MDX800 中最多16个受支持的 Omni 灯，使用游戏实际求值位置、颜色、强度和可见性。
  静态衰减结束距离必须有效且不超过10000；动画衰减/方向灯等不支持内容保持原生路径。
- 点光源如果被模型遮住，开启点阴影可能将原有光照遮暗。不要用提高强度替代检查发光位置。
  本次ED122版导入模型已实际选中，world光点Z约150.94，截图中可见持续投射轮廓。
  同场原版火把仍有强烈角状遮暗，具体遮挡几何与全阴影视觉尚未闭合；不把旧模型结论套用。
- 本包不附带、替换或覆盖用户模型。请使用你在编辑器里选择的版本。
- 未承诺全模型视觉、长期游玩或同进程换图通过；随机单帧裂缝问题也未宣称解决。

## 验证与身份

148项定向Python/static及新Win32 synthetic解析/路径测试通过；58-edge DLL构建与no-work、
receiver和10个实际dump SPIR-V离线验证通过。最终DLL在隔离2560×1440完成真实JASS
四阶段双灯开关/单位选择测试，最终截图后仍有双灯/12面提交，零颜色及receiver回退记录。
零全局输入、正常exit0、当前测试基线恢复、零新GPU事件/dump。不是全量功能/驱动验证。
这些是限定功能证据，不等于玩家视觉完成或稳定版接受。

DLL：35,486,415 bytes，SHA-256：
`74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73`。
产品版本资源沿用集成基线，不以版本号作为候选身份。完整文件身份见 `manifest.json`。
开发记录与待发布日志位于包内 `docs/`，关键日志和阴影开/关截图位于`evidence/`；
根稳定CHANGELOG未改。交付没有自动替换`E:/Work/Warcraft III/d3d9.dll`。
