// mce_kit_novel (M-NOVEL): the story VM on a small compiled story (fixture/, compiled at build
// time by mce story compile): variables and conditions, a novel run played by the bot to its
// ending with a game command, and NPC dialogue (#154) gated by a variable, saved and restored
// mid-conversation.
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "mce/audio/components.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/input/ecs.h"
#include "mce/kit/components.h"
#include "mce/kit/novel.h"
#include "mce/loc/loc.h"
#include "mce/convar.h"
#include "mce/platform/haptics.h"
#include "mce/platform/screen_reader.h"
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

// The player's language as the player sets it up: one string table per locale (the story's rows
// live there too, but the kit reads the compiled translations; the tables only name the languages).
loc::Localization languages(StringView pick) {
    loc::Localization l;
    for (const char* tag : {"en", "tr"}) {
        loc::StringTable t;
        t.set_locale(tag);
        REQUIRE(t.add("menu.play", tag[0] == 'e' ? "Play" : "Oyna"));
        l.add_table(std::move(t));
    }
    REQUIRE(l.set_locale(pick));
    return l;
}

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

TEST_CASE("screen reader (M-ACCESS): the stage says the title row, each finished page with its speaker, and the choice") {
    ConVar* reader = ConVar::find(StringId("access.screen_reader"));
    REQUIRE(reader != nullptr);
    reader->set_int(2);  // always: no reader runs here
    std::vector<std::string> said;
    platform::screen_reader::set_sink([](void* user, StringView text) { static_cast<std::vector<std::string>*>(user)->emplace_back(text); },
                                      &said);
    Novel g(0);
    g.n().instant_text = true;
    g.step();
    g.step();
    REQUIRE(g.n().mode == nv::Mode::Title);
    REQUIRE(said.size() == 1);
    CHECK(said[0] == "New game, 1 of 1");
    input::InputState enter;
    enter.confirm_pressed = true;
    for (u32 i = 0; i < 400 && g.n().mode != nv::Mode::Choice; ++i) g.step(i % 2 == 0 ? enter : input::InputState{});
    REQUIRE(g.n().mode == nv::Mode::Choice);
    for (u32 i = 0; i < 4; ++i) g.step();  // nothing new: nothing said again
    const std::vector<std::string> want{"New game, 1 of 1", "The harbour is quiet.", "Mira: Morning.", "Wave back, 1 of 2"};
    CHECK(said == want);
    input::InputState down;
    down.move.y = -1.0f;
    g.step(down);
    g.step();
    CHECK(said.back() == "Walk on, 2 of 2");

    reader->set_int(0);  // never: the stage stays quiet
    const size_t before = said.size();
    g.step(enter);
    for (u32 i = 0; i < 10; ++i) g.step();
    CHECK(said.size() == before);
    platform::screen_reader::set_sink(nullptr, nullptr);
    reader->set_int(1);
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

TEST_CASE("translations (M-MOBILE 11e): the language chain picks a table; pages, options and names map, gaps keep the base") {
    const nv::Program& p = fx::kProgram;
    REQUIRE(p.locale_count == 1);
    CHECK(std::strcmp(p.base_locale, "en") == 0);
    CHECK(nv::story_locale(p, static_cast<const loc::Localization*>(nullptr)) == nullptr);
    const loc::Localization en = languages("en");
    CHECK(nv::story_locale(p, &en) == nullptr);  // the base language: the script's own text
    const loc::Localization tr = languages("tr");
    const nv::LocaleText* lt = nv::story_locale(p, &tr);
    REQUIRE(lt != nullptr);
    CHECK(std::strcmp(lt->locale, "tr") == 0);

    Npc g;
    REQUIRE(nv::start_dialogue(g.w, g.mira, "mira"));
    u16 shown[4];
    nv::localized_page(p, lt, g.d().line, shown);
    CHECK(std::strcmp(nv::text(p, shown[0]), "Merhaba.") == 0);
    CHECK(shown[1] == 0);
    nv::localized_page(p, nullptr, g.d().line, shown);
    CHECK(std::strcmp(nv::text(p, shown[0]), "Hello there.") == 0);
    nv::advance_dialogue(g.w, g.mira);
    REQUIRE(g.d().waiting_choice);
    u16 t = 0, t2 = 0;
    nv::localized_option(p, lt, p.options[g.d().options[0]], t, t2);
    CHECK(std::strcmp(nv::text(p, t), "Tekneyi sor") == 0);
    CHECK(t2 == 0);
    CHECK(std::strcmp(nv::localized_speaker(p, lt, static_cast<u8>(fx::Character::HarbourMaster)), "Yusuf Reis") == 0);
    CHECK(std::strcmp(nv::localized_speaker(p, lt, static_cast<u8>(fx::Character::Mira)), "Mira") == 0);  // no row: the base

    // tr.csv has no row for "Then you know.": that page shows in English
    nv::set_var(g.vars(), p, fx::var::has_letter, 1);
    nv::set_var(g.vars(), p, fx::var::trust, 3);
    REQUIRE(nv::start_dialogue(g.w, g.mira, "mira"));
    nv::advance_dialogue(g.w, g.mira);
    nv::choose_dialogue(g.w, g.mira, 2);
    nv::localized_page(p, lt, g.d().line, shown);
    CHECK(std::strcmp(nv::text(p, shown[0]), "Then you know.") == 0);

    // the typewriter: the base page's clock mapped onto the shown page
    CHECK(nv::scale_reveal(0, 12, 8) == 0);
    CHECK(nv::scale_reveal(6, 12, 8) == 4);
    CHECK(nv::scale_reveal(12, 12, 8) == 8);
    CHECK(nv::scale_reveal(nv::kRevealAll, 12, 8) == 8);
}

namespace {

// SBA-59: the kit's own words and the story's titles, Turkish over the English base table.
loc::Localization kit_words() {
    loc::Localization l;
    loc::StringTable en;
    en.set_locale("en");
    REQUIRE(en.add("story.title", "Fixture"));
    REQUIRE(en.add("story.ending.quiet", "Quiet"));
    l.add_table(std::move(en));
    loc::StringTable tr;
    tr.set_locale("tr");
    REQUIRE(tr.add("story.title", "Deneme"));
    REQUIRE(tr.add("story.ending.quiet", "Sessiz son"));
    REQUIRE(tr.add("novel.new_game", "Yeni oyun"));
    REQUIRE(tr.add("novel.pages_read", "{count} sayfa okundu"));
    REQUIRE(tr.add("novel.go_on", "Devam için Enter."));
    REQUIRE(tr.add("novel.go_on.touch", "Devam için dokun."));
    REQUIRE(tr.add("novel.ending_of", "Son {n} / {total}"));
    l.add_table(std::move(tr));
    REQUIRE(l.set_locale("tr"));
    return l;
}

std::string stage_text(World& w, nv::StageRole role, u8 index) {
    std::string out;
    w.query<const nv::StagePart&, const kit::TextLabel&>().each([&](const nv::StagePart& sp, const kit::TextLabel& tl) {
        if (sp.role == role && sp.index == index) out.assign(tl.text.view().data(), tl.text.view().size());
    });
    return out;
}

}  // namespace

TEST_CASE("kit words (SBA-59): the title screen speaks the player's language; a key no table has keeps English") {
    Novel g(0);
    g.w.set_resource(kit_words());
    g.step();
    REQUIRE(g.n().mode == nv::Mode::Title);
    g.step();
    CHECK(stage_text(g.w, nv::StageRole::Title, 0) == "Deneme");
    CHECK(stage_text(g.w, nv::StageRole::ChoiceText, 0) == "Yeni oyun");

    Novel plain(0);  // no Localization: the story's own title and the English rows
    plain.step();
    plain.step();
    CHECK(stage_text(plain.w, nv::StageRole::Title, 0) == "Fixture");
    CHECK(stage_text(plain.w, nv::StageRole::ChoiceText, 0) == "New game");
}

TEST_CASE("kit words (SBA-59): panel lines, counts, placeholders, the touch wording and the English fallback") {
    const loc::Localization tr = kit_words();
    char buf[64];
    nv::kit_count(&tr, "novel.pages_read", 3, "{count} page read", "{count} pages read", buf, sizeof(buf));
    CHECK(std::string(buf) == "3 sayfa okundu");
    nv::kit_count(nullptr, "novel.pages_read", 1, "{count} page read", "{count} pages read", buf, sizeof(buf));
    CHECK(std::string(buf) == "1 page read");
    nv::kit_count(nullptr, "novel.pages_read", 7, "{count} page read", "{count} pages read", buf, sizeof(buf));
    CHECK(std::string(buf) == "7 pages read");

    nv::kit_text(&tr, "novel.go_on", "Enter to go on.", buf, sizeof(buf));
    CHECK(std::string(buf) == "Devam için Enter.");
    nv::kit_text(&tr, "novel.go_on", "Tap to go on.", buf, sizeof(buf), {}, true);
    CHECK(std::string(buf) == "Devam için dokun.");
    nv::kit_text(&tr, "novel.continue", "Continue", buf, sizeof(buf));  // no row: English, never the key
    CHECK(std::string(buf) == "Continue");

    const loc::Arg args[] = {{"n", "2"}, {"total", "2"}};
    nv::kit_text(&tr, "novel.ending_of", "Ending {n} of {total}", buf, sizeof(buf), args);
    CHECK(std::string(buf) == "Son 2 / 2");
    nv::kit_text(&tr, "story.ending.quiet", "Quiet", buf, sizeof(buf));
    CHECK(std::string(buf) == "Sessiz son");

    char small[8];  // a cut never splits a code point: "Devam i" + the two bytes of "ç" do not fit
    nv::kit_text(&tr, "novel.go_on", "", small, sizeof(small));
    CHECK(std::string(small) == "Devam i");
}

TEST_CASE("translations (M-MOBILE 11e): the run, its saves and its hash are the same in every language") {
    Npc a, b;
    a.w.set_resource(languages("en"));
    b.w.set_resource(languages("tr"));
    for (Npc* g : {&a, &b}) {
        g->w.add(g->mira, nv::DialogueInput{});
        REQUIRE(nv::start_dialogue(g->w, g->mira, "mira"));
    }
    for (u32 i = 0; i < 120; ++i) {
        for (Npc* g : {&a, &b}) {
            input::InputState in;
            in.confirm_pressed = i % 20 == 19;
            in.move.y = i == 70 ? -1.0f : 0.0f;
            in.tick = g->tick;
            input::publish(g->w, in);
            g->w.tick(1.0 / 60.0, g->tick++);
        }
        CHECK(nv::dialogue_revealed(a.w, a.d()) == nv::dialogue_revealed(b.w, b.d()));
    }
    CHECK(std::memcmp(&a.d(), &b.d(), sizeof(nv::Dialogue)) == 0);
    CHECK(a.w.state_hash() == b.w.state_hash());
}

namespace {

// M-MOBILE 11f: a finger at (x, y) of a 1280 x 720 window, the default stage's 20 x 11.25 m view.
input::InputState tap(f32 x, f32 y) {
    input::InputState in;
    in.touch_count = 1;
    in.touches[0].pos = in.touches[0].origin = {x, y};
    in.touches[0].held = in.touches[0].pressed = true;
    in.jump_pressed = x >= 0.5f;  // the stock map reads a still tap on the right half as Jump too
    return in;
}

kit::ViewInfo stage_view() {
    kit::ViewInfo v;
    v.view_pixels_per_meter = 64.0f;
    return v;
}

// The window point over a stage row's centre (rows at y = 3.0 - 1.9 i metres).
f32 row_y(u32 i) { return (360.0f - (3.0f - 1.9f * static_cast<f32>(i)) * 64.0f) / 720.0f; }

}  // namespace

TEST_CASE("pointer (M-MOBILE 11f): taps alone play the novel to its end, a tap on a row picks that row") {
    Novel g(0);
    g.n().instant_text = false;
    g.w.set_resource(stage_view());
    const u32 played = platform::haptics::stats().played;
    g.step();
    REQUIRE(g.n().mode == nv::Mode::Title);
    bool missed_once = false;
    for (u32 i = 0; i < 4000 && !g.n().finished; ++i) {
        if (i % 2 == 1) {
            g.step();
            continue;
        }
        if (g.n().mode == nv::Mode::Choice && !missed_once) {
            g.step(tap(0.9f, 0.97f));  // beside the rows (and read as Jump): nothing is picked
            CHECK(g.n().mode == nv::Mode::Choice);
            CHECK(g.n().choices == 0);
            missed_once = true;
            continue;
        }
        if (g.n().mode == nv::Mode::Title) g.step(tap(0.5f, row_y(0)));             // New game
        else if (g.n().mode == nv::Mode::Choice) g.step(tap(0.5f, row_y(1)));       // "Walk on", the second row
        else g.step(tap(0.25f, 0.9f));                                              // reads on
    }
    REQUIRE(g.n().finished);
    CHECK(missed_once);
    CHECK(g.n().choices == 1);
    CHECK(g.n().ending == static_cast<u8>(fx::Ending::Quiet));  // the row tapped, not the highlighted one
    CHECK(platform::haptics::stats().played >= played + 2);     // a Selection per pick (title and choice)
}

TEST_CASE("pointer (M-MOBILE 11f): a mouse click is the Point action at the mouse; a run without taps keeps its hash") {
    auto click = [](f32 x_px, f32 y_px) {
        input::InputState in;
        in.mouse_pos = {x_px, y_px};
        in.action_names[0] = MCE_SID("Point");
        in.actions[0].held = in.actions[0].pressed = true;
        in.action_count = 1;
        in.fire_pressed = true;  // the stock map's left button is Fire too
        return in;
    };
    Novel g(0);
    g.n().instant_text = true;
    g.w.set_resource(stage_view());
    g.step();
    for (u32 i = 0; i < 4000 && !g.n().finished; ++i) {
        if (i % 2 == 1) g.step();
        else if (g.n().mode == nv::Mode::Choice || g.n().mode == nv::Mode::Title) g.step(click(640.0f, row_y(0) * 720.0f));
        else g.step(click(100.0f, 650.0f));
    }
    REQUIRE(g.n().finished);
    CHECK(g.n().ending == static_cast<u8>(fx::Ending::Letter));  // "Wave back", the first row

    // The keyboard run is the same with or without a ViewInfo (taps are the only readers).
    auto keyboard = [](bool view) {
        Novel k(0);
        k.n().instant_text = true;
        if (view) k.w.set_resource(stage_view());
        input::InputState enter;
        enter.confirm_pressed = true;
        for (u32 i = 0; i < 400 && !k.n().finished; ++i) k.step(i % 2 == 0 ? enter : input::InputState{});
        return k.w.state_hash();
    };
    CHECK(keyboard(false) == keyboard(true));
}

TEST_CASE("pointer (M-MOBILE 11f): NPC dialogue: the box finishes the typewriter then advances, a row picks, a gated row and a miss do nothing") {
    Npc g;
    g.w.add(g.mira, nv::DialogueInput{});
    kit::ViewInfo view;
    g.w.set_resource(view);
    const nv::DialogueRects r = nv::dialogue_rects(nv::dialogue_layout(g.w), view);
    auto at = [&](const nv::DialogueRect& rect) {
        return tap((rect.x0 + rect.x1) * 0.5f / 1280.0f, (rect.y0 + rect.y1) * 0.5f / 720.0f);
    };
    auto step = [&](input::InputState in = {}) {
        in.tick = g.tick;
        input::publish(g.w, in);
        g.w.tick(1.0 / 60.0, g.tick++);
    };
    REQUIRE(nv::start_dialogue(g.w, g.mira, "mira"));
    step();
    step();
    const u32 page = nv::dialogue_page_points(fx::kProgram, g.d());
    REQUIRE(nv::dialogue_revealed(g.w, g.d()) < page);
    step(tap(0.5f, 0.1f));  // above the box: nothing
    CHECK(nv::dialogue_revealed(g.w, g.d()) < page);
    step(at(r.box));  // the rest of the page
    CHECK(nv::dialogue_revealed(g.w, g.d()) == page);
    CHECK(!g.d().waiting_choice);
    step();
    step(at(r.box));  // on to the choice
    REQUIRE(g.d().waiting_choice);
    REQUIRE(g.d().option_count == 3);

    const u32 played = platform::haptics::stats().played;
    step(at(r.rows[1]));  // "Show the letter": gated; the Jump the tap also is picks nothing either
    CHECK(g.d().waiting_choice);
    CHECK(g.d().cursor == 0);
    step(at(r.rows[2]));  // "Leave"
    CHECK(!g.d().active);
    CHECK(platform::haptics::stats().played == played + 1);
}
