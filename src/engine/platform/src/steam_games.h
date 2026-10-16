#pragma once
// Steam as the game services backend (M-STEAM step 2; mce/platform/game_services.h): achievements and
// stats, leaderboards and Steam Cloud files, over the Steam loader (steam.h). Private to
// mce_platform; game_services.cpp forwards to it on desktop builds while Steam runs.

#include "mce/platform/game_services.h"

namespace mce::platform::steam::games {

// True while Steam runs and offers user stats and remote storage.
bool available();
bool start(game_services::AnswerFn fn, void* user);
void stop();
void sign_in();
void report_achievement(StringView name, u32 steps, u32 total, u16 tag);
void submit_score(StringView name, i64 score, u16 tag);
void read_blob(StringView name, u16 tag);
void write_blob(StringView name, Span<const byte> bytes, u16 tag);
void show_achievements();
void show_leaderboard(StringView name);

}  // namespace mce::platform::steam::games
