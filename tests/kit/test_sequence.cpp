// FR-ENT-100: action-list sequences — tick-math timing of every step kind, signals between
// players, the call table, loops and their validation, the .mcseq cook (with a prefab spawn
// from a cooked scene) and save mid-sequence -> resume -> the same completion tick and state.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/sequence.h"
#include "mce/physics/module.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace cook_test;
using kit::SequencePlayer;
using kit::SequenceRegistry;
using R = kit::SequenceRegistry;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Sim {
    World world;
    u64 ticks = 0;
    Sim() {
        world.register_components<LocalTransform, SaveKey>();
        kit::install_sequences(world, nullptr);
    }
    SequenceRegistry& reg() { return world.resource<SequenceRegistry>(); }
    Entity player(const char* name, math::vec3 at = {}, u32 save_key = 0) {
        SequencePlayer p;
        p.name.assign(name);
        const Entity e = world.spawn_with(LocalTransform{at, math::quat::identity(), {1, 1, 1}}, p);
        if (save_key) world.add(e, SaveKey{save_key});
        return e;
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
    // Ticks until `e`'s player is inactive (or gone); returns how many ran.
    u32 run_until_done(Entity e, u32 limit = 10000) {
        for (u32 i = 1; i <= limit; ++i) {
            run(1);
            const SequencePlayer* p = world.try_get<SequencePlayer>(e);
            if (!p || !p->active) return i;
        }
        return 0;
    }
    const SequencePlayer& state(Entity e) { return *world.try_get<SequencePlayer>(e); }
    math::vec3 pos(Entity e) { return world.try_get<LocalTransform>(e)->position; }
};

int g_calls = 0;
Entity g_called_on;
void count_call(Frame&, Entity e) {
    ++g_calls;
    g_called_on = e;
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

}  // namespace

TEST_CASE("sequence: waits complete on the tick their seconds add up to") {
    Sim s;
    const asset::SequenceStep steps[] = {R::wait(0.5f), R::emit("half"), R::wait(0.25f)};
    REQUIRE(s.reg().define("intro", steps));
    const Entity e = s.player("intro");
    CHECK(s.run_until_done(e) == 45);  // 30 + 15 ticks: a wait's tick is its own, never shared
    CHECK(s.state(e).step == 3);       // parked on End

    // Instant steps run in the same tick as the wait that precedes them.
    Sim t;
    const asset::SequenceStep instant[] = {R::emit("a"), R::emit("b"), R::wait(1.0f / 60.0f), R::emit("c")};
    REQUIRE(t.reg().define("burst", instant));
    CHECK(t.run_until_done(t.player("burst")) == 1);
}

TEST_CASE("sequence: move_to walks at its speed, relative targets start from the step's position") {
    Sim s;
    const asset::SequenceStep steps[] = {R::move_to({4, 0, 0}, 2.0f, /*relative=*/true), R::move_to({0, 0, 0}, 4.0f)};
    REQUIRE(s.reg().define("patrol", steps));
    const Entity e = s.player("patrol", {1, 1, 0});
    s.run(60);  // 1 s at 2 m/s: half way
    CHECK(s.pos(e).x == doctest::Approx(3.0f).epsilon(1e-4));
    CHECK(s.state(e).anchor.x == doctest::Approx(1.0f));
    s.run(60);  // arrives at (5, 1) on tick 120; the second move starts on tick 121
    CHECK(s.pos(e).x == doctest::Approx(5.0f));
    CHECK(s.state(e).step == 1);
    const u32 rest = s.run_until_done(e);
    CHECK(s.pos(e).x == doctest::Approx(0.0f));
    CHECK(s.pos(e).y == doctest::Approx(0.0f));
    CHECK(rest >= 76);  // |(5,1)| = 5.10 m at 4 m/s = 1.27 s
    CHECK(rest <= 78);
}

TEST_CASE("sequence: move_to drives a kinematic body through physics velocity") {
    Sim s;
    physics::install_physics2d(s.world);
    const asset::SequenceStep steps[] = {R::move_to({3, 0, 0}, 3.0f, true)};
    REQUIRE(s.reg().define("slide", steps));
    const Entity e = s.player("slide", {0, 2, 0});
    physics::RigidBody2D rb;
    rb.type = physics::BodyType::Kinematic;
    physics::Collider2D col;
    col.shape = physics::ColliderShape::box({0.5f, 0.5f});
    s.world.add(e, rb);
    s.world.add(e, col);
    const u32 ticks = s.run_until_done(e);
    CHECK(ticks >= 60);  // 1 s at 3 m/s, plus the step the body needs to see each velocity
    CHECK(ticks <= 63);
    CHECK(s.pos(e).x == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(s.pos(e).y == doctest::Approx(2.0f));
    s.run(10);  // stopped: the arrival tick zeroes the velocity
    CHECK(s.pos(e).x == doctest::Approx(3.0f).epsilon(0.01));
}

TEST_CASE("sequence: emit wakes wait_event on the next tick; anim requests reach the game") {
    Sim s;
    const asset::SequenceStep lead[] = {R::wait(0.1f), R::emit("go")};
    const asset::SequenceStep follow[] = {R::wait_event("go"), R::anim("jump")};
    REQUIRE(s.reg().define("lead", lead));
    REQUIRE(s.reg().define("follow", follow));
    const Entity a = s.player("lead");
    const Entity b = s.player("follow");
    CHECK(s.run_until_done(a) == 6);
    CHECK(s.state(b).active);  // the signal is readable from the next tick
    s.world.add_system(Phase::FixedUpdate_Post, "read_anims", [](Frame& f) {
        for (const kit::SeqAnimRequest& r : f.events().read<kit::SeqAnimRequest>())
            if (r.clip == StringId("jump")) f.world().resource<u32>()++;
    });
    s.world.set_resource(u32(0));
    CHECK(s.run_until_done(b) == 1);
    s.run(1);
    CHECK(s.world.resource<u32>() == 1);
}

TEST_CASE("sequence: calls go through the registered table; a missing one stops the player") {
    Sim s;
    g_calls = 0;
    const asset::SequenceStep steps[] = {R::call_step("count"), R::wait(0.1f), R::call_step("count")};
    REQUIRE(s.reg().define("calls", steps));
    CHECK_FALSE(s.reg().validate_calls());
    s.reg().register_call("count", &count_call);
    CHECK(s.reg().validate_calls());
    const Entity e = s.player("calls");
    CHECK(s.run_until_done(e) == 6);
    CHECK(g_calls == 2);
    CHECK(g_called_on == e);

    const asset::SequenceStep missing[] = {R::call_step("nope"), R::wait(1.0f)};
    REQUIRE(s.reg().define("missing", missing));
    const Entity m = s.player("missing");
    CHECK(s.run_until_done(m) == 1);
    CHECK(s.state(m).step == 0);  // stopped on the bad step, not past it
}

TEST_CASE("sequence: loops repeat; a loop that never waits is rejected; destroy removes the entity") {
    Sim s;
    g_calls = 0;
    s.reg().register_call("count", &count_call);
    const asset::SequenceStep loop[] = {R::call_step("count"), R::wait(0.1f)};
    REQUIRE(s.reg().define("loop", loop, /*loops=*/true));
    const Entity e = s.player("loop");
    s.run(60);
    CHECK(g_calls == 11);  // ticks 1, 6, 12, ... 60: the first wait counts its starting tick
    CHECK(s.state(e).active);

    const asset::SequenceStep spin[] = {R::call_step("count"), R::wait(0.0f)};
    CHECK_FALSE(s.reg().define("spin", spin, true));
    asset::SequenceStep bad[3] = {R::wait(1.0f), R::wait(1.0f), R::wait(1.0f)};
    bad[1].kind = asset::SequenceStepKind::End;
    CHECK_FALSE(s.reg().define("early_end", Span<const asset::SequenceStep>(bad, 3)));

    const asset::SequenceStep die[] = {R::wait(0.05f), R::destroy()};
    REQUIRE(s.reg().define("die", die));
    const Entity d = s.player("die");
    CHECK(s.run_until_done(d) == 3);
    CHECK_FALSE(s.world.alive(d));
}

TEST_CASE("sequence: save mid-step, load into a fresh world, finish on the same tick in the same state") {
    const asset::SequenceStep steps[] = {R::move_to({3, 0, 0}, 1.5f, true), R::wait(0.5f), R::move_to({0, 2, 0}, 2.0f, true)};
    auto setup = [&](Sim& s) {
        REQUIRE(s.reg().define("route", steps));
        return s.player("route", {1, 0, 0}, /*save_key=*/7);
    };
    Sim ref;
    const Entity r = setup(ref);
    const u32 total = ref.run_until_done(r);
    REQUIRE(total > 0);
    const math::vec3 final_pos = ref.pos(r);

    for (u32 cut : {30u, 120u, 125u, total - 2}) {
        Sim a;
        const Entity e = setup(a);
        a.run(cut);
        Array<byte> bytes;
        save::write_save(a.world, save::SaveInfo{}, bytes);

        Sim b;
        const Entity f = setup(b);  // the base world: the scene as authored
        b.ticks = a.ticks;
        REQUIRE(save::read_save(b.world, bytes.span()));
        CHECK(b.state(f).step == a.state(e).step);
        const u32 rest = b.run_until_done(f);
        CHECK(cut + rest == total);
        CHECK(b.pos(f).x == doctest::Approx(final_pos.x));
        CHECK(b.pos(f).y == doctest::Approx(final_pos.y));
    }
}

TEST_CASE("sequence: .mcseq cooks to CHNK_SEQ, plays from a scene and spawns a prefab") {
    const std::string root = fresh_dir("sequence_cook");
    write_file(root + "/assets/spark.mcprefab", R"({ "format": "mce-prefab", "version": 1,
        "guid": "5e900000-0000-4000-8000-000000000002", "name": "spark", "entities": [
        { "name": "Spark", "components": { "LocalTransform": { "position": [0, 0, 0] } } } ] })");
    write_file(root + "/assets/intro.mcseq", R"({ "format": "mce-sequence", "version": 1,
        "guid": "5e900000-0000-4000-8000-000000000001", "name": "intro", "steps": [
        { "wait": 0.25 },
        { "move_to": [2, 0], "speed": 4, "relative": true },
        { "spawn": "guid:5e900000-0000-4000-8000-000000000002", "at": [0, 1], "relative": true },
        { "emit": "intro_done" } ] })");
    write_file(root + "/assets/level.mcscene", R"({ "format": "mce-scene", "version": 1,
        "guid": "5e900000-0000-4000-8000-000000000003", "name": "level", "entities": [
        { "name": "Hero", "components": {
            "LocalTransform": { "position": [1, 0, 0] },
            "SequencePlayer": { "sequence": "guid:5e900000-0000-4000-8000-000000000001" } } } ] })");
    World w;
    cook::CookService svc;
    const cook::CookReport rep = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diag_text(rep));
    REQUIRE(rep.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid("5e900000-0000-4000-8000-000000000001").value());
    REQUIRE(key != asset::kInvalidAssetKey);
    auto h = assets.load_sync<asset::Sequence>(key);
    REQUIRE(h);
    auto view = asset::SequenceView::read(assets.resolve(h.value()));
    REQUIRE(view);
    CHECK(view.value().name() == StringView("intro"));
    CHECK_FALSE(view.value().loops());
    REQUIRE(view.value().steps.size() == 5);
    CHECK(view.value().steps[1].kind == asset::SequenceStepKind::MoveTo);
    CHECK((view.value().steps[1].flags & asset::SequenceStepFlags::Relative) != 0);
    CHECK(view.value().steps[2].asset() == asset::parse_guid("5e900000-0000-4000-8000-000000000002").value());
    CHECK(view.value().steps[4].kind == asset::SequenceStepKind::End);
    assets.release(asset::AnyHandle(h.value()));

    kit::install_sequences(w, &assets);
    Array<Entity> spawned;
    REQUIRE(scene::load(w, assets, asset::parse_guid("5e900000-0000-4000-8000-000000000003").value(), &spawned));
    REQUIRE(spawned.size() == 1);
    const Entity hero = spawned[0];
    u64 tick = 0;
    for (; tick < 600 && w.try_get<SequencePlayer>(hero)->active; ++tick) w.tick(kDt, tick);
    CHECK(tick == 15 + 30);  // 0.25 s, then 2 m at 4 m/s
    CHECK(w.try_get<LocalTransform>(hero)->position.x == doctest::Approx(3.0f));
    u32 entities = 0;
    math::vec3 spark;
    w.query<const LocalTransform>().each([&](Entity e, const LocalTransform& t) {
        ++entities;
        if (e != hero) spark = t.position;
    });
    CHECK(entities == 2);
    CHECK(spark.x == doctest::Approx(3.0f));
    CHECK(spark.y == doctest::Approx(1.0f));
}

TEST_CASE("sequence: .mcseq errors name the step") {
    const std::string root = fresh_dir("sequence_errors");
    write_file(root + "/assets/bad.mcseq", R"({ "format": "mce-sequence", "version": 1,
        "guid": "5e900000-0000-4000-8000-000000000011", "loop": true, "steps": [
        { "emit": "a" }, { "jump": 3 } ] })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    CHECK_FALSE(r.ok());
    const std::string text = diag_text(r);
    CHECK(text.find("unknown step") != std::string::npos);
    CHECK(text.find("bad.mcseq:3") != std::string::npos);

    const std::string root2 = fresh_dir("sequence_errors2");
    write_file(root2 + "/assets/spin.mcseq", R"({ "format": "mce-sequence", "version": 1,
        "guid": "5e900000-0000-4000-8000-000000000012", "loop": true, "steps": [
        { "emit": "a" }, { "move_to": [1, 0], "speed": -1, "colour": 2 } ] })");
    const cook::CookReport r2 = svc.cook(StringView(root2.c_str()), cook::CookOptions{});
    CHECK_FALSE(r2.ok());
    CHECK(diag_text(r2).find("colour") != std::string::npos);

    const std::string root3 = fresh_dir("sequence_errors3");
    write_file(root3 + "/assets/forever.mcseq", R"({ "format": "mce-sequence", "version": 1,
        "guid": "5e900000-0000-4000-8000-000000000013", "loop": true, "steps": [
        { "emit": "a" }, { "wait": 0 } ] })");
    const cook::CookReport r3 = svc.cook(StringView(root3.c_str()), cook::CookOptions{});
    CHECK_FALSE(r3.ok());
    CHECK(diag_text(r3).find("needs a step that waits") != std::string::npos);
}
