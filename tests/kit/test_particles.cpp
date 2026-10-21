// FR-R2D-070: CPU particles — the .mcpart cook, the ParticleEmitter simulation (emission, forces,
// retirement, determinism), its sprite-batch output and the 2,000-particle burst budget.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/kit/particles.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace cook_test;
using kit::ParticleEmitter;
using kit::ParticleSystem;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kPreset{0x9a27000000000001ull, 0x8000000000000001ull};

asset::ParticlePresetChunk preset(u32 burst, f32 rate = 0.0f, f32 duration = 0.0f) {
    asset::ParticlePresetChunk p{};
    p.burst = burst;
    p.rate = rate;
    p.duration = duration;
    p.max_particles = 4096;
    p.lifetime[0] = p.lifetime[1] = 1.0f;
    p.speed[0] = p.speed[1] = 0.0f;
    p.size_start = p.size_end = 0.25f;
    for (u32 i = 0; i < 4; ++i) p.color_start[i] = p.color_end[i] = 1.0f;
    return p;
}

struct Scene {
    World world;
    ParticleSystem* ps = nullptr;
    explicit Scene(const asset::ParticlePresetChunk& p) {
        kit::install_particles(world, nullptr);
        ps = &world.resource<ParticleSystem>();
        ps->set_preset(kPreset, p);
    }
    Entity emitter(math::vec2 at, u32 seed = 1, bool destroy_when_done = false) {
        ParticleEmitter em;
        em.preset.guid = kPreset;
        em.seed = seed;
        em.destroy_when_done = destroy_when_done;
        return world.spawn_with(LocalTransform{{at.x, at.y, 0}, math::quat::identity(), {1, 1, 1}}, em);
    }
    void run(u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) ps->advance(world, kDt);
    }
};

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

TEST_CASE("particles: the start burst emits once, particles age out after their lifetime") {
    Scene s(preset(25));
    const Entity e = s.emitter({0, 0});
    s.run(1);
    CHECK(s.ps->live_count(e) == 25);
    CHECK(s.ps->stats().spawned == 25);
    s.run(58);  // 59 ticks of age < 1 s
    CHECK(s.ps->live_count(e) == 25);
    s.run(3);  // 61 steps of 1/60 s: past the lifetime whatever the float rounding
    CHECK(s.ps->live_count(e) == 0);
    s.run(30);
    CHECK(s.ps->stats().spawned == 0);  // a burst preset never re-emits
}

TEST_CASE("particles: rate emits rate x seconds, duration stops it, max_particles caps it") {
    {
        Scene s(preset(0, 30.0f));  // 30/s forever, 1 s lifetime: steady state ~30 alive
        const Entity e = s.emitter({0, 0});
        s.run(60);
        CHECK(s.ps->live_count(e) >= 29);
        CHECK(s.ps->live_count(e) <= 30);
        s.run(120);
        CHECK(s.ps->live_count(e) >= 29);
        CHECK(s.ps->live_count(e) <= 31);
    }
    {
        Scene s(preset(0, 60.0f, 0.5f));  // 0.5 s at 60/s = 30 particles total
        s.emitter({0, 0});
        u32 total = 0;
        for (u32 i = 0; i < 120; ++i) {
            s.run(1);
            total += s.ps->stats().spawned;
        }
        CHECK(total == 30);
    }
    {
        asset::ParticlePresetChunk p = preset(100);
        p.max_particles = 40;
        Scene s(p);
        const Entity e = s.emitter({0, 0});
        s.run(1);
        CHECK(s.ps->live_count(e) == 40);
        CHECK(s.ps->stats().dropped == 60);
    }
}

TEST_CASE("particles: velocity, spread, gravity and drag integrate per tick") {
    asset::ParticlePresetChunk p = preset(1);
    p.speed[0] = p.speed[1] = 2.0f;
    p.direction = 0.0f;  // +X
    p.gravity[1] = -10.0f;
    p.lifetime[0] = p.lifetime[1] = 10.0f;
    Scene s(p);
    s.emitter({1, 1});
    s.run(1);  // spawned at the emitter
    s.run(60);  // 1 s of semi-implicit Euler: x = 1 + 2, y drops ~ g/2 (plus one step of bias)
    render::SpriteSets sets;
    render::SpriteRegion whole;
    render::SpriteSetDesc sd;
    sd.regions = Span<const render::SpriteRegion>(&whole, 1);
    const render::SpriteSetId set = sets.create(sd);
    render::SpriteBatch batch(sets, mem::heap());
    render::Camera2D cam;
    batch.begin(render::compute_view_2d(cam, {640, 640}, 16.0f));
    s.ps->draw(batch, [](void* u, asset::Guid) { return *static_cast<render::SpriteSetId*>(u); }, const_cast<render::SpriteSetId*>(&set));
    batch.build();
    REQUIRE(batch.stats().quads == 1);
    // Quad center = particle position.
    math::vec2 c{0, 0};
    for (u32 i = 0; i < 4; ++i) {
        c.x += batch.vertices()[i].x * 0.25f;
        c.y += batch.vertices()[i].y * 0.25f;
    }
    CHECK(c.x == doctest::Approx(3.0f).epsilon(1e-3));
    CHECK(c.y == doctest::Approx(1.0f - 5.0f - 5.0f / 60.0f).epsilon(1e-2));

    asset::ParticlePresetChunk d = preset(1);
    d.speed[0] = d.speed[1] = 4.0f;
    d.direction = 0.0f;
    d.drag = 3.0f;
    d.lifetime[0] = d.lifetime[1] = 10.0f;
    Scene s2(d);
    s2.emitter({0, 0});
    s2.run(1);
    const u64 before = s2.ps->hash();
    s2.run(120);
    CHECK(s2.ps->hash() != before);  // moving, slowing (the hash covers velocity)
}

TEST_CASE("particles: the simulation is deterministic and the seed picks the stream") {
    asset::ParticlePresetChunk p = preset(60, 120.0f);
    p.speed[0] = 1.0f;
    p.speed[1] = 5.0f;
    p.spread = 1.0f;
    p.area[0] = 0.5f;
    p.lifetime[0] = 0.2f;
    p.lifetime[1] = 1.4f;
    p.gravity[1] = -9.8f;
    p.drag = 0.5f;
    p.spin[0] = -3.0f;
    p.spin[1] = 3.0f;
    auto run = [&](u32 seed) {
        Scene s(p);
        s.emitter({0, 0}, seed);
        s.emitter({3, 1}, seed + 7);
        s.run(600);  // 10 s, emitters busy the whole time
        return s.ps->hash();
    };
    const u64 a = run(1);
    CHECK(a == run(1));
    CHECK(a != run(2));
}

TEST_CASE("particles: bursts on demand, removed emitters drop their particles, one-shots destroy themselves") {
    Scene s(preset(0));
    const Entity e = s.emitter({0, 0});
    s.ps->burst(e, 12);
    s.run(1);
    CHECK(s.ps->live_count(e) == 12);
    s.ps->burst(e, 3);
    s.run(1);
    CHECK(s.ps->live_count(e) == 15);
    s.world.destroy(e);
    s.run(1);
    CHECK(s.ps->stats().live == 0);
    CHECK(s.ps->stats().emitters == 0);

    Scene one(preset(8));
    const Entity shot = one.emitter({0, 0}, 1, /*destroy_when_done=*/true);
    one.run(1);
    CHECK(one.world.alive(shot));
    one.run(62);  // particles live 1 s
    CHECK_FALSE(one.world.alive(shot));
}

TEST_CASE("particles: the installed system runs on the fixed tick and follows the hierarchy") {
    World w;
    ecs::install_hierarchy(w);
    kit::install_particles(w, nullptr);
    w.resource<ParticleSystem>().set_preset(kPreset, preset(5));
    const Entity parent = w.spawn_with(LocalTransform{{10, 0, 0}, math::quat::identity(), {1, 1, 1}});
    ParticleEmitter em;
    em.preset.guid = kPreset;
    const Entity child = w.spawn_with(LocalTransform{{0, 2, 0}, math::quat::identity(), {1, 1, 1}}, em);
    REQUIRE(ecs::set_parent(w, child, parent));
    w.tick(kDt, 0);
    w.tick(kDt, 1);
    CHECK(w.resource<ParticleSystem>().live_count(child) == 5);
}

TEST_CASE("particles: one MDI group per blend, culled through the sprite batch") {
    asset::ParticlePresetChunk p = preset(2000);
    p.speed[0] = 0.0f;
    p.speed[1] = 3.0f;
    p.spread = 3.14159f;
    p.flags = asset::ParticleFlags::Additive;
    Scene s(p);
    s.emitter({0, 0});
    s.run(1);
    REQUIRE(s.ps->stats().live == 2000);
    render::SpriteSets sets;
    render::SpriteRegion whole;
    render::SpriteSetDesc sd;
    sd.regions = Span<const render::SpriteRegion>(&whole, 1);
    render::SpriteSetId set = sets.create(sd);
    render::SpriteBatch batch(sets, mem::heap());
    render::Camera2D cam;
    batch.begin(render::compute_view_2d(cam, {1280, 720}, 48.0f));
    s.ps->draw(batch, [](void* u, asset::Guid) { return *static_cast<render::SpriteSetId*>(u); }, &set);
    batch.build();
    CHECK(batch.stats().quads == 2000);
    CHECK(batch.stats().groups == 1);
    REQUIRE(batch.groups().size() == 1);
    CHECK(batch.groups()[0].blend == render::SpriteBlend::Additive);

    // Far from the camera: everything culls.
    render::SpriteBatch far(sets, mem::heap());
    cam.position = {500, 500};
    far.begin(render::compute_view_2d(cam, {1280, 720}, 48.0f));
    s.ps->draw(far, [](void* u, asset::Guid) { return *static_cast<render::SpriteSetId*>(u); }, &set);
    far.build();
    CHECK(far.stats().quads == 0);
    CHECK(far.stats().culled == 2000);
}

TEST_CASE("FR-R2D-070: a 2,000-particle burst simulates and batches within budget") {
    asset::ParticlePresetChunk p = preset(2000);
    p.speed[0] = 1.0f;
    p.speed[1] = 6.0f;
    p.spread = 3.14159f;
    p.gravity[1] = -9.8f;
    p.drag = 0.4f;
    p.lifetime[0] = p.lifetime[1] = 5.0f;
    p.size_end = 0.0f;
    p.color_end[3] = 0.0f;
    Scene s(p);
    s.emitter({0, 0});
    s.run(1);
    render::SpriteSets sets;
    render::SpriteRegion whole;
    render::SpriteSetDesc sd;
    sd.regions = Span<const render::SpriteRegion>(&whole, 1);
    render::SpriteSetId set = sets.create(sd);
    render::Camera2D cam;
    const render::View2D view = render::compute_view_2d(cam, {1920, 1080}, 32.0f);

    const u32 frames = 60;
    const auto t0 = std::chrono::steady_clock::now();
    for (u32 i = 0; i < frames; ++i) {
        s.ps->advance(s.world, kDt);
        render::SpriteBatch batch(sets, mem::heap());
        batch.begin(view);
        batch.reserve(2000);
        s.ps->draw(batch, [](void* u, asset::Guid) { return *static_cast<render::SpriteSetId*>(u); }, &set);
        batch.build();
        REQUIRE(batch.stats().submitted == 2000);
    }
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
    CHECK(s.ps->stats().live == 2000);
    MESSAGE("2,000 particles: advance + batch " << ms << " ms per frame");
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(ms < 2.0);  // optimized builds only: Debug/ASan numbers are not the budget
#endif
}

TEST_CASE("particles: .mcpart cooks to CHNK_PART and drives an emitter in a cooked scene") {
    const std::string root = fresh_dir("particles_cook");
    std::vector<u8> px(4 * 4 * 4, 255);
    write_file(root + "/assets/spark.png", make_png(4, 4, px));
    write_file(root + "/assets/spark.png.mceasset", sidecar("9a270000-0000-4000-8000-000000000010", "texture"));
    write_file(root + "/assets/dust.mcpart", R"({ "format": "mce-particles", "version": 1,
        "guid": "9a270000-0000-4000-8000-000000000011", "name": "dust",
        "texture": "guid:9a270000-0000-4000-8000-000000000010",
        "burst": 16, "max_particles": 32, "lifetime": [0.5, 0.75], "speed": [1, 2],
        "direction": 90, "spread": 45, "gravity": [0, -4], "drag": 1, "size": [0.3, 0.1],
        "spin": [-90, 90], "color_end": [1, 0.5, 0, 0], "blend": "additive" })");
    write_file(root + "/assets/level.mcscene", R"({ "format": "mce-scene", "version": 1,
        "guid": "9a270000-0000-4000-8000-000000000012", "name": "level", "entities": [
        { "name": "Dust", "components": {
            "LocalTransform": { "position": [2, 3, 0] },
            "ParticleEmitter": { "preset": "guid:9a270000-0000-4000-8000-000000000011", "seed": 9 } } } ] })");
    World w;
    kit::install_particles(w, nullptr);  // registers ParticleEmitter for the scene cook's reflection
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid("9a270000-0000-4000-8000-000000000011").value());
    REQUIRE(key != asset::kInvalidAssetKey);
    auto h = assets.load_sync<asset::ParticlePreset>(key);
    REQUIRE(h);
    auto chunk = asset::ParticlePresetChunk::read(assets.resolve(h.value()));
    REQUIRE(chunk);
    const asset::ParticlePresetChunk& c = *chunk.value();
    CHECK(c.burst == 16);
    CHECK(c.max_particles == 32);
    CHECK(c.direction == doctest::Approx(1.5707963f));
    CHECK(c.spread == doctest::Approx(0.7853982f));
    CHECK(c.spin[1] == doctest::Approx(1.5707963f));
    CHECK(c.size_start == doctest::Approx(0.3f));
    CHECK(c.color_end[1] == doctest::Approx(0.5f));
    CHECK((c.flags & asset::ParticleFlags::Additive) != 0);
    CHECK(c.texture_guid() == asset::parse_guid("9a270000-0000-4000-8000-000000000010").value());
    assets.release(asset::AnyHandle(h.value()));

    kit::install_particles(w, &assets);
    Array<Entity> spawned;
    REQUIRE(scene::load(w, assets, asset::parse_guid("9a270000-0000-4000-8000-000000000012").value(), &spawned));
    REQUIRE(spawned.size() == 1);
    w.tick(kDt, 0);
    CHECK(w.resource<ParticleSystem>().live_count(spawned[0]) == 16);
}

TEST_CASE("particles: .mcpart errors name the key") {
    const std::string root = fresh_dir("particles_errors");
    write_file(root + "/assets/bad.mcpart", R"({ "format": "mce-particles", "version": 1,
        "guid": "9a270000-0000-4000-8000-000000000021", "lifetime": [2, 1], "blend": "glow", "colour": [1, 1, 1, 1] })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    CHECK_FALSE(r.ok());
    const std::string text = diag_text(r);
    CHECK(text.find("colour") != std::string::npos);

    const std::string root2 = fresh_dir("particles_errors2");
    write_file(root2 + "/assets/bad.mcpart", R"({ "format": "mce-particles", "version": 1,
        "guid": "9a270000-0000-4000-8000-000000000022", "lifetime": [2, 1], "blend": "glow" })");
    const cook::CookReport r2 = svc.cook(StringView(root2.c_str()), cook::CookOptions{});
    CHECK_FALSE(r2.ok());
    const std::string text2 = diag_text(r2);
    CHECK(text2.find("lifetime") != std::string::npos);
    CHECK(text2.find("blend") != std::string::npos);
}
