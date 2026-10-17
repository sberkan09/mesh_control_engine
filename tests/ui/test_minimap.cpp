// M-MISSION step 2: the Minimap widget. The projection maps a world position to a widget-space point with exact
// numbers (north up, heading up, the rim, the full map), the layout drops and clamps markers by the widget's
// rules, and a document draws the quads it should. CPU only.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/render/camera2d.h"
#include "mce/render/sprite_batch.h"
#include "mce/ui/document_data.h"
#include "mce/ui/minimap.h"

using namespace mce;
using namespace mce::ui;

namespace {

constexpr f32 kPi = 3.14159265358979f;

// 200 x 200 at (100, 50): centre (200, 150), the rim 100 px out; with a radius of 50 m that is 2 px per metre.
const Rect kRect{vec2(100, 50), vec2(300, 250)};

struct SkinFixture {
    render::FontMetrics metrics;
    Array<render::FontGlyph> glyphs;
    Array<u8> atlas;
    render::SpriteSets sets;
    render::Font font;
    Skin skin;
    SkinFixture() {
        builtin_font(metrics, glyphs, atlas);
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

}  // namespace

TEST_CASE("minimap: north up puts east on the right and south down, 2 px per metre") {
    MinimapPoint p = minimap_project(vec2(10, 20), vec2(0, 0), 0.0f, false, 50.0f, kRect, false);
    CHECK(p.point.x == 220.0f);
    CHECK(p.point.y == 190.0f);
    CHECK(p.inside);
    CHECK_FALSE(p.on_rim);
    // Relative to the tracked entity, not the world origin.
    p = minimap_project(vec2(110, 80), vec2(100, 100), 0.0f, false, 50.0f, kRect, false);
    CHECK(p.point.x == 220.0f);
    CHECK(p.point.y == 110.0f);  // 20 m north is 40 px up
    // The tracked entity itself is the centre; a position exactly at the radius is inside.
    p = minimap_project(vec2(5, 5), vec2(5, 5), 1.0f, false, 50.0f, kRect, false);
    CHECK(p.point.x == 200.0f);
    CHECK(p.point.y == 150.0f);
    p = minimap_project(vec2(50, 0), vec2(0, 0), 0.0f, false, 50.0f, kRect, false);
    CHECK(p.inside);
    CHECK_FALSE(p.on_rim);
    CHECK(p.point.x == 300.0f);
    // A non-square rect uses its shorter side: 400 x 200 is still a 100 px rim.
    p = minimap_project(vec2(25, 0), vec2(0, 0), 0.0f, false, 50.0f, Rect{vec2(0, 0), vec2(400, 200)}, false);
    CHECK(p.point.x == 250.0f);
    CHECK(p.point.y == 100.0f);
}

TEST_CASE("minimap: heading up turns the map so the heading points up the screen") {
    // Facing west (heading +90 degrees): a marker 10 m west is straight ahead, 20 px above the centre.
    MinimapPoint p = minimap_project(vec2(-10, 0), vec2(0, 0), kPi / 2, true, 50.0f, kRect, false);
    CHECK(p.point.x == doctest::Approx(200.0f).epsilon(1e-5));
    CHECK(p.point.y == doctest::Approx(130.0f).epsilon(1e-5));
    // 10 m north is then to the right (facing west, north is on the right hand).
    p = minimap_project(vec2(0, -10), vec2(0, 0), kPi / 2, true, 50.0f, kRect, false);
    CHECK(p.point.x == doctest::Approx(220.0f).epsilon(1e-5));
    CHECK(p.point.y == doctest::Approx(150.0f).epsilon(1e-5));
    // Facing south (heading 180): north is behind, so 10 m north lands below the centre.
    p = minimap_project(vec2(0, -10), vec2(0, 0), kPi, true, 50.0f, kRect, false);
    CHECK(p.point.x == doctest::Approx(200.0f).epsilon(1e-5));
    CHECK(p.point.y == doctest::Approx(170.0f).epsilon(1e-5));
    // Heading 0 is north up, and the flag off ignores the heading.
    p = minimap_project(vec2(10, 20), vec2(0, 0), 0.0f, true, 50.0f, kRect, false);
    CHECK(p.point.x == 220.0f);
    CHECK(p.point.y == 190.0f);
    p = minimap_project(vec2(10, 20), vec2(0, 0), 1.234f, false, 50.0f, kRect, false);
    CHECK(p.point.x == 220.0f);
    CHECK(p.point.y == 190.0f);
}

TEST_CASE("minimap: beyond the radius a marker hides or sticks to the rim") {
    MinimapPoint p = minimap_project(vec2(60, 0), vec2(0, 0), 0.0f, false, 50.0f, kRect, false);
    CHECK_FALSE(p.inside);
    CHECK_FALSE(p.on_rim);
    CHECK(p.point.x == 320.0f);  // unclamped
    p = minimap_project(vec2(60, 0), vec2(0, 0), 0.0f, false, 50.0f, kRect, true);
    CHECK_FALSE(p.inside);
    CHECK(p.on_rim);
    CHECK(p.point.x == doctest::Approx(300.0f).epsilon(1e-6));
    CHECK(p.point.y == 150.0f);
    // A 3-4-5 direction 100 m out lands 100 px along it: (60, 80) from the centre.
    p = minimap_project(vec2(60, 80), vec2(0, 0), 0.0f, false, 50.0f, kRect, true);
    CHECK(p.on_rim);
    CHECK(p.point.x == doctest::Approx(260.0f).epsilon(1e-6));
    CHECK(p.point.y == doctest::Approx(230.0f).epsilon(1e-6));
    // Inside the radius, clamping changes nothing.
    p = minimap_project(vec2(30, 40), vec2(0, 0), 0.0f, false, 50.0f, kRect, true);
    CHECK(p.inside);
    CHECK_FALSE(p.on_rim);
    CHECK(p.point.x == 260.0f);
    CHECK(p.point.y == 230.0f);
}

TEST_CASE("minimap: the full map fits the world rectangle to the widget") {
    const vec2 lo(-500, -500), hi(500, 500);
    MinimapPoint p = minimap_project_full(vec2(0, 0), lo, hi, kRect);
    CHECK(p.point.x == 200.0f);
    CHECK(p.point.y == 150.0f);
    CHECK(p.inside);
    p = minimap_project_full(vec2(-500, -500), lo, hi, kRect);
    CHECK(p.point.x == 100.0f);
    CHECK(p.point.y == 50.0f);
    p = minimap_project_full(vec2(250, -250), lo, hi, kRect);
    CHECK(p.point.x == 250.0f);
    CHECK(p.point.y == 100.0f);
    p = minimap_project_full(vec2(600, 0), lo, hi, kRect);
    CHECK_FALSE(p.inside);
    CHECK(p.point.x == 320.0f);
}

TEST_CASE("minimap: layout applies the layer mask, zoom, the rim rules and the full map") {
    Minimap m;
    m.radius = 100.0f;
    m.zoom = 2.0f;  // shows 50 m
    CHECK(minimap_radius(m) == 50.0f);
    MinimapMarker near_a, far_plain, far_always, other_layer;
    near_a.pos = vec2(10, 20);
    far_plain.pos = vec2(60, 0);
    far_always.pos = vec2(60, 0);
    far_always.always_visible = true;
    other_layer.pos = vec2(1, 1);
    other_layer.layer = 3;
    const MinimapMarker markers[] = {near_a, far_plain, far_always, other_layer};
    MinimapView view;
    Array<MinimapShown> shown;
    m.layer_mask = 0b0001;  // layer 3 hidden
    minimap_layout(m, view, Span<const MinimapMarker>(markers, 4), kRect, shown);
    REQUIRE(shown.size() == 2);  // near, and the always-visible one on the rim; the plain far one hides
    CHECK(shown[0].marker == 0);
    CHECK(shown[0].point.x == 220.0f);
    CHECK(shown[0].point.y == 190.0f);
    CHECK_FALSE(shown[0].on_rim);
    CHECK(shown[1].marker == 2);
    CHECK(shown[1].on_rim);
    CHECK(shown[1].point.x == doctest::Approx(300.0f).epsilon(1e-6));

    m.layer_mask = ~0u;
    m.clamp_to_rim = true;  // every far marker sticks now
    minimap_layout(m, view, Span<const MinimapMarker>(markers, 4), kRect, shown);
    REQUIRE(shown.size() == 4);
    CHECK(shown[1].on_rim);

    m.full_map = true;
    m.world_min = vec2(-100, -100);
    m.world_max = vec2(100, 100);
    minimap_layout(m, view, Span<const MinimapMarker>(markers, 4), kRect, shown);
    REQUIRE(shown.size() == 4);  // all inside the 200 m square, none clamped
    CHECK_FALSE(shown[1].on_rim);
    CHECK(shown[0].point.x == 210.0f);  // (10 + 100) / 200 of 200 px, from x = 100
    CHECK(shown[0].point.y == 170.0f);
}

TEST_CASE("minimap: a document draws the background, the markers and the tracked arrow") {
    SkinFixture f;
    Document doc;
    doc.set_skin(f.skin);
    Builder b(doc, doc.root());
    Builder mini = b.panel("unused");  // a plain sibling: the minimap's quads are counted on top of it
    (void)mini;
    const WidgetId w = doc.add(doc.root(), WidgetKind::Minimap, "mini");
    Anchors a;
    a.min = a.max = vec2(0, 0);
    a.offset_min = vec2(100, 50);
    a.offset_max = vec2(300, 250);
    a.pivot = vec2(0, 0);
    doc.set_anchors(w, a);
    Minimap m;
    m.radius = 50.0f;
    doc.set_minimap(w, m);
    UpdateContext ctx;
    ctx.viewport = rhi::Extent2D{640, 480};
    doc.update(ctx);
    CHECK(doc.rect(w).min.x == 100.0f);
    CHECK(doc.rect(w).max.y == 250.0f);

    auto quads = [&]() {
        render::SpriteBatch batch(f.sets, mem::heap());
        batch.begin(render::compute_screen_view(rhi::Extent2D{640, 480}), render::PassId::Screen2D);
        doc.submit(batch);
        batch.build();
        return batch.stats().quads;
    };
    const u32 base = quads();  // the "unused" panel, plus the background and the arrow (a dot and a nose)
    MinimapMarker near_a, far_plain;
    near_a.pos = vec2(10, 20);
    far_plain.pos = vec2(60, 0);
    const MinimapMarker markers[] = {near_a, far_plain};
    doc.set_minimap_view(w, MinimapView{vec2(0, 0), 0.0f}, Span<const MinimapMarker>(markers, 2));
    CHECK(quads() == base + 1);  // only the near marker
    m.clamp_to_rim = true;
    doc.set_minimap(w, m);
    CHECK(quads() == base + 2);  // the far one sticks to the rim
    // A hidden minimap draws nothing.
    doc.set_visible(w, false);
    CHECK(quads() == base - 3);
}

TEST_CASE("minimap: the codec round-trips a Minimap and a document without one has no extra bytes") {
    DocumentData d;
    WidgetData plain;
    plain.name = "p";
    d.widgets.push_back(plain);
    Array<byte> without;
    encode_ui_document(d, without);
    WidgetData mini;
    mini.name = "m";
    mini.kind = WidgetKind::Minimap;
    mini.minimap.track = "Hero";
    mini.minimap.radius = 75.0f;
    mini.minimap.full_map = true;
    mini.minimap.layer_mask = 6;
    d.widgets.push_back(mini);
    Array<byte> with;
    encode_ui_document(d, with);
    CHECK(with.size() > without.size());
    DocumentData back;
    REQUIRE(decode_ui_document(with.span(), back));
    REQUIRE(back.widgets.size() == 2);
    CHECK(back.widgets[1].minimap.track.view() == "Hero");
    CHECK(back.widgets[1].minimap.radius == 75.0f);
    CHECK(back.widgets[1].minimap.full_map);
    CHECK(back.widgets[1].minimap.layer_mask == 6u);
    CHECK_FALSE(decode_ui_document(Span<const byte>(with.data(), with.size() - 4), back));
    // A bad radius in the record does not load.
    Array<byte> bad = with;
    const f32 zero = 0.0f;
    std::memcpy(bad.data() + bad.size() - 52, &zero, 4);  // UiMinimapRecord::radius (after the 8-byte track ref)
    CHECK_FALSE(decode_ui_document(bad.span(), back));
}
