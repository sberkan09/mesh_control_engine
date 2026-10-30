// mce_ui unit tests (docs/architecture/ui.md §6): anchors and pixel snapping, container sizing,
// dirty tracking, geometric and explicit navigation, confirm edges, pointer focus, convar binding,
// the console line editor, and drawing through SpriteBatch in two groups. CPU only.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/core.h"
#include "mce/ecs/world.h"
#include "mce/log.h"
#include "mce/refl/refl.h"
#include "mce/render/camera2d.h"
#include "mce/render/sprite_batch.h"
#include "mce/ui/document_data.h"
#include "mce/ui/ui.h"

using namespace mce;
using namespace mce::ui;
using math::vec2;

namespace {

MCE_CVAR(cv_test_ui_volume, "test_ui.volume", 0.5f, "ui test convar", CVarFlags::None);
MCE_CVAR(cv_test_ui_flag, "test_ui.flag", false, "ui test convar", CVarFlags::None);
MCE_CVAR(cv_test_ui_cheat, "test_ui.cheat", false, "ui test convar", CVarFlags::Cheat);

struct UiHealth {
    f32 hp = 100.0f;
    i32 lives = 3;
    bool shielded = false;
};
MCE_COMPONENT(UiHealth, hp, lives, shielded)

enum class UiPhase : u8 { Idle, Active, Done };
MCE_ENUM(UiPhase, Idle, Active, Done)
struct UiMission {
    UiPhase phase = UiPhase::Idle;
};
MCE_COMPONENT(UiMission, phase)

// The built-in font as a headless Font (no renderer; the set id is a CPU-only sprite set).
struct Fixture {
    render::FontMetrics metrics;
    Array<render::FontGlyph> glyphs;
    Array<u8> atlas;
    render::SpriteSets sets;
    render::Font font;
    Skin skin;

    Fixture() {
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

UpdateContext frame(u32 w = 1280, u32 h = 720, f32 scale = 1.0f) {
    UpdateContext c;
    c.viewport = rhi::Extent2D{w, h};
    c.ui_scale = scale;
    c.dt = 1.0 / 60.0;
    return c;
}

bool has_event(const Document& d, UiEvent::Kind k, WidgetId w) {
    for (const UiEvent& e : d.events())
        if (e.kind == k && e.widget == w) return true;
    return false;
}
u32 count_events(const Document& d, UiEvent::Kind k) {
    u32 n = 0;
    for (const UiEvent& e : d.events()) n += e.kind == k ? 1u : 0u;
    return n;
}

}  // namespace

TEST_CASE("ui: the built-in font covers printable ASCII at 13 px") {
    Fixture f;
    CHECK(f.glyphs.size() == 95);
    CHECK(f.glyphs[0].codepoint == 32);
    CHECK(f.glyphs.back().codepoint == 126);
    CHECK(f.metrics.pixel_height == 13.0f);
    const vec2 sz = render::measure_text(f.font.face, "HELLO", {});
    CHECK(sz.x == 35.0f);  // 5 × 7 px advance
    // 'A' has ink; ' ' has none.
    u32 ink = 0;
    const render::FontGlyph& a = f.glyphs['A' - 32];
    for (u32 y = a.y0; y < a.y1; ++y)
        for (u32 x = a.x0; x < a.x1; ++x) ink += f.atlas[y * f.metrics.atlas_width + x] ? 1u : 0u;
    CHECK(ink > 10);
}

TEST_CASE("ui: anchors resolve to pixel-snapped rects at ui scale 1, 1.5 and 2") {
    Fixture f;
    for (const f32 scale : {1.0f, 1.5f, 2.0f}) {
        CAPTURE(scale);
        Document d;
        d.set_skin(f.skin);
        Builder b(d, d.root());
        const WidgetId fill = b.panel("fill").fill().id();
        const WidgetId point = b.panel("point").at({0.5f, 0.5f}, {101, 51}).id();
        Anchors stretch;
        stretch.min = {0, 1};
        stretch.max = {1, 1};
        stretch.offset_min = {10, -40};
        stretch.offset_max = {-10, -10};
        const WidgetId bar = b.panel("bar").anchors(stretch).id();
        d.update(frame(1280, 720, scale));

        CHECK(d.rect(fill).min == vec2(0, 0));
        CHECK(d.rect(fill).max == vec2(1280, 720));
        const Rect p = d.rect(point);
        CHECK(p.min.x == std::floor(640.0f - 50.5f * scale + 0.5f));
        CHECK(p.max.x == std::floor(640.0f + 50.5f * scale + 0.5f));
        CHECK(p.min.x == std::floor(p.min.x));  // whole pixels
        CHECK(p.min.y == std::floor(p.min.y));
        const Rect r = d.rect(bar);
        CHECK(r.min.x == 10.0f * scale);
        CHECK(r.max.x == 1280.0f - 10.0f * scale);
        CHECK(r.min.y == 720.0f - 40.0f * scale);
        CHECK(r.max.y == 720.0f - 10.0f * scale);
    }
}

TEST_CASE("ui: safe insets shrink the root, so edge anchors clear a notch (M-MOBILE)") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    const WidgetId fill = b.panel("fill").fill().id();
    Anchors top_bar;
    top_bar.min = {0, 0};
    top_bar.max = {1, 0};
    top_bar.offset_max = {0, 40};
    const WidgetId bar = b.panel("bar").anchors(top_bar).id();
    d.update(frame(2532, 1170));
    CHECK(d.rect(bar).min == vec2(0, 0));
    const u64 passes = d.stats().layout_passes;

    UpdateContext c = frame(2532, 1170);
    c.safe_insets = SafeInsets{141, 0, 141, 63};  // a landscape phone: notch sides and home indicator
    d.update(c);
    CHECK(d.stats().layout_passes == passes + 1);  // insets changing re-lays out
    CHECK(d.rect(fill).min == vec2(141, 0));
    CHECK(d.rect(fill).max == vec2(2532 - 141, 1170 - 63));
    CHECK(d.rect(bar).min.x == 141.f);
    CHECK(d.rect(bar).max.x == 2532.f - 141.f);
    d.update(c);
    CHECK(d.stats().layout_passes == passes + 1);  // unchanged: no layout
}

TEST_CASE("ui: a point anchor with no size takes the preferred size around its pivot") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    const WidgetId label = b.text("label", "SCORE").at({0.5f, 0.0f}, {0, 0}).id();
    d.update(frame());
    const Rect r = d.rect(label);
    CHECK(r.size().x == 35.0f);
    CHECK(r.size().y == 14.0f);  // ascent 10 + descent 4
    CHECK(r.center().x == doctest::Approx(640.0f).epsilon(0.01));
}

TEST_CASE("ui: a stack of three labelled buttons sizes itself to its text") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    StackLayout sl;
    sl.spacing = 8;
    sl.padding = 4;
    Builder col = b.stack("menu", sl).at({0.5f, 0.5f}, {0, 0});
    const WidgetId a = col.button("a", "RESUME").id();
    const WidgetId bb = col.button("b", "OPTIONS").id();
    const WidgetId c = col.button("c", "QUIT TO MENU").id();
    d.update(frame());

    // The widest label (12 chars × 7 px + 32 padding) sets the column width; Stretch gives every
    // button that width.
    const f32 widest = 12 * 7 + 32;
    CHECK(d.rect(col.id()).size().x == widest + 8);
    CHECK(d.rect(a).size().x == widest);
    CHECK(d.rect(c).size().x == widest);
    // Heights: text 14 + 14 padding each, spacing 8 between, padding 4 around.
    CHECK(d.rect(a).size().y == 28.0f);
    CHECK(d.rect(col.id()).size().y == 3 * 28.0f + 2 * 8 + 8);
    CHECK(d.rect(bb).min.y == d.rect(a).max.y + 8);
    CHECK(d.rect(c).min.y == d.rect(bb).max.y + 8);
    CHECK(d.rect(a).min.y == d.rect(col.id()).min.y + 4);
}

TEST_CASE("ui: a 3x3 grid fills row-major") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    GridLayout gl;
    gl.columns = 3;
    gl.cell = {40, 30};
    gl.spacing = {10, 5};
    Builder g = b.grid("grid", gl).at({0, 0}, {0, 0});
    Anchors tl;
    tl.pivot = {0, 0};
    g.anchors(tl);
    WidgetId cells[9];
    for (u32 i = 0; i < 9; ++i) {
        char name[8];
        std::snprintf(name, sizeof(name), "c%u", i);
        cells[i] = g.button(name, "").id();
    }
    d.update(frame());
    CHECK(d.rect(g.id()).size() == vec2(140, 100));
    for (u32 i = 0; i < 9; ++i) {
        CAPTURE(i);
        const u32 row = i / 3;
        CHECK(d.rect(cells[i]).min == vec2(f32(i % 3) * 50, f32(row) * 35));
        CHECK(d.rect(cells[i]).size() == vec2(40, 30));
    }
}

TEST_CASE("ui: a static document does no layout work after its first frame") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    d.set_modal(false);
    Builder b(d, d.root());
    b.text("hp", "HP 100").at({0, 0}, {0, 0});
    b.panel("bar").at({0.5f, 1}, {300, 20});
    d.update(frame());
    CHECK(d.stats().layout_passes == 1);
    for (int i = 0; i < 120; ++i) d.update(frame());
    CHECK(d.stats().layout_passes == 1);
    d.set_text(d.find("hp"), "HP 100");  // same text: still clean
    d.update(frame());
    CHECK(d.stats().layout_passes == 1);
    d.set_text(d.find("hp"), "HP 99");
    d.update(frame());
    CHECK(d.stats().layout_passes == 2);
    d.update(frame(1920, 1080));  // a resize relayouts once
    d.update(frame(1920, 1080));
    CHECK(d.stats().layout_passes == 3);
}

namespace {
struct Grid3 {
    Fixture f;
    Document d;
    WidgetId cells[9];
    Grid3() {
        d.set_skin(f.skin);
        Builder b(d, d.root());
        Builder g = b.grid("grid", GridLayout{3, {60, 40}, {8, 8}}).at({0.5f, 0.5f}, {0, 0});
        for (u32 i = 0; i < 9; ++i) {
            char name[8];
            std::snprintf(name, sizeof(name), "c%u", i);
            cells[i] = g.button(name, "X").id();
        }
    }
    u32 focused_index() const {
        for (u32 i = 0; i < 9; ++i)
            if (d.focused() == cells[i]) return i;
        return 99;
    }
};

// Moves one step: the direction is held for one frame, then released.
void press(Document& d, vec2 move) {
    input::InputState in;
    in.move = move;
    UpdateContext c = frame();
    c.actions = &in;
    d.update(c);
    in.move = {};
    d.update(c);
}
}  // namespace

TEST_CASE("ui: geometric navigation reaches every cell of a 3x3 grid from each corner") {
    for (const u32 corner : {0u, 2u, 6u, 8u}) {
        CAPTURE(corner);
        Grid3 g;
        g.d.set_default_focus(g.cells[corner]);
        g.d.update(frame());
        REQUIRE(g.focused_index() == corner);
        bool seen[9] = {};
        seen[corner] = true;
        // Snake: sweep the row, step a row, sweep back (directions picked from the corner).
        const f32 sx = corner % 3 == 0 ? 1.0f : -1.0f;
        const f32 sy = corner / 3 == 0 ? -1.0f : 1.0f;  // move is +y up; rows grow downwards
        f32 dir = sx;
        for (int row = 0; row < 3; ++row) {
            for (int k = 0; k < 2; ++k) {
                press(g.d, {dir, 0});
                seen[g.focused_index() < 9 ? g.focused_index() : 0] = true;
            }
            if (row < 2) {
                press(g.d, {0, sy});
                seen[g.focused_index() < 9 ? g.focused_index() : 0] = true;
            }
            dir = -dir;
        }
        for (u32 i = 0; i < 9; ++i) CHECK(seen[i]);
    }
}

TEST_CASE("ui: navigation stops at the edge and explicit edges override geometry") {
    Grid3 g;
    g.d.update(frame());
    CHECK(g.focused_index() == 0);  // first interactable in document order
    press(g.d, {-1, 0});
    CHECK(g.focused_index() == 0);  // nothing to the left
    press(g.d, {0, -1});
    CHECK(g.focused_index() == 3);  // down
    g.d.set_nav(g.cells[3], NavEdges{{}, {}, {}, g.cells[8]});
    press(g.d, {1, 0});
    CHECK(g.focused_index() == 8);  // the authored right edge wins over the geometric 4
}

TEST_CASE("ui: holding a direction repeats after the delay") {
    Grid3 g;
    g.d.update(frame());
    input::InputState in;
    in.move = {1, 0};
    UpdateContext c = frame();
    c.actions = &in;
    g.d.update(c);
    CHECK(g.focused_index() == 1);
    for (int i = 0; i < 10; ++i) g.d.update(c);  // < 0.4 s
    CHECK(g.focused_index() == 1);
    for (int i = 0; i < 20; ++i) g.d.update(c);
    CHECK(g.focused_index() == 2);
}

TEST_CASE("ui: the named UI_Navigate action drives navigation when the map defines it") {
    Grid3 g;
    g.d.update(frame());
    input::InputState in;
    in.action_count = 1;
    in.action_names[0] = StringId(kNavigateAction);
    in.actions[0].x = 1;
    in.actions[0].held = true;
    in.move = {0, -1};  // ignored: the named action wins
    UpdateContext c = frame();
    c.actions = &in;
    g.d.update(c);
    CHECK(g.focused_index() == 1);
}

TEST_CASE("ui: confirm held for 30 ticks emits one Confirmed") {
    Grid3 g;
    g.d.update(frame());
    input::InputState in;
    UpdateContext c = frame();
    c.actions = &in;
    u32 confirmed = 0;
    for (int t = 0; t < 30; ++t) {
        in.confirm_pressed = t == 0;  // the action layer reports the edge once
        g.d.update(c);
        confirmed += count_events(g.d, UiEvent::Confirmed);
    }
    CHECK(confirmed == 1);
    in.confirm_pressed = false;
    in.cancel_pressed = true;
    g.d.update(c);
    CHECK(has_event(g.d, UiEvent::Cancelled, g.d.root()));
}

TEST_CASE("ui: a pointer press focuses; navigation continues from there") {
    Grid3 g;
    g.d.update(frame());
    const vec2 p = g.d.rect(g.cells[4]).center();
    UpdateContext c = frame();
    c.pointer = p;
    g.d.update(c);
    CHECK(g.d.hovered() == g.cells[4]);
    CHECK(g.focused_index() == 0);  // hover is not focus
    c.pointer_pressed = c.pointer_down = true;
    g.d.update(c);
    CHECK(g.focused_index() == 4);
    CHECK(g.d.state(g.cells[4]) == WidgetState::Pressed);
    c.pointer_pressed = c.pointer_down = false;
    c.pointer_released = true;
    g.d.update(c);
    CHECK(has_event(g.d, UiEvent::Confirmed, g.cells[4]));
    press(g.d, {1, 0});
    CHECK(g.focused_index() == 5);
}

TEST_CASE("ui: a release off the pressed widget does not click; blocks_pointer shields what is below") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    const WidgetId btn = b.button("go", "GO").at({0.5f, 0.5f}, {100, 40}).id();
    d.update(frame());
    UpdateContext c = frame();
    c.pointer = d.rect(btn).center();
    c.pointer_pressed = c.pointer_down = true;
    d.update(c);
    c.pointer_pressed = c.pointer_down = false;
    c.pointer = {5, 5};
    c.pointer_released = true;
    d.update(c);
    CHECK(count_events(d, UiEvent::Confirmed) == 0);

    b.panel("modal").fill().blocks_pointer();
    c = frame();
    c.pointer = d.rect(btn).center();
    d.update(c);
    CHECK(d.hovered() == d.find("modal"));
}

TEST_CASE("ui: a finger taps a widget like a mouse click (apply_touch_pointer)") {
    Grid3 g;
    g.d.update(frame());
    UpdateContext c = frame();
    const vec2 p = g.d.rect(g.cells[4]).center();
    input::InputState s;
    s.touch_count = 1;
    s.touches[0].pos = vec2(p.x / static_cast<f32>(c.viewport.width), p.y / static_cast<f32>(c.viewport.height));
    s.touches[0].held = s.touches[0].pressed = true;
    apply_touch_pointer(c, s);
    CHECK(c.pointer.x == doctest::Approx(p.x));
    CHECK(c.pointer.y == doctest::Approx(p.y));
    g.d.update(c);
    CHECK(g.focused_index() == 4);
    s.touches[0].held = s.touches[0].pressed = false;
    s.touches[0].released = true;
    apply_touch_pointer(c, s);
    CHECK_FALSE(c.pointer_down);
    g.d.update(c);
    CHECK(has_event(g.d, UiEvent::Confirmed, g.cells[4]));

    // No touch: the host's mouse pointer stays.
    UpdateContext m = frame();
    m.pointer = {3, 4};
    apply_touch_pointer(m, input::InputState{});
    CHECK(m.pointer.x == 3.f);
}

TEST_CASE("ui: a non-modal document never takes focus") {
    Grid3 g;
    g.d.set_modal(false);
    g.d.update(frame());
    CHECK(g.d.focused().is_null());
    press(g.d, {1, 0});
    CHECK(g.d.focused().is_null());
}

TEST_CASE("ui: sliders and toggles bind two-way to convars; cheats are refused") {
    Fixture f;
    cv_test_ui_volume.reset();
    cv_test_ui_flag.reset();
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    Builder col = b.stack("opts").at({0.5f, 0.5f}, {300, 0});
    const WidgetId vol = col.slider("volume", 0.0f, 1.0f, 0.1f).bind_convar("test_ui.volume").id();
    const WidgetId flag = col.toggle("flag", "FLAG").bind_convar("test_ui.flag").id();
    CHECK_FALSE(d.bind_convar(flag, "test_ui.cheat"));
    CHECK_FALSE(d.bind_convar(flag, "test_ui.missing"));
    d.update(frame());
    CHECK(d.value(vol) == doctest::Approx(0.5f));
    CHECK(d.value(flag) == 0.0f);

    // UI -> convar: the slider steps right, the toggle confirms.
    d.focus(vol);
    press(d, {1, 0});
    CHECK(cv_test_ui_volume.get_float() == doctest::Approx(0.6f));
    d.focus(flag);
    input::InputState in;
    in.confirm_pressed = true;
    UpdateContext c = frame();
    c.actions = &in;
    d.update(c);
    CHECK(has_event(d, UiEvent::ValueChanged, flag));
    CHECK(cv_test_ui_flag.get_bool());

    // Convar -> UI: a console change shows on the next update without an event.
    console::execute("test_ui.volume 0.2");
    d.update(frame());
    CHECK(d.value(vol) == doctest::Approx(0.2f));
    CHECK(count_events(d, UiEvent::ValueChanged) == 0);
    cv_test_ui_volume.reset();
    cv_test_ui_flag.reset();
}

TEST_CASE("ui: removal is deferred and invalidates ids; find walks paths") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    Builder menu = b.panel("pause");
    Builder col = menu.stack("buttons");
    const WidgetId resume = col.button("resume", "RESUME").id();
    CHECK(d.find("pause/buttons/resume") == resume);
    CHECK(d.find("pause/nope").is_null());
    d.update(frame());
    CHECK(d.focused() == resume);
    const u32 before = d.stats().widgets;
    d.remove(menu.id());
    CHECK(d.valid(resume));  // until the next update
    d.update(frame());
    CHECK_FALSE(d.valid(resume));
    CHECK_FALSE(d.valid(menu.id()));
    CHECK(d.focused().is_null());
    CHECK(d.stats().widgets == before - 3);
    // The freed slot is reused with a new generation: the old id stays dead.
    const WidgetId again = b.button("again", "AGAIN").id();
    const bool reused = again.index == resume.index || again.index == menu.id().index || again.index == col.id().index;
    CHECK(reused);
    CHECK_FALSE(d.valid(resume));
}

TEST_CASE("ui: the console edits, submits and recalls history") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    const WidgetId con = b.console("console").fill().id();
    d.update(frame());
    REQUIRE(d.focused() == con);
    UpdateContext c = frame();
    c.text_input = "r.vsyncx";
    d.update(c);
    c.text_input = {};
    c.key_backspace = true;
    d.update(c);
    CHECK(d.console_input(con) == "r.vsync");
    c.key_backspace = false;
    c.text_input = " 0";
    c.key_enter = true;
    d.update(c);
    CHECK(has_event(d, UiEvent::Submitted, con));
    CHECK(d.console_submitted() == "r.vsync 0");
    CHECK(d.console_input(con).empty());
    c = frame();
    c.key_up = true;
    d.update(c);
    CHECK(d.console_input(con) == "r.vsync 0");
    c.key_up = false;
    c.key_down = true;
    d.update(c);
    CHECK(d.console_input(con).empty());

    for (u32 i = 0; i < 300; ++i) d.console_push_line(con, "line");
    CHECK(d.console_line_count(con) == 256);  // the scrollback is a ring
}

TEST_CASE("ui: a menu draws through SpriteBatch in two groups, text above its panel") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    b.panel("bg").fill().tint({0, 0, 0, 0.6f});
    Builder col = b.stack("buttons").at({0.5f, 0.5f}, {320, 0});
    col.button("resume", "RESUME");
    col.button("options", "OPTIONS");
    col.toggle("vsync", "VSYNC", true);
    col.slider("volume", 0, 1);
    d.update(frame());

    render::SpriteBatch batch(f.sets, mem::heap());
    batch.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
    d.submit(batch);
    batch.build();
    CHECK(d.stats().quads > 0);
    CHECK(batch.stats().groups == 2);
    REQUIRE(batch.groups().size() == 2);
    CHECK(batch.groups()[0].set == f.skin.set);  // quads first ...
    CHECK(batch.groups()[1].set == f.font.set);  // ... then text, whatever the set ids
}

TEST_CASE("ui: a style's text outline and shadow add copies of the glyphs (M-UI-DATA 4)") {
    Fixture f;
    const auto glyph_quads = [&](const Style* look) {
        Document d;
        d.set_skin(f.skin);
        Builder b(d, d.root());
        b.text("t", "HELLO").at({0.5f, 0.5f}, {200, 40});
        if (look) d.set_style(d.find("t"), WidgetState::Normal, *look);
        d.update(frame());
        render::SpriteBatch batch(f.sets, mem::heap());
        batch.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
        d.submit(batch);
        batch.build();
        return batch.stats().quads;
    };
    const u32 plain = glyph_quads(nullptr);
    REQUIRE(plain > 0);
    Style look;
    look.draw_quad = false;
    look.text_outline = 2.0f;
    look.text_shadow = {2, 2};
    CHECK(glyph_quads(&look) == plain * 10);  // shadow + 8 outline taps + fill
    look.text_outline_color.a = 0.0f;         // an invisible outline is skipped
    CHECK(glyph_quads(&look) == plain * 2);
}

TEST_CASE("ui: a health bar bound to UiHealth/hp updates on change and is untouched otherwise") {
    Fixture f;
    World world;
    const Entity player = world.spawn();
    world.set(player, UiHealth{});
    Document d;
    d.set_skin(f.skin);
    d.set_modal(false);
    Builder b(d, d.root());
    const WidgetId bar = b.slider("hp_bar", 0.0f, 100.0f, 0.0f).at({0.5f, 1.0f}, {300, 16}).id();
    d.set_interactable(bar, false);
    const WidgetId label = b.text("hp", "").at({0.0f, 0.0f}, {0, 0}).id();
    const WidgetId lives = b.text("lives", "").at({1.0f, 0.0f}, {0, 0}).id();
    CHECK(d.bind_property(bar, world, player, "UiHealth/hp"));
    CHECK(d.bind_property(label, world, player, "UiHealth/hp", "HP %.0f"));
    CHECK(d.bind_property(lives, world, player, "UiHealth/lives", "x%.0f"));
    CHECK_FALSE(d.bind_property(label, world, player, "UiHealth/nope"));
    CHECK_FALSE(d.bind_property(label, world, player, "NotAType/hp"));
    CHECK_FALSE(d.bind_property(label, world, player, "hp"));
    d.update(frame());
    CHECK(d.value(bar) == 100.0f);
    CHECK(d.text(label) == "HP 100");
    CHECK(d.text(lives) == "x3");
    const u64 passes = d.stats().layout_passes, updates = d.stats().binding_updates;

    for (int i = 0; i < 30; ++i) d.update(frame());  // nothing changed: no work
    CHECK(d.stats().layout_passes == passes);
    CHECK(d.stats().binding_updates == updates);

    world.try_get_mut<UiHealth>(player)->hp = 37.5f;
    d.update(frame());
    CHECK(d.value(bar) == 37.5f);
    CHECK(d.text(label) == "HP 38");
    CHECK(d.text(lives) == "x3");
    CHECK(d.stats().binding_updates == updates + 2);  // the bar and the label, not lives
    CHECK(d.stats().layout_passes == passes + 1);

    world.destroy(player);  // a dead entity reads nothing; the widget keeps its last value
    d.update(frame());
    CHECK(d.text(label) == "HP 38");
    d.remove(label);  // the binding goes with its widget
    d.update(frame());
}

TEST_CASE("ui: a panel bound to a field shows while the value is within its range (wanted stars, mission state)") {
    Fixture f;
    World world;
    const Entity player = world.spawn();
    world.set(player, UiHealth{});
    Document d;
    d.set_skin(f.skin);
    d.set_modal(false);
    Builder b(d, d.root());
    const WidgetId low = b.panel("star1").at({0.0f, 0.0f}, {8, 8}).id();
    const WidgetId high = b.panel("star3").at({0.0f, 0.0f}, {8, 8}).id();
    d.set_range(low, 1.0f, 5.0f, 1.0f);
    d.set_range(high, 3.0f, 5.0f, 1.0f);
    CHECK(d.bind_property(low, world, player, "UiHealth/lives"));
    CHECK(d.bind_property(high, world, player, "UiHealth/lives"));
    d.update(frame());  // lives 3: both shown
    CHECK(d.visible_self(low));
    CHECK(d.visible_self(high));
    world.try_get_mut<UiHealth>(player)->lives = 2;
    d.update(frame());
    CHECK(d.visible_self(low));
    CHECK_FALSE(d.visible_self(high));
    world.try_get_mut<UiHealth>(player)->lives = 0;
    d.update(frame());
    CHECK_FALSE(d.visible_self(low));

    // Above `hi` it hides again, and an enum field reads as its number: a banner shown only while the phase is Active (1).
    world.try_get_mut<UiHealth>(player)->lives = 6;
    d.update(frame());
    CHECK_FALSE(d.visible_self(low));
    const Entity mission = world.spawn();
    world.set(mission, UiMission{});
    const WidgetId banner = b.panel("banner").at({0.0f, 0.0f}, {8, 8}).id();
    d.set_range(banner, 1.0f, 1.0f, 0.0f);
    CHECK(d.bind_property(banner, world, mission, "UiMission/phase"));
    d.update(frame());
    CHECK_FALSE(d.visible_self(banner));
    world.try_get_mut<UiMission>(mission)->phase = UiPhase::Active;
    d.update(frame());
    CHECK(d.visible_self(banner));
    world.try_get_mut<UiMission>(mission)->phase = UiPhase::Done;
    d.update(frame());
    CHECK_FALSE(d.visible_self(banner));
}

TEST_CASE("ui: the developer console mirrors the log, runs convars and closes on cancel") {
    Fixture f;
    cv_test_ui_volume.reset();
    DevConsole con;
    con.init(f.skin);
    CHECK_FALSE(con.is_open());
    MCE_LOG_WARN("test_ui: a line for the console");
    con.update(frame());  // closed: still pulls the log
    const u32 lines = con.document().console_line_count(con.widget());
    CHECK(lines >= 2);  // the banner + the warning

    con.set_open(true);
    UpdateContext c = frame();
    c.text_input = "test_ui.volume 0.25";
    c.key_enter = true;
    con.update(c);
    CHECK(cv_test_ui_volume.get_float() == doctest::Approx(0.25f));
    c = frame();
    c.text_input = "no_such_thing 1";
    c.key_enter = true;
    con.update(c);
    c = frame();
    c.text_input = "help";
    c.key_enter = true;
    con.update(c);
    CHECK(con.document().console_line_count(con.widget()) > lines + 4);  // echoes, the error, the listing
    c = frame();
    c.text_input = "clear";
    c.key_enter = true;
    con.update(c);
    CHECK(con.document().console_line_count(con.widget()) == 0);

    input::InputState in;
    in.cancel_pressed = true;
    c = frame();
    c.actions = &in;
    con.update(c);
    CHECK_FALSE(con.is_open());
    cv_test_ui_volume.reset();
}

// ---- M-ACCESS: text scale and high contrast ----------------------------------------------------

namespace {
// WCAG 2 contrast ratio of two sRGB colors (alpha ignored).
f32 contrast_ratio(Color a, Color b) {
    const auto lum = [](Color c) {
        const auto lin = [](f32 v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
        return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
    };
    const f32 la = lum(a), lb = lum(b);
    return (la > lb ? la + 0.05f : lb + 0.05f) / (la > lb ? lb + 0.05f : la + 0.05f);
}
}  // namespace

TEST_CASE("M-ACCESS: text scale grows the text and the widgets sized by it, and relayouts on change") {
    Fixture f;
    Document d;
    d.set_skin(f.skin);
    Builder b(d, d.root());
    Builder col = b.stack("menu", StackLayout{}).at({0.5f, 0.5f}, {0, 0});
    const WidgetId a = col.button("a", "RESUME").id();
    d.update(frame());
    const vec2 normal = d.rect(a).size();
    UpdateContext big = frame();
    big.text_scale = 2.0f;
    d.update(big);
    const vec2 scaled = d.rect(a).size();
    CHECK(scaled.x > normal.x + 6 * 7 - 1);  // 6 glyphs, each at least 7 px wider
    CHECK(scaled.y > normal.y + 10);
    // The ui scale is untouched: the padding stays as authored.
    CHECK(scaled.x - 2 * (normal.x - 32) == doctest::Approx(32.0f));
    d.update(frame());
    CHECK(d.rect(a).size().x == normal.x);
}

TEST_CASE("M-ACCESS: the high-contrast theme keeps text at 7:1 or more and leaves the document's styles") {
    const ThemeData& hc = high_contrast_theme();
    for (WidgetKind k : {WidgetKind::Button, WidgetKind::Toggle, WidgetKind::Slider, WidgetKind::Panel, WidgetKind::Console})
        for (u32 s = 0; s < kWidgetStateCount; ++s) {
            if (s == static_cast<u32>(WidgetState::Disabled)) continue;
            const Style& st = hc.styles[static_cast<u32>(k)][s];
            INFO("kind " << static_cast<u32>(k) << " state " << s);
            CHECK(st.tint.a == 1.0f);
            CHECK(contrast_ratio(st.text_color, st.tint) >= 7.0f);
        }
    const Style& disabled = hc.styles[static_cast<u32>(WidgetKind::Button)][static_cast<u32>(WidgetState::Disabled)];
    CHECK(contrast_ratio(disabled.text_color, disabled.tint) >= 4.5f);

    // A themed style keeps its sprite, slice and text size; a bare label gains an outline.
    Style mine;
    mine.tint = {0.3f, 0.6f, 0.9f, 0.5f};
    mine.text_color = {0.5f, 0.5f, 0.5f, 1};
    mine.text_size = 22.0f;
    mine.sprite_region = 7;
    const Style b = high_contrast_style(WidgetKind::Button, WidgetState::Focused, mine);
    CHECK(b.text_size == 22.0f);
    CHECK(b.sprite_region == 7u);
    CHECK(contrast_ratio(b.text_color, b.tint) >= 7.0f);
    mine.draw_quad = false;
    const Style t = high_contrast_style(WidgetKind::Text, WidgetState::Normal, mine);
    CHECK(t.text_outline >= 1.0f);
    CHECK(t.text_outline_color.a == 1.0f);

    // Drawing with high contrast outlines a bare label (8 more copies of its glyphs) and never
    // rewrites the document's own styles.
    Fixture f;
    const auto glyph_quads = [&](bool contrast) {
        Document d;
        d.set_skin(f.skin);
        Builder root(d, d.root());
        root.text("t", "HELLO").at({0.5f, 0.5f}, {200, 40});
        UpdateContext c = frame();
        c.high_contrast = contrast;
        d.update(c);
        render::SpriteBatch batch(f.sets, mem::heap());
        batch.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
        d.submit(batch);
        batch.build();
        CHECK(d.style(d.find("t"), WidgetState::Normal).text_outline == 0.0f);
        return batch.stats().quads;
    };
    const u32 plain = glyph_quads(false);
    REQUIRE(plain > 0);
    CHECK(glyph_quads(true) == plain * 9);
}
