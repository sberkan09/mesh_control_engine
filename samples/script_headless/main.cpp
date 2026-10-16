// Headless Luau scripting spike (ADR-0041) for the determinism harness (FR-RUN-040).
//
// N entities bounce inside a box; one system integrates and reflects them. `-mode lua` runs the
// system as a Luau script through mce_script, `-mode cpp` as the same arithmetic in C++. Luau's
// vectors are float32, so both modes do the same float operations in the same order and print
// the same state hash; the timing line shows what the interpreter costs.
//
//   sample_script_headless -ticks 1200 [-entities 2000] [-mode lua|cpp]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mce/refl/macros.h"
#include "mce/script/compiler.h"

using namespace mce;
using mce::math::vec3;

namespace shl {
struct Ball {
    vec3 position;
    vec3 velocity;
    f32 bounce = 1.0f;
};
MCE_COMPONENT(Ball, position, velocity, bounce)
}  // namespace shl
using shl::Ball;

namespace {

constexpr const char* kScript = R"(
local LO = vector.create(0, 0, 0)
local HI = vector.create(40, 20, 40)

function update()
    local dt = mce.dt
    mce.each("Ball", function(e)
        local p = mce.get(e, "Ball", "position")
        local v = mce.get(e, "Ball", "velocity")
        v = vector.create(v.x, v.y - 9.81 * dt, v.z)
        p = p + v * dt
        local k = mce.get(e, "Ball", "bounce")
        if p.x < LO.x or p.x > HI.x then v = vector.create(-v.x * k, v.y, v.z) end
        if p.y < LO.y then v = vector.create(v.x, -v.y * k, v.z); p = vector.create(p.x, LO.y, p.z) end
        if p.z < LO.z or p.z > HI.z then v = vector.create(v.x, v.y, -v.z * k) end
        mce.set(e, "Ball", "position", p)
        mce.set(e, "Ball", "velocity", v)
    end)
end
)";

// The script's arithmetic in C++, rounding for rounding: Luau numbers are doubles and its vectors
// float32, so `v.y - 9.81 * dt` runs in double and rounds once in vector.create, while `v * dt`
// is a float multiply.
void balls_cpp(Frame& f) {
    const f64 dt = f.dt;
    const f32 dtf = static_cast<f32>(dt);
    f.query<Ball&>().each([&](Ball& b) {
        vec3 v = {b.velocity.x, static_cast<f32>(static_cast<f64>(b.velocity.y) - 9.81 * dt), b.velocity.z};
        vec3 p = {b.position.x + v.x * dtf, b.position.y + v.y * dtf, b.position.z + v.z * dtf};
        const f32 k = b.bounce;
        if (p.x < 0.0f || p.x > 40.0f) v.x = -v.x * k;
        if (p.y < 0.0f) {
            v.y = -v.y * k;
            p.y = 0.0f;
        }
        if (p.z < 0.0f || p.z > 40.0f) v.z = -v.z * k;
        b.position = p;
        b.velocity = v;
    });
}

u32 arg_u32(int argc, char** argv, const char* name, u32 def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return static_cast<u32>(std::strtoul(argv[i + 1], nullptr, 10));
    return def;
}

const char* arg_str(int argc, char** argv, const char* name, const char* def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

}  // namespace

int main(int argc, char** argv) {
    const u32 ticks = arg_u32(argc, argv, "-ticks", 1200);
    const u32 count = arg_u32(argc, argv, "-entities", 2000);
    const bool lua = std::strcmp(arg_str(argc, argv, "-mode", "lua"), "cpp") != 0;

    World w;
    w.register_components<Ball>();
    u64 s = 0x9E3779B97F4A7C15ull;
    auto next = [&s] {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<f32>((s >> 33) % 100000) / 100000.0f;
    };
    for (u32 i = 0; i < count; ++i) {
        Ball b;
        b.position = {next() * 40.0f, 5.0f + next() * 15.0f, next() * 40.0f};
        b.velocity = {next() * 10.0f - 5.0f, 0.0f, next() * 10.0f - 5.0f};
        b.bounce = 0.6f + next() * 0.35f;
        w.spawn_with(b);
    }

    script::ScriptHost host;
    if (lua) {
        auto added = script::add_source(host, "balls", kScript);
        if (!added) {
            std::fprintf(stderr, "script_headless: %s\n", added.error().path.c_str());
            return 1;
        }
        host.install(w);
    } else {
        w.add_system(Phase::FixedUpdate_Post, "balls", &balls_cpp);
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (u32 t = 1; t <= ticks; ++t) w.tick(1.0 / 60, t);
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();

    if (lua && host.scripts()[0].state != script::ScriptState::Running) {
        std::fprintf(stderr, "script_headless: %s\n", host.scripts()[0].error.c_str());
        return 1;
    }
    std::printf("script_headless: %s, %u entities, %u ticks, %.4f ms/tick, vm %zu KiB, state hash %016llx\n",
                lua ? "lua" : "cpp", count, ticks, ms / ticks, host.memory_used() / 1024,
                static_cast<unsigned long long>(w.state_hash()));
    return 0;
}
