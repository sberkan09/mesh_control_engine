// FR-ENT-110 (M-CINEMA 1): timelines — the .mctimeline cook and its errors, every track kind
// played from a cooked scene with bound entities, loops and restart, scrubbing, and determinism:
// two runs give the same state hash every tick, and a save mid-timeline resumes to the same end.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/captions.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/sequence.h"
#include "mce/kit/timeline.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace cook_test;
using kit::TimelinePlayer;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

constexpr const char* kIntro = "7e100000-0000-4000-8000-000000000001";
constexpr const char* kLoop = "7e100000-0000-4000-8000-000000000002";
constexpr const char* kScene = "7e100000-0000-4000-8000-000000000003";

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

// The project every test plays: a 2 s intro with one track of each state kind and the cues, a
// looping 1 s beat, and a scene with a director (the player) and five bound entities.
const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("timeline_project");
        write_file(r + "/assets/intro.mctimeline", R"({ "format": "mce-timeline", "version": 1,
            "guid": "7e100000-0000-4000-8000-000000000001", "name": "intro", "duration": 2,
            "tracks": [
              { "type": "transform", "target": 0, "keys": [
                  { "t": 0, "position": [0, 0, 0], "scale": [1, 1, 1] },
                  { "t": 1, "position": [6, 0, 0], "rotation": [0, 0, 0, 1] },
                  { "t": 2, "rotation": [0, 0.7071068, 0, 0.7071068], "scale": [2, 2, 2] } ] },
              { "type": "property", "target": 1, "path": "Camera3DComponent.fov_deg", "keys": [
                  { "t": 0, "value": 60, "ease": "step" }, { "t": 1, "value": 90 } ] },
              { "type": "activation", "target": 2, "path": "MeshRef.visible", "ranges": [ { "from": 0.5, "to": 1 } ] },
              { "type": "animation", "target": 3, "keys": [ { "t": 0.25, "clip": "wave", "loop": true, "fade": 0 } ] },
              { "type": "camera", "cuts": [ { "t": 0, "camera": 1 }, { "t": 1.5, "camera": 4 } ] },
              { "type": "event", "keys": [ { "t": 1, "emit": "beat" } ] },
              { "type": "subtitle", "lines": [ { "t": 0.5, "duration": 1, "text": "Who's there?", "speaker": "Mira" } ] }
            ] })");
        write_file(r + "/assets/loop.mctimeline", R"({ "format": "mce-timeline", "version": 1,
            "guid": "7e100000-0000-4000-8000-000000000002", "name": "loop", "duration": 1,
            "tracks": [ { "type": "event", "keys": [ { "t": 0, "emit": "tick" }, { "t": 0.5, "emit": "tock" } ] } ] })");
        write_file(r + "/assets/stage.mcscene", R"({ "format": "mce-scene", "version": 1,
            "guid": "7e100000-0000-4000-8000-000000000003", "name": "stage", "entities": [
            { "name": "Director", "components": {
                "SaveKey": { "key": 1 },
                "TimelinePlayer": { "timeline": "guid:7e100000-0000-4000-8000-000000000001",
                                    "bindings": ["$1", "$2", "$3", "$4", "$5", null, null, null] } } },
            { "name": "Door", "components": { "LocalTransform": { "position": [0, 0, 0] } } },
            { "name": "CamA", "components": { "LocalTransform": {}, "Camera3DComponent": { "fov_deg": 45, "primary": 0 } } },
            { "name": "Ghost", "components": { "LocalTransform": {}, "MeshRef": { "visible": 0 } } },
            { "name": "Actor", "components": { "LocalTransform": {}, "Animator": { "clip": "idle" } } },
            { "name": "CamB", "components": { "LocalTransform": {}, "Camera3DComponent": { "primary": 1 } } } ] })");
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        INFO(diag_text(rep));
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

struct Stage {
    asset::AssetSystem assets;
    World world;
    Array<Entity> e;  // director, door, cam_a, ghost, actor, cam_b
    u64 ticks = 0;

    Stage() {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        world.register_components<LocalTransform, SaveKey, kit::Camera3DComponent, kit::MeshRef, kit::Animator>();
        kit::install_sequences(world, &assets);
        kit::install_timelines(world, &assets);
        REQUIRE(scene::load(world, assets, asset::parse_guid(kScene).value(), &e));
        REQUIRE(e.size() == 6);
    }
    TimelinePlayer& player() { return *world.try_get_mut<TimelinePlayer>(e[0]); }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
    math::vec3 door() { return world.try_get<LocalTransform>(e[1])->position; }
    const kit::Camera3DComponent& cam(usize i) { return *world.try_get<kit::Camera3DComponent>(e[i]); }
};

u32 g_ticks = 0, g_tocks = 0;
void count_beats(Frame& f) {
    for (const kit::TimelineSignal& s : f.events().read<kit::TimelineSignal>()) {
        g_ticks += s.id == StringId("tick");
        g_tocks += s.id == StringId("tock");
    }
}

}  // namespace

TEST_CASE("timeline: .mctimeline cooks to CHNK_TIMELINE") {
    Stage s;
    const asset::AssetKey key = s.assets.find(asset::parse_guid(kIntro).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    auto h = s.assets.load_sync<asset::Timeline>(key);
    REQUIRE(h);
    auto view = asset::TimelineView::read(s.assets.resolve(h.value()));
    REQUIRE(view);
    const asset::TimelineView& v = view.value();
    CHECK(v.name() == StringView("intro"));
    CHECK(v.header->duration == doctest::Approx(2.0f));
    // The transform track becomes three LocalTransform channels, each with only the keys that set it.
    REQUIRE(v.tracks.size() == 9);
    CHECK(v.string(v.tracks[0].path) == StringView("LocalTransform.position"));
    CHECK(v.tracks[0].key_count == 2);
    CHECK(v.string(v.tracks[1].path) == StringView("LocalTransform.rotation"));
    CHECK((v.tracks[1].flags & asset::TimelineTrackFlags::Quat) != 0);
    CHECK(v.tracks[1].key_count == 2);
    CHECK(v.tracks[2].key_count == 2);
    CHECK(v.tracks[3].kind == asset::TimelineTrackKind::Property);
    CHECK(v.tracks[3].value_count == 1);
    CHECK(v.keys_of(v.tracks[3])[0].ease == asset::TimelineEase::Step);
    CHECK(v.tracks[4].kind == asset::TimelineTrackKind::Activation);
    CHECK(v.keys_of(v.tracks[4])[0].duration == doctest::Approx(0.5f));
    CHECK(v.string(v.keys_of(v.tracks[5])[0].text) == StringView("wave"));
    CHECK(v.keys_of(v.tracks[6])[1].target == 4);
    CHECK(v.keys_of(v.tracks[7])[0].id == StringId("beat").value);
    CHECK(v.string(v.keys_of(v.tracks[8])[0].text2) == StringView("Mira"));
    s.assets.release(asset::AnyHandle(h.value()));

    // Scene bindings resolve to the scene's entities.
    CHECK(s.player().bindings[0] == s.e[1]);
    CHECK(s.player().bindings[4] == s.e[5]);
    CHECK(s.player().bindings[5].is_null());
}

TEST_CASE("timeline: every track kind plays against the tick") {
    Stage s;
    // A sequence that waits for the timeline's "beat": timeline events reach sequences.
    const asset::SequenceStep listen[] = {kit::SequenceRegistry::wait_event("beat")};
    REQUIRE(s.world.resource<kit::SequenceRegistry>().define("listen", listen));
    kit::SequencePlayer sp;
    sp.name.assign("listen");
    const Entity listener = s.world.spawn_with(sp);

    s.run(1);
    CHECK(s.cam(2).primary == 1);  // cut 1: CamA
    CHECK(s.cam(5).primary == 0);
    CHECK(s.cam(2).fov_deg == doctest::Approx(60.0f));
    s.run(13);  // tick 14
    CHECK(s.world.try_get<kit::Animator>(s.e[4])->clip.view() == StringView("idle"));
    s.run(1);  // tick 15: t = 0.25
    const kit::Animator& actor = *s.world.try_get<kit::Animator>(s.e[4]);
    CHECK(actor.clip.view() == StringView("wave"));
    CHECK(actor.loop);
    CHECK(actor.fade == 0.0f);
    CHECK(actor.restart);
    s.run(14);  // tick 29
    CHECK(s.world.try_get<kit::MeshRef>(s.e[3])->visible == 0);
    StringView speaker, text;
    CHECK_FALSE(kit::timeline_subtitle(s.world, s.e[0], speaker, text));
    s.run(1);  // tick 30: t = 0.5
    CHECK(s.door().x == doctest::Approx(3.0f));
    CHECK(s.world.try_get<kit::MeshRef>(s.e[3])->visible == 1);
    REQUIRE(kit::timeline_subtitle(s.world, s.e[0], speaker, text));
    CHECK(speaker == StringView("Mira"));
    CHECK(text == StringView("Who's there?"));
    {  // what the player's subtitle view reads (M-ACCESS)
        Array<kit::CaptionLine> lines;
        kit::caption_lines(s.world, lines);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].speaker.view() == StringView("Mira"));
        CHECK(lines[0].text.view() == StringView("Who's there?"));
        CHECK(lines[0].source == s.e[0]);
    }
    s.run(29);  // tick 59
    CHECK(s.cam(2).fov_deg == doctest::Approx(60.0f));  // a step key holds
    CHECK(s.world.try_get<kit::SequencePlayer>(listener)->active);
    s.run(2);  // tick 61: the beat fired on tick 60 and the sequence saw it
    CHECK(s.cam(2).fov_deg == doctest::Approx(90.0f));
    CHECK(s.door().x == doctest::Approx(6.0f));
    CHECK(s.world.try_get<kit::MeshRef>(s.e[3])->visible == 0);
    CHECK_FALSE(s.world.try_get<kit::SequencePlayer>(listener)->active);
    s.run(29);  // tick 90: t = 1.5, the second cut
    CHECK(s.cam(2).primary == 0);
    CHECK(s.cam(5).primary == 1);
    CHECK_FALSE(kit::timeline_subtitle(s.world, s.e[0], speaker, text));
    s.run(29);  // tick 119
    CHECK(s.player().playing);
    s.run(1);  // tick 120: the end
    CHECK_FALSE(s.player().playing);
    CHECK(s.player().finished);
    CHECK(s.player().time == doctest::Approx(2.0f));
    const LocalTransform& door = *s.world.try_get<LocalTransform>(s.e[1]);
    CHECK(door.scale.x == doctest::Approx(2.0f));
    CHECK(door.rotation.y == doctest::Approx(0.7071068f));
    CHECK(door.rotation.w == doctest::Approx(0.7071068f));
    s.run(10);
    CHECK(s.player().time == doctest::Approx(2.0f));

    // restart plays it again from the start.
    s.player().restart = true;
    s.run(1);
    CHECK(s.player().playing);
    CHECK_FALSE(s.player().finished);
    CHECK_FALSE(s.player().restart);
    CHECK(s.door().x == doctest::Approx(0.1f));
    CHECK(s.cam(2).primary == 1);
}

TEST_CASE("timeline: a looping timeline fires its keys every lap") {
    Stage s;
    s.world.add_system(Phase::FixedUpdate_Post, "count_beats", &count_beats);
    g_ticks = g_tocks = 0;
    TimelinePlayer& p = s.player();
    p.timeline.guid = asset::parse_guid(kLoop).value();
    p.loop = true;
    s.run(181);  // 3 s: laps start at 0, 1, 2 and 3 s; events are read the tick after
    CHECK(g_ticks == 4);
    CHECK(g_tocks == 3);
    CHECK(s.player().playing);
    CHECK_FALSE(s.player().finished);
    CHECK(s.player().time < 0.05f);
}

TEST_CASE("timeline: scrubbing applies the state tracks and skips the cues it passes") {
    Stage s;
    const asset::SequenceStep listen[] = {kit::SequenceRegistry::wait_event("beat")};
    REQUIRE(s.world.resource<kit::SequenceRegistry>().define("listen", listen));
    kit::SequencePlayer sp;
    sp.name.assign("listen");
    const Entity listener = s.world.spawn_with(sp);

    REQUIRE(kit::timeline_scrub(s.world, s.e[0], 1.6f));
    CHECK(s.player().time == doctest::Approx(1.6f));
    CHECK(s.door().x == doctest::Approx(6.0f));
    CHECK(s.cam(5).primary == 1);
    CHECK(s.cam(2).primary == 0);
    CHECK(s.cam(2).fov_deg == doctest::Approx(90.0f));
    CHECK(s.world.try_get<kit::Animator>(s.e[4])->clip.view() == StringView("idle"));  // cues do not fire
    s.run(30);
    CHECK(s.world.try_get<kit::SequencePlayer>(listener)->active);  // the beat at 1 s was scrubbed past
    CHECK(s.player().finished);

    REQUIRE(kit::timeline_scrub(s.world, s.e[0], 0.5f));
    CHECK(s.door().x == doctest::Approx(3.0f));
    CHECK(s.cam(2).primary == 1);
    CHECK(s.world.try_get<kit::MeshRef>(s.e[3])->visible == 1);
    CHECK_FALSE(s.player().finished);
    CHECK(kit::timeline_scrub(s.world, s.e[0], 99.0f));
    CHECK(s.player().time == doctest::Approx(2.0f));
    CHECK_FALSE(kit::timeline_scrub(s.world, s.e[1], 0.0f));  // not a player
}

TEST_CASE("timeline: two runs hash the same every tick, and a save resumes to the same end") {
    Stage a, b;
    for (u32 i = 0; i < 130; ++i) {
        a.run(1);
        b.run(1);
        REQUIRE(a.world.state_hash() == b.world.state_hash());
    }

    for (u32 cut : {1u, 45u, 90u, 119u}) {
        Stage c;
        c.run(cut);
        Array<byte> bytes;
        save::write_save(c.world, save::SaveInfo{}, bytes);
        Stage d;  // the scene as authored
        d.ticks = c.ticks;
        REQUIRE(save::read_save(d.world, bytes.span()));
        CHECK(d.player().time == doctest::Approx(c.player().time));
        u32 n = 0;
        while (d.player().playing && n < 1000) {
            d.run(1);
            ++n;
        }
        CHECK(cut + n == 120);
        CHECK(d.player().finished);
        CHECK(d.door().x == doctest::Approx(6.0f));
        CHECK(d.cam(5).primary == 1);
        CHECK(d.cam(2).primary == 0);
    }
}

TEST_CASE("timeline: cook errors name the track and the file position") {
    struct Case {
        const char* tracks;
        const char* expect;
    };
    const Case cases[] = {
        {R"([ { "type": "dance", "keys": [] } ])", "unknown track type"},
        {R"([ { "type": "property", "path": "LocalTransform.wobble", "keys": [ { "t": 0, "value": 1 } ] } ])",
         "does not resolve to one field"},
        {R"([ { "type": "property", "path": "Nope.x", "keys": [ { "t": 0, "value": 1 } ] } ])", "does not name a reflected component"},
        {R"([ { "type": "property", "path": "LocalTransform.position", "keys": [ { "t": 0, "value": 1 } ] } ])",
         "expected an array of 3 numbers"},
        {R"([ { "type": "event", "keys": [ { "t": 2, "emit": "a" }, { "t": 1, "emit": "b" } ] } ])", "keys must be in time order"},
        {R"([ { "type": "camera", "cuts": [ { "t": 0, "camera": 9 } ] } ])", "binding index 0 to 7"},
        {R"([ { "type": "activation", "path": "Camera3DComponent.fov_deg", "ranges": [ { "from": 0, "to": 1 } ] } ])",
         "bool or integer field"},
        {R"([ { "type": "event", "keys": [ { "t": 0, "emit": "a" } ] } ])", "needs a 'duration'"},
        {R"([ { "type": "subtitle", "lines": [ { "t": 0, "duration": 1, "txt": "hi" } ] } ])", "txt"},
    };
    u32 i = 0;
    for (const Case& c : cases) {
        const std::string root = fresh_dir("timeline_errors_" + std::to_string(i++));
        write_file(root + "/assets/bad.mctimeline",
                   std::string(R"({ "format": "mce-timeline", "version": 1, "guid": "7e100000-0000-4000-8000-0000000000f0",
    "tracks": )") + c.tracks + " }");
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        const std::string text = diag_text(r);
        INFO(text);
        CHECK(text.find(c.expect) != std::string::npos);
        CHECK(text.find("bad.mctimeline:") != std::string::npos);
    }
}
