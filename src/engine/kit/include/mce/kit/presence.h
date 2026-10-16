#pragma once
// Rich presence and the platform overlay (M-STEAM step 3): what friends see the player doing, and the
// platform's own pages a game opens. Both are requests to the platform the stock player passes on
// (Steam while it runs, nothing elsewhere); the sim never waits on them.
//
//   - RichPresence, a data component: the presence of the first enabled one (by entity order) is
//     shown; none clears it. Put one on a level's root, or set its fields from a system.
//   - OpenOverlay, an event a game pushes ("open the achievements page", "this game's store page").
//     kit_presence keeps the requests in the OverlayRequests resource until the player takes them.
//
// RichPresence holds only authored fields and OverlayRequests is a resource (resources are not in
// the state hash), so a run hashes the same with or without a platform. kit::install_stock installs it.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

// Steam: `status` is the "status" key (the text in the friends list's game info); `steam_display` a
// token of the app's rich presence localization ("#StatusInLevel") shown in the friends list, with
// up to two substitutions ("level" = "Ashen Court" fills %level%). Empty fields are not sent.
struct RichPresence {
    bool enabled = true;
    FixedString<64> status;
    FixedString<64> steam_display;
    FixedString<32> key1;
    FixedString<64> value1;
    FixedString<32> key2;
    FixedString<64> value2;
};
MCE_COMPONENT(RichPresence, (enabled, Help{"off: this one is skipped (the next enabled one, or none, shows)"}),
              (status, Help{"plain text friends see: \"Exploring the Ashen Court\""}),
              (steam_display, Help{"Steam: a token of the app's rich presence localization, \"#StatusInLevel\""}),
              (key1, Help{"a substitution the token uses, \"level\" for %level%"}), (value1, Help{"its value"}),
              (key2, Help{"a second substitution"}), (value2, Help{"its value"}))

enum class OverlayPage : u8 { Friends, Achievements, Stats, Community, Settings, Store, Web };
MCE_ENUM(OverlayPage, Friends, Achievements, Stats, Community, Settings, Store, Web)

// Open the platform overlay on `page` (Store: this game's store page; Web: `url`). Push in
// FixedUpdate_Pre or _Post.
struct OpenOverlay {
    OverlayPage page = OverlayPage::Friends;
    FixedString<192> url;
};

// The overlay requests not yet taken by the player, oldest first (at most kMaxOverlayRequests).
struct OverlayRequests {
    Array<OpenOverlay> pending;
};
inline constexpr u32 kMaxOverlayRequests = 8;

// Registers RichPresence, OpenOverlay, the OverlayRequests resource and "kit_presence"
// (FixedUpdate_Post). Idempotent.
void install_presence(World& world);

// The presence to show: the first enabled RichPresence by entity order. False when there is none.
bool current_presence(World& world, RichPresence& out);

// Moves the pending overlay requests into `out` (cleared first).
void take_overlay_requests(World& world, Array<OpenOverlay>& out);

const char* overlay_page_name(OverlayPage p);  // "friends", "achievements", ...

}  // namespace mce::kit
