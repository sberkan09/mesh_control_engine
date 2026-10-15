// M-UI-DATA step 2: gameplay and UI documents through data (mce/ui/screen_link.h). A widget's
// action reaches the tick's InputState exactly as a scripted input line would (so replays hold
// their hash), "bind" paths resolve against the world, and the new data round-trips.

#include <doctest/doctest.h>

#include "mce/ecs/world.h"
#include "mce/input/script.h"
#include "mce/refl/refl.h"
#include "mce/ui/document_data.h"
#include "mce/ui/screen_link.h"
#include "mce/ui/ui.h"

using namespace mce;
using namespace mce::ui;

namespace {

struct LinkScore {
    i32 points = 7;
    f32 time = 1.5f;
};
MCE_COMPONENT(LinkScore, points, time)

struct LinkOther {
    i32 n = 0;
};
MCE_COMPONENT(LinkOther, n)

UpdateContext frame() {
    UpdateContext c;
    c.viewport = rhi::Extent2D{1280, 720};
    c.dt = 1.0 / 60.0;
    return c;
}

constexpr f32 kDt = 1.0f / 60.0f;

}  // namespace

TEST_CASE("ui link: a confirmed button's action equals a scripted press, tick by tick") {
    Document d;
    d.set_modal(true);
    const WidgetId resume = d.add(d.root(), WidgetKind::Button, "resume");
    d.set_action(resume, "Resume");
    d.set_default_focus(resume);
    d.update(frame());

    ActionRelay relay;
    relay.declare("Resume");
    relay.declare("Restart");
    input::InputScript none, scripted;
    // Both runs know "Restart" (a script names every action it holds on any tick); the scripted
    // run then presses "Resume" on tick 3, where the live run confirms the button.
    REQUIRE(scripted.add("Restart", 1, 1, 0.0f));
    REQUIRE(none.add("Restart", 1, 1, 0.0f));
    REQUIRE(scripted.parse("3 Resume\n").has_value());
    for (u64 tick = 1; tick <= 6; ++tick) {
        if (tick == 3) {  // the frame before tick 3: the player confirms the focused button
            input::InputState in;
            in.confirm_pressed = true;
            UpdateContext c = frame();
            c.actions = &in;
            d.update(c);
            relay.collect(d);
            CHECK(relay.pending());
        }
        input::InputState live = none.evaluate(tick);
        relay.apply(live, kDt);
        const input::InputState& want = scripted.evaluate(tick);
        CAPTURE(tick);
        CHECK(live.has_action(StringId("Resume")) == (tick == 3 || tick == 4));  // pressed, then released
        CHECK(live.has_action(StringId("Resume")) == want.has_action(StringId("Resume")));
        CHECK(input::hash_state(live) == input::hash_state(want));
    }
}

TEST_CASE("ui link: a slider's change carries its value; cancel raises the modal's action") {
    Document d;
    d.set_modal(true);
    const WidgetId veil = d.add(d.root(), WidgetKind::Panel, "veil");
    d.set_action(veil, "Close");
    const WidgetId vol = d.add(veil, WidgetKind::Slider, "volume");
    d.set_range(vol, 0.0f, 1.0f, 0.25f);
    d.set_action(vol, "Volume");
    d.set_default_focus(vol);
    d.update(frame());
    d.open_modal(veil);
    d.update(frame());

    ActionRelay relay;
    input::InputState in;
    in.move = {1, 0};  // a step right
    UpdateContext c = frame();
    c.actions = &in;
    d.update(c);
    relay.collect(d);
    input::InputState out;
    relay.apply(out, kDt);
    CHECK(out.pressed(StringId("Volume")));
    CHECK(out.axis(StringId("Volume")) == doctest::Approx(0.25f));

    in = input::InputState{};
    in.cancel_pressed = true;
    d.update(c);
    relay.collect(d);
    input::InputState next;
    relay.apply(next, kDt);
    CHECK(next.pressed(StringId("Close")));
    CHECK(next.released(StringId("Volume")));
    // A key that already holds the action keeps its own state.
    relay.raise("Close", 1.0f);
    input::InputState held;
    held.action_names[0] = StringId("Close");
    held.actions[0].held = true;
    held.actions[0].held_time = 0.5f;
    held.action_count = 1;
    relay.apply(held, kDt);
    CHECK_FALSE(held.actions[0].pressed);
    CHECK(held.actions[0].held_time == 0.5f);
}

TEST_CASE("ui link: bind_document binds to the screen's entity first, retries what is missing") {
    World world;
    const Entity screen = world.spawn();
    const Entity other = world.spawn();
    world.set(other, LinkScore{3, 0.5f});
    Document d;
    d.set_modal(false);
    const WidgetId points = d.add(d.root(), WidgetKind::Text, "points");
    d.set_binding(points, "LinkScore/points", "P %.0f");
    const WidgetId later = d.add(d.root(), WidgetKind::Text, "later");
    d.set_binding(later, "LinkOther/n", "");
    const WidgetId bad = d.add(d.root(), WidgetKind::Text, "bad");
    d.set_binding(bad, "NoSuchComponent/x", "");

    Entity ents[] = {screen, other};
    CHECK(bind_document(d, world, screen, Span<const Entity>(ents, 2)) == 1);  // LinkOther is nowhere yet
    CHECK(d.binding_done(points));
    CHECK(d.binding_done(bad));
    CHECK_FALSE(d.binding_done(later));
    d.update(frame());
    CHECK(d.text(points) == "P 3");  // the other entity: the screen has no LinkScore

    world.set(screen, LinkOther{9});
    CHECK(bind_document(d, world, screen, Span<const Entity>(ents, 2)) == 0);
    d.update(frame());
    CHECK(d.text(later) == "9");
}

TEST_CASE("ui link: a binding's format is checked before printf sees it") {
    CHECK(valid_number_format(""));
    CHECK(valid_number_format("%g"));
    CHECK(valid_number_format("SCORE %.0f"));
    CHECK(valid_number_format("%-8.2e%%"));
    CHECK(valid_number_format("x%5.1F!"));
    CHECK_FALSE(valid_number_format("%s"));
    CHECK_FALSE(valid_number_format("%d"));
    CHECK_FALSE(valid_number_format("%n"));
    CHECK_FALSE(valid_number_format("%f %f"));
    CHECK_FALSE(valid_number_format("plain"));
    CHECK_FALSE(valid_number_format("%"));
    CHECK_FALSE(valid_number_format("%100f"));
    CHECK_FALSE(valid_number_format("%*f"));
    Document d;
    const WidgetId t = d.add(d.root(), WidgetKind::Text, "t");
    World world;
    const Entity e = world.spawn();
    world.set(e, LinkScore{});
    CHECK_FALSE(d.bind_property(t, world, e, "LinkScore/points", "%s"));
}

TEST_CASE("ui link: actions and bindings survive encode / decode and build / extract") {
    DocumentData data;
    WidgetData w;
    w.name.assign("score");
    w.kind = WidgetKind::Text;
    w.bind.assign("LinkScore/points");
    w.format.assign("SCORE %.0f");
    data.widgets.push_back(w);
    WidgetData b;
    b.name.assign("pause");
    b.kind = WidgetKind::Button;
    b.action.assign("Pause");
    data.widgets.push_back(b);

    Array<byte> chunk;
    encode_ui_document(data, chunk);
    DocumentData back;
    REQUIRE(decode_ui_document(chunk.span(), back).has_value());
    REQUIRE(back.widgets.size() == 2);
    CHECK(back.widgets[0].bind.view() == "LinkScore/points");
    CHECK(back.widgets[0].format.view() == "SCORE %.0f");
    CHECK(back.widgets[1].action.view() == "Pause");

    Document doc;
    REQUIRE(build_document(back, doc).has_value());
    CHECK(doc.action(doc.find("pause")) == "Pause");
    CHECK(doc.binding(doc.find("score")) == "LinkScore/points");
    DocumentData out;
    extract_document(doc, out);
    REQUIRE(out.widgets.size() == 2);
    CHECK(out.widgets[0].format.view() == "SCORE %.0f");
    CHECK(out.widgets[1].action.view() == "Pause");

    data.widgets[0].format.assign("%s");  // a format printf would misread never loads
    encode_ui_document(data, chunk);
    CHECK_FALSE(decode_ui_document(chunk.span(), back).has_value());
}
