#include "war3_game.h"
#include <windows.h>
#include <iostream>

// Globals
uint32_t            game_version = 0x27a; // Default to 1.27a
CGameWar3*          game_war3;
CGameState*         game_state;
CGxDevice*          gx_device;
GameTimer*          game_timer;
GameTable*          game_table;

HMODULE GameDLL;
HMODULE StormDLL;

uintptr_t pGameDLL;
uintptr_t pStorm;

uintptr_t constFloatStart;
uintptr_t constFloatEnd;

static uintptr_t g_getGameOptObj = 0;
static uintptr_t g_setGameOptValue = 0;
static uintptr_t g_getGameOptValue = 0;

void base_game_init() {
    uint32_t ptr;

    // game.dll
    GameDLL = GetModuleHandleA("game.dll");
    pGameDLL = (uintptr_t)GameDLL;

    // storm.dll
    StormDLL = GetModuleHandleA("storm.dll");
    pStorm = (uintptr_t)StormDLL;

    if (!pGameDLL) {
        // OutputDebugStringA("[Jass] game.dll not found!\n");
        return; 
    }

    switch (game_version)
    {
    case(0x24e):
        ptr         = *(uint32_t*)      (pGameDLL + 0xAF16A8);
        game_war3   = *(CGameWar3**)    (pGameDLL + 0xACD44C);
        game_timer  = *(GameTimer**)    (pGameDLL + 0xACE1C0);
        break;
    case(0x26a):
        ptr         = *(uint32_t*)  (pGameDLL + 0xADA848);
        game_war3   = *(CGameWar3**)(pGameDLL + 0xAB65F4);
        gx_device   = *(CGxDevice**)(pGameDLL + 0xACBD40);
        game_timer  = *(GameTimer**)(pGameDLL + 0xAB7368);
        break;
    case(0x27a):
        ptr             = *(uint32_t*)  (pGameDLL + 0xBE3740);
        game_war3       = *(CGameWar3**)(pGameDLL + 0xBE4238);
        gx_device       = *(CGxDevice**)(pGameDLL + 0xBC5420);
        game_timer      = *(GameTimer**)(pGameDLL + 0xBB82BC);
        constFloatStart = pGameDLL + 0xBB81C4;
        constFloatEnd   = pGameDLL + 0xBB82AC;
        g_getGameOptObj   = pGameDLL + 0x023E00;
        g_setGameOptValue = pGameDLL + 0x025A70;
        g_getGameOptValue = pGameDLL + 0x023ED0;
        break;
    default:
        break;
    }

    // CGameState
    if (game_war3)
        game_state = game_war3->game_state;
}

bool war3_get_game_opt_value(GAME_OPTION option, uint32_t* outValue) {
    if (!outValue || !pGameDLL || !g_getGameOptObj || !g_getGameOptValue)
        return false;

    void* optObj = call_std<void*>(g_getGameOptObj);
    if (!optObj)
        return false;

    uint32_t value = 0;
    call_this<void>(g_getGameOptValue, optObj, option, &value);
    *outValue = value;
    return true;
}

bool war3_set_game_opt_value(GAME_OPTION option, uint32_t value) {
    if (!pGameDLL || !g_getGameOptObj || !g_setGameOptValue)
        return false;

    void* optObj = call_std<void*>(g_getGameOptObj);
    if (!optObj)
        return false;

    return call_this<bool>(g_setGameOptValue, optObj, option, value);
}

#include "war3_jass_native.h"
#include "war3_jass_types.h"
#include "war3_jass_convert.h"

void war3_preinit() {
    static bool preinitialized = false;
    if (preinitialized) return;
    preinitialized = true;

    base_game_init();
    jass_init();
    jass_convert_init();
}

void war3_init() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    base_game_init();
    jass_init();
    jass_convert_init();
    jass_native_init();
}
