// Steam as the game services backend (M-STEAM step 2): ISteamUserStats for achievements and
// leaderboards, ISteamRemoteStorage (Steam Cloud) for the cloud-save blobs, the overlay for the
// platform's own screens. The flat entry points are looked up through the loader (steam.cpp); their
// signatures are the flat API's published C ones.
//
// - Achievements: one-shot (or at their total) SetAchievement + StoreStats; below the total,
//   IndicateAchievementProgress shows the progress (Steam keeps incremental progress in a stat the
//   game's Steamworks page defines, so the unlock is still the engine's to make at the total).
// - Leaderboards: FindLeaderboard once per name (the handle is kept), then UploadLeaderboardScore
//   with "keep best": the board's sort order is set on the Steamworks page. Both are asynchronous
//   calls answered through steam::run_callbacks. Steam scores are 32-bit; larger ones are clamped.
// - Cloud saves: Steam Cloud files under the save's name, read and written at once.

#include "steam_games.h"

#include <cstdint>
#include <cstring>

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/log.h"
#include "mce/platform/steam.h"

namespace mce::platform::steam::games {
namespace {

using game_services::Answer;
using game_services::AnswerKind;

using BoolObjFn = bool (*)(void* self);
using BoolNameFn = bool (*)(void* self, const char* name);
using ProgressFn = bool (*)(void* self, const char* name, u32 current, u32 max);
using FindFn = u64 (*)(void* self, const char* name);
using UploadFn = u64 (*)(void* self, u64 board, i32 method, i32 score, const i32* details, i32 detail_count);
using FileWriteFn = bool (*)(void* self, const char* name, const void* data, i32 size);
using FileReadFn = i32 (*)(void* self, const char* name, void* data, i32 size);
using FileSizeFn = i32 (*)(void* self, const char* name);
using OverlayFn = void (*)(void* self, const char* dialog);

// Callback ids of the call results (k_iCallback of LeaderboardFindResult_t, LeaderboardScoreUploaded_t).
constexpr i32 kLeaderboardFound = 1104;
constexpr i32 kScoreUploaded = 1106;
constexpr i32 kKeepBest = 1;  // ELeaderboardUploadScoreMethod

constexpr const char* kUserStats[] = {"SteamAPI_SteamUserStats_v013", "SteamAPI_SteamUserStats_v012"};
constexpr const char* kStorage[] = {"SteamAPI_SteamRemoteStorage_v016"};
constexpr const char* kUser[] = {"SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022", "SteamAPI_SteamUser_v021"};
constexpr const char* kFriends[] = {"SteamAPI_SteamFriends_v017"};

template <usize N>
void* find_interface(const char* const (&names)[N]) {
    return interface(Span<const char* const>(names, N));
}

template <typename F>
F fn(const char* name) {
    return reinterpret_cast<F>(entry(name));
}

struct Board {
    FixedString<128> name;
    u64 handle = 0;
};

// A score on its way: finding the board, then uploading.
struct Upload {
    bool used = false;
    u16 tag = 0;
    i32 score = 0;
    FixedString<128> board;
};

struct State {
    game_services::AnswerFn fn = nullptr;
    void* user = nullptr;
    bool started = false;
    void* stats = nullptr;
    void* storage = nullptr;
    Board boards[32];
    u32 board_count = 0;
    Upload uploads[kMaxCalls];
};
State g;

void answer(AnswerKind kind, bool ok, u16 tag, Span<const byte> bytes = {}, bool missing = false) {
    if (!g.fn) return;
    Answer a;
    a.kind = kind;
    a.ok = ok;
    a.tag = tag;
    a.bytes = bytes;
    a.missing = missing;
    g.fn(g.user, a);
}

u64 known_board(StringView name) {
    for (u32 i = 0; i < g.board_count; ++i)
        if (g.boards[i].name.view() == name) return g.boards[i].handle;
    return 0;
}

void on_uploaded(void* user, Span<const byte> result, bool failed) {
    Upload& u = *static_cast<Upload*>(user);
    const bool ok = !failed && !result.empty() && result[0] != byte{0};  // m_bSuccess
    const u16 tag = u.tag;
    u = Upload{};
    answer(AnswerKind::Score, ok, tag);
}

void upload(Upload& u, u64 board) {
    const auto up = fn<UploadFn>("SteamAPI_ISteamUserStats_UploadLeaderboardScore");
    const u64 call = up ? up(g.stats, board, kKeepBest, u.score, nullptr, 0) : 0;
    if (!await_call(call, kScoreUploaded, &on_uploaded, &u)) {
        const u16 tag = u.tag;
        u = Upload{};
        answer(AnswerKind::Score, false, tag);
    }
}

void on_found(void* user, Span<const byte> result, bool failed) {
    Upload& u = *static_cast<Upload*>(user);
    // LeaderboardFindResult_t: m_hSteamLeaderboard (8 bytes), then m_bLeaderboardFound.
    u64 board = 0;
    if (!failed && result.size() >= 9 && result[8] != byte{0}) std::memcpy(&board, result.data(), sizeof(board));
    if (board == 0) {
        MCE_LOG_WARN("steam: no leaderboard '{}' on the app's Steamworks page", u.board.c_str());
        const u16 tag = u.tag;
        u = Upload{};
        return answer(AnswerKind::Score, false, tag);
    }
    if (g.board_count < sizeof(g.boards) / sizeof(g.boards[0])) g.boards[g.board_count++] = Board{u.board, board};
    upload(u, board);
}

}  // namespace

bool available() { return running() && find_interface(kUserStats) && find_interface(kStorage); }

bool start(game_services::AnswerFn answer_fn, void* user) {
    g = State{};
    g.fn = answer_fn;
    g.user = user;
    if (!running()) return false;
    g.stats = find_interface(kUserStats);
    g.storage = find_interface(kStorage);
    if (!g.stats || !g.storage) return false;
    // SDKs before 1.61 load the player's stats only on request; later ones do it at init and drop the call.
    if (const auto request = fn<BoolObjFn>("SteamAPI_ISteamUserStats_RequestCurrentStats")) request(g.stats);
    g.started = true;
    return true;
}

void stop() { g = State{}; }

void sign_in() {
    // Steam has no separate sign-in: a running client is the signed-in player.
    const auto logged_on = fn<BoolObjFn>("SteamAPI_ISteamUser_BLoggedOn");
    void* u = find_interface(kUser);
    answer(AnswerKind::SignedIn, g.started && logged_on && u && logged_on(u), 0);
}

void report_achievement(StringView name, u32 steps, u32 total, u16 tag) {
    if (!g.started) return answer(AnswerKind::Achievement, false, tag);
    const FixedString<128> z(name);
    bool ok = false;
    if (steps >= total) {
        const auto set = fn<BoolNameFn>("SteamAPI_ISteamUserStats_SetAchievement");
        const auto store = fn<BoolObjFn>("SteamAPI_ISteamUserStats_StoreStats");
        ok = set && store && set(g.stats, z.c_str()) && store(g.stats);
    } else {
        const auto progress = fn<ProgressFn>("SteamAPI_ISteamUserStats_IndicateAchievementProgress");
        ok = progress && progress(g.stats, z.c_str(), steps, total);
    }
    answer(AnswerKind::Achievement, ok, tag);
}

void submit_score(StringView name, i64 score, u16 tag) {
    if (!g.started) return answer(AnswerKind::Score, false, tag);
    Upload* u = nullptr;
    for (Upload& x : g.uploads)
        if (!x.used) {
            u = &x;
            break;
        }
    if (!u) return answer(AnswerKind::Score, false, tag);
    u->used = true;
    u->tag = tag;
    u->score = score > INT32_MAX ? INT32_MAX : score < INT32_MIN ? INT32_MIN : static_cast<i32>(score);
    u->board = name;
    if (const u64 board = known_board(name)) return upload(*u, board);
    const auto find = fn<FindFn>("SteamAPI_ISteamUserStats_FindLeaderboard");
    const u64 call = find ? find(g.stats, u->board.c_str()) : 0;
    if (!await_call(call, kLeaderboardFound, &on_found, u)) {
        *u = Upload{};
        answer(AnswerKind::Score, false, tag);
    }
}

void read_blob(StringView name, u16 tag) {
    if (!g.started) return answer(AnswerKind::BlobRead, false, tag);
    const FixedString<64> z(name);
    const auto exists = fn<BoolNameFn>("SteamAPI_ISteamRemoteStorage_FileExists");
    const auto size_of = fn<FileSizeFn>("SteamAPI_ISteamRemoteStorage_GetFileSize");
    const auto read = fn<FileReadFn>("SteamAPI_ISteamRemoteStorage_FileRead");
    if (!exists || !size_of || !read) return answer(AnswerKind::BlobRead, false, tag);
    if (!exists(g.storage, z.c_str())) return answer(AnswerKind::BlobRead, true, tag, {}, true);
    const i32 size = size_of(g.storage, z.c_str());
    if (size < 0) return answer(AnswerKind::BlobRead, false, tag);
    Array<byte> bytes;
    bytes.resize_uninitialized(usize(size));
    const i32 got = size > 0 ? read(g.storage, z.c_str(), bytes.data(), size) : 0;
    answer(AnswerKind::BlobRead, got == size, tag, bytes.span());
}

void write_blob(StringView name, Span<const byte> bytes, u16 tag) {
    if (!g.started) return answer(AnswerKind::BlobWritten, false, tag);
    const FixedString<64> z(name);
    const auto write = fn<FileWriteFn>("SteamAPI_ISteamRemoteStorage_FileWrite");
    answer(AnswerKind::BlobWritten, write && write(g.storage, z.c_str(), bytes.data(), i32(bytes.size())), tag);
}

void show_achievements() {
    const auto overlay = fn<OverlayFn>("SteamAPI_ISteamFriends_ActivateGameOverlay");
    if (void* f = find_interface(kFriends); g.started && overlay && f) overlay(f, "achievements");
}

void show_leaderboard(StringView) {
    // The overlay has no page for one leaderboard: its stats page lists them.
    const auto overlay = fn<OverlayFn>("SteamAPI_ISteamFriends_ActivateGameOverlay");
    if (void* f = find_interface(kFriends); g.started && overlay && f) overlay(f, "stats");
}

}  // namespace mce::platform::steam::games
