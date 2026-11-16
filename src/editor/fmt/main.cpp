// mce_scene_fmt — keep scene files in the canonical text (M-SCENEFMT).
//
//   mce_scene_fmt <scene.mcscene>... [--write]
//
// Without --write: prints `<file>: not canonical` for each scene whose text differs from what the editor would save.
// With --write: rewrites those scenes and prints `<file>: formatted`. Exit 0: canonical (or rewritten), 1: some are not,
// 2: usage, a scene that cannot be read or does not parse, or a write that failed.

#include <cstdio>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/core.h"
#include "mce/editor/scene_document.h"

namespace {

using namespace mce;

int usage(std::FILE* out = stderr, int rc = 2) {
    std::fprintf(out,
                 "usage: mce_scene_fmt <scene.mcscene>... [--write]\n"
                 "  Checks that each scene is in the canonical text the editor saves (one component object per line, non-default\n"
                 "  fields only). --write rewrites the scenes that are not. Exit code 0: canonical or rewritten, 1: some are not,\n"
                 "  2: usage or a scene that cannot be read.\n");
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (StringView(argv[1]) == "--help" || StringView(argv[1]) == "-h")) return usage(stdout, 0);
    bool write = false;
    Array<const char*> scenes;
    for (int i = 1; i < argc; ++i) {
        const StringView a(argv[i]);
        if (a == "--write")
            write = true;
        else if (a.size() > 1 && a[0] == '-' && a[1] == '-')
            return usage();
        else
            scenes.push_back(argv[i]);
    }
    if (scenes.empty()) return usage();
    bool failed = false, differs = false;
    for (const char* path : scenes) {
        cook::String text;
        if (!cook::fs::read_text(path, text)) {
            std::fprintf(stderr, "mce_scene_fmt: cannot read %s\n", path);
            failed = true;
            continue;
        }
        editor::SceneDocument doc;
        cook::DiagList diags;
        if (!doc.parse(text.view(), path, diags)) {
            for (const cook::Diagnostic& d : diags.all()) {
                cook::String line;
                cook::format_diagnostic(d, line);
                std::fprintf(stderr, "%s\n", line.c_str());
            }
            failed = true;
            continue;
        }
        cook::String canonical;
        doc.serialize(canonical);
        if (canonical == text) continue;
        if (!write) {
            std::printf("%s: not canonical\n", path);
            differs = true;
            continue;
        }
        const Span<const byte> bytes(reinterpret_cast<const byte*>(canonical.c_str()), canonical.size());
        if (!cook::fs::write_file_atomic(path, bytes)) {
            std::fprintf(stderr, "mce_scene_fmt: cannot write %s\n", path);
            failed = true;
            continue;
        }
        std::printf("%s: formatted\n", path);
    }
    return failed ? 2 : differs ? 1 : 0;
}
