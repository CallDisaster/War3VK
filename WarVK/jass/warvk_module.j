// =============================================================================
// WarVK 内置模块 · 地图侧 API
//
// 依赖：WarVK 1.22.02+ 且已加载 warvk:v1 桥接（WarVKIsBridgeAvailable()）。
// 依赖：宿主已按 war3_module_loader.cpp 编译，并已接入 Dispatch 分流。
//
// 设计要点：
//   - 不注册任何自定义 native，不写游戏内存；全部走 warvk:v1 字符串载体。
//   - 命令名格式固定为 "<模块名>.<函数名>"，模块名与 DLL 同名。
//   - 参数通过 WarVKModuleArg* 拼成 "…;i:1;r:2.5;s:text" 形式追加在命令后。
//   - 单条消息上限 512 字节（含前缀），文本参数内不允许出现 ';'。
//
// 用法：
//   call WarVKModuleLoad("wvk_sample")
//   if WarVKModuleCount() > 0 then
//       set v = WarVKModuleCallInt("wvk_sample.add", WarVKModuleArgInt(2) + WarVKModuleArgInt(3))
//   endif
// =============================================================================

// ---- 参数拼装 --------------------------------------------------------------

function WarVKModuleBoolToken takes boolean value returns string
    if value then
        return "1"
    endif
    return "0"
endfunction

function WarVKModuleArgInt takes integer value returns string
    return ";i:" + I2S(value)
endfunction

function WarVKModuleArgReal takes real value returns string
    return ";r:" + R2S(value)
endfunction

function WarVKModuleArgBool takes boolean value returns string
    return ";b:" + WarVKModuleBoolToken(value)
endfunction

// 句柄：单位/物品/特效等的 GetHandleId
function WarVKModuleArgHandle takes integer handleId returns string
    return ";d:" + I2S(handleId)
endfunction

function WarVKModuleArgText takes string value returns string
    return ";s:" + value
endfunction

// ---- 模块生命周期 ----------------------------------------------------------

// 返回 1 表示已加载（含重复加载的幂等情形），0 表示失败。
// 失败原因可用 WarVKModuleLastError() 查看。
function WarVKModuleLoad takes string name returns integer
    return GetLocalizedHotkey("warvk:v1;module.load;s:" + name)
endfunction

function WarVKModuleUnload takes string name returns integer
    return GetLocalizedHotkey("warvk:v1;module.unload;s:" + name)
endfunction

function WarVKModuleCount takes nothing returns integer
    return GetLocalizedHotkey("warvk:v1;module.count")
endfunction

// "<模块名>|<版本>|<命令数>"
function WarVKModuleInfo takes integer index returns string
    return GetLocalizedString("warvk:v1;module.info;i:" + I2S(index))
endfunction

// 逗号分隔的命令名列表
function WarVKModuleCommands takes integer index returns string
    return GetLocalizedString("warvk:v1;module.commands;i:" + I2S(index))
endfunction

function WarVKModuleLastError takes nothing returns string
    return GetLocalizedString("warvk:v1;module.lastError")
endfunction

// 卸载全部模块（换图会自动执行，一般不需要手动调用）。
function WarVKModuleReset takes nothing returns nothing
    call Preloader("warvk:v1;module.reset")
endfunction

// ---- 通用调用 --------------------------------------------------------------

// 无返回值命令
function WarVKModuleCallVoid takes string command, string args returns nothing
    call Preloader("warvk:v1;" + command + args)
endfunction

// 返回整数命令
function WarVKModuleCallInt takes string command, string args returns integer
    return GetLocalizedHotkey("warvk:v1;" + command + args)
endfunction

// 返回实数命令（宿主把实数格式化为文本回传，这里解析回 real）
function WarVKModuleCallReal takes string command, string args returns real
    return S2R(GetLocalizedString("warvk:v1;" + command + args))
endfunction

// 返回文本命令
function WarVKModuleCallText takes string command, string args returns string
    return GetLocalizedString("warvk:v1;" + command + args)
endfunction
