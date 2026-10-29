// M-HARDEN2 1: the small files the editor and the MCP server keep or read back, fed mutated text: the recent-files
// list, the camera bookmarks, the project's .mceproj (budgets and a patch over it) and the locale tables the strings
// grid reads. Nothing here may crash, hang or fire an MCE_ASSERT on any text, and what a reader accepts must survive
// a write and a second read.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "json_swap.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/editor/camera_bookmarks.h"
#include "mce/editor/project.h"
#include "mce/editor/project_settings.h"
#include "mce/editor/recent_list.h"
#include "mce/editor/strings_grid.h"
#include "mce/log.h"
#include "mutator.h"

using namespace mce;

namespace {

u32 iterations(u32 base) {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : base;
}

// Mutant `it` of `seed`: even iterations flip bytes, odd ones swap one JSON value (when `json`) so type checks are reached.
std::string mutant(fuzz::Mutator& m, std::vector<std::uint64_t>& storage, const std::string& seed, u32 it, bool json) {
    const Span<const byte> bytes(reinterpret_cast<const byte*>(seed.data()), seed.size());
    if (json && it % 2 == 1) return fuzz::swap_value(m, bytes);
    const usize len = m.mutate(bytes, storage);
    return std::string(reinterpret_cast<const char*>(storage.data()), len);
}

std::string scratch_dir(const char* name) {
    const char* root = std::getenv("MCE_FUZZ_ROOT");
    std::filesystem::path p = std::filesystem::path(root ? root : ".") / "fuzz_editor_files" / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

}  // namespace

TEST_CASE("fuzz: the recent-files list survives mutated text and reads back what it wrote") {
    log::set_level(log::Level::Off);
    const std::string seed = "/p/one.mcscene\n/p/two.mcscene\r\n\n/p/three.mcscene\n/p/one.mcscene\n";
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x4EC01);
    const u32 n = iterations(3000);
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: recent list iteration " << it);
        const std::string text = mutant(m, storage, seed, it, false);
        editor::RecentList list(8);
        list.parse(StringView(text.data(), text.size()));
        CHECK(list.items().size() <= list.cap());
        const cook::String out = list.serialize();
        editor::RecentList back(8);
        back.parse(out.view());
        REQUIRE(back.items().size() == list.items().size());
        for (usize i = 0; i < back.items().size(); ++i) CHECK(back.items()[i].view() == list.items()[i].view());
    }
}

TEST_CASE("fuzz: camera bookmarks survive mutated text and only hold finite views") {
    log::set_level(log::Level::Off);
    editor::CameraBookmarks good;
    editor::EditorCamera2D cam2d;
    editor::OrbitCamera orbit;
    for (u32 s : {0u, 3u, 8u}) good.save(s, cam2d, orbit);
    const std::string seed(good.serialize().c_str());
    REQUIRE(!seed.empty());
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0xCA4B0);
    const u32 n = iterations(3000);
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: bookmarks iteration " << it);
        const std::string text = mutant(m, storage, seed, it, false);
        editor::CameraBookmarks b;
        b.parse(StringView(text.data(), text.size()));
        editor::EditorCamera2D c2;
        editor::OrbitCamera o;
        for (u32 s = 0; s < editor::CameraBookmarks::kSlots; ++s) {
            if (!b.recall(s, c2, o)) continue;
            CHECK(std::isfinite(c2.center.x));
            CHECK(std::isfinite(c2.zoom));
            CHECK(std::isfinite(o.distance));
        }
        editor::CameraBookmarks again;
        again.parse(b.serialize().view());
        CHECK(again.count() == b.count());
    }
}

TEST_CASE("fuzz: a mutated .mceproj never crashes the budget reader or a settings patch") {
    log::set_level(log::Level::Off);
    const std::string root = scratch_dir("mceproj");
    std::filesystem::create_directories(root + "/assets");
    const std::string file = root + "/fuzz.mceproj";
    const std::string seed =
        "{ \"name\": \"fuzz\", \"title\": \"Fuzz\", \"engine_version\": \"0.9.0\", \"games\": [\"a\", \"b\"], \"plugins\": [],\n"
        "  \"start_scene\": \"5ce2e000-0000-4000-8000-00000000f001\",\n"
        "  \"budgets\": { \"mobile\": { \"draws\": 300, \"frame_ms\": 33.3 }, \"web\": { \"draws\": 500 } } }\n";
    editor::EditorProject project;
    REQUIRE(project.open(root));
    cook::JsonDoc patch_doc;
    cook::JsonError je;
    REQUIRE(patch_doc.parse("{ \"title\": \"Patched\", \"games\": [\"x\"], \"budgets\": { \"mobile\": { \"draws\": 10 } } }", je));
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0xB06E7);
    const u32 n = iterations(1500);
    u64 read = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: mceproj iteration " << it);
        const std::string text = mutant(m, storage, seed, it, true);
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
        }
        for (usize t = 0; t < perf::kTargetCount; ++t) {
            perf::Budget b;
            cook::String err;
            if (editor::project_budget(project, static_cast<perf::Target>(t), b, err)) ++read;
        }
        cook::String err;
        (void)editor::patch_project_file(project, patch_doc.root(), err);
    }
    std::printf("fuzz: mceproj: %u mutants, %llu budget reads\n", n, static_cast<unsigned long long>(read));
    CHECK(read > 0);
}

TEST_CASE("fuzz: mutated locale tables load into the strings grid and what loads saves") {
    log::set_level(log::Level::Off);
    const std::string root = scratch_dir("strings");
    std::filesystem::create_directories(root + "/assets/loc");
    const std::string seed =
        "key,text,note\n# menu strings\nmenu.play,Play,button\nmenu.greeting,\"Hello, {name}!\",\nscore.coins#one,{count} coin,\n"
        "score.coins#other,{count} coins,\n";
    std::ofstream(root + "/assets/loc/tr.csv") << "key,text\nmenu.play,Oyna\n";
    std::ofstream(root + "/assets/loc/en.csv.mceasset") << R"({ "guid": "5ce4e000-0000-4000-8000-0000000000e1", "importer": "strings" })";
    std::ofstream(root + "/assets/loc/tr.csv.mceasset") << R"({ "guid": "5ce4e000-0000-4000-8000-0000000000e2", "importer": "strings" })";
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x57A1C);
    const u32 n = iterations(600);
    u64 loaded = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: strings iteration " << it);
        const std::string text = mutant(m, storage, seed, it, false);
        {
            std::ofstream out(root + "/assets/loc/en.csv", std::ios::binary | std::ios::trunc);
            out << text;
        }
        editor::EditorProject project;
        REQUIRE(project.open(root));
        editor::StringsGrid grid;
        cook::String err;
        if (!grid.load(project, err)) continue;
        ++loaded;
        if (!grid.keys().empty() && !grid.locales().empty()) grid.set_text(0, 0, "edited");
        (void)grid.add_key("fuzz.added");
        cook::String serr;
        (void)grid.save(project, serr);  // a table the cook refuses is not written; either way, no crash
    }
    std::printf("fuzz: strings grid: %u mutants, %llu loaded\n", n, static_cast<unsigned long long>(loaded));
    CHECK(loaded > 0);
}
