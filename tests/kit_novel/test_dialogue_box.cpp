// The default NPC dialogue box (mce/kit/novel_ui.h, #154) as a view of the sim: it shows the
// conversation the DialogueInput entity runs, typed out as the sim reveals it, with the options
// highlighted and gated as the sim sees them; it hides when the conversation ends. Headless: the
// built-in font on a CPU-only sprite set, no renderer.
#include <doctest/doctest.h>

#include "mce/input/ecs.h"
#include "mce/kit/novel.h"
#include "mce/kit/novel_ui.h"
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
