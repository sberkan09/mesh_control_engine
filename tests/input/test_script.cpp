// InputScript (mce/input/script.h): scripted action states for headless runs, with the
// InputSystem's edge rules.

#include <doctest/doctest.h>

#include "mce/input/script.h"

using namespace mce;
using namespace mce::input;

TEST_CASE("input script: text form parses into segments") {
    InputScript s;
    REQUIRE(s.parse("# a comment\n10-70 Move=1,0 Jump\n100 Fire\n120- Move=-1,0  # trailing\n\n").has_value());
    REQUIRE(s.segments().size() == 4);
    CHECK(s.segments()[0].from == 10);
    CHECK(s.segments()[0].to == 70);
    CHECK(s.segments()[0].name.view() == "Move");
    CHECK(s.segments()[0].x == 1.f);
    CHECK(s.segments()[0].y == 0.f);
    CHECK(s.segments()[1].name.view() == "Jump");
    CHECK(s.segments()[1].x == 1.f);
    CHECK(s.segments()[2].from == 100);
    CHECK(s.segments()[2].to == 100);
    CHECK(s.segments()[3].to == ~u64(0));
    CHECK(s.segments()[3].x == -1.f);
    CHECK(s.last_tick() == 100);  // open-ended segments do not count

    InputScript bad;
    Result<void> r = bad.parse("ok 1\n");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == Errc::ParseError);
    CHECK(r.error().detail == 1);
    CHECK(r.error().path.view().starts_with("line 1:"));
    CHECK_FALSE(bad.parse("0 Jump").has_value());       // ticks are 1-based
    CHECK_FALSE(bad.parse("5-3 Jump").has_value());     // inverted range
    CHECK_FALSE(bad.parse("5").has_value());            // no action
    CHECK_FALSE(bad.parse("5 Jump=x").has_value());     // bad value
    CHECK(bad.parse("5 Jump=false Fire=true Move=0.5,0.25").has_value());
    CHECK(bad.segments()[0].x == 0.f);
    CHECK(bad.segments()[2].y == 0.25f);
}

TEST_CASE("input script: edges, hold time and the fast-path fields") {
    InputScript s;
    REQUIRE(s.add("Jump", 3, 5));
    REQUIRE(s.add("Move", 2, 10, 1.f, 0.f));
    REQUIRE(s.add("Fire", 4, 4));
    REQUIRE(s.add("Dash", 5, 6, 0.5f));

    const InputState& t1 = s.evaluate(1);
    CHECK(t1.tick == 1);
    CHECK(t1.action_count == 0);
    CHECK_FALSE(t1.jump_held);

    const InputState& t2 = s.evaluate(2);
    CHECK(t2.move.x == 1.f);
    CHECK(t2.action(MCE_SID("Move")).pressed);
    CHECK(t2.action(MCE_SID("Move")).held_time == doctest::Approx(1.f / 60.f));

    const InputState& t3 = s.evaluate(3);
    CHECK(t3.jump_held);
    CHECK(t3.jump_pressed);
    CHECK_FALSE(t3.jump_released);
    CHECK_FALSE(t3.action(MCE_SID("Move")).pressed);
    CHECK(t3.action(MCE_SID("Move")).held_time == doctest::Approx(2.f / 60.f));

    const InputState& t4 = s.evaluate(4);
    CHECK(t4.jump_held);
    CHECK_FALSE(t4.jump_pressed);
    CHECK(t4.fire_pressed);

    const InputState& t5 = s.evaluate(5);
    CHECK_FALSE(t5.fire_pressed);
    CHECK(t5.has_action(MCE_SID("Fire")));  // released this tick: still named, with the edge
    CHECK(t5.action(MCE_SID("Fire")).released);
    CHECK(t5.action(MCE_SID("Dash")).x == 0.5f);
    CHECK(t5.action(MCE_SID("Dash")).pressed);

    const InputState& t6 = s.evaluate(6);
    CHECK_FALSE(t6.jump_held);
    CHECK(t6.jump_released);
    CHECK_FALSE(t6.has_action(MCE_SID("Fire")));  // gone after its release tick

    const InputState& t11 = s.evaluate(11);  // a jump in ticks: no stale edges
    CHECK_FALSE(t11.action(MCE_SID("Move")).released);
    CHECK(t11.action_count == 0);

    // As an IInputSource the loop samples it tick by tick.
    app::IInputSource& src = s;
    CHECK(src.sample_for_tick(12, 0)->tick == 12);
}

TEST_CASE("input script: a later segment wins for the same action") {
    InputScript s;
    REQUIRE(s.add("Move", 1, 10, 1.f, 0.f));
    REQUIRE(s.add("Move", 5, 6, -1.f, 0.f));
    CHECK(s.evaluate(4).move.x == 1.f);
    CHECK(s.evaluate(5).move.x == -1.f);
    CHECK_FALSE(s.evaluate(5).action(MCE_SID("Move")).pressed);
    CHECK(s.evaluate(7).move.x == 1.f);
}

TEST_CASE("input script: Touch puts a finger down and lifts it, Mouse moves the pointer (M-MOBILE 11f)") {
    InputScript s;
    REQUIRE(static_cast<bool>(s.parse("3 Touch=0.25,0.75\n5-6 Mouse=640,360 Point\n")));
    CHECK(s.evaluate(2).touch_points().size() == 0);
    const InputState& down = s.evaluate(3);
    REQUIRE(down.touch_points().size() == 1);
    CHECK(down.touch_points()[0].pressed);
    CHECK(down.touch_points()[0].held);
    CHECK(down.touch_points()[0].pos.x == doctest::Approx(0.25f));
    CHECK(down.touch_points()[0].pos.y == doctest::Approx(0.75f));
    CHECK(!down.has_action(MCE_SID("Touch")));  // not an action
    const InputState& up = s.evaluate(4);
    REQUIRE(up.touch_points().size() == 1);
    CHECK(up.touch_points()[0].released);
    CHECK(!up.touch_points()[0].held);
    CHECK(s.evaluate(5).mouse_pos_px().x == doctest::Approx(640.0f));
    CHECK(s.state().pressed(MCE_SID("Point")));
    CHECK(s.evaluate(6).touch_points().size() == 0);
    CHECK(!s.state().pressed(MCE_SID("Point")));
}
