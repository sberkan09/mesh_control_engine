// mce_cook — command-line cook (assets.md §5.1, FR-AST-070).
//
//   mce_cook <project-dir> [--module <game module>] [--base <pack>]... [--pack] [--force] [--adopt] [--quiet]
//            [--textures rgba8|astc]
//
// A project with a mod.json is a mod (ADR-0034): its pack carries a ModInfo chunk and drops into a
// game's mods folder as <id>.mce.
//
// Exit codes (CI contract): 0 = cooked cleanly, 1 = cook errors (listed as
// "file:line:col: error: message [stage]"), 2 = usage error.

#include <cstdio>
#include <cstring>

#include "mce/cook/cook.h"
#include "mce/log.h"
#include "mce/module_host.h"

namespace {

int usage(std::FILE* out = stderr, int rc = 2) {
    std::fputs(
        "usage: mce_cook <project-dir> [--module <game module>] [--base <pack>]... [--pack] [--force] [--adopt] [--quiet]\n"
        "                [--textures rgba8|astc]\n"
        "  Cooks <project-dir>/assets into <project-dir>/cooked (incremental: the cook DB skips what is up to date).\n"
        "  --module <path>  load this game module (lib<game>.so / .dll) first, so scenes that use its\n"
        "                   components cook; without it only the engine's components are known\n"
        "  --base <pack>    a game pack (or cooked dir) this project builds on, on top of mod.json's \"base\":\n"
        "                   references may name its assets, and same-GUID assets here override them\n"
        "  --pack    also write <project-dir>/assets.mce (shipping pack; a mod's carries its mod.json identity)\n"
        "  --force   ignore the cook DB and re-cook every asset\n"
        "  --adopt   create .mceasset sidecars for unregistered sources\n"
        "  --quiet   print errors and the summary only\n"
        "  --textures rgba8|astc  texture storage: rgba8 (default, desktops) or ASTC 4x4 blocks (phones)\n"
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
    const char* module_path = nullptr;
    cook::CookOptions opt;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const StringView a(argv[i]);
        if (a == "--module" && i + 1 < argc) module_path = argv[++i];
        else if (a == "--base" && i + 1 < argc) opt.base_packs.push_back(cook::String(argv[++i]));
        else if (a == "--pack") opt.pack = true;
        else if (a == "--force") opt.force = true;
        else if (a == "--adopt") opt.adopt = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--textures" && i + 1 < argc) {
            const StringView t(argv[++i]);
            if (t == "astc") opt.textures = cook::TextureCompression::Astc4x4;
            else if (t != "rgba8") return usage();
        }
        else if (a == "--help" || a == "-h" || a == "-help") return usage(stdout, 0);
        else if (!a.empty() && a[0] == '-') return usage();
        else if (!project) project = argv[i];
        else return usage();
    }
    if (!project) return usage();

    log::init(log::LogDesc{"mce_cook", nullptr, log::Level::Warn, true});
    // The game module stays mapped until after the cook: its reflected components register with
    // mce_refl as it maps, and the cook keeps pointers to those records while it runs.
    struct ModuleGuard {
        LoadedModule module;
        ~ModuleGuard() { unload_game_module(module); }
    } game;
    if (module_path) {
        Result<LoadedModule> loaded = open_game_module(module_path);
        if (!loaded) {
            FixedString<512> msg;
            format_error(loaded.error(), msg);
            std::fprintf(stderr, "mce_cook: cannot load game module %s: %s\n", module_path, msg.c_str());
            log::shutdown();
            return 1;
        }
        game.module = loaded.value();
    }
    int rc = 1;
    {  // the cook and its report die before the module unmaps (importers hold its component records)
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
        if (report.mod || report.base_assets)
            std::printf("mce_cook: %s on %u base asset(s), %u override(s)\n", report.mod ? "mod" : "project", report.base_assets,
                        report.overrides);
        rc = report.ok() ? 0 : 1;
    }
    log::shutdown();
    return rc;
}
