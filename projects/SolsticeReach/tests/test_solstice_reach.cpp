// Solstice Reach: the game played through its real systems on the cooked pack.
//
//   - the C++ terrain port matches the Python generator's height field to the float, and the drawn
//     Terrain3D samples the same field (the collider and the drawn island agree)
//   - the authored island has what the design needs: seven heliostats in a chain, shards, a spawn
//   - the bot (autopilot over the authored waypoints) aligns every mirror, the hour follows
//   - two runs hash the same (determinism)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/ecs/hierarchy.h"
#include "mce/input/ecs.h"
#include "mce/kit/kit3d.h"
#include "mce/scene/scene.h"
#include "solstice_reach.h"
#include "terrain.h"

using namespace mce;
using namespace solstice_reach;

namespace {

struct Game {
    asset::AssetSystem assets;
    World w;
    u64 tick = 0;

    explicit Game(bool autopilot) {
        const StringView roots[1] = {StringView(SOLSTICE_PACK)};
        REQUIRE(static_cast<bool>(assets.mount(Span<const StringView>(roots, 1))));
        input::install(w);
        ReachState preset;
        preset.autopilot = autopilot ? 1u : 0u;
        preset.mode = Mode::Playing;
        w.set_resource(preset);
        REQUIRE(static_cast<bool>(solstice_reach::install(w)));
        asset::Guid scene;
        for (asset::AssetKey k = 0; k < assets.asset_count(); ++k)
            if (assets.info(k).type == asset::ChunkType::Scene) scene = assets.info(k).guid;
        REQUIRE(!scene.is_null());
        REQUIRE(static_cast<bool>(scene::load(w, assets, scene)));
        step();
    }
    void step(input::InputState in = {}) {
        in.tick = tick;
        input::publish(w, in);
        w.tick(1.0 / 60.0, tick++);
    }
    ReachState& gs() { return w.resource<ReachState>(); }
    const Progress& progress() { return *w.try_get<Progress>(gs().progress); }
    const Keeper& keeper() { return *w.try_get<Keeper>(gs().keeper); }
    template <class C>
    u32 count() {
        return static_cast<u32>(w.query<const C&>().count());
    }
};

}  // namespace

TEST_CASE("solstice_reach: the terrain port matches the generator") {
    FILE* f = std::fopen(SOLSTICE_REF, "r");
    REQUIRE(f != nullptr);
    double x, z, h;
    int n = 0;
    double worst = 0.0;
    while (std::fscanf(f, "%lf %lf %lf", &x, &z, &h) == 3) {
        const double got = terrain::height(x, z);
        worst = std::max(worst, std::fabs(got - h));
        ++n;
    }
    std::fclose(f);
    CHECK(n >= 400);
    CHECK(worst < 1e-9);
}

TEST_CASE("solstice_reach: the island is authored") {
    Game g(false);
    CHECK(g.gs().started == 1);
    CHECK_FALSE(g.gs().keeper.is_null());
    CHECK_FALSE(g.gs().progress.is_null());
    CHECK(g.count<Heliostat>() == kMirrors);
    CHECK(g.count<Sunshard>() >= 30);
    CHECK(g.count<Waypoint>() >= kMirrors);
    // The chain: index i's next is index i+1 (the last ends the chain).
    u32 chained = 0;
    g.w.query<const Heliostat&>().each([&](const Heliostat& h) {
        if (h.index + 1 < kMirrors) {
            const Heliostat* nx = g.w.try_get<Heliostat>(h.next);
            if (nx && nx->index == h.index + 1) ++chained;
        }
    });
    CHECK(chained == kMirrors - 1);
    // The Keeper stands on the ground at the spawn.
    const Keeper& k = g.keeper();
    CHECK(k.pos.y > terrain::height_f(k.pos.x, k.pos.z));
    CHECK(k.pos.y < terrain::height_f(k.pos.x, k.pos.z) + 3.0f);
}

TEST_CASE("solstice_reach: the island is a painted Terrain3D on the collider's height field (M-TERRAIN 6)") {
    Game g(false);
    Entity island;
    asset::Guid guid;
    u32 terrains = 0;
    g.w.query<const kit::Terrain3D&>().each([&](Entity e, const kit::Terrain3D& t) {
        island = e;
        guid = t.terrain.guid;
        ++terrains;
    });
    REQUIRE(terrains == 1);
    auto h = g.assets.load_sync<asset::Terrain>(guid);
    REQUIRE(static_cast<bool>(h));
    auto view = asset::TerrainView::read(g.assets.resolve(h.value()));
    REQUIRE(static_cast<bool>(view));
    const asset::TerrainView& v = view.value();
    CHECK(v.header->layer_count == 4);
    const f32 half = 0.5f * v.header->size[0];
    // Every sample stands on the field to its 16-bit step; between samples the drawn surface stays
    // within a few centimetres on the walkable ground.
    f64 worst = 0.0;
    for (u32 z = 0; z < v.resolution(); z += 3)
        for (u32 x = 0; x < v.resolution(); x += 3) {
            const f64 px = -half + f64(x) * v.spacing_x(), pz = -half + f64(z) * v.spacing_z();
            worst = std::max(worst, std::fabs(v.sample(x, z) - terrain::height(px, pz)));
        }
    CHECK(worst < 0.002);
    // The grass grows from the grass layer as the island's child.
    u32 foliage = 0;
    g.w.query<const kit::TerrainFoliage3D&, const ChildOf&>().each([&](const kit::TerrainFoliage3D& f, const ChildOf& c) {
        foliage += c.parent == island && f.layer == 1;
    });
    CHECK(foliage == 1);
    g.assets.release(asset::AnyHandle(h.value()));
}

TEST_CASE("solstice_reach: the bot aligns every mirror and the hour follows") {
    Game g(true);
    const f32 hour0 = g.progress().hour;
    CHECK(hour0 == doctest::Approx(kHourKeys[0]));
    u32 ticks = 0;
    const u32 limit = 60 * 60 * 12;  // twelve minutes of sim at most
    while (g.progress().mirrors < kMirrors && ticks < limit) {
        g.step();
        ++ticks;
    }
    CHECK(g.progress().mirrors == kMirrors);
    CHECK(g.gs().mode == Mode::Finale);
    // The finale runs its course into the dawn.
    for (u32 i = 0; i < 60 * 40; ++i) g.step();
    CHECK(g.progress().finale_done == 1);
    CHECK(g.gs().mode == Mode::Dawn);
    CHECK(g.progress().hour == doctest::Approx(kHourKeys[8]).epsilon(0.01));
    CHECK(g.progress().shards > 0);
}

TEST_CASE("solstice_reach: two runs hash the same") {
    Game a(true), b(true);
    for (u32 i = 0; i < 60 * 30; ++i) {
        a.step();
        b.step();
    }
    CHECK(hash_game(a.w, 1) == hash_game(b.w, 1));
    CHECK(a.keeper().pos.x == b.keeper().pos.x);
}
