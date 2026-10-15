// M-CINEMA 5 (ADR-0042): the VideoPlayer playhead (play, end, loop, restart) on a cooked .mpg, its
// sound following the playhead through an offline mixer (start, seek, stop), and a timeline video
// track driving a player, including a skip to the end. The pictures are render-side (test_video in
// tests/render decodes them; the player smoke run draws them).

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/audio/audio.h"
#include "mce/audio/components.h"
#include "mce/cook/cook.h"
#include "mce/kit/timeline.h"
#include "mce/kit/video.h"

using namespace mce;
using namespace cook_test;
using kit::VideoPlayer;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr const char* kFilm = "7e7b0000-0000-4000-8000-000000000001";
constexpr const char* kShow = "7e7b0000-0000-4000-8000-000000000002";

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

// The 2 s red/blue clip, and a 4 s timeline that shows it on its own entity from 1 s for 1.5 s.
const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("video_project");
        write_file(r + "/assets/film.mpg", read_file(MCE_TEST_VIDEO));
        write_file(r + "/assets/film.mpg.mceasset", sidecar(kFilm, "video"));
        write_file(r + "/assets/show.mctimeline", std::string(R"({ "format": "mce-timeline", "version": 1,
            "guid": ")") + kShow + R"(", "name": "show", "duration": 4,
            "tracks": [ { "type": "video", "keys": [ { "t": 1, "video": "guid:)" + kFilm + R"(", "duration": 1.5, "gain_db": -6 } ] } ] })");
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
    mem::UniquePtr<audio::AudioDevice> mixer;
    u64 ticks = 0;

    explicit Stage(bool sound = false) {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        if (sound) {
            audio::AudioDesc d;
            d.backend = audio::Backend::Offline;
            auto m = audio::AudioDevice::create(d);
            REQUIRE(m);
            mixer = static_cast<mem::UniquePtr<audio::AudioDevice>&&>(m.value());
            world.set_resource(audio::AudioRef{mixer.get(), nullptr});
        }
        kit::install_video(world, &assets);
        kit::install_timelines(world, &assets);
    }
    Entity film(bool playing = true) {
        const Entity e = world.spawn();
        VideoPlayer p;
        p.video.guid = asset::parse_guid(kFilm).value();
        p.playing = playing;
        world.set(e, p);
        return e;
    }
    VideoPlayer& player(Entity e) { return *world.try_get_mut<VideoPlayer>(e); }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            world.tick(kDt, ticks++);
            if (mixer) {
                f32 block[2 * 800];
                mixer->render_offline(block, 800);  // one tick of 48 kHz audio
                mixer->update();
            }
        }
    }
    kit::VideoRegistry& reg() { return *world.try_resource<kit::VideoRegistry>(); }
};

}  // namespace

TEST_CASE("video: a VideoPlayer plays to its end, loops and restarts") {
    Stage s;
    const Entity e = s.film();
    s.run(30);
    CHECK(s.player(e).duration == doctest::Approx(2.0f).epsilon(0.05));
    CHECK(s.player(e).time == doctest::Approx(0.5f));
    CHECK(s.player(e).playing);
    s.run(120);
    CHECK_FALSE(s.player(e).playing);
    CHECK(s.player(e).finished);
    CHECK(s.player(e).time == doctest::Approx(s.player(e).duration));

    s.player(e).restart = true;
    s.run(1);
    CHECK(s.player(e).playing);
    CHECK_FALSE(s.player(e).finished);
    CHECK(s.player(e).time == doctest::Approx(kDt));

    s.player(e).loop = true;
    s.run(150);  // 2.5 s from the restart
    CHECK(s.player(e).playing);
    CHECK(s.player(e).time == doctest::Approx(151.0f * kDt - s.player(e).duration).epsilon(0.02));

    // Paused, the playhead stays; a player without a loadable video stays still.
    s.player(e).playing = false;
    const f32 at = s.player(e).time;
    s.run(10);
    CHECK(s.player(e).time == doctest::Approx(at));
    const Entity none = s.world.spawn();
    s.world.set(none, VideoPlayer{});
    s.run(10);
    CHECK(s.player(none).time == doctest::Approx(0.0f));
    CHECK(s.player(none).duration == doctest::Approx(0.0f));
}

TEST_CASE("video: the sound follows the playhead") {
    Stage s(true);
    const Entity e = s.film();
    s.run(2);
    REQUIRE(s.reg().voices().size() == 1);
    const audio::AudioHandle first = s.reg().voices()[0].handle;
    CHECK(s.mixer->is_playing(first));
    s.run(10);
    CHECK(s.reg().voices()[0].handle == first);  // steady play keeps one voice

    // A jump restarts the sound at the new playhead.
    s.player(e).time = 1.2f;
    s.run(1);
    REQUIRE(s.reg().voices().size() == 1);
    CHECK_FALSE(s.reg().voices()[0].handle == first);

    // Muted, paused or another speed: no voice.
    s.player(e).sound = false;
    s.run(1);
    CHECK(s.reg().voices().empty());
    s.player(e).sound = true;
    s.player(e).speed = 2.0f;
    s.run(1);
    CHECK(s.reg().voices().empty());
    s.player(e).speed = 1.0f;
    s.run(1);
    CHECK(s.reg().voices().size() == 1);
    s.player(e).playing = false;
    s.run(1);
    CHECK(s.reg().voices().empty());

    // A destroyed player takes its sound with it.
    s.player(e).playing = true;
    s.run(1);
    REQUIRE(s.reg().voices().size() == 1);
    s.world.destroy(e);
    s.run(1);
    CHECK(s.reg().voices().empty());
}

TEST_CASE("video: a timeline video track drives the player, and a skip stops it") {
    Stage s;
    const Entity e = s.film(false);
    s.player(e).video.guid = asset::Guid{};  // the track sets the video
    kit::TimelinePlayer tp;
    tp.timeline.guid = asset::parse_guid(kShow).value();
    s.world.set(e, tp);
    s.run(30);  // 0.5 s: before the key
    CHECK_FALSE(s.player(e).playing);
    s.run(42);  // 1.2 s: 0.2 s into the video
    CHECK(s.player(e).playing);
    CHECK(s.player(e).video.guid == asset::parse_guid(kFilm).value());
    CHECK(s.player(e).time == doctest::Approx(0.2f).epsilon(0.05));
    CHECK(s.player(e).gain_db == doctest::Approx(-6.0f));
    s.run(90);  // 2.7 s: past the key's 1.5 s
    CHECK_FALSE(s.player(e).playing);
    CHECK(s.player(e).finished);

    // Scrubbing back into the range shows the frame there.
    REQUIRE(kit::timeline_scrub(s.world, e, 1.5f));
    CHECK(s.player(e).playing);
    CHECK(s.player(e).time == doctest::Approx(0.5f));
}
