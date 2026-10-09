// mce_cook — command-line cook (assets.md §5.1, FR-AST-070).
//
//   mce_cook <project-dir> [--pack] [--force] [--adopt] [--quiet]
//
// Exit codes (CI contract): 0 = cooked cleanly, 1 = cook errors (listed as
// "file:line:col: error: message [stage]"), 2 = usage error.

#include <cstdio>
#include <cstring>

#include "mce/cook/cook.h"
#include "mce/log.h"

namespace {

int usage(std::FILE* out = stderr, int rc = 2) {
    std::fputs(
        "usage: mce_cook <project-dir> [--pack] [--force] [--adopt] [--quiet]\n"
        "  Cooks <project-dir>/assets into <project-dir>/cooked (incremental: the cook DB skips what is up to date).\n"
        "  --pack    also write <project-dir>/assets.mce (shipping pack)\n"
        "  --force   ignore the cook DB and re-cook every asset\n"
        "  --adopt   create .mceasset sidecars for unregistered sources\n"
        "  --quiet   print errors and the summary only\n"
        "  --help    this text\n",
        out);
    return rc;
}

const char* status_name(mce::cook::AssetStatus s) {
    switch (s) {
        case mce::cook::AssetStatus::Cooked: return "cooked";
        case mce::cook::AssetStatus::UpToDate: return "up-to-date";
        case mce::cook::AssetStatus::Failed: return "FAILED";
        case mce::cook::AssetStatus::Blocked: return "blocked";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace mce;
    const char* project = nullptr;
    cook::CookOptions opt;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const StringView a(argv[i]);
        if (a == "--pack") opt.pack = true;
        else if (a == "--force") opt.force = true;
        else if (a == "--adopt") opt.adopt = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--help" || a == "-h" || a == "-help") return usage(stdout, 0);
        else if (!a.empty() && a[0] == '-') return usage();
        else if (!project) project = argv[i];
        else return usage();
    }
    if (!project) return usage();

    log::init(log::LogDesc{"mce_cook", nullptr, log::Level::Warn, true});
    cook::CookService service;
    const cook::CookReport report = service.cook(project, opt);

    for (const cook::Diagnostic& d : report.diags.all()) {
        if (quiet && d.severity != cook::Severity::Error) continue;
        cook::String line;
        cook::format_diagnostic(d, line);
        std::fprintf(d.severity == cook::Severity::Error ? stderr : stdout, "%s\n", line.c_str());
    }
    if (!quiet) {
        for (const cook::AssetReport& a : report.assets)
            std::printf("  %-10s %-8s assets/%s\n", status_name(a.status), asset::chunk_type_name(a.type), a.path.c_str());
    }
    std::printf("mce_cook: %u cooked, %u up to date, %u failed; %u error(s), %u warning(s); manifest %016llx", report.cooked,
                report.up_to_date, report.failed, report.diags.error_count(), report.diags.warning_count(),
                static_cast<unsigned long long>(report.manifest_hash));
    if (opt.pack && report.pack_hash) std::printf("; pack %016llx", static_cast<unsigned long long>(report.pack_hash));
    std::printf("\n");
    log::shutdown();
    return report.ok() ? 0 : 1;
}
