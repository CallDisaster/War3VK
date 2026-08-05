local ffi = require "ffi"

ffi.cdef[[
typedef void* HMODULE;
typedef int (__stdcall *WarVKInitializeFn)(void);

HMODULE __stdcall GetModuleHandleA(const char* lpModuleName);
HMODULE __stdcall LoadLibraryA(const char* lpLibFileName);
void*   __stdcall GetProcAddress(HMODULE hModule, const char* lpProcName);
unsigned long __stdcall GetEnvironmentVariableA(const char* lpName, char* lpBuffer, unsigned long nSize);
unsigned long __stdcall GetLastError(void);
void __stdcall OutputDebugStringA(const char* lpOutputString);
]]

local M = {}

local function cstr(s)
  return tostring(s or "")
end

local function debug_log(message)
  ffi.C.OutputDebugStringA("[WarVK Loader] " .. cstr(message) .. "\n")
end

local function is_null(p)
  return p == nil or p == ffi.NULL
end

local function try_load(path)
  local existing = ffi.C.GetModuleHandleA(path)
  if not is_null(existing) then
    debug_log("module already loaded: " .. path)
    return existing, path
  end

  local h = ffi.C.LoadLibraryA(path)
  if not is_null(h) then
    debug_log("LoadLibraryA succeeded: " .. path)
    return h, path
  end

  debug_log("LoadLibraryA failed: " .. path .. " error=" .. tonumber(ffi.C.GetLastError()))
  return nil, nil
end

local function call_initialize(module)
  local proc = ffi.C.GetProcAddress(module, "WarVK_Initialize")
  if is_null(proc) then
    proc = ffi.C.GetProcAddress(module, "Initialize")
  end

  if is_null(proc) then
    debug_log("missing WarVK_Initialize/Initialize export")
    return false
  end

  local init = ffi.cast("WarVKInitializeFn", proc)
  local ok, result = pcall(init)

  if not ok then
    debug_log("initialize export raised: " .. cstr(result))
    return false
  end

  debug_log("initialize export returned: " .. cstr(result))
  return true
end

function M.load()
  local env_path = ffi.new("char[1024]")
  local env_len = ffi.C.GetEnvironmentVariableA("WARVK_DLL_PATH", env_path, 1024)

  local candidates = {}
  if env_len > 0 then
    candidates[#candidates + 1] = ffi.string(env_path)
  end

  local defaults = {
    "WarVK.dll",
    ".\\WarVK.dll",
    ".\\WarVK\\WarVK.dll",
    ".\\WarVK\\bin\\WarVK.dll",
    "WarVK\\WarVK.dll",
    "WarVK\\bin\\WarVK.dll",
  }
  for _, path in ipairs(defaults) do
    candidates[#candidates + 1] = path
  end

  for _, path in ipairs(candidates) do
    local module = try_load(path)
    if module then
      return call_initialize(module)
    end
  end

  debug_log("all candidates failed; put WarVK.dll next to War3.exe or in .\\WarVK\\bin")
  return false
end

return M.load()
