// M-MISSION step 3b: the stock player feeds a document's Minimap widgets. A world with a Mission whose Objective
// (reach a zone) has a MapMarker, plus a plain marker, is ticked; the feed hands the widget exactly those markers
// at the right positions and the tracked entity's place, a document without a Minimap is left alone, and the
// world (state hash) is the same with and without the feed. CPU only.

#include <doctest/doctest.h>

#include "minimap_feed.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/map_markers.h"
#include "mce/kit/missions.h"
#include "mce/render/camera2d.h"
#include "mce/render/sprite_batch.h"
#include "mce/scene/entity_names.h"

using namespace mce;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct SkinFixture {
    render::FontMetrics metrics;
    Array<render::FontGlyph> glyphs;
    Array<u8> atlas;
    render::SpriteSets sets;
    render::Font font;
    ui::Skin skin;
    SkinFixture() {
        ui::builtin_font(metrics, glyphs, atlas);
        render::SpriteSetDesc fd;
        fd.coverage = true;
        font.face = render::FontFace(metrics, glyphs.span(), {});
        font.set = sets.create(fd);
        const render::SpriteRegion white{0, 0.25f, 0.25f, 0.75f, 0.75f, 1.0f, 1.0f};
        render::SpriteSetDesc wd;
        wd.regions = Span<const render::SpriteRegion>(&white, 1);
        skin.set = sets.create(wd);
        skin.white_region = 0;
        skin.font = &font;
    }
};

// The player at (5, 0, -3), the zone at (10, 0, -5): a mission with one reach objective, a bare MapMarker on the
// mission (it follows the active objective's zone), and a plain marker at (3, 7, -4) on layer 2.
struct Scene {
    World w;
    u64 t = 0;
    Entity player, zone, mission, shop;
    Scene() {
        w.register_components<LocalTransform, SaveKey>();
        kit::install_map_markers(w, nullptr);
        player = w.spawn_with(SaveKey{1}, at(5, 0, -3));
        zone = w.spawn_with(SaveKey{2}, at(10, 0, -5));
        scene::EntityNames names;
        names.rows.push_back(scene::EntityNames::Row{StringId("Player").value, player});
        w.set_resource(names);
        kit::Objective o;
        o.kind = kit::ObjectiveKind::ReachZone;
        o.target = zone;
        o.radius = 1.0f;
        const Entity reach = w.spawn_with(SaveKey{3}, o);
        kit::Mission m;
        m.id = "intro";
        m.objective = reach;
        m.actor = player;
        m.start_on_load = 1;
        kit::MapMarker follow;
        follow.icon = 4;
        follow.color = math::Color{1, 1, 0, 1};
        mission = w.spawn_with(SaveKey{4}, m, follow);
        kit::MapMarker plain;
        plain.layer = 2;
        plain.icon = 1;
        shop = w.spawn_with(SaveKey{5}, at(3, 7, -4), plain);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
};

// A Minimap widget (200 x 200 at 100, 50; radius 50 m: 2 px per metre) in a document.
struct Hud {
    SkinFixture f;
    ui::Document doc;
    ui::WidgetId mini;
    explicit Hud(bool with_minimap) {
        doc.set_skin(f.skin);
        ui::Builder b(doc, doc.root());
        (void)b.panel("frame");
        if (!with_minimap) return;
        mini = doc.add(doc.root(), ui::WidgetKind::Minimap, "mini");
        ui::Anchors a;
        a.min = a.max = ui::vec2(0, 0);
        a.offset_min = ui::vec2(100, 50);
        a.offset_max = ui::vec2(300, 250);
        a.pivot = ui::vec2(0, 0);
        doc.set_anchors(mini, a);
        ui::Minimap m;
        m.radius = 50.0f;
        doc.set_minimap(mini, m);
        ui::UpdateContext ctx;
        ctx.viewport = rhi::Extent2D{640, 480};
        doc.update(ctx);
    }
    u32 quads() {
        render::SpriteBatch batch(f.sets, mem::heap());
        batch.begin(render::compute_screen_view(rhi::Extent2D{640, 480}), render::PassId::Screen2D);
        doc.submit(batch);
        batch.build();
        return batch.stats().quads;
    }
};

}  // namespace

TEST_CASE("player minimap feed: markers and the tracked view reach the widget with exact numbers") {
    Scene s;
    s.run(5);  // the mission starts, the reach objective is active
    Array<kit::MapMarkerView> shown;
    kit::collect_map_markers(s.w, shown);
    REQUIRE(shown.size() == 2);

    ui::Minimap spec;
    ui::MinimapView view;
    Array<ui::MinimapMarker> markers;
    player::build_minimap_feed(s.w, spec, shown.span(), view, markers);
    CHECK(view.tracked.x == 5.0f);
    CHECK(view.tracked.y == -3.0f);
    CHECK(view.heading == doctest::Approx(0.0f).epsilon(1e-6));
    REQUIRE(markers.size() == 2);
    CHECK(markers[0].pos.x == 10.0f);  // the objective's zone, from the mission's marker
    CHECK(markers[0].pos.y == -5.0f);
    CHECK(markers[0].icon == 4);
    CHECK(markers[0].layer == 0);
    CHECK(markers[1].pos.x == 3.0f);   // the shop; the height is dropped
    CHECK(markers[1].pos.y == -4.0f);
    CHECK(markers[1].layer == 2);
    CHECK(markers[1].icon == 1);

    // A track name that nobody has leaves the view at the origin and still lists the markers.
    spec.track = "nobody";
    player::build_minimap_feed(s.w, spec, shown.span(), view, markers);
    CHECK(view.tracked.x == 0.0f);
    CHECK(view.tracked.y == 0.0f);
    CHECK(markers.size() == 2);
}

TEST_CASE("player minimap feed: a document's Minimap draws the fed markers, an objective marker goes with its objective") {
    Scene s;
    Hud hud(true);
    player::MinimapFeed feed;
    feed.bind(hud.doc);
    REQUIRE_FALSE(feed.empty());
    const u32 base = hud.quads();  // no markers fed yet: frame, background and the tracked arrow
    s.run(5);
    feed.feed(s.w, hud.doc);
    // Player (5, -3), zone (10, -5) at 10 px, -4 px; shop (3, -4) at -4 px, -2 px: both inside 50 m.
    CHECK(hud.quads() == base + 2);

    // The player walks into the zone: the objective completes, the mission ends and its marker goes.
    s.w.try_get_mut<LocalTransform>(s.player)->position = math::vec3(10, 0, -5);
    s.run(5);
    feed.feed(s.w, hud.doc);
    CHECK(hud.quads() == base + 1);

    // Far from everything the plain shop marker is beyond the radius and hides.
    s.w.try_get_mut<LocalTransform>(s.player)->position = math::vec3(500, 0, 500);
    feed.feed(s.w, hud.doc);
    CHECK(hud.quads() == base);
}

TEST_CASE("player minimap feed: a document without a Minimap has nothing to feed and the world is untouched") {
    Scene a, b;
    Hud plain(false);
    player::MinimapFeed feed;
    feed.bind(plain.doc);
    CHECK(feed.empty());
    Hud with(true);
    player::MinimapFeed fed;
    fed.bind(with.doc);
    a.run(20);
    b.run(20);
    for (u32 i = 0; i < 20; ++i) {
        feed.feed(a.w, plain.doc);
        fed.feed(b.w, with.doc);
    }
    CHECK(a.w.state_hash() == b.w.state_hash());
    // A scene with no markers at all: the feed lists nothing and the widget draws just the arrow.
    World bare;
    bare.register_components<LocalTransform>();
    kit::install_map_markers(bare, nullptr);
    const u64 before = bare.state_hash();
    const u32 quads = with.quads();
    fed.feed(bare, with.doc);
    CHECK(bare.state_hash() == before);
    CHECK(with.quads() == quads - 2);
}
