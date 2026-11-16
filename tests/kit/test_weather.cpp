// Weather state (M-WEATHER 1): intensity after N ticks of a transition is an exact ratio, the fade back, wind and
// wetness at pinned ticks, drying, the seeded schedule, a save mid-run resumes, the same run twice has the same
// state hash, a scene without Weather is unchanged and WeatherChanged fires once per change.

#include <doctest/doctest.h>

#include <cfloat>
#include <limits>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/weather.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;  // the engine's fixed rate

struct Wx {
    World w;
    u64 t = 0;
    Entity e;
    Array<u32> from, to;
    Array<u64> at;
    explicit Wx(WeatherKind target = WeatherKind::Clear, bool with = true, f32 auto_s = 0.0f, u32 seed = 0) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (with) {
            Weather c;
            c.target = target;
            c.auto_change_seconds = auto_s;
            c.seed = seed;
            e = w.spawn_with(SaveKey{1}, c);
        } else {
            e = w.spawn_with(SaveKey{1});
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            auto ev = EventAccess(w);
            for (const WeatherChanged& c : ev.read<WeatherChanged>()) {
                from.push_back(c.from);
                to.push_back(c.to);
                at.push_back(t);
            }
        }
    }
    Weather& wx() { return *w.try_get_mut<Weather>(e); }
};

}  // namespace

TEST_CASE("weather: intensity of a 30 s transition is half at 15 s and 1 at 30 s") {
    Wx x(WeatherKind::Rain);
    x.run(900);
    CHECK(x.wx().current == WeatherKind::Rain);
    CHECK(x.wx().intensity == 0.5f);
    x.run(450);
    CHECK(x.wx().intensity == 0.75f);
    x.run(450);
    CHECK(x.wx().intensity == 1.0f);
    x.run(600);
    CHECK(x.wx().intensity == 1.0f);
}

TEST_CASE("weather: clear fades the old kind back to 0") {
    Wx x(WeatherKind::Storm);
    x.run(1800);
    CHECK(x.wx().intensity == 1.0f);
    x.wx().target = WeatherKind::Clear;
    x.run(900);
    CHECK(x.wx().current == WeatherKind::Storm);
    CHECK(x.wx().intensity == 0.5f);
    x.run(900);
    CHECK(x.wx().current == WeatherKind::Clear);
    CHECK(x.wx().intensity == 0.0f);
    x.run(100);
    CHECK(x.wx().intensity == 0.0f);
}

TEST_CASE("weather: a change between kinds fades out then in") {
    Wx x(WeatherKind::Rain);
    x.run(1800);
    x.wx().target = WeatherKind::Snow;
    x.run(900);
    CHECK(x.wx().current == WeatherKind::Rain);
    CHECK(x.wx().intensity == 0.5f);
    x.run(900);  // out at 0: switched and the first step of the fade in is taken the same tick
    CHECK(x.wx().current == WeatherKind::Snow);
    CHECK(x.wx().fade == 1);
    x.run(899);
    CHECK(x.wx().intensity == 0.5f);
}

TEST_CASE("weather: wind blends toward the kind's wind") {
    Wx x(WeatherKind::Storm);
    x.run(1);
    CHECK(x.wx().wind == doctest::Approx(0.1f + 0.9f / 1800.0f).epsilon(1e-6));
    x.run(899);  // intensity 0.5
    CHECK(x.wx().wind == doctest::Approx(0.55f).epsilon(1e-6));
    x.run(900);
    CHECK(x.wx().wind == 1.0f);
    x.wx().target = WeatherKind::Clear;
    x.run(1800);
    CHECK(x.wx().wind == doctest::Approx(0.1f).epsilon(1e-6));
    x.wx().target = WeatherKind::Overcast;
    x.run(1800);
    CHECK(x.wx().wind == doctest::Approx(0.3f).epsilon(1e-6));
}

TEST_CASE("weather: wetness rises in rain and dries slowly after") {
    Wx x(WeatherKind::Rain);
    x.run(1800);  // ramp: sum of fade/1800 over 1800 ticks of 1/3600 per tick at full: about 0.25
    CHECK(x.wx().wetness == doctest::Approx(0.25f).epsilon(0.01));
    x.run(1800);
    CHECK(x.wx().wetness == doctest::Approx(0.75f).epsilon(0.01));
    x.run(3600);
    CHECK(x.wx().wetness == 1.0f);
    // Overcast is not wet: switching to it dries (fade out of rain also keeps it wet until current changes).
    x.wx().target = WeatherKind::Overcast;
    x.run(1800);  // still raining while it fades out
    CHECK(x.wx().wetness == doctest::Approx(1.0f).epsilon(1e-3));
    x.run(3600);  // 60 s of drying out of 120 s: half
    CHECK(x.wx().wetness == doctest::Approx(0.5f).epsilon(0.01));
    x.run(3700);
    CHECK(x.wx().wetness == 0.0f);
}

TEST_CASE("weather: WeatherChanged fires once per change") {
    Wx x(WeatherKind::Rain);
    x.run(3000);
    REQUIRE(x.to.size() == 1);
    CHECK(x.from[0] == 0);
    CHECK(x.to[0] == 2);
    CHECK(x.at[0] == 1);
    x.wx().target = WeatherKind::Storm;
    x.run(3600);
    REQUIRE(x.to.size() == 2);
    CHECK(x.from[1] == 2);
    CHECK(x.to[1] == 3);
    x.run(1000);
    CHECK(x.to.size() == 2);
}

TEST_CASE("weather: the seeded schedule is the same for a seed and differs for another") {
    auto kinds = [](u32 seed) {
        Wx x(WeatherKind::Clear, true, 10.0f, seed);
        Array<u32> picks;
        for (u32 i = 0; i < 12; ++i) {
            x.run(600);
            picks.push_back(static_cast<u32>(x.wx().target));
        }
        CHECK(x.wx().counter == 12);
        return picks;
    };
    const Array<u32> a = kinds(7), b = kinds(7), c = kinds(8);
    REQUIRE(a.size() == 12);
    bool same = true, differs = false;
    for (u32 i = 0; i < 12; ++i) {
        same = same && a[i] == b[i];
        differs = differs || a[i] != c[i];
        CHECK(a[i] < 5);
    }
    CHECK(same);
    CHECK(differs);
}

TEST_CASE("weather: a save mid-run resumes exactly, with the schedule counter") {
    Array<byte> bytes;
    u32 hash = 0;
    Weather expected;
    {
        Wx a(WeatherKind::Clear, true, 20.0f, 3);
        a.run(2500);
        CHECK(a.wx().counter == 2);
        save::write_save(a.w, {}, bytes);
        a.run(3000);
        expected = a.wx();
        (void)hash;
    }
    Wx b(WeatherKind::Clear, true, 0.0f, 99);  // a different base: the load overrides it
    auto st = save::read_save(b.w, bytes.span());
    REQUIRE(st);
    CHECK(b.wx().counter == 2);
    CHECK(b.wx().seed == 3);
    b.t = 2500;
    b.run(3000);
    CHECK(b.wx().counter == expected.counter);
    CHECK(b.wx().timer == expected.timer);
    CHECK(b.wx().target == expected.target);
    CHECK(b.wx().current == expected.current);
    CHECK(b.wx().fade == expected.fade);
    CHECK(b.wx().wet_q == expected.wet_q);
    CHECK(b.wx().intensity == expected.intensity);
    CHECK(b.wx().wind == expected.wind);
    CHECK(b.wx().wetness == expected.wetness);
}

TEST_CASE("weather: the same run twice has the same state hash, and no Weather changes nothing") {
    auto run = [](bool with) {
        Wx x(WeatherKind::Rain, with, 15.0f, 5);
        x.run(1500);
        if (with) x.wx().transition_seconds = 10.0f;
        x.run(1500);
        return x.w.state_hash();
    };
    CHECK(run(true) == run(true));
    CHECK(run(true) != run(false));
    Wx n(WeatherKind::Clear, false);
    n.run(100);
    CHECK(n.to.empty());
    World plain;
    plain.register_components<SaveKey>();
    install_kit3d(plain, nullptr);
    plain.spawn_with(SaveKey{1});
    for (u64 i = 0; i < 100; ++i) plain.tick(kDt, i);
    CHECK(plain.state_hash() == n.w.state_hash());
}

TEST_CASE("weather: an infinite or huge transition_seconds does not crash and never completes a fade") {
    const f32 huge[] = {std::numeric_limits<f32>::infinity(), FLT_MAX};
    for (f32 v : huge) {
        Wx x(WeatherKind::Snow);
        x.wx().transition_seconds = v;
        x.run(600);  // snowing: the wet rise divides by fade_den * ticks, which must not wrap to 0
        const Weather& c = x.wx();
        CHECK(c.fade_den != 0);
        CHECK(c.intensity >= 0.0f);
        CHECK(c.intensity <= 1.0f);
        CHECK(c.wetness >= 0.0f);
        CHECK(c.wetness <= 1.0f);
        x.wx().target = WeatherKind::Clear;
        x.run(600);
        CHECK(x.wx().intensity <= 1.0f);
    }
}
