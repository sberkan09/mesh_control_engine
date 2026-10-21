#include "undelivered.h"

#include <cmath>
#include <cstdio>

#include "story_data.h"

// Undelivered on the visual-novel kit: the kit runs the compiled script (story_data.h) over the
// scene's StoryState, presents it on the StagePart entities, reads input and plays the bot. What
// is left here is the game's own: the drawer (the @drawer command's screen, its envelopes and
// stamps), the night journal's lines, the HUD line, and the Letters page's letter rows.

namespace undelivered {

using namespace mce;
namespace st = story;
namespace nv = mce::novel;

namespace {

constexpr u32 kModuleVersion = 2;  // 2: the kit's StoryState (saves of version 1 do not carry over)

const char* kWeekday[] = {"", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
const char* kLetterTo[] = {"", "Yusuf Reis", "Feride", "Halil, the keeper", "Kaptan Hasan", "The muhtar", "Deniz", "whoever opens this drawer"};
const char* kLetterFrom[] = {"", "Kemal, Cesme, 1971", "the conservatory, Istanbul, 1978", "Coastal Safety, 1989",
                             "Nermin, 1986", "Egeport Tourism, 1992", "Tarik, Hamburg, 1983", "Nermin, April 1994"};
constexpr u16 kDelivered[8] = {0, st::var::delivered_1, st::var::delivered_2, st::var::delivered_3, st::var::delivered_4,
                               st::var::delivered_5, st::var::delivered_6, st::var::delivered_7};
constexpr u16 kRead[8] = {0, st::var::read_1, st::var::read_2, st::var::read_3, st::var::read_4, st::var::read_5, st::var::read_6, st::var::read_7};
constexpr u16 kTrust[7] = {st::var::trust_mira, st::var::trust_feride, st::var::trust_selin, st::var::trust_halil,
                           st::var::trust_yusuf, st::var::trust_hasan, st::var::trust_cemil};

// The drawer screen (a resource; not saved: the script reads its result from drawer_letter and
// drawer_action, 0 sealed / 1 read / 2 leave / 3 nothing left).
struct DrawerScreen {
    u8 cursor = 0;        // the envelope (letter - 1)
    bool action = false;  // the three action rows are up
};

i32 get(const nv::StoryState& s, u16 var) { return nv::get_var(s, var); }
void set(nv::Context& c, u16 var, i32 v) { nv::set_var(c.s, c.p, var, v); }
bool delivered(const nv::StoryState& s, u32 letter) { return get(s, kDelivered[letter]) != 0; }
bool read(const nv::StoryState& s, u32 letter) { return get(s, kRead[letter]) != 0; }
// Envelope i (letter i + 1) is gone from the drawer: delivered, or (Deniz's own) read.
bool gone(const nv::StoryState& s, u32 i) { return delivered(s, i + 1) || (i == 5 && read(s, 6)); }

void sfx(nv::Context& c, st::Sfx id, f32 gain_db) { nv::play_sfx(c, static_cast<u16>(id), gain_db); }

// ---- the bot's drawer -------------------------------------------------------------------------

void bot_drawer(nv::Context& c) {
    const st::Tag policy = static_cast<st::Tag>(c.n.autopilot - 1);
    if (policy == st::Tag::Shut) {
        set(c, st::var::drawer_letter, 0);
        set(c, st::var::drawer_action, 2);
        return;
    }
    const bool six_open = !read(c.s, 6) && !get(c.s, st::var::kept_six);
    i32 pick = 0;
    if (policy == st::Tag::Write && six_open) pick = 6;
    if (pick == 0)
        for (i32 l = 1; l <= 6; ++l)
            if (!delivered(c.s, static_cast<u32>(l)) && l != 6) {
                pick = l;
                break;
            }
    if (pick == 0 && six_open) pick = 6;
    if (pick == 0) {
        set(c, st::var::drawer_action, 3);
        return;
    }
    set(c, st::var::drawer_letter, pick);
    set(c, st::var::drawer_action, (policy == st::Tag::Leave || policy == st::Tag::Write) ? 1 : 0);  // read first, or sealed
}

// ---- the hooks -----------------------------------------------------------------------------------

// @drawer: pick an envelope, then deliver it sealed, read it first, or leave it.
bool on_command(nv::Context& c, u16 command, const nv::Op&) {
    if (command != static_cast<u16>(st::Command::Drawer)) return false;
    set(c, st::var::drawer_letter, 0);
    set(c, st::var::drawer_action, 2);
    if (c.n.autopilot) {
        bot_drawer(c);
        return false;
    }
    DrawerScreen& d = c.f.resource<DrawerScreen>();
    d.cursor = 0;
    d.action = false;
    while (d.cursor < 6 && delivered(c.s, d.cursor + 1u)) ++d.cursor;
    if (d.cursor >= 6) {
        set(c, st::var::drawer_action, 3);
        return false;
    }
    return true;
}

void on_game_input(nv::Context& c, const nv::Keys& k) {
    DrawerScreen& d = c.f.resource<DrawerScreen>();
    if (!d.action) {
        if (k.left || k.right || k.up || k.down) {
            const i32 dir = (k.right || k.down) ? 1 : -1;
            for (u32 step = 1; step < 6; ++step) {
                const u32 i = static_cast<u32>((static_cast<i32>(d.cursor) + dir * static_cast<i32>(step) + 12) % 6);
                if (!gone(c.s, i)) {
                    d.cursor = static_cast<u8>(i);
                    sfx(c, st::Sfx::Move, -12.0f);
                    break;
                }
            }
        }
        if (k.advance) {
            d.action = true;
            c.n.vis_count = 3;
            c.n.cursor = 0;
            sfx(c, st::Sfx::Envelope, -10.0f);
        }
        if (k.back) {  // leave the drawer shut today
            set(c, st::var::drawer_letter, 0);
            set(c, st::var::drawer_action, 2);
            nv::resume(c);
        }
        return;
    }
    if (k.up || k.down) {
        c.n.cursor = static_cast<u8>((c.n.cursor + (k.up ? 2 : 1)) % 3);
        sfx(c, st::Sfx::Move, -12.0f);
    }
    if (k.back) d.action = false;
    if (k.advance) {
        const u8 row = c.n.cursor;
        set(c, st::var::drawer_letter, row == 2 ? 0 : d.cursor + 1);
        set(c, st::var::drawer_action, row);  // 0 sealed, 1 read, 2 leave
        sfx(c, row == 2 ? st::Sfx::Confirm : st::Sfx::Stamp, -8.0f);
        d.action = false;
        nv::resume(c);
    }
}

void on_screen(nv::Context& c, nv::ScreenLook& look) {
    if (c.n.mode != Mode::Game) return;
    const DrawerScreen& d = c.f.resource<DrawerScreen>();
    static char to[96], from[96];
    const u32 l = d.cursor + 1u;
    std::snprintf(to, sizeof(to), "To %s", kLetterTo[l]);
    std::snprintf(from, sizeof(from), "From %s", kLetterFrom[l]);
    look.location = static_cast<u16>(st::Location::Drawer);
    look.time = static_cast<u16>(st::TimeOfDay::Noon);
    look.lut = static_cast<u16>(st::Lut::Sepia);
    look.music = static_cast<u16>(st::Music::Drawer);
    look.cast = false;
    look.box = true;
    look.rows = d.action;
    look.glow_owned = !d.action;
    look.lines[0] = to;
    look.lines[1] = from;
    look.lines[2] = d.action ? "" : "Left and right choose an envelope; Enter opens it.";
    look.lines[3] = "";
}

nv::RowText on_row(nv::Context& c, u32 row) {
    const bool mine = c.f.resource<DrawerScreen>().cursor == 5;
    const char* rows[3] = {mine ? "Keep it unopened, in your pocket" : "Deliver it sealed", mine ? "Read it, here, in her chair" : "Read it first",
                           mine ? "Not today" : "Leave it for now"};
    if (row >= 3) return {nullptr, nullptr, false};
    return {rows[row], "", true};
}

void on_present(nv::Context& c, const nv::ScreenLook&) {
    const bool drawer = c.n.mode == Mode::Game;
    const DrawerScreen& d = c.f.resource<DrawerScreen>();
    const f32 t = static_cast<f32>(c.n.ticks) / 60.0f;
    const nv::StoryState& s = c.s;
    c.f.query<const DrawerPart&, kit::Sprite&, LocalTransform&>().each([&](const DrawerPart& p, kit::Sprite& spr, LocalTransform& lt) {
        if (p.role == DrawerRole::Envelope) {
            const bool on = drawer && p.index < 7;
            const bool out = p.index < 6 && gone(s, p.index);
            const bool selected = drawer && !d.action && p.index == d.cursor;
            const f32 lum = out ? 0.6f : 1.0f;
            spr.tint = {lum, lum, lum, on ? 1.0f : 0.0f};
            const f32 sc = selected ? 1.08f + std::sin(t * 4.0f) * 0.015f : 1.0f;
            lt.scale = {sc, sc, 1.0f};
        } else {
            spr.tint.a = drawer && p.index < 6 && gone(s, p.index) ? 0.9f : 0.0f;
        }
    });
    if (!drawer || d.action) return;
    // The glow sits behind the chosen envelope (two rows: four, then three).
    c.f.query<const nv::StagePart&, LocalTransform&>().each([&](const nv::StagePart& p, LocalTransform& lt) {
        if (p.role != nv::StageRole::Glow) return;
        const u32 i = d.cursor;
        lt.position = {i < 4 ? -6.0f + 4.0f * static_cast<f32>(i) : -4.0f + 4.0f * static_cast<f32>(i - 4), i < 4 ? 1.6f : -1.6f, lt.position.z};
    });
}

bool on_panel_line(nv::Context& c, nv::Panel panel, u32 line, char* buf, u32 cap) {
    const nv::StoryState& s = c.s;
    if (panel == nv::Panel::Checkpoint) {  // the night journal
        const u32 day = static_cast<u32>(math::clamp<i32>(get(s, st::var::day), 1, 7));
        u32 out = 0, opened = 0;
        for (u32 l = 1; l <= 6; ++l) {
            out += delivered(s, l) ? 1u : 0u;
            opened += read(s, l) ? 1u : 0u;
        }
        const char* names[7] = {"Mira", "Feride", "Selin", "Halil", "Yusuf Reis", "Kaptan Hasan", "Cemil"};
        switch (line) {
            case 0: std::snprintf(buf, cap, "%s night", kWeekday[day]); break;
            case 1: std::snprintf(buf, cap, "%u of six letters out of the drawer, %u read", out, opened); break;
            case 2: std::snprintf(buf, cap, "Resolve %d", get(s, st::var::resolve)); break;
            case 3: case 4: case 5: case 6: case 7: case 8: case 9: {
                const i32 tr = get(s, kTrust[line - 3]);
                const char* word = tr >= 4 ? "trusts you" : tr >= 2 ? "is warming to you" : tr >= 1 ? "is listening" : tr <= -1 ? "is wary of you" : "is undecided";
                std::snprintf(buf, cap, "%s %s", names[line - 3], word);
                break;
            }
            case 11: std::snprintf(buf, cap, "%u pages read", c.n.pages); break;
            case 13: std::snprintf(buf, cap, "%s", c.n.autosave ? "Saved.  Enter to sleep." : "Enter to sleep."); break;
            default: break;
        }
        return true;
    }
    if (panel == nv::Panel::Extras && line >= 8 && line <= 11) {  // the Letters page's letters
        const u32 l = line - 7;
        std::snprintf(buf, cap, "letter %u %s", l, read(s, l) ? "read" : "unread");
        return true;
    }
    return false;
}

void on_hud(nv::Context& c, char* buf, u32 cap) {
    const i32 day = get(c.s, st::var::day);
    if (day < 1) return;
    u32 left = 0;
    for (u32 l = 1; l <= 6; ++l) left += delivered(c.s, l) ? 0u : 1u;
    std::snprintf(buf, cap, "%s   %u in the drawer", kWeekday[math::clamp<i32>(day, 1, 7)], left);
}

void on_new_run(nv::Context& c) { c.f.resource<DrawerScreen>() = DrawerScreen{}; }

}  // namespace

nv::StoryState* novel_save(World& world) { return nv::story_state(world); }

void new_game(World& world) { nv::new_game(world); }

ResultV install(World& world) {
    world.register_components<DrawerPart>();
    world.set_resource(DrawerScreen{});
    if (NovelState* n = world.try_resource<NovelState>(); !n || n->save_slot.empty()) {
        NovelState state = n ? *n : NovelState{};
        state.save_slot.assign(StringView(kSaveSlot));
        world.set_resource(state);
    }
    world.resource<NovelState>().module_version = kModuleVersion;
    nv::NovelHooks hooks;
    hooks.command = &on_command;
    hooks.game_input = &on_game_input;
    hooks.screen = &on_screen;
    hooks.row = &on_row;
    hooks.present = &on_present;
    hooks.panel_line = &on_panel_line;
    hooks.hud = &on_hud;
    hooks.new_run = &on_new_run;
    return nv::install(world, st::kProgram, hooks);
}

}  // namespace undelivered
