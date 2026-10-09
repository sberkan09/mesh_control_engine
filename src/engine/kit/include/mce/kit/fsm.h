#pragma once
// FSM helper (FR-ENT-090, scripting.md §2.5): state is data, the transition table is data,
// per-state behavior stays in ordinary systems that switch on the state. The helper adds only
// what the hand-written enum cascade gets wrong: timed transitions, entry/exit actions, ordered
// guard evaluation, and validation at install.
//
//   enum class Grunt : u8 { Idle, Patrol, Chase, WindUp, Recover, Count };
//   struct GruntBrain { Grunt state = Grunt::Idle; u8 pad_[3]; f32 state_time = 0; ... };
//   MCE_COMPONENT(GruntBrain, (state, Save), (state_time, Save, ReadOnly), ...)  // live in the inspector
//
//   constexpr fsm::Trans<Grunt> kGrunt[] = {
//       {Grunt::Idle,    Grunt::Patrol,  nullptr,      1.5f},                       // timed
//       {Grunt::Patrol,  Grunt::Chase,   &sees_player, -1.0f, &alert},              // guarded, entry
//       {Grunt::WindUp,  Grunt::Recover, nullptr,      0.35f, nullptr, &swing},     // exit action
//   };
//   MCE_TRY(fsm::validate(Span(kGrunt), Grunt::Idle));                 // at install
//   world.register_event<fsm::StateChanged<Grunt>>(Phase::FixedUpdate_Pre);  // optional
//   void grunt_fsm(Frame& f) { fsm::run<GruntBrain>(f, Span(kGrunt)); }
//
// Semantics (the FR-ENT-090 / scripting.md §6 test list):
//   - each tick, state_time advances by the tick's dt first, then the current state's
//     transitions are evaluated in table order and the first that passes is taken; at most one
//     transition per entity per tick;
//   - a timed transition (`after` >= 0) fires on the first tick whose state_time reaches it —
//     tick n = ceil(after / dt), with a 1e-4 s tolerance for float accumulation;
//   - taking a transition runs exit (on the entity, still in the old state), sets the state,
//     resets state_time to 0, then runs entry: exit -> entry, exactly once each;
//   - when StateChanged<S> is registered as an event it is pushed for every transition.
// Guards and actions are plain function pointers (code, never serialized); state and
// state_time are component fields and serialize like any other.

#include "mce/ecs/world.h"

namespace mce::fsm {

using Guard = bool (*)(Frame& f, Entity e);
using Action = void (*)(Frame& f, Entity e);

template <class S>
struct Trans {
    S from;
    S to;
    Guard when = nullptr;    // guarded transition: taken when this returns true
    f32 after = -1.0f;       // timed transition: taken once state_time >= after (seconds)
    Action entry = nullptr;  // runs after entering `to`
    Action exit = nullptr;   // runs before leaving `from`
};

template <class S>
struct StateChanged {
    Entity entity;
    S from;
    S to;
};

inline constexpr f32 kTimeTolerance = 1e-4f;

// Install-time validation. Errors (Errc::InvalidArgument naming the first offending row): a row
// that is both guarded and timed, or neither; two rows with the same `from` and the same guard
// (or the same delay) — ambiguous, the second can never fire; a state outside [0, S::Count) when
// the enum has a Count. Unreachable states (never a `to` and not `initial`) are warnings,
// counted in `unreachable` when given.
template <class S>
Result<void> validate(Span<const Trans<S>> table, S initial, u32* unreachable = nullptr) {
    for (usize i = 0; i < table.size(); ++i) {
        const Trans<S>& t = table[i];
        const bool timed = t.after >= 0.0f;
        if (timed == (t.when != nullptr))
            return Error(Errc::InvalidArgument, "fsm.validate",
                         timed ? "a row is both timed and guarded" : "a row is neither timed nor guarded");
        if constexpr (requires { S::Count; }) {
            constexpr u32 count = static_cast<u32>(S::Count);
            if (static_cast<u32>(t.from) >= count || static_cast<u32>(t.to) >= count)
                return Error(Errc::InvalidArgument, "fsm.validate", "a row names a state past Count");
        }
        for (usize j = 0; j < i; ++j) {
            const Trans<S>& o = table[j];
            if (o.from != t.from) continue;
            if ((t.when && o.when == t.when) || (timed && o.after >= 0.0f && o.after == t.after))
                return Error(Errc::InvalidArgument, "fsm.validate", "two rows leave one state on the same condition");
        }
    }
    if (unreachable) {
        *unreachable = 0;
        if constexpr (requires { S::Count; }) {
            for (u32 s = 0; s < static_cast<u32>(S::Count); ++s) {
                bool reached = s == static_cast<u32>(initial);
                for (const Trans<S>& t : table) reached = reached || static_cast<u32>(t.to) == s;
                if (!reached) ++*unreachable;
            }
        }
    }
    return {};
}

// One entity's step: advance the clock, take at most one transition. Returns true when the state
// changed. `push_changed` pushes StateChanged<S> (the caller checks the event is registered).
template <class S>
bool step(Frame& f, Span<const Trans<S>> table, Entity e, S& state, f32& state_time, bool push_changed = false) {
    state_time += f.dt;
    for (const Trans<S>& t : table) {
        if (t.from != state) continue;
        const bool go = t.when ? t.when(f, e) : state_time + kTimeTolerance >= t.after;
        if (!go) continue;
        const S from = state;
        if (t.exit) t.exit(f, e);
        state = t.to;
        state_time = 0.0f;
        if (t.entry) t.entry(f, e);
        if (push_changed) f.events().push(StateChanged<S>{e, from, t.to});
        return true;
    }
    return false;
}

// Runs the table over every entity with component C (fields `state` and `state_time`). Guards
// and actions run mid-query: they may read or write components (C included, which they see
// with the clock already advanced) and push commands/events, but not change structure directly.
template <class C, class S>
void run(Frame& f, Span<const Trans<S>> table) {
    const bool push_changed = f.world().template event_queue<StateChanged<S>>() != nullptr;
    f.query<C&>().each([&](Entity e, C& c) { step(f, table, e, c.state, c.state_time, push_changed); });
}

}  // namespace mce::fsm
