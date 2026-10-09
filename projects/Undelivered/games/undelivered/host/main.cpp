// The Undelivered host: the engine's player runtime with this game's module and pack. Shipped, the
// pack and the module sit next to the executable; in the build tree the fallbacks find them.
#include <cstring>
#include <vector>

#include "mce/convar.h"
#include "mce/player/player.h"

// The game's own flags. A module's convars would register after the player parsed the command
// line, so the host declares them and the module reads them by name (ConVar::find).
MCE_CVAR(cv_nv_autoplay, "nv.autoplay", 0, "The bot plays: 1 first option, 2 leave, 3 stay, 4 shut, 5 write (0 = off)", mce::CVarFlags::None);
MCE_CVAR(cv_nv_label, "nv.label", "", "Start a new run at this script label (screenshots, authoring)", mce::CVarFlags::None);
MCE_CVAR(cv_nv_skip, "nv.skip", 0, "With nv.label: advance this many pages first", mce::CVarFlags::None);
MCE_CVAR(cv_nv_instant, "nv.instant", false, "Show every page whole (no typewriter)", mce::CVarFlags::None);

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
    std::vector<char*> args(argv, argv + argc);
    bool has_save = false, headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-save") == 0) has_save = true;
        if (std::strcmp(argv[i], "-headless") == 0) headless = true;
    }
    static char slot_flag[] = "-save";
    static char slot_name[] = "undelivered_autosave";
    if (!has_save && !headless) {
        args.push_back(slot_flag);
        args.push_back(slot_name);
    }
    args.push_back(nullptr);
    return mce::player::run(static_cast<int>(args.size()) - 1, args.data(), desc);
}
