// M-STEAM step 3: rich presence and overlay requests in the sim. The first enabled RichPresence
// shows, OpenOverlay events wait in OverlayRequests (oldest dropped past the cap) until the player
// takes them, and neither changes the state hash.

#include <doctest/doctest.h>

#include "mce/ecs/world.h"
#include "mce/kit/presence.h"

using namespace mce;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct PresenceWorld {
    World world;
    u64 tick = 0;
    PresenceWorld() { kit::install_presence(world); }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, tick++);
    }
    void open(kit::OverlayPage page, const char* url = "") {
        kit::OpenOverlay o;
        o.page = page;
        o.url.assign(url);
        world.event_queue<kit::OpenOverlay>()->push(o);
    }
};

}  // namespace

TEST_CASE("M-STEAM: the first enabled RichPresence is the one shown") {
    PresenceWorld w;
    kit::RichPresence out;
    CHECK_FALSE(kit::current_presence(w.world, out));
    kit::RichPresence off;
    off.enabled = false;
    off.status.assign("hidden");
    kit::RichPresence menu;
    menu.status.assign("In the menu");
    menu.steam_display.assign("#StatusMenu");
    kit::RichPresence level;
    level.status.assign("Exploring");
    const Entity e_off = w.world.spawn_with(off);
    const Entity e_menu = w.world.spawn_with(menu);
    w.world.spawn_with(level);
    REQUIRE(kit::current_presence(w.world, out));
    CHECK(out.status.view() == StringView("In the menu"));
    CHECK(out.steam_display.view() == StringView("#StatusMenu"));
    w.world.destroy(e_menu);
    REQUIRE(kit::current_presence(w.world, out));
    CHECK(out.status.view() == StringView("Exploring"));
    w.world.try_get_mut<kit::RichPresence>(e_off)->enabled = true;
    REQUIRE(kit::current_presence(w.world, out));
    CHECK(out.status.view() == StringView("hidden"));
    CHECK(StringView(kit::overlay_page_name(kit::OverlayPage::Achievements)) == "achievements");
}

TEST_CASE("M-STEAM: OpenOverlay requests wait until the player takes them") {
    PresenceWorld w;
    w.open(kit::OverlayPage::Achievements);
    w.open(kit::OverlayPage::Web, "https://example.org");
    Array<kit::OpenOverlay> got;
    kit::take_overlay_requests(w.world, got);
    CHECK(got.empty());  // not read before a tick
    w.run(1);
    kit::take_overlay_requests(w.world, got);
    REQUIRE(got.size() == 2);
    CHECK(got[0].page == kit::OverlayPage::Achievements);
    CHECK(got[1].url.view() == StringView("https://example.org"));
    kit::take_overlay_requests(w.world, got);
    CHECK(got.empty());  // taken once

    for (u32 i = 0; i < kit::kMaxOverlayRequests + 2; ++i) w.open(i < 2 ? kit::OverlayPage::Friends : kit::OverlayPage::Store);
    w.run(1);
    kit::take_overlay_requests(w.world, got);
    REQUIRE(got.size() == kit::kMaxOverlayRequests);
    CHECK(got[0].page == kit::OverlayPage::Store);  // the two oldest dropped
}

TEST_CASE("M-STEAM: presence and overlay requests leave the state hash alone") {
    PresenceWorld a, b;
    kit::RichPresence p;
    p.status.assign("Exploring");
    a.world.spawn_with(p);
    b.world.spawn_with(p);
    a.open(kit::OverlayPage::Friends);  // only one world asks
    for (u32 i = 0; i < 5; ++i) {
        a.run(1);
        b.run(1);
        CHECK(a.world.state_hash() == b.world.state_hash());
    }
}
