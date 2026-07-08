# WarVK JASS Bridge

This directory contains the map-side WarVK JASS package for loading and calling
WarVK after the map has entered the game.

## What this package does

- It does not declare custom natives.
- It does not expose MemHack/JapiFunc API to map code.
- It does not use MemHack carrier names or the MemHack pre-DLL loader path.
- The AI loader uses the classic returnbug/native-call carrier technique
  internally (`MergeUnits`/`IgnoredUnits`), but the payload names, exports, and
  JASS command bridge are WarVK-owned.
- By default, it starts `warvk.ai` through `StartCampaignAI`. The current AI
  backend resolves Win32 loader APIs, exports the map-imported `warvk.blp`
  DLL carrier to the temp directory as `WARVKLDR<pid>.dll`, calls
  `LoadLibraryExA` on that temp DLL, then calls `WarVK_Initialize`.
- After loading, it probes and calls the WarVK command bridge through existing
  Blizzard natives:
  - `GetLocalizedHotkey("warvk:ping")`
  - `GetLocalizedString("warvk:version")`
  - `Preloader("warvk:cmd:...")`

## Loading boundary

The default map-side path is:

```jass
call StartCampaignAI(Player(PLAYER_NEUTRAL_AGGRESSIVE), "warvk.ai")
```

The current `warvk.ai` is WarVK-owned. It exports the root map import
`warvk.blp` carrier to `%TEMP%\WARVKLDR<pid>.dll`, loads that file, and calls
the exported initializer. Because `warvk.blp` is the actual WarVK runtime DLL,
the loader deliberately keeps the carrier module loaded after initialization;
do not restore the MemHack-style `FreeLibrary(tempLoader)` step unless
`warvk.blp` becomes a disposable loader DLL instead of the runtime DLL.

The prepared DLL exports both:

- `Initialize`
- `WarVK_Initialize`

The AI loader calls `WarVK_Initialize` after loading the exported
`WARVKLDR<pid>.dll` carrier. `Initialize` remains as a compatibility export for
other loaders.
`DllMain` auto-bootstrap is intentionally off by default and only enabled when
`DXVK_WARVK_BOOTSTRAP_ON_DLLMAIN=1`, because proxy `d3d9.dll` startup and
mid-game injection have different safety windows.

In mid-game direct-load mode WarVK cannot reuse the Vulkan/DXVK proxy device.
The runtime resolves War3's native `CGxDeviceD3d` singleton
(`Game.dll + 0xBC5420`) and binds its `IDirect3DDevice9*` field (`+0x584`) for
native D3D9 consumers such as WarVK Lightning. If this resolver misses, the
bridge can still answer JASS commands, but native D3D9 drawing will be skipped
until the device is found.

`package_warvk.ps1` writes `WarVK.dll`, `warvk.ai`, `warvk_loader.lua`, and
`warvk.blp`. The `.blp` file is only a byte-for-byte DLL carrier for the
map-contained loader path, not a real BLP image and not the lightning texture
source.

## Files to include

Include `jass/warvk_init.j` from the map library. It includes the bridge and
render API wrappers.

Map import paths for the current AI bridge:

- `warvk.ai`
- `warvk.blp`

The JASS bridge may also try the default imported AI path:

- `war3mapImported\warvk.ai`

The current MH-style AI backend expects the DLL carrier at the root import path
`warvk.blp`.

`warvk_loader.lua` is still packaged as a compatibility fallback for Lua-based
experiments, but it is not required by the current AI map-contained loader.

Smoke-test functions:

- `WarVK_Init()` starts bridge readiness polling.
- `WarVK_IsLoaded()` returns `true` once the DLL bridge responds.
- `WarVK_ShowLoaderStatus()` prints loader mode, route, and local error text.
- `WarVK_GetLocalLoadErrorString()` reports JASS-side loader failures before
  the DLL bridge is ready.
- `DXVK_GetPluginVersionString()` returns `WarVK JASS bridge v1` after ready.
- `TestIntReturn()` returns `1` after ready.
- `WarVK_Log("hello")` writes to the WarVK debug log after ready.

Minimal map-side test:

```jass
function WarVK_Test takes nothing returns nothing
    if (WarVK_IsLoaded()) then
        call DisplayTimedTextToPlayer(GetLocalPlayer(), 0.0, 0.0, 8.0, DXVK_GetPluginVersionString())
        call WarVK_Log("map-smoketest")
        call DXVK_SetLightingEnabled(true)
        call DXVK_SetSunColor(0.45, 0.58, 1.00)
        call DXVK_SetSunIntensity(1.35)
    else
        call DisplayTimedTextToPlayer(GetLocalPlayer(), 0.0, 0.0, 8.0, "[WarVK] bridge not ready")
    endif
endfunction
```

Common render API wrappers such as `DXVK_SetSunColor`,
`DXVK_SetShadowEnabled`, `AddPointLight`, `DXVK_OutlineAddHandle`, and
`DXVK_SetBloomEnabled` are routed through `warvk:cmd:` and call the existing
`war3shader` C++ API once the bridge is ready.

WarVK Lightning wrappers live in `jass/API/warvk_lightning.j`. They currently
support create/move/destroy, color gradient, width, curve/noise/branches,
lifetime/fade, pulse, active-count, and stats queries while coexisting with
Warcraft III's original lightning system.
