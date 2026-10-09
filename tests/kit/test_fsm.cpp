// FR-ENT-090: the FSM helper — timed transitions on the exact tick, state time reset on
// transition, exit -> entry order exactly once, guard order, one transition per tick, the
// StateChanged event, install-time validation, and state + clock surviving a save.

#include <doctest/doctest.h>

#include "mce/ecs/save.h"
#include "mce/kit/fsm.h"

using namespace mce;

namespace {

enum class Grunt : u8 { Idle, Patrol, Chase, WindUp, Recover, Count };
MCE_ENUM(Grunt, Idle, Patrol, Chase, WindUp, Recover, Count)

struct Brain {
    Grunt state = Grunt::Idle;
    u8 pad_[3] = {};
    f32 state_time = 0.0f;
    u32 sees = 0;  // test input for the guards
    u32 reach = 0;
};
MCE_COMPONENT(Brain, (state, Save), (state_time, Save, ReadOnly), sees, reach)

constexpr f32 kDt = 1.0f / 60.0f;

char g_log[64];
u32 g_log_len = 0;
void log(char c) {
    if (g_log_len + 1 < sizeof(g_log)) g_log[g_log_len++] = c;
    g_log[g_log_len] = '\0';
}
bool sees(Frame& f, Entity e) { return f.world().try_get<Brain>(e)->sees != 0; }
bool in_reach(Frame& f, Entity e) { return f.world().try_get<Brain>(e)->reach != 0; }
bool always(Frame&, Entity) { return true; }
void enter_chase(Frame&, Entity) { log('C'); }
void leave_patrol(Frame&, Entity) { log('p'); }
void swing(Frame&, Entity) { log('S'); }

constexpr fsm::Trans<Grunt> kTable[] = {
    {Grunt::Idle, Grunt::Patrol, nullptr, 1.5f},
    {Grunt::Patrol, Grunt::WindUp, &in_reach, -1.0f},  // checked first: reach beats sight
    {Grunt::Patrol, Grunt::Chase, &sees, -1.0f, &enter_chase, &leave_patrol},
    {Grunt::Chase, Grunt::WindUp, &in_reach, -1.0f},
    {Grunt::WindUp, Grunt::Recover, nullptr, 0.35f, nullptr, &swing},
    {Grunt::Recover, Grunt::Chase, nullptr, 0.6f},
};

void grunt_fsm(Frame& f) { fsm::run<Brain>(f, Span<const fsm::Trans<Grunt>>(kTable)); }

u32 g_changes = 0;
Grunt g_last_to = Grunt::Idle;
void watch(Frame& f) {
    for (const fsm::StateChanged<Grunt>& c : f.events().read<fsm::StateChanged<Grunt>>()) {
        ++g_changes;
        g_last_to = c.to;
    }
}

struct Sim {
    World w;
    u64 ticks = 0;
    explicit Sim(bool events = false) {
        w.register_components<Brain, SaveKey>();
        if (events) {
            w.register_event<fsm::StateChanged<Grunt>>(Phase::FixedUpdate_Pre);
            w.add_system(Phase::FixedUpdate_Post, "watch", &watch);
        }
        w.add_system(Phase::FixedUpdate_Pre, "grunt_fsm", &grunt_fsm);
        g_log_len = 0;
        g_log[0] = '\0';
        g_changes = 0;
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, ticks++);
    }
    Brain& b(Entity e) { return *w.try_get_mut<Brain>(e); }
};

}  // namespace

TEST_CASE("fsm: a timed transition fires on its exact tick and resets state time") {
    Sim s;
    const Entity e = s.w.spawn_with(Brain{});
    s.run(89);  // 1.5 s = tick 90
    CHECK(s.b(e).state == Grunt::Idle);
    CHECK(s.b(e).state_time == doctest::Approx(89 * kDt));
    s.run(1);
    CHECK(s.b(e).state == Grunt::Patrol);
    CHECK(s.b(e).state_time == 0.0f);
    s.run(30);
    CHECK(s.b(e).state == Grunt::Patrol);  // guards false: stays
    CHECK(s.b(e).state_time == doctest::Approx(30 * kDt));
}

TEST_CASE("fsm: exit then entry, exactly once; guards in table order; one transition per tick") {
    Sim s;
    Brain b;
    b.state = Grunt::Patrol;
    const Entity e = s.w.spawn_with(b);
    s.b(e).sees = 1;
    s.run(1);
    CHECK(s.b(e).state == Grunt::Chase);
    CHECK(StringView(g_log) == "pC");
    s.run(5);
    CHECK(StringView(g_log) == "pC");  // no re-entry while staying

    // Both guards true: the earlier row wins.
    const Entity e2 = s.w.spawn_with(b);
    s.b(e2).sees = 1;
    s.b(e2).reach = 1;
    s.run(1);
    CHECK(s.b(e2).state == Grunt::WindUp);
    // Chase -> WindUp is also open for e, but e moved only one step per tick.
    s.b(e).reach = 1;
    s.run(1);
    CHECK(s.b(e).state == Grunt::WindUp);
    CHECK(s.b(e).state_time == 0.0f);
    s.b(e).reach = 0;
    s.b(e2).reach = 0;
    s.b(e2).sees = 0;
    // WindUp 0.35 s (tick 21) -> Recover, swing on exit; Recover 0.6 s (tick 36) -> Chase.
    s.run(20);
    CHECK(s.b(e).state == Grunt::WindUp);
    s.run(1);
    CHECK(s.b(e).state == Grunt::Recover);
    CHECK(StringView(g_log) == "pCSS");  // e2 swung on the same tick
    s.run(35);
    CHECK(s.b(e).state == Grunt::Recover);
    s.run(1);
    CHECK(s.b(e).state == Grunt::Chase);
    CHECK(StringView(g_log) == "pCSS");  // Recover -> Chase has no actions
}

TEST_CASE("fsm: StateChanged is pushed when registered") {
    Sim s(true);
    s.w.spawn_with(Brain{});
    s.run(90);
    CHECK(g_changes == 1);
    CHECK(g_last_to == Grunt::Patrol);
}

TEST_CASE("fsm: validation") {
    using T = fsm::Trans<Grunt>;
    u32 unreachable = 99;
    CHECK(fsm::validate(Span<const T>(kTable), Grunt::Idle, &unreachable));
    CHECK(unreachable == 0);

    const T both[] = {{Grunt::Idle, Grunt::Patrol, &always, 1.0f}};
    CHECK(!fsm::validate(Span<const T>(both), Grunt::Idle));
    const T neither[] = {{Grunt::Idle, Grunt::Patrol}};
    CHECK(!fsm::validate(Span<const T>(neither), Grunt::Idle));
    const T dup_guard[] = {{Grunt::Patrol, Grunt::Chase, &sees}, {Grunt::Patrol, Grunt::WindUp, &sees}};
    CHECK(!fsm::validate(Span<const T>(dup_guard), Grunt::Idle));
    const T dup_time[] = {{Grunt::Idle, Grunt::Patrol, nullptr, 1.0f}, {Grunt::Idle, Grunt::Chase, nullptr, 1.0f}};
    CHECK(!fsm::validate(Span<const T>(dup_time), Grunt::Idle));
    const T past_count[] = {{Grunt::Idle, Grunt::Count, nullptr, 1.0f}};
    CHECK(!fsm::validate(Span<const T>(past_count), Grunt::Idle));

    const T partial[] = {{Grunt::Idle, Grunt::Patrol, nullptr, 1.0f}};
    CHECK(fsm::validate(Span<const T>(partial), Grunt::Idle, &unreachable));
    CHECK(unreachable == 3);  // Chase, WindUp, Recover
}

TEST_CASE("fsm: state and state time survive a save mid-state") {
    Array<byte> payload;
    u64 tick_at_save = 0;
    {
        Sim s;
        s.w.spawn_with(Brain{}, SaveKey{7});
        s.run(60);  // 1.0 s into Idle's 1.5 s
        tick_at_save = s.ticks;
        save::write_save(s.w, {}, payload);
    }
    Sim s;
    s.ticks = tick_at_save;
    const Entity e = s.w.spawn_with(Brain{}, SaveKey{7});
    REQUIRE(save::read_save(s.w, payload.span()));
    CHECK(s.b(e).state == Grunt::Idle);
    CHECK(s.b(e).state_time == doctest::Approx(60 * kDt));
    s.run(29);
    CHECK(s.b(e).state == Grunt::Idle);
    s.run(1);
    CHECK(s.b(e).state == Grunt::Patrol);  // the same tick 90 as an uninterrupted run
}
