// mce_scene_lint — mistakes in scenes that load fine (M-SCENELINT 2).
//
//   mce_scene_lint <scene.mcscene>... [--fix]
//
// One line per finding, prefixed with the file. Exit 0: no findings, 1: findings, 2: usage or a scene that cannot be read.
// With --fix (M-LINTFIX 2): repairs the findings, rewrites the scene in the canonical text and prints `<file>: fixed N`;
// exit 0 once every scene is clean, 2 when a scene cannot be read or written.

#include <cstdio>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/core.h"
#include "mce/editor/scene_lint.h"

namespace {

using namespace mce;

int usage(std::FILE* out = stderr, int rc = 2) {
    std::fprintf(out,
                 "usage: mce_scene_lint <scene.mcscene>... [--fix]\n"
                 "  Prints the mistakes in each scene: entities without a name, siblings that share a name, a scale of zero,\n"
                 "  a transform that is not a number, a missing parent. Exit code 0: none, 1: findings, 2: a scene cannot be read.\n"
                 "  --fix repairs them and rewrites the scene (names get numbered, a missing parent or reference is dropped, a\n"
                 "  transform that is not a number returns to its default, a zero scale axis becomes 1); exit code 0 afterwards.\n");
    return rc;
}

bool load(const char* path, editor::SceneDocument& doc) {
    cook::String text;
    if (!cook::fs::read_text(path, text)) {
        std::fprintf(stderr, "mce_scene_lint: cannot read %s\n", path);
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
    bool fix = false;
    for (int i = 1; i < argc; ++i)
        if (StringView(argv[i]) == "--fix") fix = true;
    bool unreadable = false, findings = false;
    for (int i = 1; i < argc; ++i) {
        if (StringView(argv[i]) == "--fix") continue;
        editor::SceneDocument doc;
        if (!load(argv[i], doc)) {
            unreadable = true;
            continue;
        }
        if (fix) {
            const usize fixed = editor::fix_scene_lint(doc);
            if (fixed == 0) continue;
            cook::String text;
            doc.serialize(text);
            const Span<const byte> bytes(reinterpret_cast<const byte*>(text.c_str()), text.size());
            if (!cook::fs::write_file_atomic(argv[i], bytes)) {
                std::fprintf(stderr, "mce_scene_lint: cannot write %s\n", argv[i]);
                unreadable = true;
                continue;
            }
            std::printf("%s: fixed %u\n", argv[i], static_cast<unsigned>(fixed));
            continue;
        }
        const Array<editor::LintFinding> found = editor::lint_scene(doc);
        if (found.empty()) continue;
        findings = true;
        const cook::String report = editor::format_lint(found.span());
        usize at = 0;
        const StringView text = report.view();
        while (at < text.size()) {
            usize end = text.find('\n', at);
            if (end == StringView::npos) end = text.size();
            std::printf("%s: %.*s\n", argv[i], static_cast<int>(end - at), text.data() + at);
            at = end + 1;
        }
    }
    if (unreadable) return 2;
    if (!findings) {
        if (!fix) std::printf("no findings\n");
        return 0;
    }
    return 1;
}
