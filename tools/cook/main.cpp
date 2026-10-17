// mce_cook — command-line cook (assets.md §5.1, FR-AST-070).
//
//   mce_cook <project-dir> [--module <game module>] [--base <pack>]... [--pack] [--force] [--adopt] [--quiet]
//            [--only <asset path>]... [--skip-errors] [--textures rgba8|bc|astc] [--max-texture-size N]
//   mce_cook --dump-fbx-scene <file.fbx>     the FBX's lights and cameras as JSON on stdout (for `mce import fbx`)
//
// A project with a mod.json is a mod (ADR-0034): its pack carries a ModInfo chunk and drops into a
// game's mods folder as <id>.mce.
//
// Exit codes (CI contract): 0 = cooked cleanly, 1 = cook errors (listed as
// "file:line:col: error: message [stage]"), 2 = usage error.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mce/cook/cook.h"
#include "mce/log.h"
#include "mce/module_host.h"

// Defined in the asset cook (fbx_importer.cpp; a stub without ufbx). Declared here instead of through the cook-private
// fbx_importer.h so no tool target needs the cook's src/ on its include path; see that header for the contract.
namespace mce::cook {
bool dump_fbx_scene_json(StringView display_name, Span<const byte> src, String& json, String& error);
}  // namespace mce::cook

namespace {

int usage(std::FILE* out = stderr, int rc = 2) {
    std::fputs(
        "usage: mce_cook <project-dir> [--module <game module>] [--base <pack>]... [--pack] [--force] [--adopt] [--quiet]\n"
        "                [--only <asset path>]... [--skip-errors] [--textures rgba8|bc|astc]\n"
        "                [--max-texture-size N]\n"
        "       mce_cook --dump-fbx-scene <file.fbx>   print the FBX's lights and cameras (world transforms, +Y up, metres) as JSON\n"
        "  Cooks <project-dir>/assets into <project-dir>/cooked (incremental: the cook DB skips what is up to date).\n"
        "  --module <path>  load this game module (lib<game>.so / .dll) first, so scenes that use its\n"
        "                   components cook; without it only the engine's components are known\n"
        "  --base <pack>    a game pack (or cooked dir) this project builds on, on top of mod.json's \"base\":\n"
        "                   references may name its assets, and same-GUID assets here override them\n"
        "  --pack    also write <project-dir>/assets.mce (shipping pack; a mod's carries its mod.json identity)\n"
        "  --force   ignore the cook DB and re-cook every asset\n"
        "  --adopt   create .mceasset sidecars for unregistered sources (a file with none is a cook error)\n"
        "  --only <path>  cook only this file or folder under assets/ and what it depends on (repeatable);\n"
        "                 cooks into cooked/ only, never the pack\n"
        "  --skip-errors  cook what can be cooked and list every failing file at the end\n"
        "  --quiet   print errors and the summary only\n"
        "  --textures rgba8|bc|astc  texture storage: rgba8 (default), BC7/BC5/BC4/BC1 blocks (desktop packs), or ASTC 4x4 (phones)\n"
        "  --max-texture-size N  cap the largest side of PNG/JPG textures at N (a power of two, 1..16384): bigger ones are\n"
        "                   halved before their mips, so the pack shrinks; a sidecar's \"max_size\" smaller than N wins.\n"
        "                   Default: no limit. A non-cooked .hdr/.exr keeps its own max_size\n"
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
        case mce::cook::AssetStatus::Skipped: return "skipped";
    }
    return "?";
}

// `mce_cook --dump-fbx-scene <file>`: JSON on stdout, 0 ok, 1 unreadable file (message on stderr).
int dump_fbx_scene(const char* path) {
    using namespace mce;
    constexpr usize kMaxBytes = usize(256) << 20;  // the importer's own file limit
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "mce_cook: cannot open %s\n", path);
        return 1;
    }
    Array<byte> bytes;
    byte buf[65536];
    for (;;) {
        const usize n = std::fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        if (bytes.size() + n > kMaxBytes) {
            std::fclose(f);
            std::fprintf(stderr, "mce_cook: %s: the file is over the importer's limit of 256 MiB\n", path);
            return 1;
        }
        const usize at = bytes.size();
        bytes.resize(at + n);
        std::memcpy(bytes.data() + at, buf, n);
    }
    std::fclose(f);
    cook::String json, error;
    if (!cook::dump_fbx_scene_json(path, Span<const byte>(bytes.data(), bytes.size()), json, error)) {
        std::fprintf(stderr, "mce_cook: %s: %s\n", path, error.c_str());
        return 1;
    }
    std::fwrite(json.data(), 1, json.size(), stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace mce;
    if (argc >= 2 && std::strcmp(argv[1], "--dump-fbx-scene") == 0) {
        if (argc != 3) return usage();
        return dump_fbx_scene(argv[2]);
    }
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
        else if (a == "--only" && i + 1 < argc) opt.paths.push_back(cook::String(argv[++i]));
        else if (a == "--skip-errors") opt.skip_errors = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--textures" && i + 1 < argc) {
            const StringView t(argv[++i]);
            if (t == "astc") opt.textures = cook::TextureCompression::Astc4x4;
            else if (t == "bc") opt.textures = cook::TextureCompression::Bc;
            else if (t != "rgba8") return usage();
        }
        else if (a == "--max-texture-size" && i + 1 < argc) {
            char* end = nullptr;
            const long long v = std::strtoll(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0') {
                std::fprintf(stderr, "mce_cook: --max-texture-size needs a number (a power of two from 1 to 16384), got '%s'\n", argv[i]);
                return 2;
            }
            if (v < 1 || !cook::valid_max_texture_size(static_cast<u32>(v > 0x7FFFFFFF ? 0x7FFFFFFF : v))) {
                std::fprintf(stderr,
                             "mce_cook: error: --max-texture-size %s is not valid: use a power of two from 1 to 16384 (for example 1024), "
                             "or leave the flag out for no limit\n",
                             argv[i]);
                return 1;
            }
            opt.max_texture_size = static_cast<u32>(v);
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
                if (a.status != mce::cook::AssetStatus::Skipped)
                std::printf("  %-10s %-8s assets/%s\n", status_name(a.status), asset::chunk_type_name(a.type), a.path.c_str());
        }
        if (!quiet) {  // the import summary: one line per mesh model
            for (const cook::MeshSummary& m : report.meshes) {
                std::printf("  mesh assets/%s: %u tris, %u submeshes, %u materials, %u joints, %u clips, %u LODs", m.path.c_str(), m.triangles,
                            m.submeshes, m.materials, m.joints, m.clips, m.lod_levels);
                if (m.warnings) std::printf(", %u warning%s", m.warnings, m.warnings == 1 ? "" : "s");
                std::printf("\n");
            }
        }
        std::printf("mce_cook: %u cooked, %u up to date, %u failed; %u error(s), %u warning(s); manifest %016llx", report.cooked,
                    report.up_to_date, report.failed, report.diags.error_count(), report.diags.warning_count(),
                    static_cast<unsigned long long>(report.manifest_hash));
        if (opt.pack && report.pack_hash) std::printf("; pack %016llx", static_cast<unsigned long long>(report.pack_hash));
        std::printf("\n");
        if (!quiet && report.sizes.total() > 0) {
            // The size report (M-CONTENT 1a): cooked bytes by asset type, the biggest assets, textures cut by the cap.
            const cook::SizeReport& s = report.sizes;
            std::printf("mce_cook: size: textures %llu, meshes %llu, audio %llu, scenes %llu, other %llu, total %llu bytes\n",
                        static_cast<unsigned long long>(s.textures), static_cast<unsigned long long>(s.meshes),
                        static_cast<unsigned long long>(s.audio), static_cast<unsigned long long>(s.scenes),
                        static_cast<unsigned long long>(s.other), static_cast<unsigned long long>(s.total()));
            std::printf("mce_cook: textures: %u, %u downscaled by max_size / --max-texture-size\n", s.texture_count,
                        s.textures_downscaled);
            std::printf("mce_cook: largest %zu asset(s):\n", static_cast<size_t>(s.largest.size()));
            for (const cook::SizeEntry& e : s.largest)
                std::printf("  %12llu bytes  assets/%s\n", static_cast<unsigned long long>(e.bytes), e.path.c_str());
        }
        if (opt.skip_errors && !report.failed_files.empty()) {
            std::printf("mce_cook: %zu file(s) could not be cooked (the rest was):\n", static_cast<size_t>(report.failed_files.size()));
            for (const cook::FailedFile& f : report.failed_files)
                std::printf("  %s: %s\n", f.file.c_str(), f.message.c_str());
        }
        if (report.subset)
            std::printf("mce_cook: only the named paths: %u asset(s) left as they were; the pack is not rewritten\n", report.skipped);
        if (report.mod || report.base_assets)
            std::printf("mce_cook: %s on %u base asset(s), %u override(s)\n", report.mod ? "mod" : "project", report.base_assets,
                        report.overrides);
        rc = report.ok() ? 0 : 1;
    }
    log::shutdown();
    return rc;
}
