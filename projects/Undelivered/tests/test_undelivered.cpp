// Undelivered: the compiled script, the authored scene, and the game played through its real
// systems by the bot (five policies, five endings), a save reloaded mid-run, determinism. It loads
// the cooked pack the build makes from assets/ (undelivered_content), so it tests the scene as the
// editor MCP server saved it.
#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>

#include "mce/asset/asset_system.h"
#include "mce/audio/components.h"
#include "mce/ecs/save.h"
#include "mce/input/ecs.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"
#include "mce/physics/module.h"
#include "mce/scene/scene.h"
#include "story_data.h"
#include "undelivered.h"

using namespace mce;
using namespace undelivered;
namespace st = story;

namespace {

struct Game {
    asset::AssetSystem assets;
    World w;
    u64 tick = 0;

    explicit Game(u8 autopilot) {
        const StringView roots[1] = {StringView(UNDELIVERED_PACK)};
        REQUIRE(static_cast<bool>(assets.mount(Span<const StringView>(roots, 1))));
        // The player runtime's order (src/player/src/player.cpp): input, physics, audio, kit, the game, the scene.
        input::install(w);
        physics::install_physics2d(w);
        w.register_components<audio::AudioSource>();  // no device here; the scene still authors the sound beds
        kit::install_particles(w, &assets);
        kit::install_sequences(w, &assets);
        NovelState state;
        state.autopilot = autopilot;
        state.autosave = false;
        w.set_resource(state);
        REQUIRE(static_cast<bool>(undelivered::install(w)));
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
    NovelState& n() { return w.resource<NovelState>(); }
    NovelSave& s() { return *novel_save(w); }
    template <class C>
    u32 count(UiRole role) {
        u32 c = 0;
        w.query<const UiPart&, const C&>().each([&](const UiPart& p, const C&) { c += p.role == role ? 1u : 0u; });
        return c;
    }
    // Ticks until an @end ran (or the budget is spent).
    bool play(u32 budget = 30000) {
        for (u32 i = 0; i < budget; ++i) {
            step();
            if (n().finished) return true;
        }
        return false;
    }
};

u8 policy(st::Tag t) { return static_cast<u8>(1 + static_cast<u8>(t)); }

}  // namespace

TEST_CASE("undelivered: the compiled script's tables are consistent") {
    REQUIRE(st::kOpCount > 1000);
    CHECK(st::kStart < st::kOpCount);
    u32 pages = 0, choices = 0;
    for (u32 i = 0; i < st::kOpCount; ++i) {
        const st::Op& op = st::kOps[i];
        switch (static_cast<st::OpKind>(op.kind)) {
            case st::OpKind::Say:
                ++pages;
                for (u16 id : {op.c, op.d, op.e, op.f}) {
                    REQUIRE(id < st::kTextCount);
                    CHECK(std::strlen(st::kText[id]) <= 63);  // TextLabel::text is a FixedString<64>
                }
                break;
            case st::OpKind::Choice:
                ++choices;
                REQUIRE(op.a + op.b <= st::kOptionCount);
                for (u32 o = op.a; o < op.a + op.b; ++o) {
                    CHECK(st::kOptions[o].target < st::kOpCount);
                    CHECK(st::kOptions[o].text < st::kTextCount);
                    CHECK(std::strlen(st::kText[st::kOptions[o].text]) <= 63);
                }
                break;
            case st::OpKind::Jump:
            case st::OpKind::Call: CHECK(op.a < st::kOpCount); break;
            case st::OpKind::If: CHECK(op.d < st::kOpCount); break;
            case st::OpKind::Letter: CHECK(op.b < st::kOpCount); CHECK(op.b == st::kLetterLabels[op.a]); break;
            case st::OpKind::LetterPage: CHECK(op.a + op.b <= st::kLetterLineCount); CHECK(op.b <= st::kLetterLinesPerPage); break;
            default: break;
        }
    }
    CHECK(pages > 400);
    CHECK(choices >= 40);
    for (u32 l = 1; l <= 7; ++l) CHECK(st::kLetterLabels[l] < st::kOpCount);
    // the last statement never falls off the end
    const st::OpKind last = static_cast<st::OpKind>(st::kOps[st::kOpCount - 1].kind);
    CHECK((last == st::OpKind::End || last == st::OpKind::Return || last == st::OpKind::Jump));
}

TEST_CASE("undelivered: the authored scene is a whole stage") {
    Game g(0);
    CHECK(g.count<kit::Sprite>(UiRole::Background) == 5);
    CHECK(g.count<kit::Sprite>(UiRole::Cast) == 3);
    CHECK(g.count<kit::TextLabel>(UiRole::Line) == 4);
    CHECK(g.count<kit::Sprite>(UiRole::ChoiceRow) == 4);
    CHECK(g.count<kit::TextLabel>(UiRole::ChoiceText) == 4);
    CHECK(g.count<kit::TextLabel>(UiRole::ChoiceText2) == 4);
    CHECK(g.count<kit::TextLabel>(UiRole::LetterLine) == 12);
    CHECK(g.count<kit::TextLabel>(UiRole::PanelLine) == 14);
    CHECK(g.count<kit::Sprite>(UiRole::Envelope) == 7);
    CHECK(g.count<kit::Sprite>(UiRole::Stamp) == 6);
    CHECK(g.count<audio::AudioSource>(UiRole::Music) == 1);
    CHECK(g.count<audio::AudioSource>(UiRole::Ambience) == 1);
    CHECK(g.count<kit::Light2D>(UiRole::Light) == 1);
    CHECK(g.count<kit::Post2DComponent>(UiRole::Camera) == 1);
    CHECK(g.count<NovelSave>(UiRole::Save) == 1);
    CHECK(g.count<SaveKey>(UiRole::Save) == 1);
    g.step();
    CHECK(g.n().mode == Mode::Title);
    CHECK(g.n().assets != nullptr);
    // the art tables found the pack's textures and sounds by path
    CHECK(g.n().bg_count[static_cast<u32>(st::Location::Pier)] == 5);
    CHECK(g.n().bg_count[static_cast<u32>(st::Location::Drawer)] == 1);
    CHECK(!g.n().cast[0][0][0].is_null());
    CHECK(!g.n().lut[static_cast<u32>(st::Lut::Night)].is_null());
    CHECK(!g.n().music[static_cast<u32>(st::Music::Harbour)].is_null());
    CHECK(!g.n().sfx[static_cast<u32>(st::Sfx::Page)].is_null());
    CHECK(!g.n().paper_normal.is_null());
}

TEST_CASE("undelivered: a player's inputs start a run, read a page and take a choice") {
    Game g(0);
    g.step();
    REQUIRE(g.n().mode == Mode::Title);
    input::InputState confirm;
    confirm.confirm_pressed = true;
    g.step(confirm);  // New game
    CHECK(g.n().mode == Mode::Reading);
    CHECK(g.s().day == 1);
    CHECK(g.n().revealed < static_cast<f32>(g.n().page_chars));
    for (u32 i = 0; i < 60; ++i) g.step();
    CHECK(g.n().revealed > 30.0f);  // 40 code points per second
    g.step(confirm);                // completes the page
    CHECK(g.n().revealed == static_cast<f32>(g.n().page_chars));
    const u32 pages = g.n().pages;
    g.step();
    g.step(confirm);  // next page
    CHECK(g.n().pages == pages + 1);
    // skip (Fire held) runs through the pages until the first choice
    input::InputState skip;
    skip.action_names[0] = MCE_SID("Fire");
    skip.actions[0].held = true;
    skip.action_count = 1;
    for (u32 i = 0; i < 600 && g.n().mode != Mode::Choice; ++i) g.step(skip);
    REQUIRE(g.n().mode == Mode::Choice);
    CHECK(g.n().vis_count == 3);
    input::InputState down;
    down.move = {0, -1};
    g.step(down);
    CHECK(g.n().cursor == 1);
    g.step();
    g.step(confirm);
    CHECK(g.n().mode == Mode::Reading);
    CHECK(g.n().choices == 1);
}

TEST_CASE("undelivered: the five bot policies play whole runs to their endings") {
    struct Row {
        st::Tag tag;
        const char* name;
    } rows[] = {{st::Tag::First, "first"}, {st::Tag::Leave, "leave"}, {st::Tag::Stay, "stay"}, {st::Tag::Shut, "shut"}, {st::Tag::Write, "write"}};
    u8 reached = 0;
    for (const Row& r : rows) {
        Game g(policy(r.tag));
        REQUIRE(g.play());
        const NovelState& n = g.n();
        INFO("policy ", r.name);
        CHECK(n.ending != 0);
        CHECK(n.pages > 100);
        CHECK(g.s().pc == 0);
        reached = static_cast<u8>(reached | (1u << n.ending));
        std::printf("undelivered bot '%s': ending %u, %u pages, %u choices, %u letters read, %u ticks\n", r.name, n.ending, n.pages,
                    n.choices, n.letters_read, n.ticks);
        if (r.tag == st::Tag::Shut) CHECK(n.ending == static_cast<u8>(st::Ending::DrawerShut));
        if (r.tag == st::Tag::Write) CHECK(n.ending == static_cast<u8>(st::Ending::ReturnAddress));
        if (r.tag == st::Tag::Leave) CHECK(n.ending == static_cast<u8>(st::Ending::OpenWater));
        if (r.tag == st::Tag::Stay) CHECK(n.ending == static_cast<u8>(st::Ending::LampKeeper));
    }
    CHECK(__builtin_popcount(reached) >= 4);
}

TEST_CASE("undelivered: a save written at a journal reloads and finishes the same run") {
    Game a(policy(st::Tag::Leave));
    // play to the third night's journal
    u32 journals = 0;
    for (u32 i = 0; i < 30000 && journals < 3; ++i) {
        a.step();
        if (a.n().mode == Mode::Journal) {
            ++journals;
            if (journals == 3) break;
        }
    }
    REQUIRE(journals == 3);
    REQUIRE(a.s().day == 3);
    const u16 pc = a.s().pc;
    Array<byte> payload;
    save::SaveInfo info;
    save::write_save(a.w, info, payload);
    REQUIRE(payload.size() > 64);

    Game b(policy(st::Tag::Leave));
    const Result<save::LoadStats> st_ = save::read_save(b.w, payload.span());
    REQUIRE(static_cast<bool>(st_));
    CHECK(st_.value().entities == 1);
    CHECK(st_.value().unknown_fields == 0);
    b.step();  // boot: the title with Continue; the bot continues the loaded run on this tick
    CHECK(b.s().day >= 3);
    CHECK(b.s().trust_mira == a.s().trust_mira);
    CHECK(b.s().delivered == a.s().delivered);
    CHECK(pc != 0);
    REQUIRE(a.play());
    REQUIRE(b.play());
    CHECK(a.n().ending == b.n().ending);
    CHECK(a.s().flags == b.s().flags);
    CHECK(a.s().delivered == b.s().delivered);
    CHECK(a.s().endings_seen == b.s().endings_seen);
}

TEST_CASE("undelivered: two bot runs hash the same") {
    Game a(policy(st::Tag::First)), b(policy(st::Tag::First));
    for (u32 i = 0; i < 150; ++i) {  // mid-run (a whole bot run is about 200 ticks)
        a.step();
        b.step();
    }
    CHECK(a.w.state_hash() == b.w.state_hash());
    CHECK(a.n().pages == b.n().pages);
    CHECK(a.n().pages > 50);
}
