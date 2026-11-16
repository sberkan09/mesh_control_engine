// M-FUZZ 3: Luau in the engine. Source text is what a modder or an agent writes (the compiler and the
// project checks read it); bytecode is what a pack carries (the host loads and runs it). Neither may crash
// the process: a script that does something wrong is marked Failed with its error, nothing else.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mce/log.h"
#include "mce/refl/macros.h"
#include "mce/script/compiler.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;
using script::ScriptHost;

namespace fuzzscript {

struct Mover {
    math::vec3 position;
    math::vec3 velocity;
    f32 bounce = 1.0f;
};
MCE_COMPONENT(Mover, position, velocity, bounce)

}  // namespace fuzzscript

using fuzzscript::Mover;

namespace {

u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 300u;
}

constexpr const char* kSource = R"(
function update()
    mce.each("Mover", function(e)
        local p = mce.get(e, "Mover", "position")
        local v = mce.get(e, "Mover", "velocity")
        p += v * mce.dt
        if p.x < 0 or p.x > 10 then
            v = vector.create(-v.x, v.y, v.z) * mce.get(e, "Mover", "bounce")
            mce.set(e, "Mover", "velocity", v)
        end
        mce.set(e, "Mover", "position", p)
    end)
    local names = { "a", "b", "c" }
    for i, n in ipairs(names) do
        if i == 2 then mce.log(n .. tostring(i)) end
    end
end
)";

}  // namespace

TEST_CASE("fuzz: Luau source survives mutation in the checks and the compiler") {
    log::set_level(log::Level::Off);
    World probe;
    probe.register_components<Mover>();
    const std::string good = kSource;
    const Span<const byte> bytes(reinterpret_cast<const byte*>(good.data()), good.size());
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x1A0A);
    u64 compiled = 0;
    const u32 n = iterations();
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: luau source iteration " << it);
        const usize len = m.mutate(bytes, storage);
        const StringView text(reinterpret_cast<const char*>(storage.data()), len);
        Array<script::CompileError> errors;
        script::check("fuzz", text, errors);
        auto bc = script::compile("fuzz", text);
        if (bc) ++compiled;
    }
    std::printf("fuzz: luau source: %llu of %u mutants compiled\n", static_cast<unsigned long long>(compiled), n);
    CHECK(compiled > 0);
}

// Luau's bytecode loader trusts its input (M-FUZZ 3 finding): a mutated chunk trips LUAU_ASSERT in debug
// builds (lvmload.cpp: constant index >= sizek) and reads out of bounds in release. The fix is a trust
// boundary, not a verifier (M-SCRIPT2 0): only the game's own pack may carry bytecode, a mod pack's Script
// entries are dropped at mount (tests/asset/test_mods.cpp). This case calls add_bytecode directly, which is
// what a trusted caller does, so it stays off by default; MCE_FUZZ_BYTECODE=1 runs it to reproduce.
TEST_CASE("fuzz: Luau bytecode survives mutation in the host" * doctest::skip(std::getenv("MCE_FUZZ_BYTECODE") == nullptr)) {
    log::set_level(log::Level::Off);
    auto good = script::compile("fuzz", kSource);
    REQUIRE(good);
    const Span<const byte> bytes(reinterpret_cast<const byte*>(good.value().data()), good.value().size());
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0xB17EC0DE);
    u64 loaded = 0, failed = 0;
    const u32 n = iterations();
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: luau bytecode iteration " << it);
        const usize len = m.mutate(bytes, storage);
        const Span<const u8> code(reinterpret_cast<const u8*>(storage.data()), len);
        World w;
        w.register_components<Mover>();
        Mover mv;
        mv.velocity = {1.f, 0.f, 0.f};
        w.spawn_with(mv);
        ScriptHost host;
        host.install(w);
        auto r = host.add_bytecode("fuzz", code);
        if (!r) continue;
        ++loaded;
        for (u64 t = 1; t <= 3; ++t) w.tick(1.0 / 60.0, t);
        for (const auto& info : host.scripts()) failed += info.state == script::ScriptState::Failed;
    }
    std::printf("fuzz: luau bytecode: %llu of %u mutants loaded, %llu of them failed while running\n",
                static_cast<unsigned long long>(loaded), n, static_cast<unsigned long long>(failed));
}
