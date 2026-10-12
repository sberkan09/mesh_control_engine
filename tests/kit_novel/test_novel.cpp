// mce_kit_novel (M-NOVEL): the story VM on a small compiled story (fixture/, compiled at build
// time by mce story compile): variables and conditions, a novel run played by the bot to its
// ending with a game command, and NPC dialogue (#154) gated by a variable, saved and restored
// mid-conversation.
#include <doctest/doctest.h>

#include <cstring>

#include "mce/audio/components.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/input/ecs.h"
#include "mce/kit/components.h"
#include "mce/kit/novel.h"
#include "novel_fixture.h"

using namespace mce;
namespace nv = mce::novel;
namespace fx = novel_fixture;

namespace {

void register_stage_types(World& w) {
    w.register_components<LocalTransform, SaveKey, kit::Sprite, kit::TextLabel, kit::Light2D, kit::Post2DComponent,
                          audio::AudioSource>();
}

u32 g_waves = 0;
u16 g_wave_arg = 0;
bool wave(nv::Context&, u16 command, const nv::Op& op) {
    CHECK(command == static_cast<u16>(fx::Command::Wave));
    ++g_waves;
    g_wave_arg = op.b;
    return false;  // no game screen: the story carries on
}

struct Novel {
    World w;
    u64 tick = 0;
    explicit Novel(u8 autopilot) {
        input::install(w);
        register_stage_types(w);
        nv::NovelState n;
        n.autopilot = autopilot;
        n.autosave = false;
        w.set_resource(n);
        nv::NovelHooks hooks;
        hooks.command = &wave;
        REQUIRE(static_cast<bool>(nv::install(w, fx::kProgram, hooks)));
        const Entity e = w.spawn();
        w.add(e, nv::StoryState{});
        w.add(e, SaveKey{1});
    }
    void step(input::InputState in = {}) {
        in.tick = tick;
        input::publish(w, in);
        w.tick(1.0 / 60.0, tick++);
    }
    nv::NovelState& n() { return w.resource<nv::NovelState>(); }
    bool play(u32 budget = 20000) {
        for (u32 i = 0; i < budget; ++i) {
            step();
            if (n().finished) return true;
        }
        return false;
    }
};

struct Npc {
    World w;
    Entity mira;
    u64 tick = 0;
    Npc() {
        input::install(w);
        w.register_components<LocalTransform>();
        REQUIRE(static_cast<bool>(nv::install_dialogue(w, fx::kProgram)));
        mira = w.spawn();
        w.add(mira, nv::Dialogue{});
        w.add(mira, SaveKey{2});
        w.tick(1.0 / 60.0, tick++);  // the runtime boots and spawns the variables' carrier
        w.tick(1.0 / 60.0, tick++);
    }
    nv::Dialogue& d() { return *w.try_get_mut<nv::Dialogue>(mira); }
    nv::StoryState& vars() { return *nv::dialogue_vars(w); }
    const char* line() { return nv::text(fx::kProgram, d().line[0]); }
    const char* option(u32 v) { return nv::text(fx::kProgram, fx::kOptions[d().options[v]].text); }
};

}  // namespace

TEST_CASE("novel: variables clamp to their declared range and conditions bind and before or") {
    nv::StoryState s;
    const nv::Program& p = fx::kProgram;
    REQUIRE(nv::find_var(p, "trust") == fx::var::trust);
    CHECK(nv::find_var(p, "nope") == nv::kNone);
    nv::set_var(s, p, fx::var::trust, 9);
    CHECK(nv::get_var(s, fx::var::trust) == 3);  // "max": 3
    nv::set_var(s, p, fx::var::trust, -4);
    CHECK(nv::get_var(s, fx::var::trust) == 0);
    nv::set_var(s, p, fx::var::has_letter, 1);
    CHECK(nv::get_var(s, fx::var::has_letter) == 1);
    CHECK(nv::get_var(s, fx::var::asked_boat) == 0);

    // the hidden option's condition: trust >= 2 and has_letter
    const nv::Option* truth = nullptr;
    for (const nv::Option& o : fx::kOptions)
        if (std::strcmp(fx::kText[o.text], "Tell her the truth") == 0) truth = &o;
    REQUIRE(truth != nullptr);
    CHECK(truth->hidden);
    CHECK(!nv::test(s, p, truth->cond));  // trust 0
    nv::set_var(s, p, fx::var::trust, 2);
    CHECK(nv::test(s, p, truth->cond));
    nv::set_var(s, p, fx::var::has_letter, 0);
    CHECK(!nv::test(s, p, truth->cond));
    CHECK(nv::test(s, p, 0));  // clause 0: always
}

TEST_CASE("novel: the compiler's speaker rule and labels") {
    // "Note: nobody waved." is narration (Note is not a character); "Yusuf:" names the harbour master.
    bool note = false, yusuf = false;
    for (const nv::Op& op : fx::kOps) {
        if (static_cast<nv::OpKind>(op.kind) != nv::OpKind::Say) continue;
        if (std::strcmp(fx::kText[op.c], "Note: nobody waved.") == 0) note = op.a == nv::kNone;
        if (std::strcmp(fx::kText[op.c], "That's her brother's hand.") == 0)
            yusuf = op.a == static_cast<u16>(fx::Character::HarbourMaster);
    }
    CHECK(note);
    CHECK(yusuf);
    CHECK(std::strcmp(nv::speaker_name(fx::kProgram, 1), "Yusuf Reis") == 0);
    CHECK(nv::find_label(fx::kProgram, "mira") < fx::kOpCount);
    CHECK(nv::find_label(fx::kProgram, "nowhere") == nv::kNone);
}

TEST_CASE("novel: the bot plays a run through a game command to its policy's ending") {
    g_waves = 0;
    Novel kind(static_cast<u8>(1 + static_cast<u8>(fx::Tag::Kind)));
    REQUIRE(kind.play());
    CHECK(kind.n().ending == static_cast<u8>(fx::Ending::Letter));
    CHECK(g_waves == 1);
    CHECK(g_wave_arg == 2);
    const nv::StoryState* s = nv::story_state(kind.w);
    REQUIRE(s != nullptr);
    CHECK(nv::get_var(*s, fx::var::trust) == 3);  // +5, clamped
    CHECK((s->endings_seen & (1u << static_cast<u32>(fx::Ending::Letter))) != 0);

    Novel first(static_cast<u8>(1 + static_cast<u8>(fx::Tag::First)));
    REQUIRE(first.play());
    CHECK(first.n().ending == static_cast<u8>(fx::Ending::Quiet));
}

TEST_CASE("novel: a player's Enter starts a run and reads it") {
    Novel g(0);
    g.n().instant_text = true;
    g.step();
    CHECK(g.n().mode == nv::Mode::Title);
    input::InputState enter;
    enter.confirm_pressed = true;
    for (u32 i = 0; i < 400 && !g.n().finished; ++i) g.step(i % 2 == 0 ? enter : input::InputState{});
    CHECK(g.n().finished);
    CHECK(g.n().ending != 0);
}

TEST_CASE("dialogue: a conversation runs from a label, a variable gates its options") {
    Npc g;
    REQUIRE(nv::dialogue_vars(g.w) != nullptr);
    REQUIRE(nv::start_dialogue(g.w, g.mira, "mira"));
    CHECK(g.d().active);
    CHECK(std::strcmp(g.line(), "Hello there.") == 0);
    CHECK(g.d().speaker == static_cast<u8>(fx::Character::Mira));
    REQUIRE(g.d().tag_count == 1);
    CHECK(std::strcmp(nv::text(fx::kProgram, g.d().tags[0]), "greet") == 0);

    nv::advance_dialogue(g.w, g.mira);  // trust +1, then the choice
    REQUIRE(g.d().waiting_choice);
    CHECK(nv::get_var(g.vars(), fx::var::trust) == 1);
    REQUIRE(g.d().option_count == 3);  // the hidden one stays hidden
    CHECK(std::strcmp(g.option(1), "Show the letter") == 0);
    nv::choose_dialogue(g.w, g.mira, 1);  // gated: has_letter is clear
    CHECK(g.d().waiting_choice);

    nv::choose_dialogue(g.w, g.mira, 0);
    CHECK(std::strcmp(g.line(), "It sails at dawn.") == 0);
    CHECK(g.d().expression == 1);  // Mira(warm)
    nv::advance_dialogue(g.w, g.mira);
    CHECK(!g.d().active);  // the top-level @return ends it
    CHECK(nv::get_var(g.vars(), fx::var::asked_boat) == 1);

    // a game system sets a flag; the next conversation offers what it unlocks
    nv::set_var(g.vars(), fx::kProgram, fx::var::has_letter, 1);
    REQUIRE(nv::start_dialogue(g.w, g.mira, "mira"));
    nv::advance_dialogue(g.w, g.mira);
    REQUIRE(g.d().waiting_choice);
    CHECK(g.d().option_count == 4);  // trust 2 and the letter: the truth is offered
    CHECK(std::strcmp(g.option(2), "Tell her the truth") == 0);
}

TEST_CASE("dialogue: events and the stock input drive it, Enter first finishes the typewriter") {
    Npc g;
    g.w.add(g.mira, nv::DialogueInput{});
    auto press = [&](bool confirm, f32 move_y = 0.0f) {
        input::InputState in;
        in.confirm_pressed = confirm;
        in.move.y = move_y;
        in.tick = g.tick;
        input::publish(g.w, in);
        g.w.tick(1.0 / 60.0, g.tick++);
    };
    nv::DialogueStart start;
    start.entity = g.mira;
    start.label.assign(StringView("mira"));
    EventAccess(g.w).push(start);  // read on the tick after the push
    press(false);
    press(false);
    REQUIRE(g.d().active);
    const u32 page = nv::dialogue_page_points(fx::kProgram, g.d());
    CHECK(page == 12);  // "Hello there."
    for (u32 i = 0; i < 6; ++i) press(false);
    const u32 typed = nv::dialogue_revealed(g.w, g.d());
    CHECK(typed > 0);
    CHECK(typed < page);  // 45 points a second: still typing
    press(true);          // the rest of the page
    CHECK(nv::dialogue_revealed(g.w, g.d()) == page);
    CHECK(!g.d().waiting_choice);
    press(false);
    press(true);  // now it advances
    CHECK(g.d().waiting_choice);
    press(false, -1.0f);  // down: to "Show the letter"
    CHECK(g.d().cursor == 1);

    // speed 0 shows whole pages: Enter advances at once
    nv::set_dialogue_speed(g.w, 0.0f);
    nv::choose_dialogue(g.w, g.mira, 0);
    CHECK(nv::dialogue_revealed(g.w, g.d()) == nv::dialogue_page_points(fx::kProgram, g.d()));
}

TEST_CASE("dialogue: a save mid-conversation restores the page, the choice and the variables") {
    Npc a;
    REQUIRE(nv::start_dialogue(a.w, a.mira, "mira"));
    nv::advance_dialogue(a.w, a.mira);
    REQUIRE(a.d().waiting_choice);
    nv::set_var(a.vars(), fx::kProgram, fx::var::has_letter, 1);
    Array<byte> bytes;
    save::write_save(a.w, save::SaveInfo{}, bytes);

    Npc b;
    REQUIRE(save::read_save(b.w, bytes.span()));
    CHECK(b.d().active);
    CHECK(b.d().waiting_choice);
    CHECK(b.d().option_count == 3);
    CHECK(nv::get_var(b.vars(), fx::var::trust) == 1);
    CHECK(nv::get_var(b.vars(), fx::var::has_letter) == 1);
    nv::choose_dialogue(b.w, b.mira, 1);  // the letter: allowed now
    CHECK(std::strcmp(b.line(), "That's her brother's hand.") == 0);
    nv::advance_dialogue(b.w, b.mira);
    CHECK(!b.d().active);
    CHECK((b.vars().endings_seen & (1u << static_cast<u32>(fx::Ending::Letter))) != 0);
}
