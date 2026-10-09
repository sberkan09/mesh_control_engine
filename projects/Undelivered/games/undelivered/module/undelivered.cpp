#include "undelivered.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/asset/asset_system.h"
#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/input/actions.h"
#include "mce/kit/sequence.h"
#include "story_data.h"

// The story VM runs the compiled script (story_data.h) over NovelSave; the presentation writes the
// result into the scene's UiPart entities every tick; input (the stock player's default map) or the
// bot moves the VM from one blocking statement to the next. Everything is tick-driven (60 Hz) so a
// run is deterministic and a headless run plays the same as a windowed one.

namespace undelivered {

using namespace mce;
namespace st = story;

namespace {

constexpr const char* kSaveSlot = "undelivered_autosave";
constexpr u32 kModuleVersion = 1;
constexpr f32 kRevealPerSecond = 40.0f;   // code points per second
constexpr u32 kSkipTicks = 5;              // a page per N ticks while skipping
constexpr u32 kFadeTicks = 40;
constexpr f32 kRowY0 = 3.0f, kRowStep = 1.9f;  // the choice rows' positions (tools/author_scene_mcp.py)
constexpr StringId kFire = MCE_SID("Fire");
constexpr StringId kNavigate = MCE_SID("Navigate");

const char* kWeekday[] = {"", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
const char* kEndingTitle[] = {"", "Return Address", "Open Water", "Lamp Keeper", "The Drawer Stays Shut", "The Last Ferry"};
const char* kLetterTo[] = {"", "Yusuf Reis", "Feride", "Halil, the keeper", "Kaptan Hasan", "The muhtar", "Deniz", "whoever opens this drawer"};
const char* kLetterFrom[] = {"", "Kemal, Cesme, 1971", "the conservatory, Istanbul, 1978", "Coastal Safety, 1989",
                             "Nermin, 1986", "Egeport Tourism, 1992", "Tarik, Hamburg, 1983", "Nermin, April 1994"};

// ---- the save's variables by (kind, index) ----------------------------------------------------

i8* trust_field(NovelSave& s, u32 i) {
    switch (i) {
        case 0: return &s.trust_mira;
        case 1: return &s.trust_feride;
        case 2: return &s.trust_selin;
        case 3: return &s.trust_halil;
        case 4: return &s.trust_yusuf;
        case 5: return &s.trust_hasan;
        default: return &s.trust_cemil;
    }
}

i16* counter_field(NovelSave& s, u32 i) {
    i16* fields[] = {&s.resolve, &s.day, &s.drawer_letter, &s.drawer_action, &s.afternoon, &s.aux0, &s.aux1, &s.aux2,
                     &s.aux3, &s.aux4, &s.aux5, &s.aux6, &s.aux7, &s.mira_days, &s.selin_days, &s.yusuf_days, &s.alone_days};
    static_assert(sizeof(fields) / sizeof(fields[0]) == st::kCounterCount);
    return fields[i < st::kCounterCount ? i : 0];
}

i32 get_var(NovelSave& s, u16 packed) {
    const u32 kind = packed >> 8, i = packed & 0xFF;
    switch (kind) {
        case st::kVarTrust: return *trust_field(s, i);
        case st::kVarDelivered: return (s.delivered >> i) & 1;
        case st::kVarRead: return (s.read >> i) & 1;
        case st::kVarFlag: return (s.flags >> i) & 1;
        default: return *counter_field(s, i);
    }
}

void set_var(NovelSave& s, u16 packed, i32 value) {
    const u32 kind = packed >> 8, i = packed & 0xFF;
    switch (kind) {
        case st::kVarTrust: *trust_field(s, i) = static_cast<i8>(math::clamp(value, -3, 6)); break;
        case st::kVarDelivered: s.delivered = static_cast<u8>(value ? s.delivered | (1u << i) : s.delivered & ~(1u << i)); break;
        case st::kVarRead: s.read = static_cast<u8>(value ? s.read | (1u << i) : s.read & ~(1u << i)); break;
        case st::kVarFlag: s.flags = value ? s.flags | (1u << i) : s.flags & ~(1u << i); break;
        default: *counter_field(s, i) = static_cast<i16>(math::clamp(value, -32768, 32767)); break;
    }
}

bool gate_passes(NovelSave& s, u8 gate, u16 var, i16 n) {
    switch (static_cast<st::Gate>(gate)) {
        case st::Gate::None: return true;
        case st::Gate::Set: return get_var(s, var) != 0;
        case st::Gate::Clear: return get_var(s, var) == 0;
        case st::Gate::Ge: return get_var(s, var) >= n;
        case st::Gate::Le: return get_var(s, var) <= n;
        case st::Gate::Eq: return get_var(s, var) == n;
    }
    return false;
}

u32 utf8_points(StringView s) {
    u32 n = 0;
    for (char c : s) n += (static_cast<u8>(c) & 0xC0) != 0x80 ? 1u : 0u;
    return n;
}

// The prefix of `s` holding `points` code points.
StringView utf8_prefix(StringView s, u32 points) {
    usize i = 0;
    u32 n = 0;
    while (i < s.size()) {
        if ((static_cast<u8>(s[i]) & 0xC0) != 0x80) {
            if (n == points) break;
            ++n;
        }
        ++i;
    }
    return StringView(s.data(), i);
}

// ---- assets by name ---------------------------------------------------------------------------

bool starts_with(StringView s, StringView p) { return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0; }

i32 index_in(const char* const* names, u32 count, StringView s) {
    for (u32 i = 0; i < count; ++i)
        if (s == StringView(names[i])) return static_cast<i32>(i);
    return -1;
}

const char* kLocationNames[] = {"none", "pier", "cafe", "postoffice", "lighthouse", "radio", "cove", "school", "grocery", "drawer"};
const char* kCastNames[] = {"mira", "feride", "selin", "halil", "yusuf", "hasan", "cemil"};
const char* kExprNames[] = {"neutral", "warm", "troubled", "sharp", "open"};
const char* kPoseNames[] = {"stand", "gesture"};
const char* kLutNames[] = {"neutral", "warm", "cold", "sepia", "night"};
const char* kMusicNames[] = {"none", "harbour", "cafe", "drawer", "light", "cove", "sunday"};
const char* kAmbienceNames[] = {"none", "sea", "cafe", "wind", "clock", "radio"};
const char* kSfxNames[] = {"none", "page", "move", "confirm", "envelope", "stamp", "horn", "motor", "static", "bell"};

// Splits "a_b_rest" into its underscore parts (up to 3).
u32 split3(StringView s, StringView out[3]) {
    u32 n = 0;
    usize start = 0;
    for (usize i = 0; i <= s.size() && n < 3; ++i) {
        if (i == s.size() || s[i] == '_') {
            out[n++] = StringView(s.data() + start, i - start);
            start = i + 1;
            if (n == 2 && i < s.size()) {  // the rest is the third part
                out[n++] = StringView(s.data() + start, s.size() - start);
                break;
            }
        }
    }
    return n;
}

StringView strip_ext(StringView s) {
    for (usize i = s.size(); i-- > 0;)
        if (s[i] == '.') return StringView(s.data(), i);
    return s;
}

void build_tables(NovelState& n) {
    asset::AssetSystem& a = *n.assets;
    for (asset::AssetKey k = 0; k < a.asset_count(); ++k) {
        const asset::AssetInfo info = a.info(k);
        const StringView path = info.path;
        if (info.type == asset::ChunkType::Texture) {
            if (starts_with(path, "textures/bg/")) {
                StringView parts[3];
                if (split3(strip_ext(StringView(path.data() + 12, path.size() - 12)), parts) < 2) continue;
                const i32 loc = index_in(kLocationNames, st::kLocationCount, parts[0]);
                const i32 layer = parts[1].size() == 1 ? parts[1][0] - '0' : -1;
                if (loc < 0 || layer < 0 || layer > 4) continue;
                n.bg[loc][layer] = info.guid;
                n.bg_count[loc] = static_cast<u8>(math::max<u32>(n.bg_count[loc], static_cast<u32>(layer) + 1));
            } else if (starts_with(path, "textures/cast/")) {
                StringView parts[3];
                if (split3(strip_ext(StringView(path.data() + 14, path.size() - 14)), parts) < 3) continue;
                const i32 c = index_in(kCastNames, st::kCastCount, parts[0]);
                const i32 e = index_in(kExprNames, 5, parts[1]);
                const i32 p = index_in(kPoseNames, 2, parts[2]);
                if (c >= 0 && e >= 0 && p >= 0) n.cast[c][e][p] = info.guid;
            } else if (starts_with(path, "textures/lut_")) {
                const i32 l = index_in(kLutNames, 5, strip_ext(StringView(path.data() + 13, path.size() - 13)));
                if (l >= 0) n.lut[l] = info.guid;
            } else if (path == StringView("textures/paper_normal.png")) {
                n.paper_normal = info.guid;
            }
        } else if (info.type == asset::ChunkType::Sfx) {
            if (starts_with(path, "audio/music/")) {
                const i32 m = index_in(kMusicNames, 7, strip_ext(StringView(path.data() + 12, path.size() - 12)));
                if (m >= 0) n.music[m] = info.guid;
            } else if (starts_with(path, "audio/ambience/")) {
                const i32 m = index_in(kAmbienceNames, 6, strip_ext(StringView(path.data() + 15, path.size() - 15)));
                if (m >= 0) n.ambience[m] = info.guid;
            } else if (starts_with(path, "audio/sfx/")) {
                const i32 m = index_in(kSfxNames, 10, strip_ext(StringView(path.data() + 10, path.size() - 10)));
                if (m >= 0) n.sfx[m] = info.guid;
            }
        }
    }
}

void play_sfx(Frame& f, const NovelState& n, st::Sfx id, f32 gain_db = -6.0f) {
    const asset::Guid g = n.sfx[static_cast<u32>(id)];
    if (g.is_null()) return;
    audio::PlaySfx ev;
    ev.clip = g;
    ev.gain_db = gain_db;
    ev.bus = audio::Bus_UI;
    f.events().push(ev);
}

// ---- the VM --------------------------------------------------------------------------------------

void log_page(NovelState& n, u8 speaker, const u16* lines) {
    LogEntry& e = n.log[n.log_head];
    e.speaker = speaker;
    for (u32 i = 0; i < 4; ++i) e.line[i] = lines[i];
    n.log_head = static_cast<u8>((n.log_head + 1) % 64);
    if (n.log_count < 64) ++n.log_count;
}

void begin_page(NovelState& n, const st::Op& op) {
    n.page_speaker = static_cast<u8>(op.a);
    const u16 lines[4] = {op.c, op.d, op.e, op.f};
    n.page_chars = 0;
    for (u32 i = 0; i < 4; ++i) {
        n.page_line[i] = lines[i];
        n.page_chars += utf8_points(StringView(st::kText[lines[i]]));
    }
    n.revealed = (n.instant_text || n.autopilot) ? static_cast<f32>(n.page_chars) : 0.0f;
    log_page(n, n.page_speaker, lines);
    n.mode = Mode::Reading;
    ++n.pages;
}

void begin_choice(NovelState& n, NovelSave& s, const st::Op& op) {
    n.opt_first = op.a;
    n.opt_count = op.b;
    n.vis_count = 0;
    for (u32 i = 0; i < op.b && n.vis_count < 4; ++i) {
        const st::Option& o = st::kOptions[op.a + i];
        if (o.hidden && !gate_passes(s, o.gate, o.gate_var, o.gate_n)) continue;
        n.vis[n.vis_count++] = static_cast<u8>(i);
    }
    n.cursor = 0;
    // The cursor starts on the first option that can be taken.
    for (u32 v = 0; v < n.vis_count; ++v) {
        const st::Option& o = st::kOptions[op.a + n.vis[v]];
        if (gate_passes(s, o.gate, o.gate_var, o.gate_n)) {
            n.cursor = static_cast<u8>(v);
            break;
        }
    }
    n.mode = Mode::Choice;
}

void set_slot(NovelSave& s, u32 slot, u8 c, u8 e, u8 p) {
    u8* cast[3] = {&s.cast0, &s.cast1, &s.cast2};
    u8* expr[3] = {&s.expr0, &s.expr1, &s.expr2};
    u8* pose[3] = {&s.pose0, &s.pose1, &s.pose2};
    *cast[slot] = c;
    *expr[slot] = e;
    *pose[slot] = p;
}

u8 slot_cast(const NovelSave& s, u32 slot) { return slot == 0 ? s.cast0 : slot == 1 ? s.cast1 : s.cast2; }

void save_now(Frame& f, NovelState& n) {
    if (!n.autosave) return;
    save::SaveInfo info;
    info.module_version = kModuleVersion;
    info.tick = f.tick;
    if (Result<void> r = save::save_game(f.world(), kSaveSlot, info); !r) {
        FixedString<512> why;
        format_error(r.error(), why);
        MCE_LOG_ERROR("undelivered: autosave failed: {}", why.view());
    } else {
        ++n.saves;
    }
}

void bot_drawer(NovelState& n, NovelSave& s);
void title_menu(NovelState& n, const NovelSave& s);

// Runs statements until one waits for the player (or the bot, next tick).
void run(Frame& f, NovelState& n, NovelSave& s) {
    for (u32 guard = 0; guard < 4096; ++guard) {
        if (s.pc >= st::kOpCount) {
            n.mode = Mode::Title;
            s.pc = 0;
            return;
        }
        const st::Op& op = st::kOps[s.pc];
        ++n.ops;
        switch (static_cast<st::OpKind>(op.kind)) {
            case st::OpKind::Nop: ++s.pc; break;
            case st::OpKind::Say:
                ++s.pc;
                begin_page(n, op);
                return;
            case st::OpKind::Choice:
                begin_choice(n, s, op);
                if (n.vis_count == 0) {  // nothing to pick: fall through (a script bug, not a hang)
                    ++s.pc;
                    n.mode = Mode::Reading;
                    break;
                }
                return;
            case st::OpKind::Jump: s.pc = op.a; break;
            case st::OpKind::Call:
                if (s.depth < 4) {
                    u16* ret[4] = {&s.ret0, &s.ret1, &s.ret2, &s.ret3};
                    *ret[s.depth++] = static_cast<u16>(s.pc + 1);
                }
                s.pc = op.a;
                break;
            case st::OpKind::Return:
                if (s.depth == 0) {
                    ++s.pc;
                    break;
                }
                {
                    u16* ret[4] = {&s.ret0, &s.ret1, &s.ret2, &s.ret3};
                    s.pc = *ret[--s.depth];
                }
                break;
            case st::OpKind::If: s.pc = gate_passes(s, static_cast<u8>(op.a), op.b, static_cast<i16>(op.c)) ? op.d : s.pc + 1; break;
            case st::OpKind::Set: set_var(s, op.a, get_var(s, op.a) + static_cast<i16>(op.c)); ++s.pc; break;
            case st::OpKind::SetTo: set_var(s, op.a, static_cast<i16>(op.c)); ++s.pc; break;
            case st::OpKind::Bg:
                if (s.bg != op.a || s.bg_time != op.b) n.fade = 1.0f;  // a scene change goes through black
                s.bg = static_cast<u8>(op.a);
                s.bg_time = static_cast<u8>(op.b);
                s.bg_lut = static_cast<u8>(op.c);
                ++s.pc;
                break;
            case st::OpKind::Music: s.music = static_cast<u8>(op.a); ++s.pc; break;
            case st::OpKind::Ambience: s.ambience = static_cast<u8>(op.a); ++s.pc; break;
            case st::OpKind::Sfx: play_sfx(f, n, static_cast<st::Sfx>(op.a), -3.0f); ++s.pc; break;
            case st::OpKind::Show:
                // A character already on stage moves to the new slot.
                for (u32 i = 0; i < 3; ++i)
                    if (i != op.a && slot_cast(s, i) == op.b) set_slot(s, i, 255, 0, 0);
                set_slot(s, op.a, static_cast<u8>(op.b), static_cast<u8>(op.c), static_cast<u8>(op.d));
                ++s.pc;
                break;
            case st::OpKind::Hide:
                for (u32 i = 0; i < 3; ++i)
                    if (op.a == 255 || slot_cast(s, i) == op.a) set_slot(s, i, 255, 0, 0);
                ++s.pc;
                break;
            case st::OpKind::Expr:
            case st::OpKind::Pose: {
                u8* expr[3] = {&s.expr0, &s.expr1, &s.expr2};
                u8* pose[3] = {&s.pose0, &s.pose1, &s.pose2};
                for (u32 i = 0; i < 3; ++i)
                    if (slot_cast(s, i) == op.a) *(op.kind == static_cast<u16>(st::OpKind::Expr) ? expr[i] : pose[i]) = static_cast<u8>(op.b);
                ++s.pc;
                break;
            }
            case st::OpKind::Letter:
                n.letter_number = static_cast<u8>(op.a);
                ++n.letters_read;
                play_sfx(f, n, st::Sfx::Envelope, -4.0f);
                if (s.depth < 4) {
                    u16* ret[4] = {&s.ret0, &s.ret1, &s.ret2, &s.ret3};
                    *ret[s.depth++] = static_cast<u16>(s.pc + 1);
                }
                s.pc = op.b;
                break;
            case st::OpKind::LetterPage:
                n.letter_first = op.a;
                n.letter_count = op.b;
                n.mode = Mode::Letter;
                ++s.pc;
                return;
            case st::OpKind::Drawer:
                ++s.pc;
                s.drawer_letter = 0;
                s.drawer_action = 2;
                if (n.autopilot) {
                    bot_drawer(n, s);
                    break;
                }
                n.drawer_cursor = 0;
                while (n.drawer_cursor < 6 && (s.delivered >> (n.drawer_cursor + 1)) & 1) ++n.drawer_cursor;
                if (n.drawer_cursor >= 6) {
                    s.drawer_action = 3;
                    break;
                }
                n.mode = Mode::Drawer;
                return;
            case st::OpKind::Journal:
                ++s.pc;
                s.pages = n.pages;
                save_now(f, n);
                n.mode = Mode::Journal;
                return;
            case st::OpKind::Ending:
                ++s.pc;
                n.ending = static_cast<u8>(op.a);
                s.endings_seen = static_cast<u8>(s.endings_seen | (1u << op.a));
                n.endings_this_session = static_cast<u8>(n.endings_this_session | (1u << op.a));
                n.mode = Mode::Ending;
                n.fade = 1.0f;
                return;
            case st::OpKind::End:
                s.pc = 0;
                s.depth = 0;
                n.finished = true;
                n.fade = 1.0f;
                title_menu(n, s);
                return;
            case st::OpKind::Fade:
                ++s.pc;
                n.fade = 1.0f;
                n.hold_ticks = n.autopilot ? 0 : kFadeTicks;
                n.mode = Mode::Fade;
                return;
            case st::OpKind::Wait:
                ++s.pc;
                n.hold_ticks = n.autopilot ? 0 : op.a;
                n.mode = Mode::Wait;
                return;
        }
    }
    MCE_LOG_ERROR("undelivered: the script ran 4096 statements without waiting at pc {}", s.pc);
    n.mode = Mode::Title;
    s.pc = 0;
}

void choose(Frame& f, NovelState& n, NovelSave& s, u32 visible_index) {
    const st::Option& o = st::kOptions[n.opt_first + n.vis[visible_index]];
    if (!gate_passes(s, o.gate, o.gate_var, o.gate_n)) return;
    play_sfx(f, n, st::Sfx::Confirm, -8.0f);
    s.pc = o.target;
    ++n.choices;
    run(f, n, s);
}

void start_run(Frame& f, NovelState& n, NovelSave& s) {
    const u8 seen = s.endings_seen;
    s = NovelSave{};
    s.endings_seen = seen;
    s.pc = st::kStart;
    n.log_count = n.log_head = 0;
    n.finished = false;
    n.ending = 0;
    n.pages = n.choices = n.letters_read = 0;
    n.fade = 1.0f;
    run(f, n, s);
}

// ---- the bot -----------------------------------------------------------------------------------

st::Tag bot_policy(const NovelState& n) { return static_cast<st::Tag>(n.autopilot - 1); }

void bot_drawer(NovelState& n, NovelSave& s) {
    const st::Tag policy = bot_policy(n);
    if (policy == st::Tag::Shut) {
        s.drawer_letter = 0;
        s.drawer_action = 2;
        return;
    }
    i32 pick = 0;
    if (policy == st::Tag::Write && !((s.read >> 6) & 1) && !((s.flags >> static_cast<u32>(st::Flag::KeptSix)) & 1)) pick = 6;
    if (pick == 0)
        for (i32 l = 1; l <= 6; ++l)
            if (!((s.delivered >> l) & 1) && l != 6) {
                pick = l;
                break;
            }
    if (pick == 0 && !((s.read >> 6) & 1) && !((s.flags >> static_cast<u32>(st::Flag::KeptSix)) & 1)) pick = 6;
    if (pick == 0) {
        s.drawer_action = 3;
        return;
    }
    s.drawer_letter = static_cast<i16>(pick);
    s.drawer_action = (policy == st::Tag::Leave || policy == st::Tag::Write) ? 1 : 0;  // read first, or sealed
}

void bot_step(Frame& f, NovelState& n, NovelSave& s) {
    switch (n.mode) {
        case Mode::Title:
            if (s.pc != 0) run(f, n, s);  // a loaded save: continue it
            else start_run(f, n, s);
            break;
        case Mode::Reading:
        case Mode::Letter:
        case Mode::Journal:
        case Mode::Ending:
        case Mode::Fade:
        case Mode::Wait: run(f, n, s); break;
        case Mode::Choice: {
            const st::Tag policy = bot_policy(n);
            // The writing bot wants the lighthouse (the radio room), so it takes `stay` options after its own.
            const st::Tag prefer[2] = {policy, policy == st::Tag::Write ? st::Tag::Stay : policy};
            i32 pick = -1;
            for (const st::Tag want : prefer)
                for (u32 v = 0; v < n.vis_count && pick < 0; ++v) {
                    const st::Option& o = st::kOptions[n.opt_first + n.vis[v]];
                    if (static_cast<st::Tag>(o.tags) == want && gate_passes(s, o.gate, o.gate_var, o.gate_n)) pick = static_cast<i32>(v);
                }
            for (u32 v = 0; v < n.vis_count && pick < 0; ++v) {
                const st::Option& o = st::kOptions[n.opt_first + n.vis[v]];
                if (gate_passes(s, o.gate, o.gate_var, o.gate_n)) pick = static_cast<i32>(v);
            }
            if (pick >= 0) choose(f, n, s, static_cast<u32>(pick));
            break;
        }
        default: break;
    }
}

// ---- input ---------------------------------------------------------------------------------------

struct Keys {
    bool advance = false, up = false, down = false, left = false, right = false, skip = false, log = false, back = false;
};

Keys read_keys(Frame& f, NovelState& n) {
    Keys k;
    const input::InputState& in = f.resource<input::InputState>();
    k.advance = in.confirm_pressed || in.fire_pressed || in.jump_pressed;
    math::vec2 nav = in.move;
    if (in.has_action(kNavigate)) {
        const math::vec2 a = in.axis2(kNavigate);
        if (std::fabs(a.x) + std::fabs(a.y) > std::fabs(nav.x) + std::fabs(nav.y)) nav = a;
    }
    k.up = nav.y > 0.5f && n.prev_move_y <= 0.5f;
    k.down = nav.y < -0.5f && n.prev_move_y >= -0.5f;
    k.right = nav.x > 0.5f && n.prev_move_x <= 0.5f;
    k.left = nav.x < -0.5f && n.prev_move_x >= -0.5f;
    n.prev_move_x = nav.x;
    n.prev_move_y = nav.y;
    k.skip = in.has_action(kFire) ? in.held(kFire) : false;
    const math::vec2 wheel = in.mouse_wheel();
    k.log = wheel.y > 0.0f;
    k.back = wheel.y < 0.0f || in.cancel_pressed;
    return k;
}

void title_menu(NovelState& n, const NovelSave& s) {
    n.vis_count = s.pc != 0 ? 3 : 2;
    n.cursor = s.pc != 0 ? 1 : 0;
    n.mode = Mode::Title;
}

void player_step(Frame& f, NovelState& n, NovelSave& s) {
    const Keys k = read_keys(f, n);
    switch (n.mode) {
        case Mode::Title:
            if (k.up || k.down) {
                n.cursor = static_cast<u8>((n.cursor + (k.up ? n.vis_count - 1 : 1)) % n.vis_count);
                play_sfx(f, n, st::Sfx::Move, -12.0f);
            }
            if (k.advance) {
                // rows: New game, [Continue,] Letters
                const bool has_continue = s.pc != 0;
                if (n.cursor == 0) {
                    start_run(f, n, s);
                } else if (has_continue && n.cursor == 1) {
                    n.fade = 1.0f;
                    run(f, n, s);
                } else {
                    n.mode = Mode::Letters;
                }
                play_sfx(f, n, st::Sfx::Confirm, -8.0f);
            }
            break;
        case Mode::Letters:
            if (k.advance || k.back) title_menu(n, s);
            break;
        case Mode::Reading: {
            const bool complete = n.revealed >= static_cast<f32>(n.page_chars);
            if (k.log) {
                n.log_scroll = 0;
                n.mode = Mode::Backlog;
                break;
            }
            if (k.skip && n.skip_cooldown == 0) {
                n.revealed = static_cast<f32>(n.page_chars);
                n.skip_cooldown = kSkipTicks;
                run(f, n, s);
                break;
            }
            if (k.advance) {
                if (!complete) {
                    n.revealed = static_cast<f32>(n.page_chars);
                } else {
                    play_sfx(f, n, st::Sfx::Page, -14.0f);
                    run(f, n, s);
                }
            }
            break;
        }
        case Mode::Backlog:
            if (k.up || k.log) n.log_scroll = static_cast<u8>(math::min<u32>(n.log_scroll + 1, n.log_count > 1 ? n.log_count - 1 : 0));
            if (k.down || k.back) {
                if (n.log_scroll == 0) n.mode = Mode::Reading;
                else --n.log_scroll;
            }
            if (k.advance) n.mode = Mode::Reading;
            break;
        case Mode::Choice:
            if ((k.up || k.down) && n.vis_count > 0) {
                n.cursor = static_cast<u8>((n.cursor + (k.up ? n.vis_count - 1 : 1)) % n.vis_count);
                play_sfx(f, n, st::Sfx::Move, -12.0f);
            }
            if (k.advance) choose(f, n, s, n.cursor);
            break;
        case Mode::Letter:
            if (k.advance || k.skip) {
                play_sfx(f, n, st::Sfx::Page, -14.0f);
                run(f, n, s);
            }
            break;
        case Mode::Drawer: {
            auto free = [&](u32 i) { return !((s.delivered >> (i + 1)) & 1) && !(i == 5 && (s.read >> 6) & 1); };
            if (k.left || k.right || k.up || k.down) {
                const i32 dir = (k.right || k.down) ? 1 : -1;
                for (u32 step = 1; step < 6; ++step) {
                    const u32 c = static_cast<u32>((static_cast<i32>(n.drawer_cursor) + dir * static_cast<i32>(step) + 12) % 6);
                    if (free(c)) {
                        n.drawer_cursor = static_cast<u8>(c);
                        play_sfx(f, n, st::Sfx::Move, -12.0f);
                        break;
                    }
                }
            }
            if (k.advance) {
                n.vis_count = 3;
                n.cursor = 0;
                n.mode = Mode::DrawerAction;
                play_sfx(f, n, st::Sfx::Envelope, -10.0f);
            }
            if (k.back) {  // leave the drawer shut today
                s.drawer_letter = 0;
                s.drawer_action = 2;
                run(f, n, s);
            }
            break;
        }
        case Mode::DrawerAction:
            if (k.up || k.down) {
                n.cursor = static_cast<u8>((n.cursor + (k.up ? 2 : 1)) % 3);
                play_sfx(f, n, st::Sfx::Move, -12.0f);
            }
            if (k.back) n.mode = Mode::Drawer;
            if (k.advance) {
                s.drawer_letter = static_cast<i16>(n.cursor == 2 ? 0 : n.drawer_cursor + 1);
                s.drawer_action = static_cast<i16>(n.cursor);  // 0 sealed, 1 read, 2 leave
                play_sfx(f, n, n.cursor == 2 ? st::Sfx::Confirm : st::Sfx::Stamp, -8.0f);
                run(f, n, s);
            }
            break;
        case Mode::Journal:
        case Mode::Ending:
            if (k.advance) {
                play_sfx(f, n, st::Sfx::Confirm, -8.0f);
                run(f, n, s);
            }
            break;
        case Mode::Fade:
        case Mode::Wait:
            if (n.hold_ticks > 0) --n.hold_ticks;
            else run(f, n, s);
            break;
        default: break;
    }
}

// ---- systems -----------------------------------------------------------------------------------

void nv_setup(Frame& f) {
    NovelState& n = f.resource<NovelState>();
    if (n.mode != Mode::Boot) return;
    if (!n.assets)
        if (kit::SequenceRegistry* reg = f.try_resource<kit::SequenceRegistry>()) n.assets = reg->assets();
    if (n.assets) build_tables(n);
    else MCE_LOG_ERROR("undelivered: no asset system reachable (no SequenceRegistry resource); art and sound stay blank");
    f.query<const UiPart&, const NovelSave&>().each([&](Entity e, const UiPart& p, const NovelSave&) {
        if (p.role == UiRole::Save && n.save_entity.is_null()) n.save_entity = e;
    });
    if (n.save_entity.is_null()) {
        MCE_LOG_ERROR("undelivered: the scene has no NovelSave entity (UiPart Save); nothing to run");
        n.mode = Mode::Done;
        return;
    }
    NovelSave& s = *f.world().try_get_mut<NovelSave>(n.save_entity);
    // The paper layers are lit sprites: give every background the paper normal map.
    f.query<const UiPart&, kit::Sprite&>().each([&](const UiPart& p, kit::Sprite& sp) {
        if (p.role == UiRole::Background && !n.paper_normal.is_null()) sp.normal_map = asset::AssetRef<asset::Texture>{n.paper_normal, {}};
    });
    n.fade = 1.0f;
    title_menu(n, s);

    // The host's flags (games/undelivered/host/main.cpp), when there is one.
    if (const ConVar* c = ConVar::find(MCE_SID("nv.autoplay"))) n.autopilot = static_cast<u8>(math::clamp(c->get_int(), 0, 5));
    if (const ConVar* c = ConVar::find(MCE_SID("nv.instant"))) n.instant_text = c->get_bool();
    const ConVar* label = ConVar::find(MCE_SID("nv.label"));
    if (label && !label->get_string().empty()) {
        const StringView want = label->get_string();
        u16 at = 0xFFFF;
        for (u32 i = 0; i < sizeof(st::kLabelOps) / sizeof(st::kLabelOps[0]); ++i)
            if (want == StringView(st::kLabelNames[i])) at = st::kLabelOps[i];
        if (at == 0xFFFF) {
            MCE_LOG_ERROR("undelivered: no script label '{}'", want);
        } else {
            start_run(f, n, s);
            s.pc = at;
            s.depth = 0;
            n.mode = Mode::Reading;
            run(f, n, s);
            i32 skip = 0;
            if (const ConVar* c = ConVar::find(MCE_SID("nv.skip"))) skip = c->get_int();
            for (i32 i = 0; i < skip && (n.mode == Mode::Reading || n.mode == Mode::Letter || n.mode == Mode::Fade || n.mode == Mode::Wait); ++i)
                run(f, n, s);
            n.revealed = n.instant_text ? static_cast<f32>(n.page_chars) : 0.0f;
            n.fade = 0.0f;
            MCE_LOG_INFO("undelivered: started at label '{}' (pc {}, mode {}, location {}, time {})", want, s.pc, static_cast<u32>(n.mode), s.bg, s.bg_time);
        }
    }
}

void nv_step(Frame& f) {
    NovelState& n = f.resource<NovelState>();
    if (n.mode == Mode::Boot || n.mode == Mode::Done) return;
    NovelSave* s = f.world().try_get_mut<NovelSave>(n.save_entity);
    if (!s) return;
    ++n.ticks;
    if (n.skip_cooldown > 0) --n.skip_cooldown;
    if (n.mode == Mode::Reading && n.revealed < static_cast<f32>(n.page_chars))
        n.revealed = math::min(n.revealed + kRevealPerSecond * f.dt, static_cast<f32>(n.page_chars));
    if (n.autopilot) bot_step(f, n, *s);
    else player_step(f, n, *s);
    n.fade = math::max(0.0f, n.fade - f.dt * 1.6f);
}

// ---- presentation ------------------------------------------------------------------------------

struct Palette {
    math::Color paper, cast, ambient, lamp;
    math::vec2 lamp_pos;
    f32 lamp_intensity = 1.0f;
};

Palette make_palette(math::Color paper, math::Color cast, math::Color ambient, math::Color lamp, math::vec2 lamp_pos, f32 intensity) {
    Palette p;
    p.paper = paper;
    p.cast = cast;
    p.ambient = ambient;
    p.lamp = lamp;
    p.lamp_pos = lamp_pos;
    p.lamp_intensity = intensity;
    return p;
}

Palette palette(st::TimeOfDay t) {
    switch (t) {
        case st::TimeOfDay::Morning:
            return make_palette({0.96f, 0.98f, 1.0f, 1}, {0.98f, 0.99f, 1.0f, 1}, {0.92f, 0.95f, 1.0f, 1}, {1.0f, 0.95f, 0.85f, 1}, {-7.0f, 4.5f}, 1.2f);
        case st::TimeOfDay::Noon:
            return make_palette({1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1.0f, 0.98f, 0.9f, 1}, {0.0f, 5.5f}, 1.0f);
        case st::TimeOfDay::Evening:
            return make_palette({1.0f, 0.9f, 0.78f, 1}, {1.0f, 0.94f, 0.86f, 1}, {0.95f, 0.8f, 0.65f, 1}, {1.0f, 0.75f, 0.45f, 1}, {7.0f, 2.5f}, 1.6f);
        default:
            return make_palette({0.55f, 0.6f, 0.8f, 1}, {0.72f, 0.75f, 0.9f, 1}, {0.35f, 0.4f, 0.6f, 1}, {1.0f, 0.8f, 0.55f, 1}, {4.0f, 0.5f}, 2.2f);
    }
}

math::Color mul(math::Color a, math::Color b, f32 alpha) { return {a.r * b.r, a.g * b.g, a.b * b.b, alpha}; }

void set_texture(kit::Sprite& sp, asset::Guid g) {
    if (sp.texture.guid == g) return;
    sp.texture = asset::AssetRef<asset::Texture>{g, {}};
}

void set_text(kit::TextLabel& t, StringView s) {
    if (t.text.view() == s) return;
    t.text.assign(s);
}

const char* speaker_name(u8 c) {
    if (c >= st::kCharacterCount) return "";
    switch (static_cast<st::Character>(c)) {
        case st::Character::Yusuf: return "Yusuf Reis";
        case st::Character::Hasan: return "Kaptan Hasan";
        case st::Character::Feride: return "Feride Han\xC4\xB1m";
        case st::Character::Cemil: return "Cemil, the muhtar";
        default: return st::kCharacterNames[c];
    }
}

// What the choice rows show in the current mode (text, second line, selectable).
struct RowText {
    const char* a = "";
    const char* b = "";
    bool enabled = true;
};

RowText row_text(const NovelState& n, const NovelSave& s, u32 row) {
    RowText r;
    if (n.mode == Mode::Choice) {
        if (row >= n.vis_count) return {nullptr, nullptr, false};
        const st::Option& o = st::kOptions[n.opt_first + n.vis[row]];
        r.a = st::kText[o.text];
        r.b = st::kText[o.text2];
        r.enabled = gate_passes(const_cast<NovelSave&>(s), o.gate, o.gate_var, o.gate_n);
        return r;
    }
    if (n.mode == Mode::Title) {
        const bool has_continue = s.pc != 0;
        const char* rows[3] = {"New game", has_continue ? "Continue" : "Letters", "Letters"};
        if (row >= n.vis_count || row >= 3) return {nullptr, nullptr, false};
        r.a = rows[row];
        return r;
    }
    if (n.mode == Mode::DrawerAction) {
        const bool mine = n.drawer_cursor == 5;
        const char* rows[3] = {mine ? "Keep it unopened, in your pocket" : "Deliver it sealed", mine ? "Read it, here, in her chair" : "Read it first",
                               mine ? "Not today" : "Leave it for now"};
        if (row >= 3) return {nullptr, nullptr, false};
        r.a = rows[row];
        return r;
    }
    return {nullptr, nullptr, false};
}

void nv_present(Frame& f) {
    NovelState& n = f.resource<NovelState>();
    if (n.mode == Mode::Boot || n.mode == Mode::Done) return;
    const NovelSave* sp = f.world().try_get<NovelSave>(n.save_entity);
    if (!sp) return;
    const NovelSave& s = *sp;
    const bool title = n.mode == Mode::Title || n.mode == Mode::Letters;
    const bool drawer = n.mode == Mode::Drawer || n.mode == Mode::DrawerAction;
    const u32 loc = title ? static_cast<u32>(st::Location::Pier) : drawer ? static_cast<u32>(st::Location::Drawer) : s.bg;
    const st::TimeOfDay tod = title ? st::TimeOfDay::Evening : drawer ? st::TimeOfDay::Noon : static_cast<st::TimeOfDay>(s.bg_time);
    const Palette pal = palette(tod);
    const f32 t = static_cast<f32>(n.ticks) / 60.0f;
    const bool rows_up = n.mode == Mode::Choice || n.mode == Mode::Title || n.mode == Mode::DrawerAction;
    const bool panel_up = n.mode == Mode::Journal || n.mode == Mode::Backlog || n.mode == Mode::Ending || n.mode == Mode::Letters;
    const bool box_up = n.mode == Mode::Reading || n.mode == Mode::Backlog || drawer;
    const bool complete = n.revealed >= static_cast<f32>(n.page_chars);
    char buf[128];

    f.query<const UiPart&, kit::Sprite&, LocalTransform&>().each([&](const UiPart& p, kit::Sprite& spr, LocalTransform& lt) {
        switch (p.role) {
            case UiRole::Background: {
                const bool on = loc != 0 && p.index < n.bg_count[loc];
                if (on) set_texture(spr, n.bg[loc][p.index]);
                spr.tint = mul(pal.paper, math::Color::white(), on ? 1.0f : 0.0f);
                // the paper theatre: near layers drift a little more than far ones
                const f32 k = static_cast<f32>(p.index);
                lt.position.x = std::sin(t * 0.21f + k * 1.7f) * 0.025f * k;
                lt.position.y = std::sin(t * 0.17f + k * 0.9f) * 0.015f * k;
                break;
            }
            case UiRole::Cast: {
                const u8 c = slot_cast(s, p.index);
                const bool on = !title && !drawer && c < st::kCastCount;
                f32& a = n.cast_alpha[p.index < 3 ? p.index : 0];
                a += ((on ? 1.0f : 0.0f) - a) * 0.18f;
                if (on) {
                    const u8 e = p.index == 0 ? s.expr0 : p.index == 1 ? s.expr1 : s.expr2;
                    const u8 po = p.index == 0 ? s.pose0 : p.index == 1 ? s.pose1 : s.pose2;
                    set_texture(spr, n.cast[c][e < 5 ? e : 0][po < 2 ? po : 0]);
                }
                const bool talking = n.page_speaker == c + 1;  // cast index + 1 == Character (Deniz is 0)
                const bool someone_else = n.page_speaker < st::kCharacterCount && n.page_speaker != 0 && !talking && n.mode == Mode::Reading;
                const f32 dim = someone_else ? 0.68f : 1.0f;
                spr.tint = mul(pal.cast, {dim, dim, dim, 1}, a < 0.01f ? 0.0f : a);
                lt.position.y = -1.0f + (1.0f - a) * -0.3f + (talking ? std::sin(t * 6.0f) * 0.02f : 0.0f);
                break;
            }
            case UiRole::Dim: spr.tint.a = (rows_up && n.mode != Mode::Title) || panel_up ? 0.45f : 0.0f; break;
            case UiRole::Fade: spr.tint.a = n.fade; break;
            case UiRole::Box: spr.tint.a = box_up ? 1.0f : 0.0f; break;
            case UiRole::NameTag: spr.tint.a = n.mode == Mode::Reading && n.page_speaker < st::kCharacterCount ? 1.0f : 0.0f; break;
            case UiRole::ChoiceRow: {
                const RowText r = row_text(n, s, p.index);
                const bool on = rows_up && r.a != nullptr;
                const bool selected = on && p.index == n.cursor;
                const f32 lum = !r.enabled ? 0.45f : selected ? 1.0f : 0.78f;
                spr.tint = {lum, lum, lum, on ? 1.0f : 0.0f};
                spr.size.y = (r.b && r.b[0]) ? 1.75f : 0.97f;
                break;
            }
            case UiRole::LetterCard: spr.tint.a = n.mode == Mode::Letter ? 1.0f : 0.0f; break;
            case UiRole::Panel: spr.tint.a = panel_up ? 1.0f : 0.0f; break;
            case UiRole::Envelope: {
                const bool on = drawer && p.index < 7;
                const bool gone = p.index < 6 && (((s.delivered >> (p.index + 1)) & 1) || (p.index == 5 && ((s.read >> 6) & 1)));
                const bool selected = n.mode == Mode::Drawer && p.index == n.drawer_cursor;
                const f32 lum = gone ? 0.6f : 1.0f;
                spr.tint = {lum, lum, lum, on ? 1.0f : 0.0f};
                const f32 sc = selected ? 1.08f + std::sin(t * 4.0f) * 0.015f : 1.0f;
                lt.scale = {sc, sc, 1.0f};
                break;
            }
            case UiRole::Stamp: {
                const bool on = drawer && p.index < 6 && (((s.delivered >> (p.index + 1)) & 1) || (p.index == 5 && ((s.read >> 6) & 1)));
                spr.tint.a = on ? 0.9f : 0.0f;
                break;
            }
            case UiRole::Glow: {
                const bool on = rows_up || n.mode == Mode::Drawer;
                spr.tint.a = on ? 0.35f + std::sin(t * 3.0f) * 0.08f : 0.0f;
                if (n.mode == Mode::Drawer) {
                    const u32 i = n.drawer_cursor;
                    lt.position = {i < 4 ? -6.0f + 4.0f * static_cast<f32>(i) : -4.0f + 4.0f * static_cast<f32>(i - 4), i < 4 ? 1.6f : -1.6f, lt.position.z};
                } else {
                    lt.position = {0.0f, kRowY0 - kRowStep * static_cast<f32>(n.cursor), lt.position.z};
                }
                break;
            }
            default: break;
        }
    });

    f.query<const UiPart&, kit::TextLabel&, LocalTransform&>().each([&](const UiPart& p, kit::TextLabel& tl, LocalTransform& lt) {
        switch (p.role) {
            case UiRole::NameText:
                set_text(tl, n.mode == Mode::Reading && n.page_speaker < st::kCharacterCount ? StringView(speaker_name(n.page_speaker)) : StringView());
                break;
            case UiRole::Line: {
                if (drawer) {
                    const u32 l = n.drawer_cursor + 1;
                    if (p.index == 0) std::snprintf(buf, sizeof(buf), "To %s", kLetterTo[l]);
                    else if (p.index == 1) std::snprintf(buf, sizeof(buf), "From %s", kLetterFrom[l]);
                    else if (p.index == 2) std::snprintf(buf, sizeof(buf), "%s", n.mode == Mode::Drawer ? "Left and right choose an envelope; Enter opens it." : "");
                    else buf[0] = 0;
                    set_text(tl, StringView(buf));
                    tl.color = {0.3f, 0.24f, 0.19f, 1};
                    break;
                }
                if (!(n.mode == Mode::Reading || n.mode == Mode::Backlog) || p.index >= 4) {
                    set_text(tl, StringView());
                    break;
                }
                // reveal: code points up to `revealed`, spent line by line
                u32 budget = static_cast<u32>(n.revealed);
                StringView shown;
                for (u32 i = 0; i <= p.index; ++i) {
                    const StringView line(st::kText[n.page_line[i]]);
                    const u32 pts = utf8_points(line);
                    if (i == p.index) shown = complete ? line : utf8_prefix(line, math::min(budget, pts));
                    budget = budget > pts ? budget - pts : 0;
                }
                set_text(tl, shown);
                const bool narration = n.page_speaker >= st::kCharacterCount;
                tl.color = narration ? math::Color{0.36f, 0.29f, 0.23f, 1} : math::Color{0.16f, 0.12f, 0.1f, 1};
                break;
            }
            case UiRole::ChoiceText:
            case UiRole::ChoiceText2: {
                const RowText r = row_text(n, s, p.index);
                const char* txt = p.role == UiRole::ChoiceText ? r.a : r.b;
                set_text(tl, rows_up && txt ? StringView(txt) : StringView());
                const bool two = r.b && r.b[0];
                const f32 base = kRowY0 - kRowStep * static_cast<f32>(p.index);
                lt.position.y = p.role == UiRole::ChoiceText ? base + (two ? 0.33f : 0.0f) : base - 0.37f;
                const f32 ink = !r.enabled ? 0.55f : (p.index == n.cursor ? 0.12f : 0.3f);  // dark ink on pale rows
                tl.color = {ink, ink * 0.85f, ink * 0.75f, 1};
                break;
            }
            case UiRole::LetterLine:
                set_text(tl, n.mode == Mode::Letter && p.index < n.letter_count ? StringView(st::kText[st::kLetterLines[n.letter_first + p.index]]) : StringView());
                break;
            case UiRole::PanelLine: {
                buf[0] = 0;
                if (n.mode == Mode::Journal) {
                    const u32 day = static_cast<u32>(math::clamp<i32>(s.day, 1, 7));
                    u32 delivered = 0;
                    for (u32 l = 1; l <= 6; ++l) delivered += (s.delivered >> l) & 1;
                    const char* names[7] = {"Mira", "Feride", "Selin", "Halil", "Yusuf Reis", "Kaptan Hasan", "Cemil"};
                    const i8 trust[7] = {s.trust_mira, s.trust_feride, s.trust_selin, s.trust_halil, s.trust_yusuf, s.trust_hasan, s.trust_cemil};
                    switch (p.index) {
                        case 0: std::snprintf(buf, sizeof(buf), "%s night", kWeekday[day]); break;
                        case 1: std::snprintf(buf, sizeof(buf), "%u of six letters out of the drawer, %u read", delivered, static_cast<u32>(__builtin_popcount(s.read & 0x7E))); break;
                        case 2: std::snprintf(buf, sizeof(buf), "Resolve %d", static_cast<int>(s.resolve)); break;
                        case 3: case 4: case 5: case 6: case 7: case 8: case 9: {
                            const u32 i = p.index - 3;
                            const char* word = trust[i] >= 4 ? "trusts you" : trust[i] >= 2 ? "is warming to you" : trust[i] >= 1 ? "is listening" : trust[i] <= -1 ? "is wary of you" : "is undecided";
                            std::snprintf(buf, sizeof(buf), "%s %s", names[i], word);
                            break;
                        }
                        case 11: std::snprintf(buf, sizeof(buf), "%u pages read", n.pages); break;
                        case 13: std::snprintf(buf, sizeof(buf), "%s", n.autosave ? "Saved.  Enter to sleep." : "Enter to sleep."); break;
                        default: break;
                    }
                } else if (n.mode == Mode::Ending) {
                    if (p.index == 1) std::snprintf(buf, sizeof(buf), "%s", kEndingTitle[n.ending < 6 ? n.ending : 0]);
                    else if (p.index == 3) std::snprintf(buf, sizeof(buf), "Ending %u of 5", static_cast<u32>(n.ending));
                    else if (p.index == 13) std::snprintf(buf, sizeof(buf), "Enter");
                } else if (n.mode == Mode::Letters) {
                    if (p.index == 0) std::snprintf(buf, sizeof(buf), "Letters");
                    else if (p.index >= 2 && p.index <= 6) {
                        const u32 e = p.index - 1;
                        std::snprintf(buf, sizeof(buf), "%s  %s", (s.endings_seen >> e) & 1 ? "*" : "-", (s.endings_seen >> e) & 1 ? kEndingTitle[e] : "an ending not yet reached");
                    } else if (p.index >= 8 && p.index <= 11) {
                        const u32 l = p.index - 7;
                        std::snprintf(buf, sizeof(buf), "letter %u %s", l, (s.read >> l) & 1 ? "read" : "unread");
                    } else if (p.index == 13) std::snprintf(buf, sizeof(buf), "Enter to go back");
                } else if (n.mode == Mode::Backlog) {
                    // the last 14 lines before the scroll point, newest at the bottom
                    i32 want = 13 - static_cast<i32>(p.index) + static_cast<i32>(n.log_scroll);  // lines back from the newest
                    for (u32 e = 0; e < n.log_count && want >= 0; ++e) {
                        const LogEntry& le = n.log[(n.log_head + 63 - e) % 64];
                        u32 lines = 0;
                        for (u32 i = 0; i < 4; ++i) lines += le.line[i] ? 1u : 0u;
                        if (want < static_cast<i32>(lines)) {
                            const u32 li = lines - 1 - static_cast<u32>(want);
                            if (li == 0 && le.speaker < st::kCharacterCount)
                                std::snprintf(buf, sizeof(buf), "%s: %s", speaker_name(le.speaker), st::kText[le.line[li]]);
                            else
                                std::snprintf(buf, sizeof(buf), "%s", st::kText[le.line[li]]);
                            want = -1;
                            break;
                        }
                        want -= static_cast<i32>(lines);
                    }
                }
                set_text(tl, StringView(buf));
                break;
            }
            case UiRole::Hud:
                if (!title && s.day >= 1) {
                    u32 left = 0;
                    for (u32 l = 1; l <= 6; ++l) left += ((s.delivered >> l) & 1) ? 0u : 1u;
                    std::snprintf(buf, sizeof(buf), "%s   %u in the drawer", kWeekday[math::clamp<i32>(s.day, 1, 7)], left);
                    set_text(tl, StringView(buf));
                } else {
                    set_text(tl, StringView());
                }
                break;
            case UiRole::Title: set_text(tl, title ? StringView("Undelivered") : StringView()); break;
            default: break;
        }
    });

    // music and ambience: one looping source each; a change stops the old voice and restarts
    const audio::AudioRef* ar = f.try_resource<audio::AudioRef>();
    f.query<const UiPart&, audio::AudioSource&>().each([&](const UiPart& p, audio::AudioSource& src) {
        asset::Guid want;
        if (p.role == UiRole::Music) want = n.music[title ? static_cast<u32>(st::Music::Harbour) : drawer ? static_cast<u32>(st::Music::Drawer) : s.music];
        else if (p.role == UiRole::Ambience) want = n.ambience[title ? static_cast<u32>(st::Ambience::Sea) : s.ambience];
        else return;
        if (src.clip.guid == want) return;
        if (ar && ar->device && !src.handle.is_null()) ar->device->stop(src.handle, 1.2f);
        src.handle = {};
        src.clip = asset::AssetRef<asset::Sfx>{want, {}};
        src.loop = true;
        src.started = false;
        src.play_on_start = !want.is_null();
    });

    f.query<const UiPart&, kit::Light2D&, LocalTransform&>().each([&](const UiPart& p, kit::Light2D& l, LocalTransform& lt) {
        if (p.role == UiRole::AmbientLight) {
            l.kind = kit::Light2DKind::Ambient;
            l.color = pal.ambient;
        } else if (p.role == UiRole::Light) {
            l.kind = kit::Light2DKind::Point;
            l.color = pal.lamp;
            l.intensity = pal.lamp_intensity * 0.45f * (0.95f + std::sin(t * 2.3f) * 0.05f);
            l.radius = 7.5f;
            lt.position.x += (pal.lamp_pos.x - lt.position.x) * 0.05f;
            lt.position.y += (pal.lamp_pos.y - lt.position.y) * 0.05f;
        }
    });

    f.query<const UiPart&, kit::Post2DComponent&>().each([&](const UiPart& p, kit::Post2DComponent& post) {
        if (p.role != UiRole::Camera) return;
        const u32 lut = title ? static_cast<u32>(st::Lut::Warm) : drawer ? static_cast<u32>(st::Lut::Sepia) : s.bg_lut;
        const asset::Guid g = n.lut[lut < 5 ? lut : 0];
        if (!g.is_null() && post.lut.guid != g) post.lut = asset::AssetRef<asset::Texture>{g, {}};
        post.color_grade = !g.is_null();
        post.vignette = true;
    });
}

}  // namespace

NovelSave* novel_save(World& world) {
    NovelState* n = world.try_resource<NovelState>();
    if (!n || n->save_entity.is_null()) return nullptr;
    return world.try_get_mut<NovelSave>(n->save_entity);
}

void new_game(World& world) {
    if (NovelSave* s = novel_save(world)) {
        const u8 seen = s->endings_seen;
        *s = NovelSave{};
        s->endings_seen = seen;
        s->pc = st::kStart;
    }
    if (NovelState* n = world.try_resource<NovelState>()) {
        n->mode = Mode::Reading;
        n->fade = 1.0f;
    }
}

ResultV install(World& world) {
    if (!world.try_resource<input::InputState>()) input::install(world);
    world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
    world.register_components<UiPart, NovelSave>();
    world.add_system(Phase::FixedUpdate_Pre, "nv_setup", &nv_setup);
    world.add_system(Phase::FixedUpdate_Pre, "nv_step", &nv_step, "nv_setup");
    world.add_system(Phase::FixedUpdate_Post, "nv_present", &nv_present);
    if (!world.try_resource<NovelState>()) world.set_resource(NovelState{});
    return ok();
}

}  // namespace undelivered
