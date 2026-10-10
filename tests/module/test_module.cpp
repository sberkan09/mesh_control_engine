// Game-module loader contract tests (scripting.md §6.1, §6.2, §3.1 ComponentId identity).
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

#include "mce/core.h"
#include "mce/module_host.h"
#include "mce/platform/library.h"
#include "module_types.h"

using namespace mce;
using namespace mce::modtest;

namespace {

// Registered by the host image; the module reads it through the shared engine (one registry).
MCE_CVAR(cv_host_probe, "test.module_host_cvar", 41, "read by the test game module", CVarFlags::None);

constexpr f64 kDt = 1.0 / 60.0;

void run_ticks(World& w, u64 n) {
    for (u64 i = 1; i <= n; ++i) w.tick(kDt, i);
}

i32 total_hp(World& w) {
    i32 sum = 0;
    w.query<const Health&>().each([&](const Health& h) { sum += h.hp; });
    return sum;
}

Errc load_error(const char* path) {
    World w;
    Result<LoadedModule> r = load_game_module(path, w);
    if (r) {
        unload_game_module(r.value());  // not reached in the error tests; keeps the slot table clean
        return Errc::Ok;
    }
    return r.error().code;
}

}  // namespace

TEST_CASE("module: load, install, tick, unload") {
    const u32 before = loaded_module_count();
    LoadedModule mod;
    {
        World w;
        // The host registers Health first; the module registers Velocity first.
        const ecs::ComponentId host_health = w.component_id<Health>();

        Result<LoadedModule> r = load_game_module(MODTEST_GOOD, w);
        REQUIRE(r.has_value());
        mod = r.value();
        CHECK(mod.loaded());
        CHECK(StringView(mod.info->name) == "modtest");
        CHECK(mod.info->module_version == 3);
        CHECK(mod.info->engine_id == engine_id());
        CHECK(loaded_module_count() == before + 1);

        // Copy-and-load: the mapped image is a copy next to the original, never the build output.
        CHECK(mod.loaded_path.view() != mod.source_path.view());
        CHECK(mod.loaded_path.view().find("_loaded_") != StringView::npos);
        CHECK(platform::file_exists(mod.loaded_path));

        // ComponentId identity across the boundary (finding F11 CI test).
        const Probe& probe = w.resource<Probe>();
        CHECK(probe.health_id == host_health);
        CHECK(probe.velocity_id == w.component_id<Velocity>());
        CHECK(probe.installs == 1);
        // One engine instance: the module saw the convar the host image registered.
        CHECK(probe.host_cvar == 41);

        // Module systems run in the host's world: Startup spawns, FixedUpdate_Pre bleeds 1/tick.
        run_ticks(w, 10);
        CHECK(w.query<const Health&>().count() == kSpawned);
        CHECK(total_hp(w) == static_cast<i32>(kSpawned) * (kStartHp - 10));
    }  // World destroyed first: it holds module code pointers.
    const FixedString<512> copy = mod.loaded_path;
    unload_game_module(mod);
    CHECK_FALSE(mod.loaded());
    CHECK(loaded_module_count() == before);
    CHECK_FALSE(platform::file_exists(copy));  // the copy is deleted on unload
}

TEST_CASE("module: reload at the play boundary reproduces the state hash") {
    u64 hashes[2] = {};
    for (u64& h : hashes) {
        LoadedModule mod;
        {
            World w;
            Result<LoadedModule> r = load_game_module(MODTEST_GOOD, w);
            REQUIRE(r.has_value());
            mod = r.value();
            run_ticks(w, 600);
            h = w.state_hash();
        }
        unload_game_module(mod);
    }
    CHECK(hashes[0] == hashes[1]);
}

TEST_CASE("module: two live copies of one module map distinct images") {
    LoadedModule ma;
    LoadedModule mb;
    {
        World a;
        World b;
        Result<LoadedModule> ra = load_game_module(MODTEST_GOOD, a);
        Result<LoadedModule> rb = load_game_module(MODTEST_GOOD, b);
        REQUIRE(ra.has_value());
        REQUIRE(rb.has_value());
        ma = ra.value();
        mb = rb.value();
        CHECK(ma.loaded_path.view() != mb.loaded_path.view());
        run_ticks(a, 5);
        run_ticks(b, 7);
        CHECK(total_hp(a) == static_cast<i32>(kSpawned) * (kStartHp - 5));
        CHECK(total_hp(b) == static_cast<i32>(kSpawned) * (kStartHp - 7));
    }
    unload_game_module(ma);
    unload_game_module(mb);
}

TEST_CASE("module: a read-only folder maps the module in place") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("mce_module_ro_" + std::to_string(fs::file_size(MODTEST_GOOD)));
    std::error_code ec;
    fs::remove_all(dir, ec);
    REQUIRE(fs::create_directories(dir));
    const fs::path source = dir / fs::path(MODTEST_GOOD).filename();
    REQUIRE(fs::copy_file(MODTEST_GOOD, source));
    fs::permissions(dir, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove);
    // Root (and Windows, where folder bits do not stop writes) can still write: nothing to prove there.
    std::FILE* probe = std::fopen((dir / "probe").string().c_str(), "wb");
    const bool writable = probe != nullptr;
    if (probe) std::fclose(probe);
    if (!writable) {
        LoadedModule mod;
        {
            World w;
            Result<LoadedModule> r = load_game_module(source.string().c_str(), w);
            REQUIRE(r.has_value());
            mod = r.value();
            CHECK(mod.loaded_path.view() == mod.source_path.view());
            run_ticks(w, 3);
            CHECK(total_hp(w) == static_cast<i32>(kSpawned) * (kStartHp - 3));
        }
        unload_game_module(mod);
        CHECK(fs::exists(source));  // the source is never deleted
    } else {
        MESSAGE("folder stays writable for this user (root?); in-place mapping not exercised");
    }
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add);
    fs::remove_all(dir, ec);
}

TEST_CASE("module: one module per world") {
    LoadedModule mod;
    {
        World w;
        Result<LoadedModule> r = load_game_module(MODTEST_GOOD, w);
        REQUIRE(r.has_value());
        mod = r.value();
        const u32 count = loaded_module_count();
        Result<LoadedModule> again = load_game_module(MODTEST_GOOD, w);
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == Errc::AlreadyExists);
        CHECK(loaded_module_count() == count);
    }
    unload_game_module(mod);
}

TEST_CASE("module: failures are diagnosable errors, never crashes") {
    const u32 before = loaded_module_count();

    SUBCASE("missing file") {
        CHECK(load_error(MODTEST_SCRATCH_DIR "/does_not_exist.so") == Errc::ModuleNotFound);
    }
    SUBCASE("not a shared library") {
        const char* junk = MODTEST_SCRATCH_DIR "/not_a_module.so";
        if (std::FILE* f = std::fopen(junk, "wb")) {
            std::fputs("this is not an ELF image", f);
            std::fclose(f);
        }
        World w;
        Result<LoadedModule> r = load_game_module(junk, w);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == Errc::ModuleNotFound);
        CHECK(StringView(r.error().stage) == "module.open");
        std::remove(junk);
    }
    SUBCASE("no entry point") {
        CHECK(load_error(MODTEST_NO_ENTRY) == Errc::ModuleMissingEntry);
    }
    SUBCASE("stale engine id") {
        CHECK(load_error(MODTEST_STALE) == Errc::EngineIdMismatch);
    }
    // Every failure above released its copy-and-load slot and deleted its copy.
    CHECK(loaded_module_count() == before);
}

TEST_CASE("module: install failure is reported and keeps the image mapped") {
    const u32 before = loaded_module_count();
    {
        World w;
        Result<LoadedModule> r = load_game_module(MODTEST_INSTALL_FAIL, w);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == Errc::ModuleInstallFailed);
        CHECK(StringView(r.error().stage) == "module.install");
    }
    // The half-installed World could have held module code pointers, so the image stays mapped.
    CHECK(loaded_module_count() == before + 1);
}
