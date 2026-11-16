// mce_bench — headless micro-benchmarks of the engine's hot paths (M-BENCH).
//
//   mce_bench [--quick] [--json] [--entities N] [--repeats N]
//
// ecs_iterate     query<Position&, const Velocity&>().each over N entities (NFR-T-01: 100k in 1.5 ms)
// ecs_spawn       spawn_with N entities
// ecs_destroy     destroy those N entities
// ecs_add_remove  add then remove one component on every entity (two archetype moves each)
// jobs_run_wait   N trivial jobs through jobs::run and one wait_for
// container_write N/100 chunks of 4 KiB through ContainerWriter (TOC, alignment, both CRCs)
// container_open  ContainerView::open on that file (header and payload CRC verification)
// container_find  one find(type, guid) per chunk on the opened file
//
// Each case runs `repeats` times on a fresh world and reports the best time, so a cold cache or a busy core costs
// one repeat, not the number. The numbers belong to the machine that ran them; the tool gates nothing. --quick
// shrinks the sizes for the CI smoke test, which checks only that every case ran and printed its fields.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mce/asset/container.h"
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

struct Case {
    const char* name = "";
    u64 items = 0;
    double best_ms = 0;
    u64 bytes = 0;  // payload size of the cases that move bytes (0: not reported)
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

Case bench_iterate(const Options& o) {
    Case r{"ecs_iterate", o.entities, 1e30};
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
void bench_spawn_destroy(const Options& o, Case& spawn, Case& destroy) {
    spawn = Case{"ecs_spawn", o.entities, 1e30};
    destroy = Case{"ecs_destroy", o.entities, 1e30};
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

Case bench_add_remove(const Options& o) {
    Case r{"ecs_add_remove", o.entities, 1e30};
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

Case bench_jobs(const Options& o) {
    Case r{"jobs_run_wait", o.entities, 1e30};
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

// ---- assets: a container of N/100 chunks of 4 KiB, each with its own GUID ----------------------------------------------

constexpr usize kChunkBytes = 4096;

u32 chunk_count_for(const Options& o) { return std::max<u32>(o.entities / 100, 16); }

void build_container(u32 chunks, Array<byte>& out) {
    Array<byte> payload;
    payload.resize(kChunkBytes);
    for (usize i = 0; i < payload.size(); ++i) payload[i] = static_cast<byte>(i * 31u);
    asset::ContainerWriter w;
    for (u32 i = 0; i < chunks; ++i) {
        asset::ContainerWriter::ChunkDesc d;
        d.type = asset::ChunkType::Texture;
        d.guid = asset::Guid{0x1000u + i, 0x2000u + i};
        payload[0] = static_cast<byte>(i);  // distinct payloads, same size
        w.add(d, payload.span());
    }
    w.finish(out);
}

void bench_container(const Options& o, Case& write, Case& open, Case& find) {
    const u32 chunks = chunk_count_for(o);
    const u64 total = u64(chunks) * kChunkBytes;
    write = Case{"container_write", chunks, 1e30, total};
    open = Case{"container_open", chunks, 1e30, total};
    find = Case{"container_find", chunks, 1e30, 0};
    Array<byte> file;
    for (u32 rep = 0; rep < o.repeats; ++rep) {
        file.clear();
        auto t0 = Clock::now();
        build_container(chunks, file);
        write.best_ms = std::min(write.best_ms, ms_since(t0));
        t0 = Clock::now();
        auto view = asset::ContainerView::open(file.span(), "bench.mce");
        open.best_ms = std::min(open.best_ms, ms_since(t0));
        if (!view) {
            std::fprintf(stderr, "mce_bench: container_open failed\n");
            std::exit(1);
        }
        t0 = Clock::now();
        for (u32 i = 0; i < chunks; ++i) g_sink += view.value().find(asset::ChunkType::Texture, asset::Guid{0x1000u + i, 0x2000u + i});
        find.best_ms = std::min(find.best_ms, ms_since(t0));
    }
}

bool parse_u32(const char* s, u32& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v == 0 || v > 100000000ul) return false;
    out = static_cast<u32>(v);
    return true;
}

void print(const Case* results, usize n, const Options& o) {
    if (o.json) {
        std::printf("{\"format\":\"mce-bench\",\"version\":1,\"entities\":%u,\"repeats\":%u,\"results\":[", o.entities, o.repeats);
        for (usize i = 0; i < n; ++i) {
            const Case& r = results[i];
            std::printf("%s{\"name\":\"%s\",\"items\":%llu,\"best_ms\":%.4f,\"ns_per_item\":%.2f", i ? "," : "", r.name,
                        static_cast<unsigned long long>(r.items), r.best_ms, r.best_ms * 1e6 / double(r.items));
            if (r.bytes > 0) std::printf(",\"mb_per_s\":%.1f", double(r.bytes) / 1048576.0 / (r.best_ms / 1000.0));
            std::printf("}");
        }
        std::printf("],\"checksum\":%llu}\n", static_cast<unsigned long long>(g_sink));
        return;
    }
    std::printf("%-16s %10s %12s %12s %10s\n", "case", "items", "best ms", "ns/item", "MB/s");
    for (usize i = 0; i < n; ++i) {
        const Case& r = results[i];
        std::printf("%-16s %10llu %12.4f %12.2f", r.name, static_cast<unsigned long long>(r.items), r.best_ms,
                    r.best_ms * 1e6 / double(r.items));
        if (r.bytes > 0) std::printf(" %10.1f", double(r.bytes) / 1048576.0 / (r.best_ms / 1000.0));
        std::printf("\n");
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
    Case results[8];
    results[0] = bench_iterate(o);
    bench_spawn_destroy(o, results[1], results[2]);
    results[3] = bench_add_remove(o);
    results[4] = bench_jobs(o);
    bench_container(o, results[5], results[6], results[7]);
    print(results, 8, o);
    return 0;
}
