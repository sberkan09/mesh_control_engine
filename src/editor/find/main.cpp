// mce_scene_find — the entities of scenes that match a question (M-SCENEFIND 2).
//
//   mce_scene_find <scene.mcscene>... [--name <part>] [--component <Type>] [--prefab]
//
// One line per match, prefixed with the file: `scenes/a.mcscene: Room/Wall (id 2)`. Every condition given must hold.
// Exit 0: at least one match, 1: none, 2: usage, an unknown component or a scene that cannot be read.

#include <cstdio>
#include <cstring>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/core.h"
#include "mce/editor/scene_find.h"
#include "mce/refl/type.h"

namespace {

using namespace mce;

int usage(std::FILE* out = stderr, int rc = 2) {
    std::fprintf(out,
                 "usage: mce_scene_find <scene.mcscene>... [--name <part>] [--component <Type>] [--prefab]\n"
                 "  Prints the entities that match, one line each: file, hierarchy path and id. --name: part of the name,\n"
                 "  ignoring case; --component: a component type the entity carries; --prefab: only prefab instances.\n"
                 "  Every condition given must hold. Exit code 0: matches, 1: none, 2: usage or a scene that cannot be read.\n");
    return rc;
}

bool load(const char* path, editor::SceneDocument& doc) {
    cook::String text;
    if (!cook::fs::read_text(path, text)) {
        std::fprintf(stderr, "mce_scene_find: cannot read %s\n", path);
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
    editor::FindQuery query;
    Array<const char*> scenes;
    for (int i = 1; i < argc; ++i) {
        const StringView a(argv[i]);
        if (a == "--prefab") {
            query.prefab_instances_only = true;
        } else if (a == "--name" || a == "--component") {
            if (i + 1 >= argc) return usage();
            const char* value = argv[++i];
            if (a == "--name") {
                query.name = StringView(value);
            } else {
                query.component = refl::find_type(StringId(value));
                if (query.component == nullptr) {
                    std::fprintf(stderr, "mce_scene_find: unknown component %s\n", value);
                    return 2;
                }
            }
        } else if (a.size() > 1 && a[0] == '-' && a[1] == '-') {
            return usage();
        } else {
            scenes.push_back(argv[i]);
        }
    }
    if (scenes.empty()) return usage();
    bool unreadable = false;
    bool any = false;
    for (const char* path : scenes) {
        editor::SceneDocument doc;
        if (!load(path, doc)) {
            unreadable = true;
            continue;
        }
        for (const editor::FindHit& h : editor::find_entities(doc, query)) {
            std::printf("%s: %s (id %u)\n", path, h.path.c_str(), static_cast<unsigned>(h.id));
            any = true;
        }
    }
    return unreadable ? 2 : any ? 0 : 1;
}
