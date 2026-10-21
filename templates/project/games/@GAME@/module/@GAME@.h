#pragma once
// @PROJECT@ gameplay: the install function the module entry hands the host.

#include "components.h"
#include "mce/gameplay.h"

namespace @GAME@ {

// Registers the game's components and systems on top of the engine's.
mce::ResultV install(mce::World& world);

}  // namespace @GAME@
