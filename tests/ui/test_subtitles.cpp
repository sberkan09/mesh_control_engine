// M-ACCESS step 1: the subtitle view. Which lines show (speech, sounds, the newest few), how they
// read ("Name: line", "[sound]"), where the boxes sit (centred, stacked up from the bottom of the
// safe area) and how the size, background and speaker settings change them. CPU only.

#include <doctest/doctest.h>

#include "mce/convar.h"
#include "mce/render/camera2d.h"
#include "mce/render/sprite_batch.h"
#include "mce/ui/subtitles.h"

using namespace mce;
using namespace mce::ui;

namespace {

struct SubFixture {
    render::FontMetrics metrics;
    Array<render::FontGlyph> glyphs;
    Array<u8> atlas;
    render::SpriteSets sets;
    render::Font font;
    Skin skin;

    SubFixture() {
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

    u32 quads(const SubtitleView& v) {
        render::SpriteBatch batch(sets, mem::heap());
        batch.begin(render::compute_screen_view(rhi::Extent2D{1280, 720}), render::PassId::Screen2D);
        v.submit(batch);
        batch.build();
        return batch.stats().quads;
    }
};

UpdateContext screen() {
    UpdateContext c;
    c.viewport = rhi::Extent2D{1280, 720};
    return c;
}

const SubtitleLine kLines[] = {
    {"Mira", "Who's there?", SubtitleKind::Speech},
    {"", "door slams", SubtitleKind::Sound},
    {"", "The wind picks up.", SubtitleKind::Speech},
    {"Ada", "Run!", SubtitleKind::Speech},
};

}  // namespace

TEST_CASE("M-ACCESS: subtitles show speech by default, sounds when captions are on, the newest last") {
    SubFixture f;
    SubtitleView v;
    v.set_skin(f.skin);
    SubtitleStyle st;
    v.update(Span<const SubtitleLine>(kLines, 4), screen(), st);
    REQUIRE(v.shown() == 3);  // max_lines 3: the oldest speech line drops; the sound is hidden
    CHECK(v.shown_text(0) == StringView("Mira: Who's there?"));
    CHECK(v.shown_text(1) == StringView("The wind picks up."));
    CHECK(v.shown_text(2) == StringView("Ada: Run!"));
    // Stacked up from the bottom, each centred on the screen.
    CHECK(v.box(2).max.y <= 720.0f - st.bottom + 1.0f);
    CHECK(v.box(1).max.y < v.box(2).min.y);
    CHECK(v.box(0).max.y < v.box(1).min.y);
    for (u32 i = 0; i < 3; ++i) CHECK(v.box(i).center().x == doctest::Approx(640.0f).epsilon(0.01));

    st.sounds = true;
    v.update(Span<const SubtitleLine>(kLines, 4), screen(), st);
    REQUIRE(v.shown() == 3);
    CHECK(v.shown_text(0) == StringView("[door slams]"));

    st.speech = false;
    v.update(Span<const SubtitleLine>(kLines, 4), screen(), st);
    REQUIRE(v.shown() == 1);
    CHECK(v.shown_text(0) == StringView("[door slams]"));

    st = SubtitleStyle{};
    st.speaker_names = false;
    v.update(Span<const SubtitleLine>(kLines, 4), screen(), st);
    CHECK(v.shown_text(2) == StringView("Run!"));
}

TEST_CASE("M-ACCESS: subtitle size, background and safe area") {
    SubFixture f;
    SubtitleView v;
    v.set_skin(f.skin);
    const SubtitleLine one[] = {{"Ada", "Run!", SubtitleKind::Speech}};
    SubtitleStyle st;
    v.update(Span<const SubtitleLine>(one, 1), screen(), st);
    const f32 h1 = v.box(0).size().y, w1 = v.box(0).size().x;
    const u32 with_box = f.quads(v);
    CHECK(with_box == 1 + 9);  // the box and the nine glyphs of "Ada: Run!"

    st.size = 2.0f;
    v.update(Span<const SubtitleLine>(one, 1), screen(), st);
    CHECK(v.box(0).size().y == doctest::Approx(2.0f * h1).epsilon(0.1));
    CHECK(v.box(0).size().x > 1.8f * w1);

    UpdateContext c = screen();
    c.text_scale = 2.0f;  // access.text_scale grows subtitles too
    v.update(Span<const SubtitleLine>(one, 1), c, SubtitleStyle{});
    CHECK(v.box(0).size().y == doctest::Approx(2.0f * h1).epsilon(0.1));

    st = SubtitleStyle{};
    st.background = 0.0f;  // no box: the glyphs are outlined (eight more copies each)
    v.update(Span<const SubtitleLine>(one, 1), screen(), st);
    CHECK(f.quads(v) == 9 * 9);

    c = screen();
    c.safe_insets.bottom = 50.0f;  // a home indicator
    v.update(Span<const SubtitleLine>(one, 1), c, SubtitleStyle{});
    CHECK(v.box(0).max.y <= 720.0f - 50.0f - SubtitleStyle{}.bottom + 1.0f);

    // A long line wraps inside the width instead of running off the screen.
    const SubtitleLine long_line[] = {{"Narrator",
                                       "The rain had not stopped for three days, and the river was higher than anyone in the "
                                       "village could remember, higher even than the year the mill washed away.",
                                       SubtitleKind::Speech}};
    v.update(Span<const SubtitleLine>(long_line, 1), screen(), SubtitleStyle{});
    CHECK(v.box(0).size().x <= 1280.0f * 0.8f + 1.0f);
    CHECK(v.box(0).size().y > 2.0f * h1);
}

TEST_CASE("M-ACCESS: subtitle_style reads the access convars") {
    SubtitleStyle st = subtitle_style();
    CHECK(st.speech);
    CHECK_FALSE(st.sounds);
    REQUIRE(console::execute("access.captions 1"));
    REQUIRE(console::execute("access.subtitle_size 9"));
    REQUIRE(console::execute("access.subtitle_background 0.25"));
    st = subtitle_style();
    CHECK(st.sounds);
    CHECK(st.size == 3.0f);  // clamped
    CHECK(st.background == 0.25f);
    ConVar::find(StringId("access.captions"))->reset();
    ConVar::find(StringId("access.subtitle_size"))->reset();
    ConVar::find(StringId("access.subtitle_background"))->reset();
}
