# WarVK 通用内置 DLL 加载器（warvk:v1 模块 ABI）

日期：2026-10-23
状态：宿主侧与协议层已实现并通过单测；模块二进制尚未做端到端进图验证。

---

## 1. 目标

让编辑器/地图作者可以：

1. 写一个 32 位 DLL，导出 `WarVKModuleInit`；
2. 把这个 DLL 作为**地图导入文件**随图分发（`warvk_modules\<name>.dll`）；
3. 在地图 JASS 里 `WarVKModuleLoad("name")` 之后，直接用 `warvk:v1` 协议调用 DLL 里的 C++ 函数。

**与 MemHack 的关系**：只借鉴"随图分发、运行时加载"这一点。MemHack 的做法是
AI 脚本 → 任意内存写 → 拼 x86 thunk → LoadLibrary → **打补丁替换 JASS 原生表**。
本方案不写游戏内存、不改原生表、不需要 AI 脚本、不需要外部注入器——
因为 WarVK 本身就是进程内的 d3d9 代理，加载 DLL 是它分内的事。

| 维度 | MemHack | 本方案 |
|---|---|---|
| 载体 | `tiara.ai`（AI 脚本）+ `tiara.blp`（改名 DLL） | 地图导入 `warvk_modules\<name>.dll` |
| 触发 | `StartCampaignAI` + `I2R` 魔数 | `Preloader("warvk:v1;module.load;s:name")` |
| 能力来源 | AI VM 数组越界 → 任意内存读写 | 宿主直接 `LoadLibrary` |
| 函数暴露 | 替换 JASS native（`EXSetEffectXY` 等） | 命令表登记，走 `warvk:v1` |
| JASS 侧风险 | 依赖 AI VM 行为，版本敏感 | 只依赖公有字符串协议 |
| 崩溃影响 | 野生代码段，难以隔离 | 标准 DLL，可 FreeLibrary |

---

## 2. 数据流

```
地图 JASS
  WarVKModuleLoad("wvk_sample")
        │  拼接 "warvk:v1;module.load;s:wvk_sample"
        ▼
  Preloader(...) / GetLocalizedString(...)      ← 公开原生，不新增 native
        ▼
WarVK d3d9.dll  →  japi::Dispatch(carrier, payload)
        │  前缀检查通过后
        ▼
  modules::IsModulePayload()  ── false ──► 原有 122 条内置命令
        │ true
        ▼
  modules::Dispatch()
        ├─ module.* 控制命令（load/unload/count/info/commands/lastError/reset）
        └─ <模块名>.<函数名> → 查已加载模块命令表 → 类型检查 → 调用 DLL 函数
        ▼
  Reply（Void/Int/Real/Text）→ 按载体回传 JASS
```

---

## 3. 线上格式

```
warvk:v1;module.load;s:wvk_sample
warvk:v1;module.count
warvk:v1;module.info;i:0
warvk:v1;wvk_sample.add;i:2;i:3
warvk:v1;wvk_sample.scale;i:3;r:1.5
```

约束（沿用既有协议）：

- 单条 ≤ 512 字节；
- ≤ 16 个参数；
- 类型标记 `i` 整数 / `r` 实数 / `d` 句柄 / `b` 布尔 / `s` 文本；
- 文本参数内不允许 `;`（否则无法无歧义分帧）。

返回按载体决定：

| 结果类型 | Hotkey 载体 | LocalizedString 载体 | Preloader 载体 |
|---|---|---|---|
| 整数 | 原生整数返回 | — | 丢弃 |
| 实数 | — | 格式化为文本（`%.6g`），JASS 侧 `S2R` | 丢弃 |
| 文本 | — | 原生字符串返回 | 丢弃 |
| 无 | — | — | 丢弃 |

---

## 4. 模块 ABI

头文件：`src/d3d9/war3/japi/warvk_module_abi.h`（纯 C，无 C++ 依赖）。

模块只需要导出：

```cpp
extern "C" __declspec(dllexport)
int32_t __cdecl WarVKModuleInit(const WarVKModuleHostV1* host,
                                const WarVKModuleDescV1** outDesc);
```

`outDesc` 指向模块内静态描述：

```cpp
static const WarVKModuleCommandV1 cmds[] = {
  {"wvk_sample.add", "ii", WARVK_RESULT_INT, &InvokeAdd, nullptr},
};
static const WarVKModuleDescV1 desc = {
  WARVK_MODULE_ABI_VERSION, "wvk_sample", "1.0.0",
  cmds, 1, &Shutdown, nullptr,
};
```

宿主服务（模块可长期持有，进程级单例）：

| 成员 | 用途 |
|---|---|
| `log` | 分级日志，默认进 OutputDebugString |
| `readFile` / `freeFile` | 读文件（先 MPQ 后本地），用于模块自带资源 |
| `getGameInfo` | 游戏版本、是否在游戏内、`mapEpoch` |
| `getHostFeatureFlags` | 宿主能力位，与 `kFeature*` 一致 |

**约定**：`abiVersion` 不符必须拒绝，不做"尽量兼容"；`desc->name` 必须等于请求的模块名；
命令名必须是 `<模块名>.<函数名>`，全进程唯一。

---

## 5. 加载流程与安全策略

```
module.load
  → 名字校验 [a-z0-9_-]{1,32}
  → 解析字节：warvk_modules\<name>.dll（MPQ，地图导入）→ WarVK\modules\<name>.dll → <name>.dll
  → PE 校验：MZ / PE\0\0 / Machine=0x14C / IMAGE_FILE_DLL / ≤ 64 MiB
  → 策略：WarVK\modules\allowed.txt 逐行 "<name> <sha256>"
          文件不存在 = 开发模式（放行 + 告警）；存在则必须命中
  → 写 %TEMP%\wvkmod_<name>_<pid>_<tick>.dll
  → LoadLibraryExW(绝对路径, LOAD_WITH_ALTERED_SEARCH_PATH)
  → GetProcAddress("WarVKModuleInit") → 调用 → 校验描述
  → 登记命令（与其它模块交叉查重）
  → 失败则 FreeLibrary + 删除临时文件
```

设计取舍：

- **为什么要落临时文件**：`LoadLibrary` 只认路径。手工映射（manual map）能免落盘，但要自己实现重定位、导入、TLS、异常表，风险远大于收益。落盘后用 `FreeLibrary` + `DeleteFile` 清理，已在 `module.unload` / 换图 / 进程退出时覆盖。
- **为什么默认只允许 32 位**：war3 是 32 位进程，64 位模块在 PE 校验阶段就拒绝，避免 `LoadLibrary` 报难懂的错。
- **为什么有 allowed.txt**：地图是可以从任何地方下载的。没有允许清单时，任何地图都能让 WarVK 加载任意 DLL = 一键远程代码执行。允许清单是唯一把"本地开发"和"玩家运行下载图"区分开的开关。

---

## 6. 生命周期

| 事件 | 行为 |
|---|---|
| `module.load` | 加载并登记 |
| `module.unload` | 调 `desc->shutdown` → `FreeLibrary` → 删临时文件 |
| 换图 / JASS VM 重建 | `war3_hook_jass.cpp` 的 `Hook_InitJassNatives` 里调 `modules::ResetAuthorState()`，全部卸载，`mapEpoch++` |
| 进程退出 | 由 OS 回收（临时文件可能残留，命名含 pid，可安全清理） |

---

## 7. 代码清单

| 文件 | 作用 |
|---|---|
| `src/d3d9/war3/japi/warvk_module_abi.h` | 模块 ABI（给 DLL 作者） |
| `src/d3d9/war3/japi/war3_module_loader.h/.cpp` | 加载器 + 命令路由 + 策略 |
| `WarVK/jass/warvk_module.j` | 地图侧 JASS 封装 |
| `WarVK/modules/wvk_sample/wvk_sample.cpp` | 示例模块 |
| `AutoTest/test_war3_module_protocol.cpp` | 协议层单测 |
| `src/d3d9/meson.build` | 新增 TU 与测试目标 |
| `src/d3d9/war3/japi/war3_japi_v1.cpp` | Dispatch 内 6 行分流 |
| `src/d3d9/war3/hooks/war3_hook_jass.cpp` | 换图时 reset |

### 集成补丁（共三处，已落地）

1. `war3_japi_v1.cpp::Dispatch`，前缀检查之后：

```cpp
if (modules::IsModulePayload(payload)) {
  Reply moduleReply = modules::Dispatch(carrier, payload);
  if (!moduleReply.ok() && moduleReply.error != ErrorCode::None)
    static_cast<void>(Failure(moduleReply.error));
  return moduleReply;
}
```

2. `war3_hook_jass.cpp::Hook_InitJassNatives`：`japi::modules::ResetAuthorState();`

3. `meson.build`：主目标 + 3 个 JAPI 测试目标加入 `war3/japi/war3_module_loader.cpp`，
   新增 `war3_module_protocol_test`。

---

## 8. 使用示例

**C++**（`WarVK/modules/wvk_sample/wvk_sample.cpp` 是完整可编译版本）：

```cpp
int32_t __cdecl InvokeAdd(void*, const WarVKModuleArgV1* a, int32_t n,
                          WarVKModuleValueV1* out) {
  if (n != 2 || a[0].type != WARVK_ARG_INT || a[1].type != WARVK_ARG_INT)
    return WARVK_MODULE_E_ARGUMENT;
  out->kind = WARVK_RESULT_INT;
  out->i = a[0].i + a[1].i;
  return WARVK_MODULE_OK;
}
```

**JASS**：

```jass
call WarVKModuleLoad("wvk_sample")
if WarVKModuleCount() > 0 then
    set n = WarVKModuleCallInt("wvk_sample.add", WarVKModuleArgInt(2) + WarVKModuleArgInt(3))
    set r = WarVKModuleCallReal("wvk_sample.scale", WarVKModuleArgInt(3) + WarVKModuleArgReal(1.5))
    call WarVKModuleCallVoid("wvk_sample.log", WarVKModuleArgText("hello"))
endif
```

**构建示例模块**（MinGW，与主工程同一工具链）：

```
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ \
  -o wvk_sample.dll wvk_sample.cpp
```

---

## 9. 验证状态

已验证：

- `d3d9.dll` 在 MinGW i686 / C++17 下增量构建并链接通过；
- `war3_module_protocol_test`：9 组用例全过（前缀识别、控制命令、缺参、越界、未知子命令、不消费非模块载荷、Reset 幂等）；
- 既有回归全过：`war3_japi_v1_protocol`、`warvk_weather_protocol`（211 项）、`war3_managed_object_reset`（26 项）。

未验证（需要真机进图）：

- 真实 32 位模块的 `LoadLibrary` + `WarVKModuleInit` 调用链；
- MPQ 内 `warvk_modules\*.dll` 的读取（依赖 `FileManager` 初始化时序）；
- 换图时卸载是否残留句柄/临时文件。

---

## 10. 后续路线

1. **端到端冒烟**：编译 `wvk_sample.dll`，塞进测试图，进图跑 `WarVKModuleLoad` + `add`，核对返回值。
2. **签名与信任**：把 `allowed.txt` 升级为"编辑器签名 + 公钥校验"，避免用户手抄 sha256。
3. **免落盘**：需要规避杀软/临时目录策略时，再上 manual map（按 import 表 + 重定位 + TLS 顺序实现，单独测试目标）。
4. **命令命名空间隔离**：按地图名+模块名双前缀，避免多张图共用模块名时语义漂移。
5. **JAPI 侧反向调用**：给模块一个 `warvk_call` 宿主服务，让 C++ 也能主动触发现有 122 条命令（目前只能 JASS → C++ 单向）。
