// mce_scene streaming (FR-ENT-120): SceneChunk entities load their chunk scene when a
// StreamingFocus comes near and destroy what they spawned when it leaves; worlds without chunks
// are untouched.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/origin.h"
#include "mce/ecs/transform.h"
#include "mce/scene/scene.h"
#include "mce/scene/streaming.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr const char* kBase = "5ce4e000-2222-4000-8000-000000000001";
constexpr const char* kChunkA = "5ce4e000-2222-4000-8000-00000000000a";
constexpr const char* kChunkB = "5ce4e000-2222-4000-8000-00000000000b";
constexpr const char* kChunkC = "5ce4e000-2222-4000-8000-00000000000c";
constexpr const char* kNoChunks = "5ce4e000-2222-4000-8000-000000000002";
constexpr const char* kOrigin = "5ce4e000-2222-4000-8000-000000000003";

void write_scene(const std::string& root, const char* file, const char* guid, const std::string& entities) {
    write_file(root + "/assets/" + file, std::string("{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"") + guid +
                                             "\", \"name\": \"" + file + "\", \"entities\": [" + entities + "] }");
}

std::string chunk_entity(const char* name, float x, const char* scene, const char* extra = "") {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  R"({ "name": "%s", "components": { "LocalTransform": { "position": [%g, 0, 0] },
                     "SceneChunk": { "scene": "guid:%s", "half_extents": [5, 5, 5], "load_distance": 2,
                                     "unload_distance": 4%s } } })",
                  name, static_cast<double>(x), scene, extra);
    return buf;
}

// Base scene: the focus (entity 0) and three chunks along +X: A around 0, B around 20, C (pinned)
// around 40. A spawns a parent and a child, B and C one entity each.
std::string make_streaming_project(const char* name) {
    const std::string root = fresh_dir(name);
    write_scene(root, "base.mcscene", kBase,
                std::string(R"({ "name": "Player", "components": { "LocalTransform": {}, "StreamingFocus": {} } },)") +
                    chunk_entity("ChunkA", 0, kChunkA) + "," + chunk_entity("ChunkB", 20, kChunkB) + "," +
                    chunk_entity("ChunkC", 40, kChunkC, R"(, "pinned": true)"));
    write_scene(root, "chunk_a.mcscene", kChunkA,
                R"({ "name": "Rock", "components": { "LocalTransform": { "position": [1, 0, 0] } } },
                   { "name": "Moss", "components": { "LocalTransform": { "position": [0, 1, 0] }, "ChildOf": { "parent": "$0" } } })");
    write_scene(root, "chunk_b.mcscene", kChunkB, R"({ "name": "Tree", "components": { "LocalTransform": { "position": [20, 0, 0] } } })");
    write_scene(root, "chunk_c.mcscene", kChunkC, R"({ "name": "Tower", "components": { "LocalTransform": { "position": [40, 0, 0] } } })");
    // The same world with a floating origin (threshold 16 m, 8 m cells).
    write_scene(root, "origin.mcscene", kOrigin,
                std::string(R"({ "name": "Player", "components": { "LocalTransform": {}, "StreamingFocus": {} } },)") +
                    chunk_entity("ChunkA", 0, kChunkA) + "," + chunk_entity("ChunkB", 20, kChunkB) + "," +
                    R"({ "name": "Origin", "components": { "FloatingOrigin3D": { "threshold": 16, "snap": 8 } } })");
    write_scene(root, "plain.mcscene", kNoChunks, R"({ "name": "Lonely", "components": { "LocalTransform": {} } })");
    return root;
}

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

struct Fixture {
    std::string root;
    asset::AssetSystem assets;
    World world;
    Array<Entity> base;  // [0] focus, [1] A, [2] B, [3] C

    explicit Fixture(const char* name, bool async, const char* scene = kBase) : root(make_streaming_project(name)) {
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        INFO(diag_text(r));
        REQUIRE(r.ok());
        const std::string cooked = root + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        scene::StreamingConfig cfg;
        cfg.async = async;
        scene::install_streaming(world, assets, cfg);
        REQUIRE(scene::load(world, assets, asset::parse_guid(scene).value(), &base));
    }

    void move_focus(f32 x) { world.try_get_mut<LocalTransform>(base[0])->position.x = x; }
    scene::ChunkState state(Entity chunk) const {
        const scene::SceneChunkStatus* s = world.try_get<scene::SceneChunkStatus>(chunk);
        return s ? s->state : scene::ChunkState::Unloaded;
    }
    scene::SceneStreaming& streaming() { return world.resource<scene::SceneStreaming>(); }
};

}  // namespace

TEST_CASE("streaming: chunks load near the focus and unload past the hysteresis band") {
    Fixture f("stream_sync", false);
    REQUIRE(f.base.size() == 4);
    const Entity A = f.base[1], B = f.base[2], C = f.base[3];
    CHECK(f.world.entity_count() == 4);  // nothing streams before the first pump

    scene::pump_streaming(f.world, scene::PumpMode::Settle);
    CHECK(f.state(A) == scene::ChunkState::Loaded);    // the focus is inside A's box
    CHECK(f.state(B) == scene::ChunkState::Unloaded);  // 15 m from B's box
    CHECK(f.state(C) == scene::ChunkState::Loaded);    // pinned
    CHECK(f.world.entity_count() == 4 + 2 + 1);
    REQUIRE(f.streaming().entities(A).size() == 2);
    const Entity rock = f.streaming().entities(A)[0], moss = f.streaming().entities(A)[1];
    REQUIRE(f.world.try_get<ChildOf>(moss));
    CHECK(f.world.try_get<ChildOf>(moss)->parent == rock);  // entity references inside a chunk are stamped
    CHECK(f.world.try_get<scene::SceneChunkStatus>(A)->entity_count == 2);
    CHECK(f.streaming().stats().loaded == 2);

    // Walk into B: B loads, A (12 m away, past its 4 m unload distance) goes with its entities.
    f.move_focus(17.0f);
    scene::pump_streaming(f.world);
    CHECK(f.state(A) == scene::ChunkState::Unloaded);
    CHECK(f.state(B) == scene::ChunkState::Loaded);
    CHECK_FALSE(f.world.alive(rock));
    CHECK_FALSE(f.world.alive(moss));
    CHECK(f.streaming().entities(A).empty());
    CHECK(f.world.entity_count() == 4 + 1 + 1);

    // 3.5 m from B's box: inside the unload distance, so B stays; outside A's load distance.
    f.move_focus(11.5f);
    scene::pump_streaming(f.world);
    CHECK(f.state(B) == scene::ChunkState::Loaded);
    CHECK(f.state(A) == scene::ChunkState::Unloaded);
    // 5 m: past B's unload distance.
    f.move_focus(10.0f);
    scene::pump_streaming(f.world);
    CHECK(f.state(B) == scene::ChunkState::Unloaded);
    CHECK(f.world.entity_count() == 4 + 1);
    CHECK(f.streaming().stats().total_loads == 3);
    CHECK(f.streaming().stats().total_unloads == 2);

    // disabled wins over pinned.
    f.world.try_get_mut<scene::SceneChunk>(C)->disabled = true;
    scene::pump_streaming(f.world);
    CHECK(f.state(C) == scene::ChunkState::Unloaded);
    CHECK(f.world.entity_count() == 4);
}

TEST_CASE("streaming: budgeted pumps instantiate one chunk at a time; removed chunks take their entities") {
    Fixture f("stream_budget", false);
    const Entity A = f.base[1], B = f.base[2];
    f.world.try_get_mut<scene::SceneChunk>(B)->load_distance = 100.0f;  // A and B both wanted
    f.world.try_get_mut<scene::SceneChunk>(B)->unload_distance = 100.0f;
    scene::pump_streaming(f.world);
    CHECK(f.streaming().stats().loaded == 1);  // budget 1 per pump
    scene::pump_streaming(f.world);
    scene::pump_streaming(f.world);
    CHECK(f.state(A) == scene::ChunkState::Loaded);
    CHECK(f.state(B) == scene::ChunkState::Loaded);
    CHECK(f.streaming().stats().loaded == 3);

    // Destroying the chunk entity unloads what it spawned on the next pump.
    const Entity tree = f.streaming().entities(B)[0];
    f.world.destroy(B);
    scene::pump_streaming(f.world);
    CHECK_FALSE(f.world.alive(tree));
    CHECK(f.streaming().stats().chunks == 2);

    // Pointing a loaded chunk at another scene swaps its contents.
    f.world.try_get_mut<scene::SceneChunk>(A)->scene.guid = asset::parse_guid(kChunkC).value();
    scene::pump_streaming(f.world);
    REQUIRE(f.streaming().entities(A).size() == 1);
    CHECK(f.world.try_get<LocalTransform>(f.streaming().entities(A)[0])->position.x == 40.0f);
}

TEST_CASE("streaming: a chunk whose scene is missing fails without touching the world") {
    Fixture f("stream_missing", false);
    const Entity A = f.base[1];
    f.world.try_get_mut<scene::SceneChunk>(A)->scene.guid = asset::parse_guid("5ce4e000-2222-4000-8000-0000000000ff").value();
    scene::pump_streaming(f.world, scene::PumpMode::Settle);
    CHECK(f.state(A) == scene::ChunkState::Failed);
    CHECK(f.streaming().stats().failed == 1);
    CHECK(f.world.entity_count() == 4 + 1);  // C still loads
}

TEST_CASE("streaming: async loads read off-thread and land on a later pump") {
    Fixture f("stream_async", true);
    const Entity A = f.base[1], C = f.base[3];
    // The pump polls right after requesting, so a fast read may already land in the same pump;
    // the budget still holds (one instantiation per pump).
    scene::pump_streaming(f.world);
    CHECK(f.state(A) != scene::ChunkState::Unloaded);
    CHECK(f.state(C) != scene::ChunkState::Unloaded);
    CHECK(f.streaming().stats().loaded <= 1);
    f.assets.wait_async();
    scene::pump_streaming(f.world);
    CHECK(f.state(A) == scene::ChunkState::Loaded);  // first requested, first in
    CHECK(f.streaming().stats().loaded <= 2);
    scene::pump_streaming(f.world);
    CHECK(f.state(C) == scene::ChunkState::Loaded);
    CHECK(f.world.entity_count() == 4 + 2 + 1);

    // Leaving while a load may still be in flight cancels it; nothing arrives later.
    f.move_focus(17.0f);
    scene::pump_streaming(f.world);
    CHECK(f.state(A) == scene::ChunkState::Unloaded);
    CHECK(f.state(f.base[2]) != scene::ChunkState::Unloaded);
    f.move_focus(-100.0f);
    scene::pump_streaming(f.world);
    CHECK(f.state(f.base[2]) == scene::ChunkState::Unloaded);
    f.assets.wait_async();
    scene::pump_streaming(f.world);
    CHECK(f.world.entity_count() == 4 + 1);  // only pinned C

    // Settle loads what is wanted now, whatever the async setting.
    f.move_focus(0.0f);
    scene::pump_streaming(f.world, scene::PumpMode::Settle);
    CHECK(f.state(A) == scene::ChunkState::Loaded);
}

TEST_CASE("streaming: synchronous streaming is deterministic") {
    u64 hashes[2] = {};
    for (u64& h : hashes) {
        Fixture f("stream_hash", false);
        scene::pump_streaming(f.world, scene::PumpMode::Settle);
        for (f32 x : {5.0f, 12.0f, 17.0f, 30.0f, 3.0f}) {
            f.move_focus(x);
            scene::pump_streaming(f.world);
        }
        h = f.world.state_hash();
    }
    CHECK(hashes[0] == hashes[1]);
}

TEST_CASE("streaming: a world without chunks is untouched") {
    Fixture f("stream_none", true, kNoChunks);
    const u64 before = f.world.state_hash();
    scene::pump_streaming(f.world, scene::PumpMode::Settle);
    scene::pump_streaming(f.world);
    CHECK(f.world.state_hash() == before);
    CHECK(f.streaming().stats().chunks == 0);

    World bare;  // never installed: the pump is a no-op
    scene::pump_streaming(bare);
    CHECK(bare.entity_count() == 0);
}

TEST_CASE("streaming: distance to a box") {
    const math::vec3 c{0, 0, 0}, half{1, 2, 3};
    CHECK(scene::distance_to_box({0.5f, 1, 2}, c, half) == 0.0f);
    CHECK(scene::distance_to_box({4, 0, 0}, c, half) == doctest::Approx(3.0f));
    CHECK(scene::distance_to_box({4, 6, 0}, c, half) == doctest::Approx(5.0f));
}

namespace {

struct Score {
    i32 value = 0;
};
i32 g_startups = 0;

}  // namespace

TEST_CASE("scene change: request_change swaps every entity for the target scene and reruns Startup") {
    Fixture f("scene_change", false);
    g_startups = 0;
    f.world.add_system(Phase::Startup, "count_startup", [](Frame&) { ++g_startups; });
    f.world.set_resource(Score{7});
    scene::pump_streaming(f.world, scene::PumpMode::Settle);
    const Entity spare = f.world.spawn();  // spawned at run time, not by the scene
    f.world.tick(1.0 / 60.0, 0);
    CHECK(g_startups == 1);
    REQUIRE(f.world.resource<scene::SceneChange>().current == StringView("base.mcscene"));

    // Nothing pending: nothing happens.
    CHECK(scene::apply_change(f.world, f.assets, &f.base).value() == false);

    // An unknown target is an error and leaves the world as it was.
    const u32 before = f.world.entity_count();
    scene::request_change(f.world, "scenes/nowhere.mcscene");
    CHECK_FALSE(scene::apply_change(f.world, f.assets, &f.base));
    CHECK(f.world.entity_count() == before);
    CHECK(f.world.resource<scene::SceneChange>().next.empty());

    // By source path (a leading assets/ is fine): the streamed chunks, the run-time entity and the
    // base scene all go; the resource stays.
    scene::request_change(f.world, "assets/plain.mcscene");
    REQUIRE(scene::apply_change(f.world, f.assets, &f.base).value());
    CHECK(f.world.entity_count() == 1);
    REQUIRE(f.base.size() == 1);
    CHECK_FALSE(f.world.alive(spare));
    CHECK(f.world.resource<Score>().value == 7);
    const scene::SceneChange& c = f.world.resource<scene::SceneChange>();
    CHECK(c.current == StringView("plain.mcscene"));
    CHECK(c.current_guid == asset::parse_guid(kNoChunks).value());
    CHECK(c.changes == 1);
    f.world.tick(1.0 / 60.0, 1);
    CHECK(g_startups == 2);

    // By GUID text, back to the base scene (its chunks stream in again).
    scene::request_change(f.world, std::string("guid:").append(kBase));
    REQUIRE(scene::apply_change(f.world, f.assets, &f.base).value());
    scene::pump_streaming(f.world, scene::PumpMode::Settle);
    CHECK(f.base.size() == 4);
    CHECK(f.world.entity_count() == 4 + 2 + 1);  // A (parent and child) and pinned C, as at the start
    CHECK(scene::find_scene(f.assets, "chunk_b.mcscene").value() == asset::parse_guid(kChunkB).value());
}

TEST_CASE("scene change: scenes that name each other in a loop cook (scene references are links)") {
    const std::string root = fresh_dir("scene_loop");
    constexpr const char* kTitle = "5ce4e000-3333-4000-8000-000000000001";
    constexpr const char* kLevel = "5ce4e000-3333-4000-8000-000000000002";
    const auto link = [](const char* name, const char* to) {
        return std::string(R"({ "name": ")") + name + R"(", "components": { "LocalTransform": { "position": [1000, 0, 0] },
                     "SceneChunk": { "scene": "guid:)" + to + R"(", "half_extents": [1, 1, 1], "load_distance": 0, "unload_distance": 0 } } })";
    };
    write_scene(root, "title.mcscene", kTitle, link("ToLevel", kLevel));
    write_scene(root, "level.mcscene", kLevel, link("ToTitle", kTitle));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diag_text(r));
    CHECK(r.ok());
    // A link to a scene that is not there is still an error.
    write_scene(root, "level.mcscene", kLevel, link("ToNowhere", "5ce4e000-3333-4000-8000-0000000000ff"));
    CHECK_FALSE(svc.cook(StringView(root.c_str()), cook::CookOptions{}).ok());
}

TEST_CASE("streaming: a floating origin rebases around the focus and chunks land relative to it") {
    Fixture f("stream_origin", false, kOrigin);
    REQUIRE(f.base.size() == 4);
    const Entity A = f.base[1], B = f.base[2];
    scene::pump_streaming(f.world, scene::PumpMode::Settle);
    CHECK(world_origin(f.world) == nullptr);  // the focus is at the origin: nothing to do yet
    CHECK(f.state(A) == scene::ChunkState::Loaded);
    const Entity rock = f.streaming().entities(A)[0];

    // 15 m out: inside the 16 m threshold, no rebase.
    f.move_focus(15.0f);
    scene::pump_streaming(f.world);
    CHECK(world_origin(f.world) == nullptr);

    // 17 m: the world moves by round(17 / 8) = 2 cells = 16 m, before the chunks are judged.
    f.move_focus(17.0f);
    scene::pump_streaming(f.world);
    const WorldOrigin* o = world_origin(f.world);
    REQUIRE(o);
    CHECK(o->cell[0] == 2);
    CHECK(o->serial == 1);
    CHECK(f.world.try_get<LocalTransform>(f.base[0])->position.x == 1.0f);
    CHECK(f.world.try_get<LocalTransform>(B)->position.x == 4.0f);
    CHECK(f.state(B) == scene::ChunkState::Loaded);
    REQUIRE(f.streaming().entities(B).size() == 1);
    // The tree is authored at x = 20 (absolute); it lands at 20 - 16.
    CHECK(f.world.try_get<LocalTransform>(f.streaming().entities(B)[0])->position.x == 4.0f);
    CHECK(to_absolute(f.world, f.world.try_get<LocalTransform>(f.streaming().entities(B)[0])->position).x == 20.0);
    CHECK_FALSE(f.world.alive(rock));  // A is 12 m behind: unloaded as before

    // Back to A: it reloads in place (rock at absolute 1), and no further rebase happens near the origin.
    f.move_focus(-15.0f);  // absolute 1
    scene::pump_streaming(f.world);
    CHECK(o->serial == 1);
    CHECK(f.state(A) == scene::ChunkState::Loaded);
    CHECK(f.world.try_get<LocalTransform>(f.streaming().entities(A)[0])->position.x == -15.0f);
}

TEST_CASE("streaming: floating origin runs are deterministic") {
    u64 hashes[2] = {};
    for (u64& h : hashes) {
        Fixture f("stream_origin_det", false, kOrigin);
        scene::pump_streaming(f.world, scene::PumpMode::Settle);
        for (int i = 0; i < 60; ++i) {
            f.move_focus(f.world.try_get<LocalTransform>(f.base[0])->position.x + 0.75f);
            scene::pump_streaming(f.world);
        }
        h = f.world.state_hash();
        CHECK(world_origin(f.world)->serial >= 2);
    }
    CHECK(hashes[0] == hashes[1]);
}
