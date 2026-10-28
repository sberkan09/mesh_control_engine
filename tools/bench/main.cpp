// mce_bench — headless micro-benchmarks of the engine's hot paths (M-BENCH).
//
//   mce_bench [--quick] [--json] [--entities N] [--repeats N]
//
// ecs_iterate     query<Position&, const Velocity&>().each over N entities (NFR-T-01: 100k in 1.5 ms)
// ecs_spawn       spawn_with N entities
// ecs_destroy     destroy those N entities
// ecs_add_remove  add then remove one component on every entity (two archetype moves each)
// jobs_run_wait   N trivial jobs through jobs::run and one wait_for
//
// Each case runs `repeats` times on a fresh world and reports the best time, so a cold cache or a busy core costs
// one repeat, not the number. The numbers belong to the machine that ran them; the tool gates nothing. --quick
// shrinks the sizes for the CI smoke test, which checks only that every case ran and printed its fields.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/jobs.h"

using namespace mce;

namespace {

struct Position {
    f32 x, y, z;
};
struct Velocity {
    f32 x, y, z;
};
struct Tag {
    u32 value;
};

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

struct Result {
    const char* name = "";
    u64 items = 0;
    double best_ms = 0;
};

struct Options {
    u32 entities = 100000;
    u32 repeats = 7;
    bool json = false;
};

u64 g_sink = 0;  // keeps the optimizer from deleting the work

void spawn_all(World& w, u32 n, Array<Entity>* out) {
    for (u32 i = 0; i < n; ++i) {
        const Entity e = w.spawn_with(Position{f32(i), 0, 0}, Velocity{1, 0.5f, 0.25f});
        if (out != nullptr) out->push_back(e);
    }
}

Result bench_iterate(const Options& o) {
    Result r{"ecs_iterate", o.entities, 1e30};
    World w;
    spawn_all(w, o.entities, nullptr);
    for (u32 rep = 0; rep < o.repeats; ++rep) {
        const auto t0 = Clock::now();
        w.query<Position&, const Velocity&>().each([](Position& p, const Velocity& v) {
            p.x += v.x;
            p.y += v.y;
            p.z += v.z;
        });
        r.best_ms = std::min(r.best_ms, ms_since(t0));
    }
    w.query<const Position&>().each([](const Position& p) { g_sink += static_cast<u64>(p.x); });
    return r;
}

// Spawn and destroy share one run per repeat: destroy needs the spawned handles.
void bench_spawn_destroy(const Options& o, Result& spawn, Result& destroy) {
    spawn = Result{"ecs_spawn", o.entities, 1e30};
    destroy = Result{"ecs_destroy", o.entities, 1e30};
    for (u32 rep = 0; rep < o.repeats; ++rep) {
        World w;
        Array<Entity> handles;
        handles.reserve(o.entities);
        auto t0 = Clock::now();
        spawn_all(w, o.entities, &handles);
        spawn.best_ms = std::min(spawn.best_ms, ms_since(t0));
        t0 = Clock::now();
        for (const Entity e : handles) w.destroy(e);
        destroy.best_ms = std::min(destroy.best_ms, ms_since(t0));
        g_sink += w.entity_count();
    }
}

Result bench_add_remove(const Options& o) {
    Result r{"ecs_add_remove", o.entities, 1e30};
    for (u32 rep = 0; rep < o.repeats; ++rep) {
        World w;
        Array<Entity> handles;
        handles.reserve(o.entities);
        spawn_all(w, o.entities, &handles);
        const auto t0 = Clock::now();
        for (const Entity e : handles) w.add(e, Tag{1});
        for (const Entity e : handles) w.remove<Tag>(e);
        r.best_ms = std::min(r.best_ms, ms_since(t0));
        g_sink += w.query<const Position&>().count();
    }
    return r;
}

void count_job(void* ctx) { ++*static_cast<u32*>(ctx); }

Result bench_jobs(const Options& o) {
    Result r{"jobs_run_wait", o.entities, 1e30};
    jobs::init(0);
    for (u32 rep = 0; rep < o.repeats; ++rep) {
        u32 done = 0;
        jobs::Counter counter;
        const auto t0 = Clock::now();
        for (u32 i = 0; i < o.entities; ++i) jobs::run(&count_job, &done, &counter);
        jobs::wait_for(&counter);
        r.best_ms = std::min(r.best_ms, ms_since(t0));
        g_sink += done;
    }
    jobs::shutdown();
    return r;
}

bool parse_u32(const char* s, u32& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v == 0 || v > 100000000ul) return false;
    out = static_cast<u32>(v);
    return true;
}

void print(const Result* results, usize n, const Options& o) {
    if (o.json) {
        std::printf("{\"format\":\"mce-bench\",\"version\":1,\"entities\":%u,\"repeats\":%u,\"results\":[", o.entities, o.repeats);
        for (usize i = 0; i < n; ++i) {
            const Result& r = results[i];
            std::printf("%s{\"name\":\"%s\",\"items\":%llu,\"best_ms\":%.4f,\"ns_per_item\":%.2f}", i ? "," : "", r.name,
                        static_cast<unsigned long long>(r.items), r.best_ms, r.best_ms * 1e6 / double(r.items));
        }
        std::printf("],\"checksum\":%llu}\n", static_cast<unsigned long long>(g_sink));
        return;
    }
    std::printf("%-16s %10s %12s %12s\n", "case", "items", "best ms", "ns/item");
    for (usize i = 0; i < n; ++i) {
        const Result& r = results[i];
        std::printf("%-16s %10llu %12.4f %12.2f\n", r.name, static_cast<unsigned long long>(r.items), r.best_ms,
                    r.best_ms * 1e6 / double(r.items));
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--json") == 0) o.json = true;
        else if (std::strcmp(a, "--quick") == 0) {
            o.entities = 2000;
            o.repeats = 2;
        } else if (std::strcmp(a, "--entities") == 0 && i + 1 < argc && parse_u32(argv[i + 1], o.entities)) ++i;
        else if (std::strcmp(a, "--repeats") == 0 && i + 1 < argc && parse_u32(argv[i + 1], o.repeats)) ++i;
        else {
            std::fprintf(stderr, "usage: mce_bench [--quick] [--json] [--entities N] [--repeats N]\n");
            return 2;
        }
    }
    Result results[5];
    results[0] = bench_iterate(o);
    bench_spawn_destroy(o, results[1], results[2]);
    results[3] = bench_add_remove(o);
    results[4] = bench_jobs(o);
    print(results, 5, o);
    return 0;
}
