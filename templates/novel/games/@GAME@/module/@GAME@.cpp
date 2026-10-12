#include "@GAME@.h"

#include "mce/kit/novel.h"
#include "story_data.h"

namespace @GAME@ {

using namespace mce;

ResultV install(World& world) {
    // The kit's runtime state: a host or test may have set it first (the bot, no autosave).
    if (!world.try_resource<novel::NovelState>()) world.set_resource(novel::NovelState{});
    world.resource<novel::NovelState>().save_slot.assign(StringView(kSaveSlot));
    // NovelHooks give the game its own screens: an @<command> declared in story.json "commands",
    // a HUD line, extra panel rows (see mce/kit/novel.h). This novel needs none yet.
    novel::NovelHooks hooks;
    return novel::install(world, story::kProgram, hooks);
}

}  // namespace @GAME@
