// M-AI 3: perception. Sight in 3D (range, the cone, line of sight through World3D, teams) and in
// the 2D plane (through World2D), the memory that forgets a lost target, hearing from a tree's
// make_noise, the blackboard keys, the staggered checks of a crowd, the debug lines, and two runs
// hashing the same with a save resuming mid-memory.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/ai.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/perception.h"
#include "mce/physics/module.h"

using namespace mce;
using namespace cook_test;
using kit::Perceiver;
using kit::Stimulus;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr const char* kWatch = "a3000000-0000-4000-8000-000000000001";
constexpr const char* kShout = "a3000000-0000-4000-8000-000000000002";

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("ai_perception_project");
        write_file(r + "/assets/watch.mcbt", R"({ "format": "mce-bt", "version": 1, "guid": "a3000000-0000-4000-8000-000000000001",
            "name": "watch", "blackboard": { "target": "entity", "sees": "bool", "last_seen": "vec3", "heard_at": "vec3" },
            "root": {"wait": 1000} })");
        write_file(r + "/assets/shout.mcbt", R"({ "format": "mce-bt", "version": 1, "guid": "a3000000-0000-4000-8000-000000000002",
            "name": "shout", "root": {"sequence": [{"wait": 0.05}, {"task": "make_noise", "loudness": 8}, {"wait": 1000}]} })");
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        INFO(diag_text(rep));
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

LocalTransform at(f32 x, f32 y, f32 z, math::quat r = math::quat::identity()) { return LocalTransform{{x, y, z}, r, {1, 1, 1}}; }

struct Stage {
    asset::AssetSystem assets;
    World world;
    u64 ticks = 0;

    explicit Stage(bool physics3d = false, bool physics2d = false) {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        world.register_components<LocalTransform, SaveKey>();
        if (physics2d) {
            physics::PhysicsConfig cfg;
            cfg.gravity = {0.0f, 0.0f};
            physics::install_physics2d(world, cfg);
        }
        if (physics3d) kit::install_kit3d(world, nullptr);
        kit::install_ai(world, &assets);
    }
    template <class... Cs>
    Entity watcher(const LocalTransform& t, const Perceiver& p, const Cs&... cs) {
        kit::AiBrain b;
        b.tree.guid = asset::parse_guid(kWatch).value();
        return world.spawn_with(t, p, b, kit::Blackboard{}, cs...);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
    const Perceiver& of(Entity e) { return *world.try_get<Perceiver>(e); }
};

Perceiver eyes(f32 range = 10.0f, f32 angle = 90.0f) {
    Perceiver p;
    p.sight_range = range;
    p.sight_angle = angle;
    p.eye = {0, 0, 0};
    p.team = 1;
    return p;
}

Stimulus shown(u32 team = 2) {
    Stimulus s;
    s.team = team;
    s.offset = {0, 0, 0};
    return s;
}

}  // namespace

TEST_CASE("perception: sight in 3D: range, cone, teams, and the blackboard") {
    Stage s;
    const Entity w = s.watcher(at(0, 0, 0), eyes());  // looks down -Z, a 90 degree cone
    const Entity ahead = s.world.spawn_with(at(0, 0, -5), shown());
    s.run(1);
    CHECK(s.of(w).sees == 1);
    CHECK(s.of(w).target == ahead);
    CHECK(s.of(w).sightings == 1);
    Entity t;
    REQUIRE(kit::bb_entity(s.world, w, "target", t));
    CHECK(t == ahead);
    f32 sees = 0;
    REQUIRE(kit::bb_number(s.world, w, "sees", sees));
    CHECK(sees == 1.0f);
    math::vec3 last;
    REQUIRE(kit::bb_vector(s.world, w, "last_seen", last));
    CHECK(last.z == -5.0f);

    struct Case {
        math::vec3 pos;
        u32 team;
        bool seen;
    };
    const Case cases[] = {
        {{0, 0, 5}, 2, false},                    // behind
        {{0, 0, -11}, 2, false},                  // out of range
        {{4, 0, -5}, 2, true},                    // 38.7 degrees off: inside the 45 degree half cone
        {{6, 0, -5}, 2, false},                   // 50.2 degrees off
        {{0, 0, -5}, 1, false},                   // the perceiver's own team
        {{0, 3, -5}, 2, true},                    // above: the cone is round, not flat
    };
    for (const Case& c : cases) {
        Stage t2;
        const Entity p = t2.watcher(at(0, 0, 0), eyes());
        t2.world.spawn_with(at(c.pos.x, c.pos.y, c.pos.z), shown(c.team));
        t2.run(1);
        INFO(c.pos.x, " ", c.pos.y, " ", c.pos.z, " team ", c.team);
        CHECK((t2.of(p).sees == 1) == c.seen);
    }

    // The nearest of two in sight; visibility scales the range.
    Stage u;
    const Entity p = u.watcher(at(0, 0, 0), eyes());
    u.world.spawn_with(at(0, 0, -8), shown());
    const Entity near = u.world.spawn_with(at(1, 0, -3), shown());
    Stimulus faint = shown();
    faint.visibility = 0.2f;  // seen from 2 m
    u.world.spawn_with(at(0, 0, -2.5f), faint);
    u.run(1);
    CHECK(u.of(p).target == near);
}

TEST_CASE("perception: a wall blocks the line of sight in 3D; in 2D through World2D") {
    {
        Stage s(true);
        const Entity w = s.watcher(at(0, 1, 0), eyes());
        const Entity thief = s.world.spawn_with(at(0, 1, -8), shown());
        kit::BoxCollider3D wall;
        wall.half = {2.0f, 2.0f, 0.2f};
        const Entity box = s.world.spawn_with(at(0, 1, -4), wall);
        s.run(2);
        CHECK(s.of(w).sees == 0);
        CHECK(s.world.resource<kit::PerceptionStats>().rays == 1);
        // Out from behind the wall.
        s.world.try_get_mut<LocalTransform>(thief)->position = {5, 1, -6};
        s.run(1);
        CHECK(s.of(w).sees == 1);
        (void)box;
    }
    {
        Stage s(false, true);
        Perceiver p = eyes();
        p.space = kit::PerceptionSpace::Plane2D;  // looks along +X
        const Entity w = s.watcher(at(0, 0, 0), p);
        const Entity thief = s.world.spawn_with(at(8, 0, 0), shown());
        physics::RigidBody2D rb;
        rb.type = physics::BodyType::Static;
        physics::Collider2D col;
        col.shape = physics::ColliderShape::box({0.2f, 2.0f});
        s.world.spawn_with(at(4, 0, 0), rb, col);
        s.run(2);
        CHECK(s.of(w).sees == 0);
        s.world.try_get_mut<LocalTransform>(thief)->position = {6, 5, 0};
        s.run(1);
        CHECK(s.of(w).sees == 1);
        // Turned away (about +Z), it loses sight.
        s.world.try_get_mut<LocalTransform>(w)->rotation = rotation_z(3.14159265f);
        s.run(1);
        CHECK(s.of(w).sees == 0);
        CHECK(s.of(w).target == thief);  // remembered
    }
}

TEST_CASE("perception: a lost target is remembered for `forget` seconds, then dropped") {
    Stage s;
    Perceiver p = eyes();
    p.forget = 1.0f;
    const Entity w = s.watcher(at(0, 0, 0), p);
    const Entity thief = s.world.spawn_with(at(0, 0, -5), shown());
    s.run(5);
    s.world.try_get_mut<Stimulus>(thief)->visible = 0;
    s.run(30);
    CHECK(s.of(w).sees == 0);
    CHECK(s.of(w).target == thief);
    CHECK(s.of(w).awareness == doctest::Approx(0.5f).epsilon(0.02));
    Entity t;
    CHECK(kit::bb_entity(s.world, w, "target", t));
    s.run(30);
    CHECK(s.of(w).target.is_null());
    CHECK_FALSE(kit::bb_entity(s.world, w, "target", t));
    math::vec3 last;
    REQUIRE(kit::bb_vector(s.world, w, "last_seen", last));  // where to look stays
    CHECK(last.z == -5.0f);
    // Seen again: a second sighting.
    s.world.try_get_mut<Stimulus>(thief)->visible = 1;
    s.run(1);
    CHECK(s.of(w).sightings == 2);
}

TEST_CASE("perception: make_noise is heard within loudness x hearing, not by the shouter's team") {
    Stage s;
    Perceiver deaf = eyes();
    deaf.hearing = 0.0f;
    const Entity near = s.watcher(at(5, 0, 0), eyes());
    const Entity far = s.watcher(at(12, 0, 0), eyes());
    const Entity sharp = [&] {
        Perceiver p = eyes();
        p.hearing = 2.0f;
        return s.watcher(at(12, 0, 1), p);
    }();
    const Entity none = s.watcher(at(1, 0, 0), deaf);
    Perceiver ally = eyes();
    ally.team = 2;
    const Entity mate = s.watcher(at(2, 0, 0), ally);
    kit::AiBrain b;
    b.tree.guid = asset::parse_guid(kShout).value();
    s.world.spawn_with(at(0, 0, 0), b, shown(2));
    u32 heard = 0;
    for (u32 i = 0; i < 6; ++i) {
        s.run(1);
        heard += s.world.resource<kit::PerceptionStats>().heard;
    }
    CHECK(heard == 2);
    CHECK(s.of(near).hearings == 1);
    CHECK(s.of(far).hearings == 0);
    CHECK(s.of(sharp).hearings == 1);
    CHECK(s.of(none).hearings == 0);
    CHECK(s.of(mate).hearings == 0);
    math::vec3 h;
    REQUIRE(kit::bb_vector(s.world, near, "heard_at", h));
    CHECK(h.x == 0.0f);
    CHECK_FALSE(kit::bb_vector(s.world, far, "heard_at", h));
}

TEST_CASE("perception: a crowd staggers its checks; the debug lines") {
    Stage s;
    Perceiver p = eyes(30.0f, 360.0f);
    p.interval = 4;
    for (u32 row = 0; row < 20; ++row)
        for (u32 col = 0; col < 20; ++col) s.world.spawn_with(at(static_cast<f32>(col), 0, static_cast<f32>(row)), p);
    for (u32 i = 0; i < 10; ++i) s.world.spawn_with(at(static_cast<f32>(i), 0, -3), shown());
    for (u32 i = 0; i < 8; ++i) {
        s.run(1);
        const kit::PerceptionStats& st = s.world.resource<kit::PerceptionStats>();
        CHECK(st.checks == 100);
        CHECK(st.rays <= st.checks * 10);
    }
    u32 seeing = 0;
    s.world.query<const Perceiver>().each([&](Entity, const Perceiver& q) { seeing += q.sees; });
    CHECK(seeing == 400);

    Stage d;
    d.watcher(at(0, 0, 0), eyes());
    d.world.spawn_with(at(0, 0, -5), shown());
    d.run(1);
    Array<kit::PerceptionLine> lines;
    kit::perception_debug_lines(d.world, lines);
    CHECK(lines.size() == 2 + 12 + 1);
    CHECK(lines.back().b.z == -5.0f);
    CHECK(lines.back().color.g == 1.0f);
}

TEST_CASE("perception: two runs hash the same; a save mid-memory resumes the same") {
    auto setup = [](Stage& s) {
        Perceiver p = eyes();
        p.forget = 2.0f;
        kit::AiBrain brain;
        brain.tree.guid = asset::parse_guid(kWatch).value();
        s.world.spawn_with(SaveKey{1}, at(0, 0, 0), p, brain, kit::Blackboard{});
        s.world.spawn_with(SaveKey{2}, at(-6, 0, -4), shown());
    };
    auto step = [](Stage& s) {
        s.world.query<const Stimulus, LocalTransform>().each([&](Entity, const Stimulus&, LocalTransform& t) { t.position.x += 0.05f; });
        s.run(1);
    };
    Stage a, b;
    setup(a);
    setup(b);
    for (u32 i = 0; i < 300; ++i) {
        step(a);
        step(b);
        REQUIRE(a.world.state_hash() == b.world.state_hash());
    }
    Stage c;
    setup(c);
    for (u32 i = 0; i < 150; ++i) step(c);
    Array<byte> bytes;
    save::write_save(c.world, save::SaveInfo{}, bytes);
    Stage d;
    setup(d);
    d.ticks = c.ticks;
    REQUIRE(save::read_save(d.world, bytes.span()));
    for (u32 i = 0; i < 150; ++i) step(d);
    u32 sight_a = 0, sight_d = 0;
    a.world.query<const Perceiver>().each([&](Entity, const Perceiver& q) { sight_a = q.sightings * 1000 + q.sees; });
    d.world.query<const Perceiver>().each([&](Entity, const Perceiver& q) { sight_d = q.sightings * 1000 + q.sees; });
    CHECK(sight_a == sight_d);
}
