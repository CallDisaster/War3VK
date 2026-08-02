# WarVK JAPI

WarVK 的运行时内置于 DXVK `d3d9.dll`。地图通过 Warcraft III 自带的
`Preloader`、`GetLocalizedHotkey` 和 `GetLocalizedString` 调用
`warvk:v1` 协议，不声明额外 Native，也不需要 `war3map.dll` 提供这套接口。

## 目录

- `action.txt`、`call.txt`、`define.txt`：YDWE MapUI 定义。
- `jass/warvk_init.j`：地图侧入口，会引入桥、常量和全部公开 API。
- `jass/warvk_api.j`：55 个 `WarVK...` 函数。
- `jass/warvk_smoke_test.j`：点光与点阴影的两阶段验收函数。
- `loader/warvk.ai`：地图内 AI 加载路线。
- `loader/warvk_loader.lua`：Lua 加载兼容路线。
- `package_warvk.ps1`：生成 `WarVK.dll` 与地图载荷。

## YDWE 接入

将根目录的 `action.txt`、`call.txt`、`define.txt` 合并到目标
`ui/MapUI`，并让地图预处理入口包含 `jass/warvk_init.j`。如果使用地图内
加载路线，同时导入 `warvk.ai` 和由构建脚本生成的 `warvk.blp`。

DXVK 已作为代理 DLL 启动时，初始化函数会直接检测到 bridge；无需再次加载 DLL。

## 验收

测试触发器先调用：

```jass
local integer lightId = WarVKBeginPointLightSmokeTest(x, y, z)
```

保留该点光至少一个渲染帧并截图，然后调用：

```jass
call WarVKFinishPointLightSmokeTest(lightId)
```

验收信息包括 API 版本、协议版本、功能位、正数对象 ID、对象计数变化以及点阴影。
当前实现的功能位为 `0x1E07`：Sun、CSM、PointLight、Lightning、
ManagedObject、Time 和 Stats。未接通的功能会返回
`WARVK_ERROR_UNSUPPORTED_FEATURE`。
