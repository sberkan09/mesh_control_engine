// M-CINEMA 4: cutscenes — the world's cutscene hold (gameplay stops, timelines and cameras play on, a
// pause stops both), camera rails, look-ats, blends and 3D shake, and a Cutscene's letterbox,
// hold-to-skip and its release of the hold.

#include <doctest/doctest.h>

#include "mce/asset/formats.h"
#include "mce/cook/timeline_text.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/ecs.h"
#include "mce/input/replay.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/stock.h"
#include "mce/kit/timeline.h"

using namespace mce;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

u32 g_gameplay = 0, g_cinema = 0, g_menu = 0;
void gameplay(Frame&) { ++g_gameplay; }
void cinema(Frame&) { ++g_cinema; }
void menu(Frame&) { ++g_menu; }

u32 g_beats = 0, g_ends = 0;
void count_signals(Frame& f) {
    for (const kit::TimelineSignal& s : f.events().read<kit::TimelineSignal>()) {
        g_beats += s.id == StringId("beat");
        g_ends += s.id == StringId("end");
    }
}

// One tick; `skip_held` > 0 holds Cancel for that long.
void tick(World& w, u64& t, f32 skip_held = 0.0f) {
    input::InputState in;
    in.tick = t;
    if (skip_held > 0.0f) {
        in.action_count = 1;
        in.action_names[0] = StringId("Cancel");
        in.actions[0].held = true;
        in.actions[0].held_time = skip_held;
    }
    input::publish(w, in);
    w.tick(kDt, t++);
}

bool close_to(math::vec3 a, math::vec3 b, f32 eps = 1e-3f) { return math::length(a - b) < eps; }

// A 1 s cutscene timeline: a cut to camera 0, a 0.25 s blend to camera 1 at 0.5 s, event keys at
// 0.5 s ("beat") and 0.9 s ("end").
asset::Guid define_intro(World& w) {
    cook::TimelineDocument d;
    d.name = "intro";
    d.duration = 1.0f;
    cook::TimelineDocTrack cam;
    cam.type = cook::TimelineDocTrackType::Camera;
    cook::TimelineDocKey k;
    k.t = 0.0f;
    k.camera = 0;
    cam.keys.push_back(k);
    k.t = 0.5f;
    k.camera = 1;
    k.blend = 0.25f;
    cam.keys.push_back(k);
    d.tracks.push_back(static_cast<cook::TimelineDocTrack&&>(cam));
    cook::TimelineDocTrack ev;
    ev.type = cook::TimelineDocTrackType::Event;
    cook::TimelineDocKey e;
    e.t = 0.5f;
    e.name = "beat";
    ev.keys.push_back(e);
    e.t = 0.9f;
    e.name = "end";
    ev.keys.push_back(e);
    d.tracks.push_back(static_cast<cook::TimelineDocTrack&&>(ev));
    Array<byte> payload;
    cook::build_timeline_chunk(d, payload);
    const asset::Guid guid{0x7e300000'00004000ull, 0x8000000000000001ull};
    REQUIRE(w.resource<kit::TimelineRegistry>().define(guid, payload.span()));
    return guid;
}

struct Cinema {
    World w;
    u64 t = 0;
    Entity director, cam_a, cam_b;
    Cinema() {
        input::install(w);
        kit::install_stock(w, nullptr);
        w.add_system(Phase::FixedUpdate_Pre, "test_gameplay", &gameplay);
        w.add_system(Phase::FixedUpdate_Post, "test_signals", &count_signals);
        w.run_during_cutscenes(Phase::FixedUpdate_Post, "test_signals");
        const asset::Guid intro = define_intro(w);
        cam_a = w.spawn();
        w.add(cam_a, LocalTransform{});
        w.add(cam_a, kit::Camera3DComponent{});
        cam_b = w.spawn();
        LocalTransform tb;
        tb.position = {10.0f, 0.0f, 0.0f};
        w.add(cam_b, tb);
        kit::Camera3DComponent cb;
        cb.primary = 0;
        cb.fov_deg = 90.0f;
        w.add(cam_b, cb);
        director = w.spawn();
        kit::TimelinePlayer p;
        p.timeline.guid = intro;
        p.bindings[0] = cam_a;
        p.bindings[1] = cam_b;
        w.add(director, p);
        w.add(director, kit::Cutscene{});
        g_gameplay = g_beats = g_ends = 0;
    }
    const kit::Cutscene& cut() { return *w.try_get<kit::Cutscene>(director); }
    const kit::TimelinePlayer& player() { return *w.try_get<kit::TimelinePlayer>(director); }
};

}  // namespace

TEST_CASE("cinematic: a cutscene hold stops gameplay only; a pause stops the cutscene's systems too") {
    World w;
    w.add_system(Phase::FixedUpdate_Pre, "gameplay", &gameplay);
    w.add_system(Phase::FixedUpdate_Post, "cinema", &cinema);
    w.run_during_cutscenes(Phase::FixedUpdate_Post, "cinema");
    w.add_system(Phase::FixedUpdate_Pre, "menu", &menu);
    w.run_when_held(Phase::FixedUpdate_Pre, "menu");
    g_gameplay = g_cinema = g_menu = 0;
    u64 t = 0;
    w.tick(kDt, t++);
    CHECK(g_gameplay == 1);
    CHECK(g_cinema == 1);
    w.set_cutscene_held(true);
    CHECK(w.held());
    CHECK_FALSE(w.paused());
    w.tick(kDt, t++);
    CHECK(g_gameplay == 1);  // held
    CHECK(g_cinema == 2);    // plays on
    CHECK(g_menu == 2);
    w.set_held(true);  // a pause screen over the cutscene
    w.tick(kDt, t++);
    CHECK(g_cinema == 2);
    CHECK(g_menu == 3);
    w.set_held(false);
    w.set_cutscene_held(false);
    CHECK_FALSE(w.held());
    w.tick(kDt, t++);
    CHECK(g_gameplay == 2);
    CHECK(g_cinema == 3);
}

TEST_CASE("cinematic: rails pass through their points, look-ats face their target") {
    kit::CameraRail3D r;
    r.count = 3;
    r.points[0] = {0, 0, 0};
    r.points[1] = {4, 0, 0};
    r.points[2] = {4, 0, -4};
    CHECK(close_to(kit::rail_point(r, 0.0f), r.points[0]));
    CHECK(close_to(kit::rail_point(r, 0.5f), r.points[1]));
    CHECK(close_to(kit::rail_point(r, 1.0f), r.points[2]));
    math::vec3 tangent;
    kit::rail_point(r, 0.25f, &tangent);
    CHECK(tangent.x > 0.0f);
    // A looping rail closes back to its first point.
    r.loop = true;
    CHECK(close_to(kit::rail_point(r, 1.0f), r.points[0]));
    CHECK(close_to(kit::rail_point(r, 1.0f / 3.0f), r.points[1]));

    CHECK(close_to(math::rotate(kit::look_rotation({0, 0, -1}), {0, 0, -1}), {0, 0, -1}));
    for (const math::vec3 d : {math::vec3{1, 0, 0}, math::vec3{0, 0, 1}, math::normalize(math::vec3{1, 2, -3})})
        CHECK(close_to(math::rotate(kit::look_rotation(d * 5.0f), {0, 0, -1}), d));

    World w;
    input::install(w);
    kit::install_stock(w, nullptr);
    const Entity target = w.spawn();
    LocalTransform tt;
    tt.position = {0, 0, -10};
    w.add(target, tt);
    const Entity cam = w.spawn();
    w.add(cam, LocalTransform{});
    w.add(cam, kit::Camera3DComponent{});
    kit::CameraRail3D rail;
    rail.count = 2;
    rail.points[0] = {-5, 0, 0};
    rail.points[1] = {5, 0, 0};
    rail.speed = 0.5f;  // two seconds end to end
    w.add(cam, rail);
    kit::CameraLookAt3D look;
    look.target = target;
    w.add(cam, look);
    u64 t = 0;
    for (u32 i = 0; i < 60; ++i) tick(w, t);
    const LocalTransform& ct = *w.try_get<LocalTransform>(cam);
    CHECK(ct.position.x == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(close_to(math::rotate(ct.rotation, {0, 0, -1}), math::normalize(tt.position - ct.position)));
    for (u32 i = 0; i < 90; ++i) tick(w, t);
    CHECK(w.try_get<LocalTransform>(cam)->position.x == doctest::Approx(5.0f));  // stops at the end
}

TEST_CASE("cinematic: a cutscene holds gameplay while its timeline plays and blends between cameras") {
    Cinema c;
    tick(c.w, c.t);
    CHECK(c.cut().active);
    CHECK(c.w.cutscene_held());
    CHECK(c.cut().bars > 0.0f);
    CHECK(c.w.try_get<kit::Camera3DComponent>(c.cam_a)->primary == 1);
    for (u32 i = 0; i < 35; ++i) tick(c.w, c.t);  // 0.6 s: 0.1 s into the blend to camera B
    CHECK(g_gameplay == 0);
    CHECK(g_beats == 1);
    CHECK(c.w.try_get<kit::Camera3DComponent>(c.cam_b)->primary == 1);
    CHECK(c.w.try_get<kit::Camera3DComponent>(c.cam_a)->primary == 0);
    const kit::CameraBlend3D* b = c.w.try_get<kit::CameraBlend3D>(c.cam_b);
    REQUIRE(b);
    CHECK(b->from == c.cam_a);
    CHECK(b->weight > 0.0f);
    CHECK(b->weight < 1.0f);
    // The view is between the two cameras.
    kit::Kit3DExtract extract;
    render::Camera3D view;
    REQUIRE(extract.camera(c.w, 1.0f, view));
    CHECK(view.position.x > 0.1f);
    CHECK(view.position.x < 9.9f);
    CHECK(view.fov_y > math::radians(60.5f));
    CHECK(view.fov_y < math::radians(89.5f));
    for (u32 i = 0; i < 30; ++i) tick(c.w, c.t);
    CHECK(c.player().finished);
    CHECK(g_ends == 1);
    CHECK(c.w.try_get<kit::CameraBlend3D>(c.cam_b)->weight == 1.0f);
    REQUIRE(extract.camera(c.w, 1.0f, view));
    CHECK(view.position.x == doctest::Approx(10.0f));
    // The timeline ended: the hold lifts on the next tick and the bars slide out.
    tick(c.w, c.t);
    CHECK_FALSE(c.cut().active);
    CHECK_FALSE(c.w.cutscene_held());
    CHECK_FALSE(c.cut().skipped);
    const u32 ran = g_gameplay;
    CHECK(ran >= 1);
    for (u32 i = 0; i < 40; ++i) tick(c.w, c.t);
    CHECK(c.cut().bars == 0.0f);
    kit::CutsceneView v;
    CHECK_FALSE(kit::cutscene_view(c.w, v));
}

TEST_CASE("cinematic: holding the skip action skips to the end, fires the remaining events and releases the hold") {
    Cinema c;
    for (u32 i = 0; i < 6; ++i) tick(c.w, c.t);
    tick(c.w, c.t, 0.5f);  // half the hold
    CHECK(c.cut().skip_progress == doctest::Approx(0.5f));
    CHECK(c.cut().active);
    kit::CutsceneView v;
    REQUIRE(kit::cutscene_view(c.w, v));
    CHECK(v.show_skip);
    CHECK(v.skip_progress == doctest::Approx(0.5f));
    tick(c.w, c.t, 1.0f);
    CHECK(c.cut().skipped);
    CHECK_FALSE(c.cut().active);
    CHECK(c.player().finished);
    CHECK(c.player().time == doctest::Approx(1.0f));
    CHECK_FALSE(c.w.cutscene_held());
    CHECK(g_gameplay == 1);  // the gameplay system after kit_cutscene ran on the skip tick
    CHECK(c.w.try_get<kit::Camera3DComponent>(c.cam_b)->primary == 1);  // the end's camera
    tick(c.w, c.t);
    CHECK(g_beats == 1);
    CHECK(g_ends == 1);
}

// M-CINEMA's replay gate: a cutscene skipped part way, recorded as an input stream, replays to the
// same state hash, and a run with another input does not.
TEST_CASE("cinematic: a recorded cutscene with a skip replays to the same state hash") {
    auto input_at = [](u64 t) {
        input::InputState in;
        in.tick = t;
        if (t >= 10 && t < 40) {  // hold Cancel: with skip_hold 0.4 s the skip lands 24 ticks in
            in.action_count = 1;
            in.action_names[0] = StringId("Cancel");
            in.actions[0].held = true;
            in.actions[0].pressed = t == 10;
            in.actions[0].held_time = static_cast<f32>(t - 10 + 1) * kDt;
        }
        return in;
    };
    auto short_hold = [](Cinema& c) { c.w.try_get_mut<kit::Cutscene>(c.director)->skip_hold = 0.4f; };
    input::InputRecorder rec;
    Cinema live;
    short_hold(live);
    for (u64 i = 0; i < 90; ++i) {
        const input::InputState in = input_at(live.t);
        rec.record(in);
        input::publish(live.w, in);
        live.w.tick(kDt, live.t++);
    }
    CHECK(live.cut().skipped);
    const u64 live_hash = live.w.state_hash();

    Cinema replay;
    short_hold(replay);
    input::InputReplayer rp(rec.bytes());
    input::InputState in;
    while (rp.next(in)) {
        input::publish(replay.w, in);
        replay.w.tick(kDt, replay.t++);
    }
    CHECK(replay.t == 90);
    CHECK(replay.cut().skipped);
    CHECK(replay.w.state_hash() == live_hash);

    Cinema watched;  // no skip: the cutscene plays out, another hash
    short_hold(watched);
    for (u64 i = 0; i < 90; ++i) tick(watched.w, watched.t);
    CHECK_FALSE(watched.cut().skipped);
    CHECK(watched.w.state_hash() != live_hash);
}

TEST_CASE("cinematic: a ShakeTrigger shakes a 3D camera; reduced motion is the view's, not the trauma's") {
    World w;
    input::install(w);
    kit::install_stock(w, nullptr);
    const Entity cam = w.spawn();
    w.add(cam, LocalTransform{});
    w.add(cam, kit::Camera3DComponent{});
    w.add(cam, kit::CameraShake3D{});
    const Entity boom = w.spawn();
    kit::ShakeTrigger s;
    s.trauma = 0.8f;
    s.on = kit::FireOn::Start;
    w.add(boom, s);
    u64 t = 0;
    for (u32 i = 0; i < 3; ++i) tick(w, t);
    const kit::CameraShake3D& sh = *w.try_get<kit::CameraShake3D>(cam);
    CHECK(sh.trauma > 0.6f);
    CHECK(math::length(sh.offset) + math::length(sh.offset_rotation) > 0.0f);
    kit::Kit3DExtract extract;
    render::Camera3D view;
    REQUIRE(extract.camera(w, 1.0f, view));
    CHECK_FALSE(close_to(view.position, {0, 0, 0}, 1e-6f));
    for (u32 i = 0; i < 120; ++i) tick(w, t);
    CHECK(w.try_get<kit::CameraShake3D>(cam)->trauma == 0.0f);  // decayed
}

TEST_CASE("cinematic: camera_blend_weight is a smoothstep, 1 without a source or a duration") {
    World w;
    kit::CameraBlend3D b;
    CHECK(kit::camera_blend_weight(b) == 1.0f);  // no `from`
    b.from = w.spawn();
    b.duration = 0.0f;
    CHECK(kit::camera_blend_weight(b) == 1.0f);  // no duration
    b.duration = 2.0f;
    b.elapsed = 0.0f;
    CHECK(kit::camera_blend_weight(b) == 0.0f);
    b.elapsed = 1.0f;
    CHECK(kit::camera_blend_weight(b) == doctest::Approx(0.5f));
    b.elapsed = 2.0f;
    CHECK(kit::camera_blend_weight(b) == 1.0f);
    b.elapsed = 5.0f;
    CHECK(kit::camera_blend_weight(b) == 1.0f);  // past the end stays at 1
    b.elapsed = 0.5f;
    const f32 early = kit::camera_blend_weight(b);
    b.elapsed = 1.5f;
    const f32 late = kit::camera_blend_weight(b);
    CHECK(early < 0.25f);
    CHECK(late > 0.75f);
    CHECK(early + late == doctest::Approx(1.0f));  // symmetric
}
