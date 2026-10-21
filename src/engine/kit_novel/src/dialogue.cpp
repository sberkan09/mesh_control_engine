// NPC dialogue on the story VM (novel.h, #154): a Dialogue component runs a compiled program from
// a label outside a novel stage. The variables are the world's StoryState's, so a game system
// and the script read and write the same flags and counters, and both are in the save.
#include <cstring>

#include "mce/asset/asset_system.h"
#include "mce/audio/components.h"
#include "mce/ecs/save.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/components.h"
#include "mce/kit/novel.h"
#include "mce/kit/sequence.h"
#include "mce/log.h"
#include "mce/platform/haptics.h"

namespace mce::novel {

namespace {

// The dialogue's runtime (a resource; not saved).
struct DialogueRuntime {
    const Program* program = nullptr;
    bool booted = false;
    asset::Guid sfx[NovelState::kSounds];
    f32 prev_move_y = 0.0f;
    f32 speed = 45.0f;  // typewriter points per second
    DialogueLayout layout;  // where the default box draws, for taps
};

void build_sfx(DialogueRuntime& rt, asset::AssetSystem& a) {
    const Program& p = *rt.program;
    const StringView prefix("audio/sfx/");
    for (asset::AssetKey k = 0; k < a.asset_count(); ++k) {
        const asset::AssetInfo info = a.info(k);
        const StringView path = info.path;
        if (info.type != asset::ChunkType::Sfx || path.size() <= prefix.size() || std::memcmp(path.data(), prefix.data(), prefix.size()) != 0)
            continue;
        StringView name(path.data() + prefix.size(), path.size() - prefix.size());
        for (usize i = name.size(); i-- > 0;)
            if (name[i] == '.') {
                name = StringView(name.data(), i);
                break;
            }
        for (u32 i = 1; i < p.sfx_count && i < NovelState::kSounds; ++i)
            if (name == StringView(p.sfx[i])) rt.sfx[i] = info.guid;
    }
}

Entity vars_entity(World& w) {
    Entity found;
    w.query<const StoryState&>().each([&](Entity e, const StoryState&) {
        if (found.is_null()) found = e;
    });
    return found;
}

void clear_page(Dialogue& d) {
    d.speaker = 255;
    d.expression = 255;
    d.tag_count = 0;
    d.reveal = 0;
    d.waiting_choice = false;
    d.option_count = 0;
    for (u16& l : d.line) l = 0;
}

// Runs statements until the conversation waits on a page or a choice, or ends. @sfx pushes a
// PlaySfx into `events` (null: silent).
void run(Dialogue& d, StoryState& vars, const Program& p, const DialogueRuntime* rt, EventAccess* events) {
    clear_page(d);
    for (u32 guard = 0; guard < 4096; ++guard) {
        if (!d.active || d.pc >= p.op_count) {
            d.active = false;
            return;
        }
        const Op& op = p.ops[d.pc];
        switch (static_cast<OpKind>(op.kind)) {
            case OpKind::Say:
                ++d.pc;
                d.speaker = op.a == kNone ? 255 : static_cast<u8>(op.a);
                d.expression = op.b == kNone ? 255 : static_cast<u8>(op.b);
                d.line[0] = op.c;
                d.line[1] = op.d;
                d.line[2] = op.e;
                d.line[3] = op.f;
                return;
            case OpKind::Tag:
                if (d.tag_count < 4) d.tags[d.tag_count++] = op.a;
                ++d.pc;
                break;
            case OpKind::Choice: {
                d.opt_first = op.a;
                d.option_count = 0;
                for (u32 i = 0; i < op.b && d.option_count < 4; ++i) {
                    const Option& o = p.options[op.a + i];
                    if (o.hidden && !test(vars, p, o.cond)) continue;
                    d.options[d.option_count++] = static_cast<u16>(op.a + i);
                }
                if (d.option_count == 0) {
                    ++d.pc;
                    break;
                }
                d.cursor = 0;
                for (u32 v = 0; v < d.option_count; ++v)
                    if (test(vars, p, p.options[d.options[v]].cond)) {
                        d.cursor = static_cast<u8>(v);
                        break;
                    }
                d.waiting_choice = true;
                return;
            }
            case OpKind::Jump: d.pc = op.a; break;
            case OpKind::Call:
                if (d.depth < 4) d.ret[d.depth++] = static_cast<u16>(d.pc + 1);
                d.pc = op.a;
                break;
            case OpKind::Return:
                if (d.depth == 0) {  // a conversation called as a whole ends at its top-level @return
                    d.active = false;
                    return;
                }
                d.pc = d.ret[--d.depth];
                break;
            case OpKind::If: d.pc = test(vars, p, op.a) ? op.b : static_cast<u16>(d.pc + 1); break;
            case OpKind::Add: set_var(vars, p, op.a, get_var(vars, op.a) + static_cast<i16>(op.b)); ++d.pc; break;
            case OpKind::Set: set_var(vars, p, op.a, static_cast<i16>(op.b)); ++d.pc; break;
            case OpKind::Sfx:
                if (events && rt && op.a < NovelState::kSounds && !rt->sfx[op.a].is_null()) {
                    audio::PlaySfx ev;
                    ev.clip = rt->sfx[op.a];
                    ev.gain_db = -3.0f;
                    ev.bus = audio::Bus_UI;
                    events->push(ev);
                }
                ++d.pc;
                break;
            case OpKind::Ending:
                vars.endings_seen |= 1u << (op.a & 31);
                ++d.pc;
                break;
            case OpKind::End: d.active = false; return;
            default: ++d.pc; break;  // stage statements have no stage here
        }
    }
    d.active = false;
}

StoryState* ensure_vars(World& w, Commands* cmd) {
    const Entity e = vars_entity(w);
    if (!e.is_null()) return w.try_get_mut<StoryState>(e);
    if (cmd) {
        cmd->spawn(StoryState{}, SaveKey{kDialogueSaveKey});
        return nullptr;
    }
    const Entity made = w.spawn();
    w.add(made, StoryState{});
    w.add(made, SaveKey{kDialogueSaveKey});
    return w.try_get_mut<StoryState>(made);
}

bool start(Dialogue& d, StoryState& vars, const Program& p, StringView label, const DialogueRuntime* rt, EventAccess* events) {
    const u16 at = find_label(p, label);
    if (at == kNone) {
        MCE_LOG_ERROR("dialogue: no label '{}'", label);
        return false;
    }
    d.pc = at;
    d.depth = 0;
    d.active = true;
    run(d, vars, p, rt, events);
    return true;
}

void advance(Dialogue& d, StoryState& vars, const Program& p, const DialogueRuntime* rt, EventAccess* events) {
    if (!d.active || d.waiting_choice) return;
    run(d, vars, p, rt, events);
}

void choose(Dialogue& d, StoryState& vars, const Program& p, u8 visible, const DialogueRuntime* rt, EventAccess* events) {
    if (!d.active || !d.waiting_choice || visible >= d.option_count) return;
    const Option& o = p.options[d.options[visible]];
    if (!test(vars, p, o.cond)) return;
    d.pc = o.target;
    d.waiting_choice = false;
    run(d, vars, p, rt, events);
}

u32 revealed(const DialogueRuntime& rt, const Program& p, const Dialogue& d) {
    const u32 all = dialogue_page_points(p, d);
    if (d.reveal == kRevealAll || rt.speed <= 0.0f) return all;
    const u32 shown = static_cast<u32>(static_cast<f32>(d.reveal) * rt.speed / 60.0f);
    return shown < all ? shown : all;
}

void dialogue_run(Frame& f) {
    DialogueRuntime& rt = f.resource<DialogueRuntime>();
    World& w = f.world();
    if (!rt.booted) {
        rt.booted = true;
        if (kit::SequenceRegistry* reg = f.try_resource<kit::SequenceRegistry>(); reg && reg->assets()) build_sfx(rt, *reg->assets());
        ensure_vars(w, &f.commands());
    }
    StoryState* vars = dialogue_vars(w);
    if (!vars) return;  // spawned this tick; requests wait for it
    const Program& p = *rt.program;
    // The typewriter runs on sim ticks, so a replay reveals the same text on the same tick.
    f.query<Dialogue&>().each([&](Dialogue& d) {
        if (d.active && !d.waiting_choice && d.reveal < kRevealAll - 1) ++d.reveal;
    });
    EventAccess events = f.events();
    for (const DialogueStart& r : events.read<DialogueStart>())
        if (Dialogue* d = w.try_get_mut<Dialogue>(r.entity)) start(*d, *vars, p, r.label.view(), &rt, &events);
    for (const DialogueAdvance& r : events.read<DialogueAdvance>())
        if (Dialogue* d = w.try_get_mut<Dialogue>(r.entity)) advance(*d, *vars, p, &rt, &events);
    for (const DialogueChoose& r : events.read<DialogueChoose>())
        if (Dialogue* d = w.try_get_mut<Dialogue>(r.entity)) choose(*d, *vars, p, r.option, &rt, &events);

    // The stock map drives the marked conversation: Enter advances or picks, up/down move.
    const input::InputState* in = f.try_resource<input::InputState>();
    if (!in) return;
    // A tap or click (pointer_pressed) hits the box's rows and the box itself; on its tick the
    // keys' confirm is ignored (a touch the stock map also reads as Jump must not pick twice).
    const kit::ViewInfo* view_res = f.try_resource<kit::ViewInfo>();
    const kit::ViewInfo view = view_res ? *view_res : kit::ViewInfo{};
    math::vec2 at;
    const bool tap = pointer_pressed(*in, view, at);
    const DialogueRects rects = dialogue_rects(rt.layout, view);
    const bool confirm = !tap && (in->confirm_pressed || in->jump_pressed);
    const f32 y = in->move.y;
    const bool up = y > 0.5f && rt.prev_move_y <= 0.5f, down = y < -0.5f && rt.prev_move_y >= -0.5f;
    rt.prev_move_y = y;
    f.query<const DialogueInput&, Dialogue&>().each([&](const DialogueInput& di, Dialogue& d) {
        if (!di.enabled || !d.active) return;
        if (tap) {
            if (d.waiting_choice) {
                for (u32 v = 0; v < d.option_count && v < 4; ++v)
                    if (rects.rows[v].contains(at) && test(*vars, p, p.options[d.options[v]].cond)) {
                        d.cursor = static_cast<u8>(v);
                        platform::haptics::play(platform::haptics::Preset::Selection);  // output only
                        choose(d, *vars, p, d.cursor, &rt, &events);
                        break;
                    }
            } else if (rects.box.contains(at)) {
                if (revealed(rt, p, d) < dialogue_page_points(p, d)) d.reveal = kRevealAll;
                else advance(d, *vars, p, &rt, &events);
            }
            return;
        }
        if (d.waiting_choice) {
            if ((up || down) && d.option_count > 0) d.cursor = static_cast<u8>((d.cursor + (up ? d.option_count - 1 : 1)) % d.option_count);
            if (confirm) choose(d, *vars, p, d.cursor, &rt, &events);
        } else if (confirm) {
            if (revealed(rt, p, d) < dialogue_page_points(p, d)) d.reveal = kRevealAll;  // the rest of the page first
            else advance(d, *vars, p, &rt, &events);
        }
    });
}

}  // namespace

ResultV install_dialogue(World& world, const Program& program) {
    if (!world.try_resource<input::InputState>()) input::install(world);
    world.register_components<StoryState, Dialogue, DialogueInput, SaveKey>();
    world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
    world.register_event<DialogueStart>(Phase::FixedUpdate_Pre);
    world.register_event<DialogueAdvance>(Phase::FixedUpdate_Pre);
    world.register_event<DialogueChoose>(Phase::FixedUpdate_Pre);
    DialogueRuntime rt;
    rt.program = &program;
    world.set_resource(rt);
    world.add_system(Phase::FixedUpdate_Pre, "dialogue_run", &dialogue_run);
    return ok();
}

const Program* dialogue_program(World& world) {
    const DialogueRuntime* rt = world.try_resource<DialogueRuntime>();
    return rt ? rt->program : nullptr;
}

StoryState* dialogue_vars(World& world) {
    const Entity e = vars_entity(world);
    return e.is_null() ? nullptr : world.try_get_mut<StoryState>(e);
}

bool start_dialogue(World& world, Entity e, StringView label) {
    const DialogueRuntime* rt = world.try_resource<DialogueRuntime>();
    Dialogue* d = world.try_get_mut<Dialogue>(e);
    if (!rt || !d) return false;
    StoryState* vars = ensure_vars(world, nullptr);
    d = world.try_get_mut<Dialogue>(e);  // a spawn may have moved it
    if (!vars || !d) return false;
    return start(*d, *vars, *rt->program, label, rt, nullptr);
}

void advance_dialogue(World& world, Entity e) {
    const DialogueRuntime* rt = world.try_resource<DialogueRuntime>();
    Dialogue* d = world.try_get_mut<Dialogue>(e);
    StoryState* vars = dialogue_vars(world);
    if (rt && d && vars) advance(*d, *vars, *rt->program, rt, nullptr);
}

void choose_dialogue(World& world, Entity e, u8 visible_option) {
    const DialogueRuntime* rt = world.try_resource<DialogueRuntime>();
    Dialogue* d = world.try_get_mut<Dialogue>(e);
    StoryState* vars = dialogue_vars(world);
    if (rt && d && vars) choose(*d, *vars, *rt->program, visible_option, rt, nullptr);
}

u32 dialogue_page_points(const Program& p, const Dialogue& d) {
    if (d.waiting_choice) return 0;
    u32 n = 0;
    for (u16 id : d.line)
        if (id != 0) n += utf8_points(StringView(text(p, id)));
    return n;
}

u32 dialogue_revealed(World& world, const Dialogue& d) {
    const DialogueRuntime* rt = world.try_resource<DialogueRuntime>();
    return rt && rt->program ? revealed(*rt, *rt->program, d) : 0;
}

void set_dialogue_layout(World& world, const DialogueLayout& layout) {
    if (DialogueRuntime* rt = world.try_resource<DialogueRuntime>()) rt->layout = layout;
}

DialogueLayout dialogue_layout(World& world) {
    const DialogueRuntime* rt = world.try_resource<DialogueRuntime>();
    return rt ? rt->layout : DialogueLayout{};
}

void set_dialogue_speed(World& world, f32 points_per_second) {
    if (DialogueRuntime* rt = world.try_resource<DialogueRuntime>()) rt->speed = points_per_second < 0.0f ? 0.0f : points_per_second;
}

}  // namespace mce::novel
