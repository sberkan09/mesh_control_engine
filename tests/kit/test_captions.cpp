// M-ACCESS step 1: captions in the sim. ShowCaption lines and their timeout, SoundCaption tags on an
// SfxTrigger (one line per source, refreshed by a repeat) and on a looping AudioSource (up while it
// plays), the line cap, and the state hash: captions never change it.

#include <doctest/doctest.h>

#include "mce/audio/components.h"
#include "mce/ecs/world.h"
#include "mce/kit/captions.h"
#include "mce/kit/effects.h"

using namespace mce;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct CaptionWorld {
    World world;
    u64 tick = 0;
    CaptionWorld() {
        world.register_components<audio::AudioSource>();
        kit::install_effects(world, nullptr);
        kit::install_captions(world);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, tick++);
    }
    Array<kit::CaptionLine> lines() {
        Array<kit::CaptionLine> out;
        kit::caption_lines(world, out);
        return out;
    }
    void say(const char* speaker, const char* text, f32 seconds) {
        kit::ShowCaption c;
        c.speaker.assign(speaker);
        c.text.assign(text);
        c.seconds = seconds;
        world.event_queue<kit::ShowCaption>()->push(c);
    }
};

}  // namespace

TEST_CASE("M-ACCESS: ShowCaption lines show for their seconds, newest last, at most kMaxCaptions") {
    CaptionWorld w;
    w.say("Ada", "Run!", 0.5f);
    w.run(1);
    Array<kit::CaptionLine> l = w.lines();
    REQUIRE(l.size() == 1);
    CHECK(l[0].speaker.view() == StringView("Ada"));
    CHECK(l[0].text.view() == StringView("Run!"));
    CHECK(l[0].kind == kit::CaptionKind::Speech);
    w.say("", "Later.", 0.0f);  // the default 3 s
    w.run(29);
    REQUIRE(w.lines().size() == 2);
    w.run(2);  // past 0.5 s: the first line is gone
    l = w.lines();
    REQUIRE(l.size() == 1);
    CHECK(l[0].text.view() == StringView("Later."));
    w.run(180);
    CHECK(w.lines().empty());

    for (u32 i = 0; i < kit::kMaxCaptions + 2; ++i) {
        char t[8] = {'l', 'i', 'n', 'e', static_cast<char>('0' + i), 0};
        w.say("", t, 5.0f);
    }
    w.run(1);
    l = w.lines();
    REQUIRE(l.size() == kit::kMaxCaptions);
    CHECK(l[0].text.view() == StringView("line2"));  // the two oldest dropped
}

TEST_CASE("M-ACCESS: a SoundCaption captions its SfxTrigger's plays and its looping AudioSource") {
    CaptionWorld w;
    kit::SfxTrigger trig;
    trig.on = kit::FireOn::Start;  // fires on the first tick, before kit_captions first sees it
    kit::SoundCaption cap;
    cap.text.assign("[door slams]");
    cap.seconds = 1.0f;
    const Entity door = w.world.spawn_with(trig, cap);
    w.run(1);
    Array<kit::CaptionLine> l = w.lines();
    REQUIRE(l.size() == 1);
    CHECK(l[0].text.view() == StringView("[door slams]"));
    CHECK(l[0].kind == kit::CaptionKind::Sound);
    CHECK(l[0].source == door);
    w.run(50);
    w.world.try_get_mut<kit::SfxTrigger>(door)->fire = true;  // again, while the line shows
    w.world.try_get_mut<kit::SfxTrigger>(door)->on = kit::FireOn::Manual;
    w.run(1);
    REQUIRE(w.lines().size() == 1);  // one line per source, kept up
    w.run(55);
    CHECK(w.lines().size() == 1);  // refreshed: 1 s from the second play
    w.run(10);
    CHECK(w.lines().empty());

    audio::AudioSource rain;
    rain.loop = true;
    kit::SoundCaption hiss;
    hiss.text.assign("[rain]");
    const Entity e = w.world.spawn_with(rain, hiss);
    w.run(3);
    CHECK(w.lines().empty());  // not started
    w.world.try_get_mut<audio::AudioSource>(e)->started = true;
    w.run(120);
    REQUIRE(w.lines().size() == 1);  // up as long as it plays
    CHECK(w.lines()[0].text.view() == StringView("[rain]"));
    w.world.try_get_mut<audio::AudioSource>(e)->started = false;
    w.run(3);
    CHECK(w.lines().empty());
}

TEST_CASE("M-ACCESS: captions leave the state hash alone") {
    CaptionWorld a, b;
    kit::SfxTrigger trig;
    trig.on = kit::FireOn::Start;
    kit::SoundCaption cap;
    cap.text.assign("[bell]");
    a.world.spawn_with(trig, cap);
    b.world.spawn_with(trig, cap);
    a.say("Ada", "Run!", 2.0f);  // only one world shows lines
    for (u32 i = 0; i < 30; ++i) {
        a.run(1);
        b.run(1);
        CHECK(a.world.state_hash() == b.world.state_hash());
    }
    CHECK(a.lines().size() == 2);
    CHECK(b.lines().size() == 1);
}
