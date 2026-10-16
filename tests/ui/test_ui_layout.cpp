// mce_ui on many screens (M-MOBILE step 11b, #151): the UI scale from the display, anchors to the
// safe area or the whole screen, fit-width grids, scroll views (drag, inertia, wheel, clipping,
// focus into view), 9-slice images, modals and tabs, and the reflected authoring structs. CPU only;
// test_ui_gl draws the same layouts at three screen sizes.

#include <cmath>

#include <doctest/doctest.h>

#include "mce/core.h"
#include "mce/refl/refl.h"
#include "mce/render/camera2d.h"
#include "mce/render/sprite_batch.h"
#include "mce/ui/ui.h"

using namespace mce;
using namespace mce::ui;
using math::vec2;

namespace {

struct Fixture {
    render::FontMetrics metrics;
    Array<render::FontGlyph> glyphs;
    Array<u8> atlas;
    render::SpriteSets sets;
    render::Font font;
    Skin skin;
    u32 frame_region = 0;  // a 30×30-texel region for 9-slice

    Fixture() {
        builtin_font(metrics, glyphs, atlas);
        render::SpriteSetDesc fd;
        fd.coverage = true;
        font.face = render::FontFace(metrics, glyphs.span(), {});
        font.set = sets.create(fd);
        const render::SpriteRegion regions[2] = {{0, 0.25f, 0.25f, 0.75f, 0.75f, 1.0f, 1.0f},
                                                 {0, 0.5f, 0.0f, 0.8f, 0.3f, 30.0f, 30.0f}};
        render::SpriteSetDesc wd;
        wd.regions = Span<const render::SpriteRegion>(regions, 2);
        wd.pixels_per_meter = 1.0f;  // width_m 30 = 30 texels
        skin.set = sets.create(wd);
        skin.white_region = 0;
        skin.font = &font;
        frame_region = 1;
    }
};

UpdateContext frame(u32 w = 1280, u32 h = 720, f32 scale = 1.0f) {
    UpdateContext c;
    c.viewport = rhi::Extent2D{w, h};
    c.ui_scale = scale;
    c.dt = 1.0 / 60.0;
    return c;
}

u32 count_events(const Document& d, UiEvent::Kind k, WidgetId w = {}) {
    u32 n = 0;
    for (const UiEvent& e : d.events()) n += e.kind == k && (w.is_null() || e.widget == w) ? 1u : 0u;
    return n;
}

void press(Document& d, vec2 p) {
    UpdateContext c = frame();
    c.pointer = p;
    c.pointer_pressed = c.pointer_down = true;
    d.update(c);
}
void drag(Document& d, vec2 p) {
    UpdateContext c = frame();
    c.pointer = p;
    c.pointer_down = true;
    d.update(c);
}
void release(Document& d, vec2 p) {
    UpdateContext c = frame();
    c.pointer = p;
    c.pointer_released = true;
    d.update(c);
}

bool inside(const Rect& in, const Rect& out) {
    return in.min.x >= out.min.x && in.min.y >= out.min.y && in.max.x <= out.max.x && in.max.y <= out.max.y;
}

// A 200×300 vertical scroll view holding ten 50-px buttons: 500 px of content, 200 px to scroll.
struct List {
    Fixture f;
    Document d;
    WidgetId view;
    WidgetId items[10];
    List() {
        d.set_skin(f.skin);
        Builder b(d, d.root());
        StackLayout s;
        s.spacing = 0.0f;
        Builder sv = b.scroll("list", s).at({0.5f, 0.5f}, {200, 300});
        view = sv.id();
        for (u32 i = 0; i < 10; ++i) {
            char name[8] = {'i', 't', 'e', 'm', static_cast<char>('0' + i), 0};
            Anchors a;
            a.offset_max = vec2(0, 50);
            items[i] = sv.button(name, name).anchors(a).id();
        }
        d.update(frame());
    }
};

}  // namespace

TEST_CASE("layout: the UI scale follows the display, fits small screens and rounds to 1/8") {
    CHECK(compute_ui_scale({1280, 720}, 1.0f) == 1.0f);
    CHECK(compute_ui_scale({2400, 1080}, 2.625f) == 2.625f);  // a 420 dpi Android phone
    CHECK(compute_ui_scale({1080, 2400}, 2.625f) == 2.625f);  // the same phone upright
    CHECK(compute_ui_scale({2048, 1536}, 2.0f) == 2.0f);      // an iPad at 2 px per point
    CHECK(compute_ui_scale({1920, 1080}, 1.0f, 1.5f) == 1.5f);  // the user's setting on top
    CHECK(compute_ui_scale({640, 360}, 2.0f) == 1.0f);        // too small for 2: what still fits 360×640
    CHECK(compute_ui_scale({1280, 720}, 1.3f) == 1.25f);      // rounded down to 1/8
    CHECK(compute_ui_scale({64, 64}, 1.0f) == 0.25f);         // never below a quarter
    CHECK(compute_ui_scale({1280, 720}, 0.0f, 0.0f) == 1.0f);  // unknown display, no setting
}

TEST_CASE("layout: anchors are fractions of the parent, the safe area or the whole screen") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    const WidgetId safe = b.panel("hud").fill().id();
    const WidgetId backdrop = b.panel("backdrop").fill().to(AnchorTo::Screen).id();
    Builder card = b.panel("card").at({0.5f, 0.5f}, {400, 200});
    const WidgetId inner = card.panel("inner").fill().id();
    const WidgetId banner = card.panel("banner").anchors(Anchors{{0, 0}, {1, 0}, {0, 0}, {0, 40}, {0.5f, 0.5f}, AnchorTo::SafeArea}).id();
    UpdateContext c = frame(2400, 1080);
    c.safe_insets = SafeInsets{100, 0, 60, 40};
    d.update(c);
    CHECK(d.rect(safe).min == vec2(100, 0));
    CHECK(d.rect(safe).max == vec2(2340, 1040));
    CHECK(d.rect(backdrop).min == vec2(0, 0));
    CHECK(d.rect(backdrop).max == vec2(2400, 1080));
    CHECK(d.rect(inner).min == d.rect(card.id()).min);
    // Under the card, but laid against the safe area: the full safe width, 40 px tall at its top.
    CHECK(d.rect(banner).min == vec2(100, 0));
    CHECK(d.rect(banner).max == vec2(2340, 40));
    // fill() and at() keep what the anchors are fractions of.
    b.panel("bleed").to(AnchorTo::Screen).fill();
    d.update(c);
    CHECK(d.rect(d.find("bleed")).max == vec2(2400, 1080));
}

TEST_CASE("layout: a grid with columns 0 fits as many cells as the width holds") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    GridLayout g;
    g.columns = 0;
    g.cell = vec2(100, 80);
    g.spacing = vec2(10, 10);
    Builder grid = b.grid("levels", g).anchors(Anchors{{0, 0}, {0, 0}, {0, 0}, {560, 400}});
    WidgetId cells[12];
    for (u32 i = 0; i < 12; ++i) cells[i] = grid.button(i < 10 ? "l" : "m", "L").id();
    d.update(frame());
    // 560 px: five 100-px cells and four 10-px gaps fit (550), a sixth would not.
    CHECK(d.rect(cells[4]).min == vec2(440, 0));
    CHECK(d.rect(cells[5]).min == vec2(0, 90));
    // Narrower: two columns.
    d.set_anchors(grid.id(), Anchors{{0, 0}, {0, 0}, {0, 0}, {230, 700}});
    d.update(frame());
    CHECK(d.rect(cells[2]).min == vec2(0, 90));
    CHECK(d.rect(cells[3]).min == vec2(110, 90));

    // In a vertical stack the grid takes the stack's width, so its height follows the columns.
    Builder col = b.stack("col").anchors(Anchors{{0, 0}, {0, 0}, {600, 0}, {830, 700}});
    Builder inner = col.grid("g", g);
    for (u32 i = 0; i < 6; ++i) inner.button("c", "C");
    d.update(frame());
    CHECK(d.rect(inner.id()).size().x == 230.0f);
    CHECK(d.rect(inner.id()).size().y == 3 * 80.0f + 2 * 10.0f);  // 6 cells, 2 a row
}

TEST_CASE("layout: dragging a scroll view scrolls it and lets go of the pressed button") {
    List l;
    CHECK(l.d.scroll_max(l.view) == 200.0f);
    const Rect view = l.d.rect(l.view);
    const vec2 start = l.d.rect(l.items[2]).center();
    const f32 y0 = l.d.rect(l.items[2]).min.y;
    press(l.d, start);
    drag(l.d, start - vec2(0, 5));  // inside the slop: still a press
    CHECK(l.d.scroll(l.view) == 0.0f);
    CHECK(l.d.state(l.items[2]) == WidgetState::Pressed);
    drag(l.d, start - vec2(0, 20));  // past it: a drag from here on
    drag(l.d, start - vec2(0, 120));
    CHECK(l.d.scroll(l.view) == 100.0f);
    CHECK(l.d.rect(l.items[2]).min.y == y0 - 100.0f);
    CHECK(l.d.state(l.items[2]) != WidgetState::Pressed);
    // Dragging far past the end stops at it.
    drag(l.d, start - vec2(0, 300));
    CHECK(l.d.scroll(l.view) == 200.0f);
    release(l.d, start - vec2(0, 300));
    CHECK(count_events(l.d, UiEvent::Confirmed) == 0);  // a drag is not a tap
    // Items scrolled out are not hit: item 0 is above the view now.
    CHECK(l.d.rect(l.items[0]).max.y <= view.min.y);
    UpdateContext c = frame();
    c.pointer = vec2(view.center().x, view.min.y - 5.0f);
    l.d.update(c);
    CHECK(l.d.hovered().is_null());

    // A tap (no drag) on an item still clicks it.
    const vec2 tap = l.d.rect(l.items[8]).center();
    press(l.d, tap);
    release(l.d, tap);
    CHECK(count_events(l.d, UiEvent::Confirmed, l.items[8]) == 1);
}

TEST_CASE("layout: a released drag coasts, slows down and stops at the end") {
    List l;
    const vec2 start = l.d.rect(l.items[5]).center();
    press(l.d, start);
    drag(l.d, start - vec2(0, 20));
    drag(l.d, start - vec2(0, 30));
    drag(l.d, start - vec2(0, 40));  // 10 px a frame: 600 px/s
    release(l.d, start - vec2(0, 40));
    const f32 at_release = l.d.scroll(l.view);
    l.d.update(frame());
    const f32 step1 = l.d.scroll(l.view) - at_release;
    CHECK(step1 > 0.0f);
    f32 prev = l.d.scroll(l.view);
    l.d.update(frame());
    const f32 step2 = l.d.scroll(l.view) - prev;
    CHECK(step2 > 0.0f);
    CHECK(step2 < step1);  // friction
    for (u32 i = 0; i < 240; ++i) l.d.update(frame());
    prev = l.d.scroll(l.view);
    l.d.update(frame());
    CHECK(l.d.scroll(l.view) == prev);  // stopped
    CHECK(l.d.scroll(l.view) <= 200.0f);

    // A fling into the end stops there.
    l.d.set_scroll(l.view, 150.0f);
    const vec2 s2 = l.d.rect(l.items[7]).center();
    press(l.d, s2);
    for (u32 i = 1; i <= 4; ++i) drag(l.d, s2 - vec2(0, 10.0f + 30.0f * static_cast<f32>(i)));
    release(l.d, s2 - vec2(0, 130));
    for (u32 i = 0; i < 30; ++i) l.d.update(frame());
    CHECK(l.d.scroll(l.view) == 200.0f);
}

TEST_CASE("layout: the wheel scrolls the view under the pointer; set_scroll clamps") {
    List l;
    UpdateContext c = frame();
    c.pointer = l.d.rect(l.view).center();
    c.wheel = -1.0f;  // towards the user: down the list
    l.d.update(c);
    CHECK(l.d.scroll(l.view) == 48.0f);
    c.wheel = 3.0f;
    l.d.update(c);
    CHECK(l.d.scroll(l.view) == 0.0f);
    c.pointer = vec2(5, 5);  // not over it
    c.wheel = -1.0f;
    l.d.update(c);
    CHECK(l.d.scroll(l.view) == 0.0f);
    l.d.set_scroll(l.view, 1000.0f);
    CHECK(l.d.scroll(l.view) == 200.0f);
    // The content shrinks: the view settles at its new end.
    for (u32 i = 5; i < 10; ++i) l.d.remove(l.items[i]);
    l.d.update(frame());
    CHECK(l.d.scroll_max(l.view) == 0.0f);
    CHECK(l.d.scroll(l.view) == 0.0f);
}

TEST_CASE("layout: gamepad focus scrolls the focused item into view") {
    List l;
    input::InputState down;
    down.move = vec2(0, -1);
    UpdateContext c = frame();
    c.actions = &down;
    input::InputState idle;
    UpdateContext rest = frame();
    rest.actions = &idle;
    for (u32 i = 0; i < 9; ++i) {
        l.d.update(c);
        l.d.update(rest);
    }
    REQUIRE(l.d.focused() == l.items[9]);
    CHECK(inside(l.d.rect(l.items[9]), l.d.rect(l.view)));
    CHECK(l.d.scroll(l.view) == 200.0f);
    l.d.scroll_into_view(l.items[0]);
    CHECK(l.d.scroll(l.view) == 0.0f);
}

TEST_CASE("layout: a scroll view's content is cut to its rect when drawn") {
    List l;
    l.d.set_scroll(l.view, 25.0f);  // item 0 half out at the top, item 6 half out at the bottom
    l.d.update(frame());
    const Rect view = l.d.rect(l.view);
    render::SpriteBatch batch(l.f.sets, mem::heap());
    batch.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
    l.d.submit(batch);
    batch.build();
    REQUIRE(batch.vertices().size() > 0);
    bool cut_top = false, cut_bottom = false;
    for (const render::SpriteVertex& v : batch.vertices()) {
        CHECK(v.x >= view.min.x);
        CHECK(v.x <= view.max.x);
        CHECK(v.y >= view.min.y);
        CHECK(v.y <= view.max.y);
        cut_top |= v.y == view.min.y;
        cut_bottom |= v.y == view.max.y;
    }
    CHECK(cut_top);     // item 0 is cut at the top edge ...
    CHECK(cut_bottom);  // ... item 6 at the bottom one
    // Items 7..9 are below the view and not offered to the batch at all: 7 items, each a quad
    // and a 5-glyph label. Scrolled to the end, items 4..9 fill the view exactly.
    CHECK(l.d.stats().quads == 7 * 6);
    l.d.set_scroll(l.view, 200.0f);
    l.d.update(frame());
    render::SpriteBatch batch2(l.f.sets, mem::heap());
    batch2.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
    l.d.submit(batch2);
    CHECK(l.d.stats().quads == 6 * 6);
}

TEST_CASE("layout: a 9-sliced image keeps its corners and stretches the rest") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    const WidgetId img = b.image("frame", f.frame_region).slice(Slice{10, 10, 10, 10}).anchors(Anchors{{0, 0}, {0, 0}, {0, 0}, {200, 100}}).id();
    auto draw = [&](f32 scale, Array<render::SpriteVertex>& out) {
        d.update(frame(1280, 720, scale));
        render::SpriteBatch batch(f.sets, mem::heap());
        batch.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
        d.submit(batch);
        batch.build();
        out.clear();
        for (const render::SpriteVertex& v : batch.vertices()) out.push_back(v);
    };
    Array<render::SpriteVertex> v;
    draw(1.0f, v);
    REQUIRE(v.size() == 9 * 4);
    // Quad 0 is the top-left corner: 10×10 px showing the region's first third (u 0.5..0.6, v 0..0.1).
    CHECK(v[3].x == 0.0f);
    CHECK(v[3].y == 0.0f);
    CHECK(v[1].x == 10.0f);
    CHECK(v[1].y == 10.0f);
    CHECK(v[3].u == doctest::Approx(0.5f));
    CHECK(v[1].u == doctest::Approx(0.6f));
    CHECK(v[3].v == doctest::Approx(0.0f));
    CHECK(v[1].v == doctest::Approx(0.1f));
    // Quad 4 is the middle: stretched across 180×80, the region's middle third.
    CHECK(v[4 * 4 + 3].x == 10.0f);
    CHECK(v[4 * 4 + 1].x == 190.0f);
    CHECK(v[4 * 4 + 1].y == 90.0f);
    CHECK(v[4 * 4 + 1].u == doctest::Approx(0.7f));
    // At UI scale 2 the corners are 20 px; the rect itself doubled too.
    draw(2.0f, v);
    CHECK(v[1].x == 20.0f);
    CHECK(v[4 * 4 + 1].x == 380.0f);
    // A rect thinner than two corners shrinks them together (and drops the empty middle).
    d.set_anchors(img, Anchors{{0, 0}, {0, 0}, {0, 0}, {15, 100}});
    draw(1.0f, v);
    CHECK(v.size() == 6 * 4);
    CHECK(v[1].x == doctest::Approx(7.5f));
}

TEST_CASE("layout: a modal keeps focus, navigation and the pointer until it closes") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    const WidgetId play = b.button("play", "PLAY").at({0.5f, 0.3f}, {200, 50}).id();
    Builder m = b.modal("quit");
    Builder box = m.stack("box").at({0.5f, 0.6f}, {240, 0});
    const WidgetId yes = box.button("yes", "YES").id();
    const WidgetId no = box.button("no", "NO").default_focus().id();
    input::InputState idle;
    UpdateContext c = frame();
    c.actions = &idle;
    d.update(c);
    CHECK(d.focused() == play);
    CHECK_FALSE(d.visible(m.id()));

    d.open_modal(m.id());
    d.update(c);
    CHECK(d.visible(m.id()));
    CHECK(d.top_modal() == m.id());
    CHECK(d.focused() == no);
    CHECK(d.rect(m.id()).max == vec2(1280, 720));  // the veil covers the screen
    // Navigation stays inside: up from "no" reaches "yes", never "play".
    input::InputState up;
    up.move = vec2(0, 1);
    UpdateContext cu = frame();
    cu.actions = &up;
    d.update(cu);
    CHECK(d.focused() == yes);
    d.update(c);
    d.update(cu);
    CHECK(d.focused() == yes);
    // The pointer on "play" hits the veil.
    const vec2 p = d.rect(play).center();
    press(d, p);
    release(d, p);
    CHECK(count_events(d, UiEvent::Confirmed) == 0);
    CHECK(d.hovered() == m.id());
    // Cancel names the modal.
    input::InputState cancel;
    cancel.cancel_pressed = true;
    UpdateContext cc = frame();
    cc.actions = &cancel;
    d.update(cc);
    CHECK(count_events(d, UiEvent::Cancelled, m.id()) == 1);
    // Drawn two layers up: the page's labels never show through the modal's quads.
    render::SpriteBatch batch(f.sets, mem::heap());
    batch.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
    d.submit(batch, 100);
    batch.build();
    REQUIRE(batch.groups().size() == 4);
    CHECK(batch.groups()[1].set == f.font.set);  // the page's text ...
    CHECK(batch.groups()[2].set == f.skin.set);  // ... under the modal's quads
    d.close_modal(m.id());
    d.update(c);
    CHECK_FALSE(d.visible(m.id()));
    CHECK(d.focused() == play);  // given back
    CHECK(d.top_modal().is_null());
    d.update(cc);
    CHECK(count_events(d, UiEvent::Cancelled, d.root()) == 1);
}

TEST_CASE("layout: tabs show one page; a tab's button switches it") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    Builder t = b.tabs("settings").anchors(Anchors{{0, 0}, {0, 0}, {0, 0}, {600, 400}});
    const WidgetId audio = t.tab("audio", "AUDIO").button("mute", "MUTE").at({0.5f, 0.5f}, {120, 40}).id();
    const WidgetId video = t.tab("video", "VIDEO").button("vsync", "VSYNC").at({0.5f, 0.5f}, {120, 40}).id();
    d.update(frame());
    CHECK(d.selected_tab(t.id()) == 0);
    CHECK(d.visible(audio));
    CHECK_FALSE(d.visible(video));
    const WidgetId bar = d.find("settings/bar");
    const WidgetId pages = d.find("settings/pages");
    CHECK(d.rect(bar).min == vec2(0, 0));
    CHECK(d.rect(bar).max.x == 600.0f);
    CHECK(d.rect(pages).min.y == d.rect(bar).max.y);
    CHECK(d.rect(pages).max == vec2(600, 400));
    CHECK(inside(d.rect(audio), d.rect(pages)));

    const WidgetId video_tab = d.find("settings/bar/video");
    const vec2 p = d.rect(video_tab).center();
    press(d, p);
    release(d, p);
    CHECK(count_events(d, UiEvent::Confirmed, video_tab) == 1);
    CHECK(count_events(d, UiEvent::ValueChanged, t.id()) == 1);
    CHECK(d.selected_tab(t.id()) == 1);
    CHECK(d.visible(video));
    CHECK_FALSE(d.visible(audio));
    // Selecting the shown tab again changes nothing.
    press(d, p);
    release(d, p);
    CHECK(count_events(d, UiEvent::ValueChanged) == 0);
    d.select_tab(t.id(), 7);  // clamped
    CHECK(d.selected_tab(t.id()) == 1);
}

TEST_CASE("layout: the authoring structs are reflected for the editor and the MCP server") {
    const refl::TypeRecord* anchors = refl::find_type(StringId("UiAnchors"));
    REQUIRE(anchors);
    CHECK(anchors->size == sizeof(Anchors));
    bool has_to = false;
    for (const refl::FieldRecord& fr : anchors->fields)
        if (fr.name == StringId("to")) has_to = fr.kind == refl::FieldKind::Enum && fr.type_id == StringId("UiAnchorTo");
    CHECK(has_to);
    for (const char* t : {"UiStyle", "UiSlice", "UiStackLayout", "UiGridLayout"}) {
        CAPTURE(t);
        CHECK(refl::find_type(StringId(t)) != nullptr);
    }
    for (const char* e : {"UiWidgetKind", "UiAnchorTo", "UiAxis", "UiAlign"}) {
        CAPTURE(e);
        CHECK(refl::find_enum(StringId(e)) != nullptr);
    }
}

TEST_CASE("layout: a Text widget aligns its lines left, center or right across its rect") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    const WidgetId t = b.text("label", "HELLO").at({0.5f, 0.5f}, {400, 0}).id();
    auto glyph_span = [&](TextAlign a, f32& lo, f32& hi) {
        d.set_text_align(t, a);
        d.update(frame());
        render::SpriteBatch batch(f.sets, mem::heap());
        batch.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
        d.submit(batch);
        batch.build();
        REQUIRE(batch.vertices().size() > 0);
        lo = 1e9f;
        hi = -1e9f;
        for (const render::SpriteVertex& v : batch.vertices()) {
            lo = math::min(lo, v.x);
            hi = math::max(hi, v.x);
        }
    };
    CHECK(d.text_align(t) == TextAlign::Left);
    f32 lo = 0, hi = 0;
    glyph_span(TextAlign::Left, lo, hi);
    const Rect r = d.rect(t);
    REQUIRE(r.size().x == doctest::Approx(400.0f));
    CHECK(std::fabs(lo - r.min.x) <= 1.5f);
    const f32 width = hi - lo;
    CHECK(width < 200.0f);
    glyph_span(TextAlign::Center, lo, hi);
    CHECK(std::fabs(0.5f * (lo + hi) - r.center().x) <= 1.5f);
    CHECK(hi - lo == doctest::Approx(width));
    glyph_span(TextAlign::Right, lo, hi);
    CHECK(std::fabs(hi - r.max.x) <= 1.5f);
    CHECK(hi - lo == doctest::Approx(width));
}
