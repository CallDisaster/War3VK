#pragma once
#include "war3_types.h"
#include "war3_game_struct.h"

extern uint32_t				game_version;
extern CGameState*			game_state;
extern CGameWar3*			game_war3;
extern CGxDevice*			gx_device;
extern GameTimer*			game_timer;
extern GameTable*			game_table;

extern uintptr_t pGameDLL;
extern uintptr_t pStorm;

// 游戏设置项（仅保留当前需要用到的）
enum GAME_OPTION : uint32_t {
    GAME_OPTION_REFRESH_RATE = 0x04,
    GAME_OPTION_MAX_FPS = 0x16,
};

bool war3_get_game_opt_value(GAME_OPTION option, uint32_t* outValue);
bool war3_set_game_opt_value(GAME_OPTION option, uint32_t value);

void base_game_init();
void war3_preinit();
void war3_init();
