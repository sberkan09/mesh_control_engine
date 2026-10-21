// The Undelivered host: the engine's player runtime with this game's module and pack. Shipped, the
// pack and the module sit next to the executable; in the build tree the fallbacks find them.
#include "mce/kit/novel_host.h"  // the novel.* flags (novel.autoplay, novel.label, ...)
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
#if defined(MCE_PROJECT_STATIC_MODULE)
    desc.static_module = &mce_game_module;
#else
    desc.module = MCE_PROJECT_MODULE_NAME;
    desc.module_fallback = MCE_PROJECT_MODULE;
#endif
    // The autosave slot: the player loads it at start and writes it on a clean quit; the game writes
    // it at every night journal. Headless runs (CI, run_headless) and an explicit -save leave it alone.
    mce::novel::HostArgs args(argc, argv, "undelivered_autosave");
    return mce::player::run(args.argc(), args.argv(), desc);
}
