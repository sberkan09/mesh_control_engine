#pragma once
// A novel host's flags and autosave slot (M-NOVEL). Include it in the host's main.cpp only (it
// defines convars): a module's convars would register after the player parsed the command line,
// so the host declares them and the kit reads them by name.
//
//   novel.autoplay N   the bot plays: N = 1 + the index of a tag in story.json "tags" (0 = off)
//   novel.label NAME   start a new run at this script label (screenshots, authoring)
//   novel.skip N       with novel.label: advance this many pages first
//   novel.instant 1    show every page whole (no typewriter)
//
//   int main(int argc, char** argv) {
//       ...desc...
//       mce::novel::HostArgs args(argc, argv, "my_novel_autosave");
//       return mce::player::run(args.argc(), args.argv(), desc);
//   }
//
// HostArgs adds `-save <slot>` unless the command line names a save or runs headless: the player
// then loads the slot at start and writes it on a clean quit, and the kit writes it at every
// @checkpoint (NovelState::save_slot must name the same slot).

#include <cstring>

#include "mce/convar.h"

MCE_CVAR(cv_novel_autoplay, "novel.autoplay", 0, "The novel bot plays by a tag: 1 + its index in story.json \"tags\" (0 = off)",
         mce::CVarFlags::None);
MCE_CVAR(cv_novel_label, "novel.label", "", "Start a new run at this script label (screenshots, authoring)", mce::CVarFlags::None);
MCE_CVAR(cv_novel_skip, "novel.skip", 0, "With novel.label: advance this many pages first", mce::CVarFlags::None);
MCE_CVAR(cv_novel_instant, "novel.instant", false, "Show every page whole (no typewriter)", mce::CVarFlags::None);

namespace mce::novel {

class HostArgs {
public:
    HostArgs(int argc, char** argv, const char* slot) {
        bool has_save = false, headless = false;
        for (int i = 0; i < argc && n_ < kMax - 3; ++i) {
            if (i > 0 && std::strcmp(argv[i], "-save") == 0) has_save = true;
            if (i > 0 && std::strcmp(argv[i], "-headless") == 0) headless = true;
            args_[n_++] = argv[i];
        }
        if (!has_save && !headless && slot) {
            args_[n_++] = save_flag_;
            args_[n_++] = const_cast<char*>(slot);
        }
        args_[n_] = nullptr;
    }
    int argc() const { return n_; }
    char** argv() { return args_; }

private:
    static constexpr int kMax = 256;
    char save_flag_[6] = "-save";
    char* args_[kMax] = {};
    int n_ = 0;
};

}  // namespace mce::novel
