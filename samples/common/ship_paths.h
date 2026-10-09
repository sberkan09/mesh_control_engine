#pragma once
// Shared by the shippable samples: where a file the build produces lives at run time. A shipped
// build (scripts/ship.py) puts it next to the executable; a dev build falls back to the build-tree
// path the CMake target compiled in. An explicit convar value always wins.
//
// Shipped-build start-up (packaging v2), for every shippable executable:
//
//   log::init(...);
//   const samples::BuildInfo build = samples::begin_build();   // reads VERSION.txt, locks cheats
//   console::parse_command_line(argc, argv);
//   if (!samples::start_build("my_game", build)) return 0;      // -version printed; else crash hooks

#include <cstdio>

#include "mce/core.h"
#include "mce/crash.h"
#include "mce/version.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"

namespace mce::samples {

inline ConVar g_cv_version("version", false, "Print the version line and exit", CVarFlags::None);
inline ConVar g_cv_crash_reports("crash_reports", true, "Write a crash report when the game dies (mce/crash.h)",
                                 CVarFlags::RequiresRestart);
inline ConVar g_cv_crash_dir("crash_dir", "", "Crash report directory (default: the per-user MCE crashes folder)",
                             CVarFlags::RequiresRestart);
inline ConVar g_cv_crash_test("crash_test", false, "Crash on purpose after start-up (proves the crash-report chain)",
                              CVarFlags::None);

// A shipped folder carries VERSION.txt (scripts/ship.py): its "version" and "preset" lines name the
// build. Without it this is a development build, named by the engine stamp.
struct BuildInfo {
    bool shipped = false;
    FixedString<192> version;
};

inline BuildInfo read_build_info() {
    BuildInfo info;
    FixedString<1024> path(platform::base_path());
    path.append("VERSION.txt");
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        info.version.assign("dev ");
        info.version.append(engine_describe());
        return info;
    }
    info.shipped = true;
    decltype(info.version) version("unknown");  // same capacity: GCC 13 -O2 sees a 128-byte source overrun otherwise
    decltype(info.version) preset;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        StringView l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l = l.substr(0, l.size() - 1);
        if (l.starts_with("version ")) version.assign(l.substr(8));
        if (l.starts_with("preset ")) preset.assign(l.substr(7));
    }
    std::fclose(f);
    info.version.assign(version.view());
    if (!preset.empty()) {
        info.version.append(" ");
        info.version.append(preset.view());
    }
    return info;
}

// Before console::parse_command_line: a shipped build locks cheats, so neither its command line
// nor its developer console can set a Cheat convar.
inline BuildInfo begin_build() {
    BuildInfo info = read_build_info();
    if (info.shipped) console::set_cheats_allowed(false);
    return info;
}

// After it: -version prints the version line and returns false (exit 0). Otherwise logs the line,
// installs the crash handler (unless -crash_reports 0) and honours -crash_test.
inline bool start_build(const char* app_name, const BuildInfo& info) {
    if (g_cv_version.get_bool()) {
        std::printf("%s %s (engine %s)\n", app_name, info.version.c_str(), engine_describe());
        return false;
    }
    MCE_LOG_INFO("{} {} (engine {}){}", app_name, info.version.c_str(), engine_describe(),
                 info.shipped ? ", cheats locked" : "");
    if (g_cv_crash_reports.get_bool()) {
        crash::CrashDesc cd;
        cd.app_name = app_name;
        cd.version = info.version.c_str();
        const FixedString<512> dir(g_cv_crash_dir.get_string());
        if (!dir.empty()) cd.dump_dir = dir.c_str();
        crash::install(cd);
    }
    if (g_cv_crash_test.get_bool()) crash::trigger_test_crash();
    return true;
}

inline void resolve_shipped_path(StringView override_path, StringView file_name, const char* dev_path,
                                 FixedString<1024>& out) {
    out.clear();
    if (!override_path.empty()) {
        out.append(override_path);
        return;
    }
    out.append(platform::base_path());
    out.append(file_name);
    if (std::FILE* f = std::fopen(out.c_str(), "rb")) {
        std::fclose(f);
        return;
    }
    out.clear();
    out.append(dev_path);
}

// Sets the window icon from a cooked RGBA8 texture in the mounted pack (no mips needed); a missing
// icon only logs. The asset is released again: the platform copies the pixels.
inline void apply_window_icon(platform::Window& window, asset::AssetSystem& assets, StringView path) {
    for (asset::AssetKey k = 0; k < assets.asset_count(); ++k) {
        if (assets.info(k).path != path) continue;
        auto h = assets.load_sync<asset::Texture>(k);
        if (!h) break;
        auto tex = asset::TextureView::read(assets.resolve(h.value()));
        bool ok = false;
        if (tex && !tex.value().mips.empty()) {
            const Span<const byte> px = tex.value().pixels(0);
            ok = window.set_icon(tex.value().mips[0].width, tex.value().mips[0].height,
                                 Span<const u8>(reinterpret_cast<const u8*>(px.data()), px.size()));
        }
        assets.release(asset::AnyHandle(h.value()));
        if (ok) return;
        break;
    }
    MCE_LOG_WARN("no usable window icon '{}' in the pack", path);
}

}  // namespace mce::samples
