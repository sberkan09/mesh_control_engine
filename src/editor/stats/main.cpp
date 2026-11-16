// mce_scene_stats — what a scene holds at a glance (M-SCENESTATS 2).
//
//   mce_scene_stats <scene.mcscene>...
//
// With several scenes each report is headed by the file name. Exit 0: ok, 2: usage or a scene that cannot be read.

#include <cstdio>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/core.h"
#include "mce/editor/scene_stats.h"

namespace {

using namespace mce;

int usage(std::FILE* out = stderr, int rc = 2) {
    std::fprintf(out,
                 "usage: mce_scene_stats <scene.mcscene>...\n"
                 "  Prints what each scene holds: entities, roots, hierarchy depth, prefab instances, the box the local\n"
                 "  positions span and the components in use, most used first. Exit code 0: ok, 2: a scene cannot be read.\n");
    return rc;
}

bool load(const char* path, editor::SceneDocument& doc) {
    cook::String text;
    if (!cook::fs::read_text(path, text)) {
        std::fprintf(stderr, "mce_scene_stats: cannot read %s\n", path);
        return false;
    }
    cook::DiagList diags;
    if (!doc.parse(text.view(), path, diags)) {
        for (const cook::Diagnostic& d : diags.all()) {
            cook::String line;
            cook::format_diagnostic(d, line);
            std::fprintf(stderr, "%s\n", line.c_str());
        }
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (StringView(argv[1]) == "--help" || StringView(argv[1]) == "-h")) return usage(stdout, 0);
    if (argc < 2) return usage();
    bool unreadable = false;
    for (int i = 1; i < argc; ++i) {
        editor::SceneDocument doc;
        if (!load(argv[i], doc)) {
            unreadable = true;
            continue;
        }
        if (argc > 2) std::printf("%s%s:\n", i > 1 ? "\n" : "", argv[i]);
        std::printf("%s\n", editor::format_stats(editor::scene_stats(doc)).c_str());
    }
    return unreadable ? 2 : 0;
}
