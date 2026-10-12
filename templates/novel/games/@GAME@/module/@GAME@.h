#pragma once
// @PROJECT@: a visual novel on the engine's novel kit (mce/kit/novel.h). The story is
// story/*.story, compiled by the build into story_data.h (mce story compile).

#include "components.h"
#include "mce/gameplay.h"

namespace @GAME@ {

// The autosave slot: the host loads it at start (mce/kit/novel_host.h) and the kit writes it at
// every @checkpoint.
inline constexpr const char* kSaveSlot = "@GAME@_autosave";

// Installs the novel kit with the compiled story.
mce::ResultV install(mce::World& world);

}  // namespace @GAME@
