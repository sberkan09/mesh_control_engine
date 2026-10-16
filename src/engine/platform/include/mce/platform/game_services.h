#pragma once
// mce_platform (L1): the phone's own game services (M-LIVEOPS step 2): Game Center on iOS, Play
// Games Services v2 on Android. Achievements, leaderboards and saved-game blobs by the store's ids;
// liveops::PlatformGameServices maps the engine's catalog onto them and keeps cloud versions.
//
// The store wall (check_walls.py) covers this too: GameKit, Play Games and JNI live only in
// mce_platform and the stamped Android project.
//
// - Android: org.meshcontrol.games.MceGames over JNI, stamped into the Gradle project of a game
//   with a .mcservices when MCE_PLAY_GAMES_APP_ID names its Play Games project
//   (cmake/mobile/android_app.py). Cloud saves are Play Games snapshots (Saved Games turned on in
//   the Play Console).
// - iOS: GameKit (game_services_gamekit.swift), built when MCE_GAMEKIT is ON. Cloud saves are Game
//   Center saved games, which need the iCloud capability in the provisioning profile.
// - Desktop with Steam running (steam.h, M-STEAM step 2): Steam achievements, leaderboards and Steam
//   Cloud files; answers arrive from steam::run_callbacks on the main thread.
// - Everywhere else: backend() is None and start() fails.
//
// Calls are made on the main thread; answers arrive through the handler on the platform's thread,
// each carrying the call's tag (an index the caller chose).

#include "mce/types.h"

namespace mce::platform::game_services {

enum class Backend : u8 { None, GameCenter, PlayGames, Steam };

enum class AnswerKind : u8 {
    SignedIn,          // ok: signed in; otherwise the player is not (or cancelled)
    Achievement,       // ok: the platform took the report
    Score,             // ok: the platform took the score
    BlobRead,          // ok: bytes is the saved blob (empty with missing: nothing saved under the name)
    BlobWritten,       // ok: written
};

struct Answer {
    AnswerKind kind = AnswerKind::SignedIn;
    bool ok = false;
    bool missing = false;  // BlobRead: nothing saved under that name yet (ok is true)
    u16 tag = 0;
    Span<const byte> bytes;  // BlobRead; valid during the call only
};

using AnswerFn = void (*)(void* user, const Answer& a);

Backend backend();
const char* backend_name(Backend b);  // "none", "game_center", "play_games", "steam"

// Answers go to `fn` until stop(). False when backend() is None.
bool start(AnswerFn fn, void* user);
void stop();

// Silent: uses the account the device is signed in with. Interactive: shows the platform's sign-in.
void sign_in(bool interactive);
// `steps` of `total` (one-shot achievements: 1 of 1; Game Center gets a percentage).
void report_achievement(StringView id, u32 steps, u32 total, u16 tag);
void submit_score(StringView id, i64 score, u16 tag);
void read_blob(StringView name, u16 tag);
void write_blob(StringView name, Span<const byte> bytes, u16 tag);
// The platform's own screens.
void show_achievements();
void show_leaderboard(StringView id);

}  // namespace mce::platform::game_services
