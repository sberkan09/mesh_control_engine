// Room3D: the authored scene played through the game's systems on top of the kit's 3D layer
// (kit3d colliders, Physics3D, Animator), with scripted input. It loads the cooked pack the build
// makes from assets/ (room3d_content), so it tests the scene as the generator wrote it.
//
//   - the scene has the pieces the example needs; the rock field fills StaticInstances3D
//   - Move walks the hero relative to the camera, the Animator switches to "walk" and back
//   - Jump leaves the floor and lands again; the room's walls stop the walk
//   - the follow camera stays behind the hero; two runs hash the same
#include <doctest/doctest.h>

#include <cmath>

#include "mce/asset/asset_system.h"
#include "mce/input/ecs.h"
#include "mce/kit/kit3d.h"
#include "mce/scene/scene.h"
#include "room3d.h"

using namespace mce;
using namespace room3d;
using math::vec3;

namespace {

struct Game {
    asset::AssetSystem assets;
    World w;
    u64 tick = 0;

    Game() {
        const StringView roots[1] = {StringView(ROOM3D_PACK)};
        REQUIRE(static_cast<bool>(assets.mount(Span<const StringView>(roots, 1))));
        // The player runtime's order (src/player/src/player.cpp): input, kit, the game, the scene.
        input::install(w);
        kit::install_kit3d(w, &assets);
        REQUIRE(static_cast<bool>(room3d::install(w)));
        asset::Guid scene;
        for (asset::AssetKey k = 0; k < assets.asset_count(); ++k)
            if (assets.info(k).type == asset::ChunkType::Scene) scene = assets.info(k).guid;
        REQUIRE(!scene.is_null());
        REQUIRE(static_cast<bool>(scene::load(w, assets, scene)));
    }
    void step(input::InputState in = {}) {
        in.tick = tick;
        input::publish(w, in);
        w.tick(1.0 / 60.0, tick++);
    }
    void steps(u32 n, input::InputState in = {}) {
        for (u32 i = 0; i < n; ++i) step(in);
    }
    template <class C>
    Entity first() {
        Entity found;
        w.query<const C&>().each([&](Entity e, const C&) {
            if (found.is_null()) found = e;
        });
        return found;
    }
    Entity hero() { return first<Hero>(); }
    Entity camera() { return first<FollowCamera>(); }
    vec3 pos(Entity e) { return w.try_get<LocalTransform>(e)->position; }
    const Hero& h() { return *w.try_get<Hero>(hero()); }
    StringView clip() { return w.try_get<kit::Animator>(hero())->clip.view(); }
};

input::InputState move(f32 x, f32 y) {
    input::InputState in;
    in.move = {x, y};
    return in;
}

}  // namespace

TEST_CASE("room3d: the scene, its colliders and the rock field") {
    Game g;
    g.steps(2);
    CHECK(!g.hero().is_null());
    CHECK(!g.camera().is_null());
    CHECK(kit::has_camera_3d(g.w));
    // Floor, meadow, five walls, seven crates and four pillars.
    CHECK(g.w.resource<kit::Physics3D>().colliders == 18);
    const kit::StaticInstances3D& rocks = g.w.resource<kit::StaticInstances3D>();
    CHECK(rocks.meshes.size() == 1);
    CHECK(rocks.instances.size() == 900);
    CHECK(rocks.cells.size() > 20);
    for (const kit::StaticInstance3D& r : rocks.instances) {
        const f32 d = std::sqrt(r.position.x * r.position.x + r.position.z * r.position.z);
        CHECK((d >= 15.9f && d <= 140.1f));
    }
    // Standing on the floor, idle.
    g.steps(30);
    CHECK(g.h().grounded == 1);
    CHECK(std::fabs(g.pos(g.hero()).y) < 0.05f);
    CHECK(g.clip() == StringView("idle"));
}

TEST_CASE("room3d: walking, jumping and the walls") {
    Game g;
    g.steps(30);
    const vec3 start = g.pos(g.hero());
    // Forward is away from the camera (-Z at yaw 0).
    g.steps(60, move(0, 1));
    const vec3 walked = g.pos(g.hero());
    CHECK(walked.z < start.z - 3.0f);
    CHECK(std::fabs(walked.x - start.x) < 0.2f);
    CHECK(g.clip() == StringView("walk"));
    g.steps(10);
    CHECK(g.clip() == StringView("idle"));

    // A jump leaves the floor and comes back down.
    input::InputState jump;
    jump.jump_pressed = true;
    jump.jump_held = true;
    g.step(jump);
    f32 peak = 0.0f;
    for (int i = 0; i < 60; ++i) {
        g.step();
        peak = std::max(peak, g.pos(g.hero()).y);
    }
    CHECK(g.h().jumps == 1);
    CHECK(peak > 0.6f);
    CHECK(g.h().grounded == 1);
    CHECK(std::fabs(g.pos(g.hero()).y) < 0.05f);

    // The north wall (z = -12, 0.25 thick) stops a long walk; the camera stays on this side too.
    g.steps(400, move(0, 1));
    CHECK(g.pos(g.hero()).z > -11.8f);
    CHECK(g.pos(g.hero()).z < -10.5f);
    CHECK(g.pos(g.camera()).z > g.pos(g.hero()).z);
}

TEST_CASE("room3d: the follow camera turns with Look and keeps its distance") {
    Game g;
    g.steps(5);
    const FollowCamera& c = *g.w.try_get<FollowCamera>(g.camera());
    const vec3 cam = g.pos(g.camera());
    const vec3 pivot = g.pos(g.hero()) + vec3{0.0f, c.height, 0.0f};
    const vec3 d = cam - pivot;
    CHECK(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) == doctest::Approx(c.distance).epsilon(0.02));
    CHECK(cam.z > pivot.z);  // behind the hero, looking down -Z

    input::InputState look;
    look.look = {-100.0f, 0.0f};  // a mouse moved left: the camera turns left
    g.step(look);
    CHECK(c.yaw > 0.4f);
    // Forward now follows the camera: walking moves the hero toward -X as well.
    const vec3 before = g.pos(g.hero());
    g.steps(30, move(0, 1));
    CHECK(g.pos(g.hero()).x < before.x - 0.5f);
}

TEST_CASE("room3d: two scripted runs hash the same") {
    auto run = [] {
        Game g;
        for (u32 i = 0; i < 240; ++i) {
            input::InputState in = move(i < 120 ? 0.0f : 1.0f, i < 120 ? 1.0f : 0.3f);
            in.look = {i % 30 == 0 ? 12.0f : 0.0f, 0.0f};
            in.jump_pressed = i == 70;
            g.step(in);
        }
        return g.w.state_hash();
    };
    CHECK(run() == run());
}
