// The @PROJECT@ host: the engine's player runtime with this game's module and pack. Shipped, the
// pack and the module sit next to the executable; in the build tree the fallbacks find them.
#include "mce/player/player.h"

#if defined(MCE_PROJECT_STATIC_MODULE)
extern "C" const mce::ModuleInfo* mce_game_module();  // the game is linked in on this platform
#endif

int main(int argc, char** argv) {
    mce::player::PlayerDesc desc;
    desc.app_name = MCE_PROJECT_NAME;
    desc.title = MCE_PROJECT_TITLE;
    desc.pack = "assets.mce";
    desc.pack_fallback = MCE_PROJECT_PACK;
    desc.start_scene = MCE_PROJECT_START_SCENE;  // the .mceproj's start_scene
    // Data mods are off. To let players add content, uncomment these: mod packs in mods/ beside the
    // executable then load over the game's pack (docs/user-guide/19-mods.md).
    // desc.mods_dir = "mods";
    // desc.mods_game_id = MCE_PROJECT_NAME;
#if defined(MCE_PROJECT_STATIC_MODULE)
    desc.static_module = &mce_game_module;
#else
    desc.module = MCE_PROJECT_MODULE_NAME;
    desc.module_fallback = MCE_PROJECT_MODULE;
#endif
    return mce::player::run(argc, argv, desc);
}
