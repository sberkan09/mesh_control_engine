// The default NPC dialogue box (mce/kit/novel_ui.h, #154) as a view of the sim: it shows the
// conversation the DialogueInput entity runs, typed out as the sim reveals it, with the options
// highlighted and gated as the sim sees them; it hides when the conversation ends. Headless: the
// built-in font on a CPU-only sprite set, no renderer.
#include <doctest/doctest.h>

#include <cmath>

#include "mce/input/ecs.h"
#include "mce/kit/components.h"
#include "mce/kit/novel.h"
#include "mce/kit/novel_ui.h"
#include "mce/loc/loc.h"
#include "mce/render/sprite_batch.h"
#include "novel_fixture.h"

using namespace mce;
namespace nv = mce::novel;
namespace fx = novel_fixture;

namespace {

struct Skin {
    render::FontMetrics metrics;
    Array<render::FontGlyph> glyphs;
    Array<u8> atlas;
    render::SpriteSets sets;
    render::Font font;
    ui::Skin skin;
    Skin() {
        ui::builtin_font(metrics, glyphs, atlas);
        render::SpriteSetDesc fd;
        fd.coverage = true;
        font.face = render::FontFace(metrics, glyphs.span(), {});
        font.set = sets.create(fd);
        const render::SpriteRegion white{0, 0.25f, 0.25f, 0.75f, 0.75f, 1.0f, 1.0f};
        render::SpriteSetDesc wd;
        wd.regions = Span<const render::SpriteRegion>(&white, 1);
        skin.set = sets.create(wd);
        skin.font = &font;
    }
};

struct Game {
    World w;
    Entity mira;
    u64 tick = 0;
    Game() {
        input::install(w);
        REQUIRE(static_cast<bool>(nv::install_dialogue(w, fx::kProgram)));
        mira = w.spawn();
        w.add(mira, nv::Dialogue{});
        w.add(mira, nv::DialogueInput{});
        step();
        step();
    }
    void step(bool confirm = false, f32 move_y = 0.0f) {
        input::InputState in;
        in.confirm_pressed = confirm;
        in.move.y = move_y;
        in.tick = tick;
        input::publish(w, in);
        w.tick(1.0 / 60.0, tick++);
    }
};

ui::UpdateContext frame() {
    ui::UpdateContext c;
    c.viewport = rhi::Extent2D{1280, 720};
    c.dt = 1.0 / 60.0;
    return c;
}

std::string str(StringView s) { return std::string(s.data(), s.size()); }

}  // namespace

TEST_CASE("dialogue box: hidden without a conversation, then the page typed out, the choices, hidden at the end") {
    Skin skin;
    Game g;
    nv::DialogueBox box;
    const ui::Layer* layer = box.layer();
    REQUIRE(layer->init);  // the player's call; the box needs only the skin
    box.init(skin.skin);
    layer->update(layer->user, g.w, frame());
    CHECK(!box.visible());

    REQUIRE(nv::start_dialogue(g.w, g.mira, "mira"));
    g.step();
    g.step();
    box.update(g.w, frame());
    CHECK(box.visible());
    CHECK(str(box.speaker()) == "Mira");
    const std::string typing = str(box.line(0));
    CHECK(!typing.empty());
    CHECK(typing.size() < std::string("Hello there.").size());
    CHECK(std::string("Hello there.").rfind(typing, 0) == 0);  // a prefix
    CHECK(box.option_count() == 0);

    g.step(true);  // Enter: the rest of the page
    box.update(g.w, frame());
    CHECK(str(box.line(0)) == "Hello there.");
    g.step();
    g.step(true);  // Enter: on to the choice
    box.update(g.w, frame());
    REQUIRE(box.option_count() == 3);
    CHECK(str(box.option(0)) == "Ask about the boat");
    CHECK(str(box.option(1)) == "Show the letter");
    CHECK(str(box.line(0)) == "Hello there.");  // the page stays up under the choice

    // It draws quads and text while up.
    render::SpriteBatch batch(skin.sets, mem::heap());
    batch.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
    layer->submit(layer->user, batch, -40);
    CHECK(box.document().stats().quads > 0);

    g.step(false, -1.0f);  // down to "Show the letter" (gated: greyed, not taken)
    g.step();
    g.step(true);
    box.update(g.w, frame());
    CHECK(box.option_count() == 3);
    g.step(false, -1.0f);  // down to "Leave"
    g.step();
    g.step(true);
    box.update(g.w, frame());
    CHECK(!box.visible());  // the conversation ended
}

TEST_CASE("dialogue box: the page, the choices and the name tag in the player's language, switched at run time") {
    Skin skin;
    Game g;
    loc::Localization l10n;
    for (const char* tag : {"en", "tr"}) {
        loc::StringTable t;
        t.set_locale(tag);
        REQUIRE(t.add("menu.play", "-"));
        l10n.add_table(std::move(t));
    }
    REQUIRE(l10n.set_locale("tr"));
    g.w.set_resource(std::move(l10n));
    nv::DialogueBox box;
    box.init(skin.skin);

    REQUIRE(nv::start_dialogue(g.w, g.mira, "mira"));
    g.step();
    g.step();
    box.update(g.w, frame());
    const std::string typing = str(box.line(0));
    CHECK(std::string("Merhaba.").rfind(typing, 0) == 0);  // typing the Turkish line, at the base page's pace
    CHECK(typing.size() < std::string("Merhaba.").size());
    g.step(true);
    box.update(g.w, frame());
    CHECK(str(box.line(0)) == "Merhaba.");
    CHECK(str(box.speaker()) == "Mira");  // tr.csv has no row for her name: the base one

    // a language menu switches: the same page, in English on the next update, and back
    REQUIRE(g.w.resource<loc::Localization>().set_locale("en"));
    box.update(g.w, frame());
    CHECK(str(box.line(0)) == "Hello there.");
    REQUIRE(g.w.resource<loc::Localization>().set_locale("tr"));
    box.update(g.w, frame());
    CHECK(str(box.line(0)) == "Merhaba.");

    g.step();
    g.step(true);
    box.update(g.w, frame());
    REQUIRE(box.option_count() == 3);
    CHECK(str(box.option(0)) == "Tekneyi sor");
    CHECK(str(box.option(2)) == "Ayr\xC4\xB1l");
    REQUIRE(g.w.resource<loc::Localization>().set_locale("en"));
    box.update(g.w, frame());
    CHECK(str(box.option(0)) == "Ask about the boat");
    CHECK(str(box.line(0)) == "Hello there.");  // the page kept up under the choice follows too
}

TEST_CASE("dialogue box (M-MOBILE 11f): the sim hit-tests the rows the box draws, on a phone's safe area and UI scale") {
    Skin skin;
    Game g;
    kit::ViewInfo view;  // a landscape phone: a notch on the left, the home bar below, UI at 2x
    view.framebuffer_width = view.window_width = 2400;
    view.framebuffer_height = view.window_height = 1080;
    view.safe_left = 120;
    view.safe_bottom = 60;
    view.ui_scale = 2.0f;
    g.w.set_resource(view);
    nv::DialogueBox box;
    box.init(skin.skin);
    ui::UpdateContext ctx = frame();
    ctx.viewport = rhi::Extent2D{2400, 1080};
    ctx.safe_insets = ui::SafeInsets{120, 0, 0, 60};
    ctx.ui_scale = 2.0f;

    REQUIRE(nv::start_dialogue(g.w, g.mira, "mira"));
    nv::advance_dialogue(g.w, g.mira);
    box.update(g.w, ctx);
    REQUIRE(box.option_count() == 3);
    const nv::DialogueRects rects = nv::dialogue_rects(nv::dialogue_layout(g.w), view);
    auto same = [](const ui::Rect& drawn, const nv::DialogueRect& hit) {
        CHECK(std::fabs(drawn.min.x - hit.x0) <= 1.0f);
        CHECK(std::fabs(drawn.min.y - hit.y0) <= 1.0f);
        CHECK(std::fabs(drawn.max.x - hit.x1) <= 1.0f);
        CHECK(std::fabs(drawn.max.y - hit.y1) <= 1.0f);
    };
    ui::Document& doc = box.document();
    same(doc.rect(doc.find("box")), rects.box);
    for (u32 i = 0; i < 3; ++i) {
        const char path[4][13] = {"choices/row0", "choices/row1", "choices/row2", "choices/row3"};
        same(doc.rect(doc.find(StringView(path[i]))), rects.rows[i]);
    }

    // A finger on the first row as drawn: the sim picks it.
    const ui::Rect row0 = doc.rect(doc.find("choices/row0"));
    input::InputState in;
    in.touch_count = 1;
    in.touches[0].pos = {row0.center().x / 2400.0f, row0.center().y / 1080.0f};
    in.touches[0].origin = in.touches[0].pos;
    in.touches[0].held = in.touches[0].pressed = true;
    in.tick = g.tick;
    input::publish(g.w, in);
    g.w.tick(1.0 / 60.0, g.tick++);
    g.step();
    box.update(g.w, ctx);
    CHECK(box.option_count() == 0);
    CHECK(str(box.line(0)) != "Hello there.");  // "It sails at dawn." is typing

    // A game that sizes the box differently: the box and the hit test follow the world's layout.
    nv::DialogueLayout big;
    big.text_px = 40.0f;
    big.box_height = 260.0f;
    nv::set_dialogue_layout(g.w, big);
    box.update(g.w, ctx);
    same(doc.rect(doc.find("box")), nv::dialogue_rects(big, view).box);
}
