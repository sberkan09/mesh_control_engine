// The game-module entry point: built into libplatformer.so (mce_add_game_module), loaded by the
// 03_platformer host and mce_devhost through load_game_module().
#include "mce/gameplay.h"
#include "platformer.h"

MCE_GAME_MODULE("platformer", 1, &platformer::install)
