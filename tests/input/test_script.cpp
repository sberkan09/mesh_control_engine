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

TEST_CASE("input script: '=0' presses the action with value 0, '=false' does not (SBA-61)") {
    InputScript s;
    REQUIRE(s.parse("2 Quality=0\n3 Quality=2\n4 Quality=false\n").has_value());
    CHECK_FALSE(s.evaluate(1).has_action(MCE_SID("Quality")));
    const InputState& t2 = s.evaluate(2);  // a slider set to 0 raises the same press
    CHECK(t2.pressed(MCE_SID("Quality")));
    CHECK(t2.action(MCE_SID("Quality")).x == 0.f);
    const InputState& t3 = s.evaluate(3);
    CHECK(t3.held(MCE_SID("Quality")));
    CHECK(t3.action(MCE_SID("Quality")).x == 2.f);
    CHECK(s.evaluate(4).released(MCE_SID("Quality")));
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

TEST_CASE("input script: clear_segments drops the segments and the script takes new ones") {
    InputScript s;
    REQUIRE(s.add("Jump", 1, 5));
    REQUIRE(s.add("Fire", 2, 3));
    CHECK_FALSE(s.empty());
    CHECK(s.segments().size() == 2);
    s.clear_segments();
    CHECK(s.empty());
    CHECK(s.segments().size() == 0);
    REQUIRE(s.add("Jump", 9, 9));
    CHECK(s.segments().size() == 1);
    CHECK(s.segments()[0].from == 9);
}

namespace {
std::string actions_line(const char* ticks, u32 first, u32 count) {
    std::string line = ticks;
    for (u32 i = first; i < first + count; ++i) line += " a" + std::to_string(i);
    return line;
}
}  // namespace

TEST_CASE("input script: a line holds kMaxActions actions, one more is a parse error") {
    InputScript at;
    REQUIRE(at.parse(actions_line("5", 0, kMaxActions) + "\n").has_value());
    CHECK(at.segments().size() == kMaxActions);

    InputScript over;
    const Result<void> r = over.parse("1 keep\n" + actions_line("5", 0, kMaxActions + 1) + "\n");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == Errc::ParseError);
    CHECK(r.error().detail == 2);
    CHECK(r.error().path.view().find("too many actions") != StringView::npos);
}

TEST_CASE("input script: more than kMaxActions distinct actions on one tick drop the extra one") {
    InputScript s;
    REQUIRE(s.parse(actions_line("5", 0, kMaxActions) + "\n" + actions_line("5", kMaxActions, 1) + "\n").has_value());
    REQUIRE(s.segments().size() == kMaxActions + 1);
    const StringId last = s.segments()[kMaxActions].action;
    const StringId first = s.segments()[0].action;

    const InputState& st = s.evaluate(5);
    CHECK(st.action_count == kMaxActions);
    CHECK(st.held(first));
    CHECK_FALSE(st.held(last));  // the 65th is dropped (and logged), not written past the array
}

TEST_CASE("input script: an action name of 31 bytes is kept whole, 32 is refused and adds nothing") {
    const std::string s31(31, 'a'), s32(32, 'a');
    InputScript s;
    REQUIRE(s.add(s31, 1, 2));
    REQUIRE(s.segments().size() == 1);
    CHECK(s.segments()[0].name.size() == 31);
    CHECK(StringView(s.segments()[0].name) == StringView(s31));

    CHECK_FALSE(s.add(s32, 1, 2));
    CHECK(s.segments().size() == 1);  // not cut to the 31-byte action that exists

    // The same through a script line.
    InputScript line;
    CHECK_FALSE(line.parse("1 " + s32 + "\n").has_value());
    CHECK(line.segments().empty());
}

TEST_CASE("input script: a value that is not a finite number (nan, inf, 1e999) is a parse error") {
    InputScript s;
    CHECK(s.parse("5 Move=1e30,-1e30\n").has_value());  // large but finite
    for (const char* text : {"5 Move=nan\n", "5 Move=inf\n", "5 Move=-inf\n", "5 Move=1e999\n", "5 Move=0.5,nan\n", "5 Move=infinity,0\n"}) {
        InputScript bad;
        CHECK_FALSE(bad.parse(text).has_value());
    }
}
