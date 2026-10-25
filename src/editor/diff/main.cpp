// mce_scene_diff — what changed between two scenes (M-SCENEDIFF 2).
//
//   mce_scene_diff <before.mcscene> <after.mcscene>
//
// Entities are matched by id; the report names entities added, removed or changed and the components and fields that
// differ. Exit 0: identical, 1: the scenes differ, 2: usage or a scene that cannot be read.

#include <cstdio>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/core.h"
#include "mce/editor/scene_diff.h"

namespace {

using namespace mce;

int usage(std::FILE* out = stderr, int rc = 2) {
    std::fprintf(out,
                 "usage: mce_scene_diff <before.mcscene> <after.mcscene>\n"
                 "  Prints what changed between two scenes: entities added (+), removed (-) or changed (~), with the\n"
                 "  components and fields that differ. Exit code 0: identical, 1: they differ, 2: a scene cannot be read.\n");
    return rc;
}

bool load(const char* path, editor::SceneDocument& doc) {
    cook::String text;
    if (!cook::fs::read_text(path, text)) {
        std::fprintf(stderr, "mce_scene_diff: cannot read %s\n", path);
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
    if (argc != 3) return usage();
    editor::SceneDocument a, b;
    if (!load(argv[1], a) || !load(argv[2], b)) return 2;
    const editor::SceneDiff diff = editor::diff_scenes(a, b);
    if (diff.empty()) {
        std::printf("scenes are identical\n");
        return 0;
    }
    const cook::String report = editor::format_diff(diff);
    std::fwrite(report.c_str(), 1, report.view().size(), stdout);
    std::fputc('\n', stdout);
    return 1;
}
