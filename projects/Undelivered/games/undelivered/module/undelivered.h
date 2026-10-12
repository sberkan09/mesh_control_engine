#pragma once
// Undelivered gameplay on the visual-novel kit (mce/kit/novel.h): the compiled script
// (story_data.h, `mce story compile`), its tables, its hooks (the drawer, the night journal, the
// HUD line, the Letters page) and the drawer's own parts.

#include "components.h"
#include "mce/gameplay.h"
#include "mce/kit/novel.h"

namespace undelivered {

// The kit's runtime state and modes, under the names the game and its test use.
using NovelState = mce::novel::NovelState;
using Mode = mce::novel::Mode;

// The autosave slot (the host passes it with -save; the kit writes it at every night journal).
inline constexpr const char* kSaveSlot = "undelivered_autosave";

// Registers the kit (with the game's hooks) and the drawer's component.
mce::ResultV install(mce::World& world);

// The StoryState of the scene's save entity (null when the scene has none / before the first tick).
mce::novel::StoryState* novel_save(mce::World& world);

// Starts a fresh run on the next tick (keeps the slot's endings_seen).
void new_game(mce::World& world);

}  // namespace undelivered
