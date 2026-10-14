// The novel stage (novel.h): the story VM over StoryState, input and the bot moving it from one
// blocking statement to the next, and the presentation written into the scene's StagePart
// entities every tick. Generalised from Undelivered's game module (M-NOVEL): the id tables come
// from the compiled Program, the game's own screens from NovelHooks.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/asset/asset_system.h"
#include "mce/audio/components.h"
#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/components.h"
#include "mce/kit/novel.h"
#include "mce/kit/sequence.h"
#include "mce/log.h"

namespace mce::novel {

namespace {

constexpr f32 kRevealPerSecond = 40.0f;  // code points per second
constexpr u32 kSkipTicks = 5;             // a page per N ticks while skipping
constexpr u32 kFadeTicks = 40;
constexpr StringId kFire = MCE_SID("Fire");
constexpr StringId kNavigate = MCE_SID("Navigate");

// Where the choice rows sit (the scene's ChoiceRow parts, read at setup; Undelivered's layout until then).
struct RowLayout {
    f32 y[4] = {3.0f, 1.1f, -0.8f, -2.7f};
};

// ---- assets by name ---------------------------------------------------------------------------

bool starts_with(StringView s, StringView p) { return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0; }

i32 index_in(const char* const* names, u32 count, StringView s) {
    for (u32 i = 0; i < count; ++i)
        if (s == StringView(names[i])) return static_cast<i32>(i);
    return -1;
}

StringView strip_ext(StringView s) {
    for (usize i = s.size(); i-- > 0;)
        if (s[i] == '.') return StringView(s.data(), i);
    return s;
}

StringView after(StringView s, usize n) { return StringView(s.data() + n, s.size() - n); }

// Splits off the part after the last '_' (false: none).
bool split_last(StringView s, StringView& head, StringView& tail) {
    for (usize i = s.size(); i-- > 0;)
        if (s[i] == '_') {
            head = StringView(s.data(), i);
            tail = StringView(s.data() + i + 1, s.size() - i - 1);
            return true;
        }
    return false;
}

i32 cast_row_of(const Program& p, StringView id) {
    for (u32 i = 0; i < p.character_count; ++i)
        if (id == StringView(p.characters[i].id)) return p.characters[i].cast == 255 ? -1 : p.characters[i].cast;
    return -1;
}

void build_tables(NovelState& n, const Program& p) {
    asset::AssetSystem& a = *n.assets;
    for (asset::AssetKey k = 0; k < a.asset_count(); ++k) {
        const asset::AssetInfo info = a.info(k);
        const StringView path = info.path;
        if (info.type == asset::ChunkType::Font) {
            if (n.font.is_null()) n.font = info.guid;
        } else if (info.type == asset::ChunkType::Texture) {
            if (p.background_normal_map && path == StringView(p.background_normal_map)) {
                n.normal_map = info.guid;
            } else if (starts_with(path, "textures/bg/")) {
                // <location>_<layer digit>[_<note>].png
                const StringView name = strip_ext(after(path, 12));
                i32 loc = -1, layer = -1;
                for (usize i = 0; i + 1 < name.size(); ++i)
                    if (name[i] == '_' && name[i + 1] >= '0' && name[i + 1] <= '9' && (i + 2 == name.size() || name[i + 2] == '_')) {
                        loc = index_in(p.locations, p.location_count, StringView(name.data(), i));
                        layer = name[i + 1] - '0';
                        break;
                    }
                if (loc < 0 || loc >= static_cast<i32>(NovelState::kLocations) || layer < 0 || layer >= static_cast<i32>(NovelState::kLayers))
                    continue;
                n.bg[loc][layer] = info.guid;
                n.bg_count[loc] = static_cast<u8>(math::max<u32>(n.bg_count[loc], static_cast<u32>(layer) + 1));
            } else if (starts_with(path, "textures/cast/")) {
                StringView rest, pose_name, who, expr_name;
                if (!split_last(strip_ext(after(path, 14)), rest, pose_name) || !split_last(rest, who, expr_name)) continue;
                const i32 c = cast_row_of(p, who);
                const i32 e = index_in(p.expressions, p.expression_count, expr_name);
                const i32 po = index_in(p.poses, p.pose_count, pose_name);
                if (c >= 0 && c < static_cast<i32>(NovelState::kCast) && e >= 0 && e < static_cast<i32>(NovelState::kExpr) && po >= 0 &&
                    po < static_cast<i32>(NovelState::kPoses))
                    n.cast[c][e][po] = info.guid;
            } else if (starts_with(path, "textures/lut_")) {
                const i32 l = index_in(p.luts, p.lut_count, strip_ext(after(path, 13)));
                if (l >= 0 && l < static_cast<i32>(NovelState::kLuts)) n.lut[l] = info.guid;
            }
        } else if (info.type == asset::ChunkType::Sfx) {
            struct Dir {
                const char* prefix;
                const char* const* names;
                u16 count;
                asset::Guid* out;
            } dirs[3] = {{"audio/music/", p.music, p.music_count, n.music},
                         {"audio/ambience/", p.ambience, p.ambience_count, n.ambience},
                         {"audio/sfx/", p.sfx, p.sfx_count, n.sfx}};
            for (const Dir& d : dirs) {
                const StringView prefix(d.prefix);
                if (!starts_with(path, prefix)) continue;
                const i32 m = index_in(d.names, d.count, strip_ext(after(path, prefix.size())));
                if (m > 0 && m < static_cast<i32>(NovelState::kSounds)) d.out[m] = info.guid;
            }
        }
    }
}

// ---- the stage state ---------------------------------------------------------------------------

void log_page(NovelState& n, u8 speaker, const u16* lines) {
    LogEntry& e = n.log[n.log_head];
    e.speaker = speaker;
    for (u32 i = 0; i < 4; ++i) e.line[i] = lines[i];
    n.log_head = static_cast<u8>((n.log_head + 1) % 64);
    if (n.log_count < 64) ++n.log_count;
}

void set_slot(StoryState& s, u32 slot, u8 c, u8 e, u8 po) {
    if (slot >= kMaxSlots) return;
    s.cast[slot] = c;
    s.expr[slot] = e;
    s.pose[slot] = po;
}

void begin_page(Context& c, const Op& op) {
    NovelState& n = c.n;
    n.page_speaker = op.a == kNone ? 255 : static_cast<u8>(op.a);
    if (op.b != kNone)  // Speaker(expr): the speaker's slot takes the expression
        for (u32 i = 0; i < c.p.slot_count && i < kMaxSlots; ++i)
            if (c.s.cast[i] == n.page_speaker) c.s.expr[i] = static_cast<u8>(op.b);
    const u16 lines[4] = {op.c, op.d, op.e, op.f};
    n.page_chars = 0;
    for (u32 i = 0; i < 4; ++i) {
        n.page_line[i] = lines[i];
        n.page_chars += utf8_points(StringView(text(c.p, lines[i])));
    }
    n.revealed = (n.instant_text || n.autopilot) ? static_cast<f32>(n.page_chars) : 0.0f;
    log_page(n, n.page_speaker, lines);
    n.mode = Mode::Reading;
    ++n.pages;
}

bool option_open(const Context& c, const Option& o) { return test(c.s, c.p, o.cond); }

void begin_choice(Context& c, const Op& op) {
    NovelState& n = c.n;
    n.opt_first = op.a;
    n.opt_count = op.b;
    n.vis_count = 0;
    for (u32 i = 0; i < op.b && n.vis_count < 4; ++i) {
        const Option& o = c.p.options[op.a + i];
        if (o.hidden && !option_open(c, o)) continue;
        n.vis[n.vis_count++] = static_cast<u8>(i);
    }
    n.cursor = 0;
    // The cursor starts on the first option that can be taken.
    for (u32 v = 0; v < n.vis_count; ++v)
        if (option_open(c, c.p.options[op.a + n.vis[v]])) {
            n.cursor = static_cast<u8>(v);
            break;
        }
    n.mode = Mode::Choice;
}

void save_now(Frame& f, NovelState& n) {
    if (!n.autosave) return;
    save::SaveInfo info;
    info.module_version = n.module_version;
    info.tick = f.tick;
    if (Result<void> r = save::save_game(f.world(), n.save_slot.view(), info); !r) {
        FixedString<512> why;
        format_error(r.error(), why);
        MCE_LOG_ERROR("novel: autosave failed: {}", why.view());
    } else {
        ++n.saves;
    }
}

void title_menu(NovelState& n, const StoryState& s) {
    const bool has_continue = s.pc != 0;
    const bool has_extras = n.program && n.program->extras[0] != 0;
    n.vis_count = static_cast<u8>(1 + (has_continue ? 1 : 0) + (has_extras ? 1 : 0));
    n.cursor = has_continue ? 1 : 0;
    n.mode = Mode::Title;
}

void push_return(StoryState& s, u16 to) {
    if (s.depth < kMaxDepth) s.ret[s.depth++] = to;
}

}  // namespace

void play_sfx(Context& c, u16 id, f32 gain_db) {
    if (id == 0 || id >= NovelState::kSounds) return;
    const asset::Guid g = c.n.sfx[id];
    if (g.is_null()) return;
    audio::PlaySfx ev;
    ev.clip = g;
    ev.gain_db = gain_db;
    ev.bus = audio::Bus_UI;
    c.f.events().push(ev);
}

void resume(Context& c) {
    NovelState& n = c.n;
    StoryState& s = c.s;
    const Program& p = c.p;
    for (u32 guard = 0; guard < 4096; ++guard) {
        if (s.pc >= p.op_count) {
            title_menu(n, s);
            s.pc = 0;
            return;
        }
        const Op& op = p.ops[s.pc];
        ++n.ops;
        switch (static_cast<OpKind>(op.kind)) {
            case OpKind::Nop:
            case OpKind::Tag: ++s.pc; break;
            case OpKind::Say:
                ++s.pc;
                begin_page(c, op);
                return;
            case OpKind::Choice:
                begin_choice(c, op);
                if (n.vis_count == 0) {  // nothing to pick: fall through (a script bug, not a hang)
                    ++s.pc;
                    n.mode = Mode::Reading;
                    break;
                }
                return;
            case OpKind::Jump: s.pc = op.a; break;
            case OpKind::Call:
                push_return(s, static_cast<u16>(s.pc + 1));
                s.pc = op.a;
                break;
            case OpKind::Return:
                if (s.depth == 0) ++s.pc;
                else s.pc = s.ret[--s.depth];
                break;
            case OpKind::If: s.pc = test(s, p, op.a) ? op.b : static_cast<u16>(s.pc + 1); break;
            case OpKind::Add: set_var(s, p, op.a, get_var(s, op.a) + static_cast<i16>(op.b)); ++s.pc; break;
            case OpKind::Set: set_var(s, p, op.a, static_cast<i16>(op.b)); ++s.pc; break;
            case OpKind::Bg:
                if (s.bg != op.a || s.bg_time != op.b) n.fade = 1.0f;  // a scene change goes through black
                s.bg = static_cast<u8>(op.a);
                s.bg_time = static_cast<u8>(op.b);
                s.bg_lut = static_cast<u8>(op.c);
                ++s.pc;
                break;
            case OpKind::Music: s.music = static_cast<u8>(op.a); ++s.pc; break;
            case OpKind::Ambience: s.ambience = static_cast<u8>(op.a); ++s.pc; break;
            case OpKind::Sfx: play_sfx(c, op.a, -3.0f); ++s.pc; break;
            case OpKind::Show:
                // A character already on stage moves to the new slot.
                for (u32 i = 0; i < kMaxSlots; ++i)
                    if (i != op.a && s.cast[i] == op.b) set_slot(s, i, 255, 0, 0);
                set_slot(s, op.a, static_cast<u8>(op.b), static_cast<u8>(op.c), static_cast<u8>(op.d));
                ++s.pc;
                break;
            case OpKind::Hide:
                for (u32 i = 0; i < kMaxSlots; ++i)
                    if (op.a == kNone || s.cast[i] == op.a) set_slot(s, i, 255, 0, 0);
                ++s.pc;
                break;
            case OpKind::Expr:
            case OpKind::Pose:
                for (u32 i = 0; i < kMaxSlots; ++i)
                    if (s.cast[i] == op.a) (op.kind == static_cast<u16>(OpKind::Expr) ? s.expr[i] : s.pose[i]) = static_cast<u8>(op.b);
                ++s.pc;
                break;
            case OpKind::Letter:
                n.letter_number = op.a;
                ++n.letters_read;
                play_sfx(c, p.sfx_letter, -4.0f);
                push_return(s, static_cast<u16>(s.pc + 1));
                s.pc = op.b;
                break;
            case OpKind::LetterPage:
                n.letter_first = op.a;
                n.letter_count = op.b;
                n.mode = Mode::Letter;
                ++s.pc;
                return;
            case OpKind::Checkpoint:
                ++s.pc;
                s.pages = n.pages;
                save_now(c.f, n);
                n.mode = Mode::Checkpoint;
                return;
            case OpKind::Ending:
                ++s.pc;
                n.ending = static_cast<u8>(op.a);
                s.endings_seen |= 1u << (op.a & 31);
                n.endings_this_session |= 1u << (op.a & 31);
                n.mode = Mode::Ending;
                n.fade = 1.0f;
                return;
            case OpKind::End:
                MCE_LOG_INFO("novel: @end (ending {}, {} pages, {} choices)", n.ending, n.pages, n.choices);
                s.pc = 0;
                s.depth = 0;
                n.finished = true;
                n.fade = 1.0f;
                title_menu(n, s);
                return;
            case OpKind::Fade:
                ++s.pc;
                n.fade = 1.0f;
                n.hold_ticks = n.autopilot ? 0 : kFadeTicks;
                n.mode = Mode::Fade;
                return;
            case OpKind::Wait:
                ++s.pc;
                n.hold_ticks = n.autopilot ? 0 : op.a;
                n.mode = Mode::Wait;
                return;
            case OpKind::Command:
                ++s.pc;
                n.command = op.a;
                if (!n.hooks.command) {
                    MCE_LOG_ERROR("novel: @{} has no command hook (NovelHooks::command); skipped",
                                  op.a < p.command_count ? p.commands[op.a] : "?");
                    break;
                }
                if (n.hooks.command(c, op.a, op)) {
                    n.mode = Mode::Game;
                    return;
                }
                break;
        }
    }
    MCE_LOG_ERROR("novel: the script ran 4096 statements without waiting at pc {}", s.pc);
    title_menu(n, s);
    s.pc = 0;
}

namespace {

void choose(Context& c, u32 visible_index) {
    NovelState& n = c.n;
    const Option& o = c.p.options[n.opt_first + n.vis[visible_index]];
    if (!option_open(c, o)) return;
    play_sfx(c, c.p.sfx_confirm, -8.0f);
    c.s.pc = o.target;
    ++n.choices;
    resume(c);
}

void start_run(Context& c) {
    NovelState& n = c.n;
    const u32 seen = c.s.endings_seen;
    c.s = StoryState{};
    c.s.endings_seen = seen;
    c.s.pc = c.p.start;
    n.log_count = n.log_head = 0;
    n.finished = false;
    n.ending = 0;
    n.pages = n.choices = n.letters_read = 0;
    n.fade = 1.0f;
    if (n.hooks.new_run) n.hooks.new_run(c);
    resume(c);
}

// ---- the bot -----------------------------------------------------------------------------------

void bot_step(Context& c) {
    NovelState& n = c.n;
    switch (n.mode) {
        case Mode::Title:
            if (c.s.pc != 0) resume(c);  // a loaded save: continue it
            else start_run(c);
            break;
        case Mode::Reading:
        case Mode::Letter:
        case Mode::Checkpoint:
        case Mode::Ending:
        case Mode::Fade:
        case Mode::Wait: resume(c); break;
        case Mode::Game:
            if (n.hooks.game_bot) n.hooks.game_bot(c);
            else resume(c);
            break;
        case Mode::Choice: {
            // The policy's preference row (story.json "bot"), then the first option that can be taken.
            const u32 policy = n.autopilot - 1u;
            i32 pick = -1;
            for (u32 k = 0; k < 4 && pick < 0; ++k) {
                const u16 want = policy < c.p.tag_count && c.p.bot_prefer ? c.p.bot_prefer[policy * 4 + k] : (k == 0 ? static_cast<u16>(policy) : kNone);
                if (want == kNone) break;
                for (u32 v = 0; v < n.vis_count && pick < 0; ++v) {
                    const Option& o = c.p.options[n.opt_first + n.vis[v]];
                    if (o.tag == want && option_open(c, o)) pick = static_cast<i32>(v);
                }
            }
            for (u32 v = 0; v < n.vis_count && pick < 0; ++v)
                if (option_open(c, c.p.options[n.opt_first + n.vis[v]])) pick = static_cast<i32>(v);
            if (pick >= 0) choose(c, static_cast<u32>(pick));
            break;
        }
        default: break;
    }
}

// ---- input ---------------------------------------------------------------------------------------

void player_step(Context& c) {
    NovelState& n = c.n;
    StoryState& s = c.s;
    const Program& p = c.p;
    const Keys k = read_keys(c.f, n);
    switch (n.mode) {
        case Mode::Title:
            if ((k.up || k.down) && n.vis_count > 0) {
                n.cursor = static_cast<u8>((n.cursor + (k.up ? n.vis_count - 1 : 1)) % n.vis_count);
                play_sfx(c, p.sfx_move, -12.0f);
            }
            if (k.advance) {
                // rows: New game, [Continue,] [extras]
                const bool has_continue = s.pc != 0;
                if (n.cursor == 0) {
                    start_run(c);
                } else if (has_continue && n.cursor == 1) {
                    n.fade = 1.0f;
                    resume(c);
                } else {
                    n.mode = Mode::Extras;
                }
                play_sfx(c, p.sfx_confirm, -8.0f);
            }
            break;
        case Mode::Extras:
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
                resume(c);
                break;
            }
            if (k.advance) {
                if (!complete) {
                    n.revealed = static_cast<f32>(n.page_chars);
                } else {
                    play_sfx(c, p.sfx_page, -14.0f);
                    resume(c);
                }
            }
            break;
        }
        case Mode::Backlog:
            if (k.up || k.log) n.log_scroll = static_cast<u8>(math::min<u32>(n.log_scroll + 1u, n.log_count > 1 ? n.log_count - 1u : 0u));
            if (k.down || k.back) {
                if (n.log_scroll == 0) n.mode = Mode::Reading;
                else --n.log_scroll;
            }
            if (k.advance) n.mode = Mode::Reading;
            break;
        case Mode::Choice:
            if ((k.up || k.down) && n.vis_count > 0) {
                n.cursor = static_cast<u8>((n.cursor + (k.up ? n.vis_count - 1 : 1)) % n.vis_count);
                play_sfx(c, p.sfx_move, -12.0f);
            }
            if (k.advance) choose(c, n.cursor);
            break;
        case Mode::Letter:
            if (k.advance || k.skip) {
                play_sfx(c, p.sfx_page, -14.0f);
                resume(c);
            }
            break;
        case Mode::Game:
            if (n.hooks.game_input) n.hooks.game_input(c, k);
            else resume(c);
            break;
        case Mode::Checkpoint:
        case Mode::Ending:
            if (k.advance) {
                play_sfx(c, p.sfx_confirm, -8.0f);
                resume(c);
            }
            break;
        case Mode::Fade:
        case Mode::Wait:
            if (n.hold_ticks > 0) --n.hold_ticks;
            else resume(c);
            break;
        default: break;
    }
}

// ---- the default stage ---------------------------------------------------------------------------

// A stage for a scene that authors none (the novel template): Undelivered's layout on a 20 x 11.25 m
// view (1280 x 720 at 64 pixels per metre), solid panels instead of its paper textures.
void spawn_default_stage(Frame& f, const NovelState& n) {
    Commands& cmd = f.commands();
    auto at = [](f32 x, f32 y) {
        LocalTransform t;
        t.position = {x, y, 0.0f};
        return t;
    };
    auto part = [](StageRole r, u32 i = 0) {
        StagePart sp;
        sp.role = r;
        sp.index = static_cast<u8>(i);
        return sp;
    };
    auto quad = [](f32 w, f32 h, i16 layer, math::Color tint, i32 order = 0) {
        kit::Sprite s;
        s.size = {w, h};
        s.layer = layer;
        s.order = order;
        s.tint = tint;
        return s;
    };
    auto label = [&](f32 height, kit::TextAlign align, math::Color color, i16 layer) {
        kit::TextLabel t;
        t.font = asset::AssetRef<asset::Font>{n.font, {}};
        t.height = height;
        t.align = align;
        t.color = color;
        t.layer = layer;
        return t;
    };
    constexpr f32 W = 20.0f, H = 11.25f;
    const math::Color paper{0.92f, 0.88f, 0.8f, 1};
    cmd.spawn(at(0, 0), part(StageRole::Background), quad(W, H, 0, {1, 1, 1, 0}));
    for (u32 i = 0; i < 3; ++i)
        cmd.spawn(at(-5.5f + 5.5f * static_cast<f32>(i), -1.0f), part(StageRole::Cast, i), quad(5.625f, 11.25f, 2, {1, 1, 1, 0}));
    cmd.spawn(at(0, 0), part(StageRole::Dim), quad(W + 2, H + 2, 5, {0.02f, 0.015f, 0.01f, 0}));
    cmd.spawn(at(0, 0), part(StageRole::Fade), quad(W + 2, H + 2, 12, {0, 0, 0, 1}));
    cmd.spawn(at(0, 3.0f), part(StageRole::Glow), quad(14.6f, 1.2f, 6, {1, 0.9f, 0.7f, 0}));
    cmd.spawn(at(0, -3.828f), part(StageRole::Box), quad(W, 3.594f, 6, {0.07f, 0.06f, 0.06f, 0}));
    cmd.spawn(at(-6.3f, -1.95f), part(StageRole::NameTag), quad(5.0f, 0.94f, 7, {0.3f, 0.2f, 0.12f, 0}));
    cmd.spawn(at(-6.3f, -1.95f), part(StageRole::NameText), label(0.42f, kit::TextAlign::Center, {1, 0.93f, 0.8f, 1}, 8));
    for (u32 i = 0; i < 4; ++i)
        cmd.spawn(at(-6.7f, -2.85f - 0.62f * static_cast<f32>(i)), part(StageRole::Line, i), label(0.5f, kit::TextAlign::Left, paper, 8));
    for (u32 i = 0; i < 4; ++i) {
        const f32 y = 3.0f - 1.9f * static_cast<f32>(i);
        cmd.spawn(at(0, y), part(StageRole::ChoiceRow, i), quad(14.06f, 0.97f, 7, {1, 1, 1, 0}));
        cmd.spawn(at(-6.6f, y), part(StageRole::ChoiceText, i), label(0.5f, kit::TextAlign::Left, paper, 8));
        cmd.spawn(at(-6.6f, y - 0.37f), part(StageRole::ChoiceText2, i), label(0.5f, kit::TextAlign::Left, paper, 8));
    }
    cmd.spawn(at(0, 0.3f), part(StageRole::LetterCard), quad(15.6f, 10.0f, 9, {0.93f, 0.89f, 0.8f, 0}));
    for (u32 i = 0; i < 12; ++i)
        cmd.spawn(at(-6.6f, 4.4f - 0.74f * static_cast<f32>(i)), part(StageRole::LetterLine, i),
                  label(0.47f, kit::TextAlign::Left, {0.17f, 0.13f, 0.1f, 1}, 10));
    cmd.spawn(at(0, 0.2f), part(StageRole::Panel), quad(17.2f, 9.7f, 9, {0.08f, 0.07f, 0.06f, 0}));
    for (u32 i = 0; i < 14; ++i)
        cmd.spawn(at(-7.6f, 4.3f - 0.62f * static_cast<f32>(i)), part(StageRole::PanelLine, i), label(0.46f, kit::TextAlign::Left, paper, 10));
    cmd.spawn(at(9.6f, 5.15f), part(StageRole::Hud), label(0.38f, kit::TextAlign::Right, {0.85f, 0.8f, 0.72f, 0.85f}, 11));
    cmd.spawn(at(0, 4.6f), part(StageRole::Title), label(1.25f, kit::TextAlign::Center, {0.98f, 0.94f, 0.85f, 1}, 11));
    audio::AudioSource music, ambience;
    music.loop = ambience.loop = true;
    music.bus = audio::Bus_Music;
    music.gain_db = -8.0f;
    ambience.bus = audio::Bus_SFX;
    ambience.gain_db = -12.0f;
    cmd.spawn(at(0, 0), part(StageRole::Music), music);
    cmd.spawn(at(0, 0), part(StageRole::Ambience), ambience);
}

// ---- systems -----------------------------------------------------------------------------------

void novel_setup(Frame& f) {
    NovelState& n = f.resource<NovelState>();
    if (n.mode != Mode::Boot) return;
    if (!n.program) {
        n.mode = Mode::Done;
        return;
    }
    const Program& p = *n.program;
    if (!n.assets)
        if (kit::SequenceRegistry* reg = f.try_resource<kit::SequenceRegistry>()) n.assets = reg->assets();
    if (n.assets) build_tables(n, p);
    else MCE_LOG_WARN("novel: no asset system reachable (no SequenceRegistry resource); art and sound stay blank");
    f.query<const StoryState&>().each([&](Entity e, const StoryState&) {
        if (n.save_entity.is_null()) n.save_entity = e;
    });
    if (n.save_entity.is_null()) {
        MCE_LOG_ERROR("novel: the scene has no StoryState entity (with a SaveKey); nothing to run");
        n.mode = Mode::Done;
        return;
    }
    u32 parts = 0;
    RowLayout rows;
    f.query<const StagePart&, const LocalTransform&>().each([&](const StagePart& sp, const LocalTransform& lt) {
        ++parts;
        if (sp.role == StageRole::ChoiceRow && sp.index < 4) rows.y[sp.index] = lt.position.y;
    });
    f.world().set_resource(rows);
    if (parts == 0 && n.default_stage) spawn_default_stage(f, n);
    StoryState& s = *f.world().try_get_mut<StoryState>(n.save_entity);
    // Lit backgrounds: every layer takes the program's normal map.
    f.query<const StagePart&, kit::Sprite&>().each([&](const StagePart& sp, kit::Sprite& spr) {
        if (sp.role == StageRole::Background && !n.normal_map.is_null()) spr.normal_map = asset::AssetRef<asset::Texture>{n.normal_map, {}};
    });
    n.fade = 1.0f;
    title_menu(n, s);

    // The host's flags (mce/kit/novel_host.h), when it declares them.
    if (const ConVar* cv = ConVar::find(MCE_SID("novel.autoplay"))) n.autopilot = static_cast<u8>(math::clamp(cv->get_int(), 0, 255));
    if (const ConVar* cv = ConVar::find(MCE_SID("novel.instant"))) n.instant_text = n.instant_text || cv->get_bool();
    const ConVar* label = ConVar::find(MCE_SID("novel.label"));
    if (label && !label->get_string().empty()) {
        const StringView want = label->get_string();
        const u16 at = find_label(p, want);
        if (at == kNone) {
            MCE_LOG_ERROR("novel: no script label '{}'", want);
        } else {
            Context c{f, n, s, p};
            start_run(c);
            s.pc = at;
            s.depth = 0;
            n.mode = Mode::Reading;
            resume(c);
            i32 skip = 0;
            if (const ConVar* cv = ConVar::find(MCE_SID("novel.skip"))) skip = cv->get_int();
            for (i32 i = 0; i < skip && (n.mode == Mode::Reading || n.mode == Mode::Letter || n.mode == Mode::Fade || n.mode == Mode::Wait); ++i)
                resume(c);
            n.revealed = n.instant_text ? static_cast<f32>(n.page_chars) : 0.0f;
            n.fade = 0.0f;
            MCE_LOG_INFO("novel: started at label '{}' (pc {}, mode {}, location {}, time {})", want, s.pc, static_cast<u32>(n.mode), s.bg,
                         s.bg_time);
        }
    }
}

void novel_step(Frame& f) {
    NovelState& n = f.resource<NovelState>();
    if (n.mode == Mode::Boot || n.mode == Mode::Done) return;
    StoryState* s = f.world().try_get_mut<StoryState>(n.save_entity);
    if (!s) return;
    Context c{f, n, *s, *n.program};
    ++n.ticks;
    if (n.skip_cooldown > 0) --n.skip_cooldown;
    if (n.mode == Mode::Reading && n.revealed < static_cast<f32>(n.page_chars))
        n.revealed = math::min(n.revealed + kRevealPerSecond * f.dt, static_cast<f32>(n.page_chars));
    if (n.autopilot) bot_step(c);
    else player_step(c);
    n.fade = math::max(0.0f, n.fade - f.dt * 1.6f);
}

// ---- presentation ------------------------------------------------------------------------------

math::Color mul(math::Color a, math::Color b, f32 alpha) { return {a.r * b.r, a.g * b.g, a.b * b.b, alpha}; }
math::Color scale(math::Color a, f32 k, f32 alpha) { return {a.r * k, a.g * k, a.b * k, alpha}; }

void set_texture(kit::Sprite& sp, asset::Guid g) {
    if (sp.texture.guid == g) return;
    sp.texture = asset::AssetRef<asset::Texture>{g, {}};
}

void set_text(kit::TextLabel& t, StringView s) {
    if (t.text.view() == s) return;
    t.text.assign(s);
}

RowText row_text(Context& c, u32 row, bool game_rows) {
    const NovelState& n = c.n;
    RowText r;
    if (game_rows) return c.n.hooks.row ? c.n.hooks.row(c, row) : RowText{nullptr, nullptr, false};
    if (n.mode == Mode::Choice) {
        if (row >= n.vis_count) return {nullptr, nullptr, false};
        const Option& o = c.p.options[n.opt_first + n.vis[row]];
        u16 t = 0, t2 = 0;
        localized_option(c.p, story_locale(c.p, c.f.world()), o, t, t2);
        r.a = text(c.p, t);
        r.b = text(c.p, t2);
        r.enabled = option_open(c, o);
        return r;
    }
    if (n.mode == Mode::Title) {
        const bool has_continue = c.s.pc != 0;
        const char* rows[3] = {"New game", has_continue ? "Continue" : c.p.extras, has_continue ? c.p.extras : ""};
        if (row >= n.vis_count || row >= 3) return {nullptr, nullptr, false};
        r.a = rows[row];
        return r;
    }
    return {nullptr, nullptr, false};
}

void panel_line(Context& c, Panel panel, u32 line, char* buf, u32 cap) {
    buf[0] = 0;
    if (c.n.hooks.panel_line && c.n.hooks.panel_line(c, panel, line, buf, cap)) return;
    const NovelState& n = c.n;
    const Program& p = c.p;
    switch (panel) {
        case Panel::Checkpoint:
            if (line == 1) std::snprintf(buf, cap, "%u pages read", n.pages);
            else if (line == 13) std::snprintf(buf, cap, "%s", n.autosave ? "Saved.  Enter to go on." : "Enter to go on.");
            break;
        case Panel::Ending:
            if (line == 1) std::snprintf(buf, cap, "%s", n.ending < p.ending_count ? p.endings[n.ending].title : "");
            else if (line == 3) std::snprintf(buf, cap, "Ending %u of %u", static_cast<u32>(n.ending), p.ending_count > 0 ? p.ending_count - 1u : 0u);
            else if (line == 13) std::snprintf(buf, cap, "Enter");
            break;
        case Panel::Extras:
            if (line == 0) {
                std::snprintf(buf, cap, "%s", p.extras);
            } else if (line >= 2 && line < 2u + p.ending_count - 1u && line <= 12) {
                const u32 e = line - 1;
                const bool seen = (c.s.endings_seen >> e) & 1;
                std::snprintf(buf, cap, "%s  %s", seen ? "*" : "-", seen ? p.endings[e].title : "an ending not yet reached");
            } else if (line == 13) {
                std::snprintf(buf, cap, "Enter to go back");
            }
            break;
        case Panel::Backlog: {
            // the last 14 lines before the scroll point, newest at the bottom
            i32 want = 13 - static_cast<i32>(line) + static_cast<i32>(n.log_scroll);  // lines back from the newest
            const LocaleText* lt = story_locale(p, c.f.world());
            for (u32 e = 0; e < n.log_count && want >= 0; ++e) {
                const LogEntry& le = n.log[(n.log_head + 63 - e) % 64];
                u16 shown[4];
                localized_page(p, lt, le.line, shown);
                u32 lines = 0;
                for (u32 i = 0; i < 4; ++i) lines += shown[i] ? 1u : 0u;
                if (want < static_cast<i32>(lines)) {
                    const u32 li = lines - 1 - static_cast<u32>(want);
                    if (li == 0 && le.speaker < p.character_count)
                        std::snprintf(buf, cap, "%s: %s", localized_speaker(p, lt, le.speaker), text(p, shown[li]));
                    else
                        std::snprintf(buf, cap, "%s", text(p, shown[li]));
                    break;
                }
                want -= static_cast<i32>(lines);
            }
            break;
        }
    }
}

void novel_present(Frame& f) {
    NovelState& n = f.resource<NovelState>();
    if (n.mode == Mode::Boot || n.mode == Mode::Done) return;
    StoryState* sp = f.world().try_get_mut<StoryState>(n.save_entity);
    if (!sp) return;
    StoryState& s = *sp;
    const Program& p = *n.program;
    Context c{f, n, s, p};
    const RowLayout rows = f.world().try_resource<RowLayout>() ? f.resource<RowLayout>() : RowLayout{};

    const bool title = n.mode == Mode::Title || n.mode == Mode::Extras;
    const bool game = n.mode == Mode::Game;
    ScreenLook look;
    look.location = title ? p.title_location : s.bg;
    look.time = title ? p.title_time : s.bg_time;
    look.lut = title ? p.title_lut : s.bg_lut;
    look.music = title ? p.title_music : s.music;
    look.ambience = title ? p.title_ambience : s.ambience;
    look.cast = !title;
    look.box = n.mode == Mode::Reading || n.mode == Mode::Backlog;
    look.rows = n.mode == Mode::Choice || n.mode == Mode::Title;
    if (n.hooks.screen) n.hooks.screen(c, look);

    const TimeLook pal = look.time < p.time_count ? p.times[look.time] : TimeLook{"", {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, 0, 5, 1};
    const f32 t = static_cast<f32>(n.ticks) / 60.0f;
    const bool panel_up = n.mode == Mode::Checkpoint || n.mode == Mode::Backlog || n.mode == Mode::Ending || n.mode == Mode::Extras;
    const bool complete = n.revealed >= static_cast<f32>(n.page_chars);
    // The page and the name tag in the player's language; the sim types the base page, the shown
    // one keeps pace with it.
    const LocaleText* tr = story_locale(p, f.world());
    u16 page[4];
    localized_page(p, tr, n.page_line, page);
    u32 page_points = 0;
    for (u32 i = 0; i < 4; ++i) page_points += page[i] ? utf8_points(StringView(text(p, page[i]))) : 0;
    const u32 shown_budget = scale_reveal(static_cast<u32>(n.revealed), n.page_chars, page_points);
    const u32 loc = look.location < NovelState::kLocations ? look.location : 0;
    char buf[128];

    f.query<const StagePart&, kit::Sprite&, LocalTransform&>().each([&](const StagePart& part, kit::Sprite& spr, LocalTransform& lt) {
        switch (part.role) {
            case StageRole::Background: {
                const bool on = loc != 0 && part.index < n.bg_count[loc];
                if (on) set_texture(spr, n.bg[loc][part.index]);
                spr.tint = mul(pal.paper, math::Color::white(), on ? 1.0f : 0.0f);
                // the paper theatre: near layers drift a little more than far ones
                const f32 k = static_cast<f32>(part.index);
                lt.position.x = std::sin(t * 0.21f + k * 1.7f) * 0.025f * k;
                lt.position.y = std::sin(t * 0.17f + k * 0.9f) * 0.015f * k;
                break;
            }
            case StageRole::Cast: {
                const u32 slot = part.index < kMaxSlots ? part.index : 0;
                const u8 who = s.cast[slot];
                const u8 row = who < p.character_count ? p.characters[who].cast : 255;
                const bool on = look.cast && row < NovelState::kCast;
                f32& a = n.cast_alpha[slot];
                a += ((on ? 1.0f : 0.0f) - a) * 0.18f;
                if (on) {
                    const u8 e = s.expr[slot] < NovelState::kExpr ? s.expr[slot] : 0;
                    const u8 po = s.pose[slot] < NovelState::kPoses ? s.pose[slot] : 0;
                    const asset::Guid g = n.cast[row][e][po];
                    set_texture(spr, g.is_null() ? n.cast[row][0][po] : g);
                }
                const bool talking = n.page_speaker == who;
                const bool someone_else = n.page_speaker < p.character_count && !talking && n.mode == Mode::Reading &&
                                          p.characters[n.page_speaker].cast != 255;
                const f32 dim = someone_else ? 0.68f : 1.0f;
                spr.tint = mul(pal.cast, {dim, dim, dim, 1}, a < 0.01f ? 0.0f : a);
                lt.position.y = -1.0f + (1.0f - a) * -0.3f + (talking ? std::sin(t * 6.0f) * 0.02f : 0.0f);
                break;
            }
            case StageRole::Dim: spr.tint.a = (look.rows && n.mode != Mode::Title) || panel_up ? 0.45f : 0.0f; break;
            case StageRole::Fade: spr.tint.a = n.fade; break;
            case StageRole::Box: spr.tint.a = look.box ? 1.0f : 0.0f; break;
            case StageRole::NameTag: spr.tint.a = n.mode == Mode::Reading && n.page_speaker < p.character_count ? 1.0f : 0.0f; break;
            case StageRole::ChoiceRow: {
                const RowText r = row_text(c, part.index, game && look.rows);
                const bool on = look.rows && r.a != nullptr;
                const bool selected = on && part.index == n.cursor;
                const f32 lum = !r.enabled ? 0.45f : selected ? 1.0f : 0.78f;
                spr.tint = scale(p.row_tint, lum, on ? 1.0f : 0.0f);
                spr.size.y = (r.b && r.b[0]) ? 1.75f : 0.97f;
                break;
            }
            case StageRole::LetterCard: spr.tint.a = n.mode == Mode::Letter ? 1.0f : 0.0f; break;
            case StageRole::Panel: spr.tint.a = panel_up ? 1.0f : 0.0f; break;
            case StageRole::Glow: {
                const bool on = look.rows || look.glow_owned;
                spr.tint.a = on ? 0.35f + std::sin(t * 3.0f) * 0.08f : 0.0f;
                if (!look.glow_owned) lt.position = {0.0f, rows.y[n.cursor < 4 ? n.cursor : 0], lt.position.z};
                break;
            }
            default: break;
        }
    });

    f.query<const StagePart&, kit::TextLabel&, LocalTransform&>().each([&](const StagePart& part, kit::TextLabel& tl, LocalTransform& lt) {
        switch (part.role) {
            case StageRole::NameText:
                set_text(tl, n.mode == Mode::Reading && n.page_speaker < p.character_count ? StringView(localized_speaker(p, tr, n.page_speaker))
                                                                                            : StringView());
                break;
            case StageRole::Line: {
                if (game) {
                    const char* line = look.box && part.index < 4 && look.lines[part.index] ? look.lines[part.index] : "";
                    set_text(tl, StringView(line));
                    tl.color = look.line_color;
                    break;
                }
                if (!(n.mode == Mode::Reading || n.mode == Mode::Backlog) || part.index >= 4) {
                    set_text(tl, StringView());
                    break;
                }
                // reveal: code points up to `revealed`, spent line by line
                u32 budget = shown_budget;
                StringView shown;
                for (u32 i = 0; i <= part.index; ++i) {
                    const StringView line(text(p, page[i]));
                    const u32 pts = utf8_points(line);
                    if (i == part.index) shown = complete ? line : utf8_prefix(line, math::min(budget, pts));
                    budget = budget > pts ? budget - pts : 0;
                }
                set_text(tl, shown);
                tl.color = n.page_speaker >= p.character_count ? p.ink_narration : p.ink_speech;
                break;
            }
            case StageRole::ChoiceText:
            case StageRole::ChoiceText2: {
                const RowText r = row_text(c, part.index, game && look.rows);
                const char* txt = part.role == StageRole::ChoiceText ? r.a : r.b;
                set_text(tl, look.rows && txt ? StringView(txt) : StringView());
                const bool two = r.b && r.b[0];
                const f32 base = rows.y[part.index < 4 ? part.index : 0];
                lt.position.y = part.role == StageRole::ChoiceText ? base + (two ? 0.33f : 0.0f) : base - 0.37f;
                tl.color = !r.enabled ? p.ink_row_disabled : part.index == n.cursor ? p.ink_row_selected : p.ink_row;
                break;
            }
            case StageRole::LetterLine:
                set_text(tl, n.mode == Mode::Letter && part.index < n.letter_count && n.letter_first + part.index < p.letter_line_count
                                 ? StringView(text(p, p.letter_lines[n.letter_first + part.index]))
                                 : StringView());
                break;
            case StageRole::PanelLine: {
                buf[0] = 0;
                if (n.mode == Mode::Checkpoint) panel_line(c, Panel::Checkpoint, part.index, buf, sizeof(buf));
                else if (n.mode == Mode::Ending) panel_line(c, Panel::Ending, part.index, buf, sizeof(buf));
                else if (n.mode == Mode::Extras) panel_line(c, Panel::Extras, part.index, buf, sizeof(buf));
                else if (n.mode == Mode::Backlog) panel_line(c, Panel::Backlog, part.index, buf, sizeof(buf));
                set_text(tl, StringView(buf));
                break;
            }
            case StageRole::Hud:
                buf[0] = 0;
                if (!title && n.hooks.hud) n.hooks.hud(c, buf, sizeof(buf));
                set_text(tl, StringView(buf));
                break;
            case StageRole::Title: set_text(tl, title ? StringView(p.title) : StringView()); break;
            default: break;
        }
    });

    // music and ambience: one looping source each; a change stops the old voice and restarts
    const audio::AudioRef* ar = f.try_resource<audio::AudioRef>();
    f.query<const StagePart&, audio::AudioSource&>().each([&](const StagePart& part, audio::AudioSource& src) {
        asset::Guid want;
        if (part.role == StageRole::Music) want = look.music < NovelState::kSounds ? n.music[look.music] : asset::Guid{};
        else if (part.role == StageRole::Ambience) want = look.ambience < NovelState::kSounds ? n.ambience[look.ambience] : asset::Guid{};
        else return;
        if (src.clip.guid == want) return;
        if (ar && ar->device && !src.handle.is_null()) ar->device->stop(src.handle, 1.2f);
        src.handle = {};
        src.clip = asset::AssetRef<asset::Sfx>{want, {}};
        src.loop = true;
        src.started = false;
        src.play_on_start = !want.is_null();
    });

    f.query<const StagePart&, kit::Light2D&, LocalTransform&>().each([&](const StagePart& part, kit::Light2D& l, LocalTransform& lt) {
        if (part.role == StageRole::AmbientLight) {
            l.kind = kit::Light2DKind::Ambient;
            l.color = pal.ambient;
        } else if (part.role == StageRole::Light) {
            l.kind = kit::Light2DKind::Point;
            l.color = pal.lamp;
            l.intensity = pal.lamp_intensity * 0.45f * (0.95f + std::sin(t * 2.3f) * 0.05f);
            l.radius = 7.5f;
            lt.position.x += (pal.lamp_x - lt.position.x) * 0.05f;
            lt.position.y += (pal.lamp_y - lt.position.y) * 0.05f;
        }
    });

    f.query<const StagePart&, kit::Post2DComponent&>().each([&](const StagePart& part, kit::Post2DComponent& post) {
        if (part.role != StageRole::Camera) return;
        const asset::Guid g = n.lut[look.lut < NovelState::kLuts ? look.lut : 0];
        if (!g.is_null() && post.lut.guid != g) post.lut = asset::AssetRef<asset::Texture>{g, {}};
        post.color_grade = !g.is_null();
        post.vignette = true;
    });

    if (n.hooks.present) n.hooks.present(c, look);
}

}  // namespace

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

StoryState* story_state(World& world) {
    NovelState* n = world.try_resource<NovelState>();
    if (!n || n->save_entity.is_null()) return nullptr;
    return world.try_get_mut<StoryState>(n->save_entity);
}

void new_game(World& world) {
    NovelState* n = world.try_resource<NovelState>();
    if (StoryState* s = story_state(world); s && n && n->program) {
        const u32 seen = s->endings_seen;
        *s = StoryState{};
        s->endings_seen = seen;
        s->pc = n->program->start;
    }
    if (n) {
        n->mode = Mode::Reading;
        n->fade = 1.0f;
    }
}

ResultV install(World& world, const Program& program, const NovelHooks& hooks) {
    if (!world.try_resource<input::InputState>()) input::install(world);
    world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
    world.register_components<StagePart, StoryState>();
    NovelState* existing = world.try_resource<NovelState>();
    if (!existing) {
        world.set_resource(NovelState{});
        existing = world.try_resource<NovelState>();
    }
    existing->program = &program;
    existing->hooks = hooks;
    if (existing->save_slot.empty()) existing->save_slot.assign(StringView("novel_autosave"));
    world.add_system(Phase::FixedUpdate_Pre, "novel_setup", &novel_setup);
    world.add_system(Phase::FixedUpdate_Pre, "novel_step", &novel_step, "novel_setup");
    world.add_system(Phase::FixedUpdate_Post, "novel_present", &novel_present);
    return ok();
}

}  // namespace mce::novel
