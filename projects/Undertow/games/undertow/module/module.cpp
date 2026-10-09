// The game-module entry point: built into libundertow (mce_add_project), loaded by the host.
#include "undertow.h"

MCE_GAME_MODULE("undertow", 1, &undertow::install)
