// A stand-in for Valve's steam_api library (M-STEAM steps 1-3): the flat entry points mce_platform opens,
// with answers the test sets through MceSteamStub_* and a count of every call. Built as a shared
// library so the loader's real path (open, look up by name, close) runs in CI without Steam.

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#define STUB_EXPORT extern "C" __declspec(dllexport)
#else
#define STUB_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

struct Msg {
    int32_t user;
    int32_t callback;
    uint8_t* param;
    int32_t param_size;
};

struct Pending {
    int32_t id;
    std::vector<uint8_t> bytes;
};

struct Stub {
    int init_result = 0;
    bool restart = false;
    bool deck = false;
    std::map<std::string, int> calls;
    std::vector<Pending> queue;     // pushed by the test
    std::vector<Pending> ready;     // moved over by RunFrame
    std::vector<uint8_t> current;   // the callback handed out last
    int utils = 0, user = 0, friends = 0, stats = 0, storage = 0;  // their addresses are what matters
    bool logged_on = true;
    // Game services (step 2): unlocked achievements, the best score per board, Steam Cloud files.
    std::map<std::string, bool> achievements;
    std::map<std::string, int32_t> boards;  // the boards the app's Steamworks page defines
    std::map<std::string, int32_t> best;
    std::map<std::string, std::vector<uint8_t>> files;
    std::string overlay;
    std::map<std::string, std::string> presence;
    int frames = 0;
    int overlay_open_at = -1, overlay_close_at = -1;  // MCE_STEAM_STUB_OVERLAY ("open,close" frames)
    uint64_t next_call = 100;
    std::map<uint64_t, Pending> results;  // asynchronous call results, fetched by handle
    // MCE_STEAM_STUB_BOARDS ("a,b"): the boards a run of the stock player finds.
    Stub() {
        const char* list = std::getenv("MCE_STEAM_STUB_BOARDS");
        for (std::string rest = list ? list : ""; !rest.empty();) {
            const size_t comma = rest.find(',');
            boards[rest.substr(0, comma)] = 0;
            rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
        }
        if (const char* o = std::getenv("MCE_STEAM_STUB_OVERLAY"))
            std::sscanf(o, "%d,%d", &overlay_open_at, &overlay_close_at);
    }
};
Stub& s() {
    static Stub stub;
    return stub;
}
void count(const char* name) { ++s().calls[name]; }

}  // namespace

// ---- the test's side
STUB_EXPORT void MceSteamStub_Reset() { s() = Stub{}; }
STUB_EXPORT void MceSteamStub_SetInitResult(int r) { s().init_result = r; }
STUB_EXPORT void MceSteamStub_SetRestart(bool r) { s().restart = r; }
STUB_EXPORT void MceSteamStub_SetDeck(bool d) { s().deck = d; }
STUB_EXPORT int MceSteamStub_Calls(const char* name) {
    const auto it = s().calls.find(name);
    return it == s().calls.end() ? 0 : it->second;
}
STUB_EXPORT void MceSteamStub_AddBoard(const char* name) { s().boards[name] = 0; }
STUB_EXPORT bool MceSteamStub_Achieved(const char* name) { return s().achievements.count(name) != 0; }
STUB_EXPORT int32_t MceSteamStub_Best(const char* name) {
    const auto it = s().best.find(name);
    return it == s().best.end() ? -1 : it->second;
}
STUB_EXPORT const char* MceSteamStub_Overlay() { return s().overlay.c_str(); }
STUB_EXPORT void MceSteamStub_PushCallback(int id, const void* data, int size) {
    Pending p{id, {}};
    p.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    s().queue.push_back(p);
}

// ---- the flat API
STUB_EXPORT int SteamAPI_InitFlat(char* message) {
    count("SteamAPI_InitFlat");
    if (s().init_result != 0) std::strcpy(message, "stub: refused");
    return s().init_result;
}
STUB_EXPORT bool SteamAPI_RestartAppIfNecessary(uint32_t) {
    count("SteamAPI_RestartAppIfNecessary");
    return s().restart;
}
STUB_EXPORT void SteamAPI_Shutdown() { count("SteamAPI_Shutdown"); }
STUB_EXPORT void SteamAPI_ManualDispatch_Init() { count("SteamAPI_ManualDispatch_Init"); }
STUB_EXPORT int32_t SteamAPI_GetHSteamPipe() { return 7; }
STUB_EXPORT void SteamAPI_ManualDispatch_RunFrame(int32_t pipe) {
    count(pipe == 7 ? "SteamAPI_ManualDispatch_RunFrame" : "bad pipe");
    ++s().frames;
    if (s().frames == s().overlay_open_at || s().frames == s().overlay_close_at) {
        // GameOverlayActivated_t: m_bActive, m_bUserInitiated, m_nAppID, m_dwOverlayPID.
        uint8_t ev[12] = {static_cast<uint8_t>(s().frames == s().overlay_open_at ? 1 : 0), 1};
        Pending p{331, std::vector<uint8_t>(ev, ev + sizeof(ev))};
        s().queue.push_back(p);
    }
    for (Pending& p : s().queue) s().ready.push_back(p);
    s().queue.clear();
}
STUB_EXPORT bool SteamAPI_ManualDispatch_GetNextCallback(int32_t, Msg* msg) {
    if (s().ready.empty()) return false;
    s().current = s().ready.front().bytes;
    msg->user = 1;
    msg->callback = s().ready.front().id;
    msg->param = s().current.data();
    msg->param_size = static_cast<int32_t>(s().current.size());
    s().ready.erase(s().ready.begin());
    return true;
}
STUB_EXPORT void SteamAPI_ManualDispatch_FreeLastCallback(int32_t) {
    count("SteamAPI_ManualDispatch_FreeLastCallback");
    s().current.clear();
}
STUB_EXPORT void* SteamAPI_SteamUtils_v010() { return &s().utils; }
STUB_EXPORT void* SteamAPI_SteamUser_v023() { return &s().user; }
STUB_EXPORT void* SteamAPI_SteamFriends_v017() { return &s().friends; }
STUB_EXPORT uint32_t SteamAPI_ISteamUtils_GetAppID(void* self) { return self == &s().utils ? 480u : 0u; }
STUB_EXPORT bool SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck(void* self) { return self == &s().utils && s().deck; }
STUB_EXPORT bool SteamAPI_ISteamUtils_IsSteamInBigPictureMode(void*) { return false; }
STUB_EXPORT uint64_t SteamAPI_ISteamUser_GetSteamID(void* self) { return self == &s().user ? 76561197960287930ull : 0; }
STUB_EXPORT const char* SteamAPI_ISteamFriends_GetPersonaName(void* self) { return self == &s().friends ? "Stub Player" : "?"; }

// ---- game services (step 2)
namespace {

// An asynchronous call: its handle now, SteamAPICallCompleted_t (703) on the next RunFrame, and the
// result struct fetched with GetAPICallResult.
uint64_t answer_later(int32_t id, const void* result, int size) {
    const uint64_t call = s().next_call++;
    Pending r{id, {}};
    r.bytes.assign(static_cast<const uint8_t*>(result), static_cast<const uint8_t*>(result) + size);
    s().results[call] = r;
    struct {
        uint64_t call;
        int32_t callback;
        uint32_t size;
    } done{call, id, static_cast<uint32_t>(size)};
    MceSteamStub_PushCallback(703, &done, sizeof(done));
    return call;
}

}  // namespace

STUB_EXPORT void MceSteamStub_SetLoggedOn(bool on) { s().logged_on = on; }
STUB_EXPORT void* SteamAPI_SteamUserStats_v013() { return &s().stats; }
STUB_EXPORT void* SteamAPI_SteamRemoteStorage_v016() { return &s().storage; }
STUB_EXPORT bool SteamAPI_ISteamUser_BLoggedOn(void* self) { return self == &s().user && s().logged_on; }
STUB_EXPORT bool SteamAPI_ISteamUserStats_SetAchievement(void* self, const char* name) {
    count("SteamAPI_ISteamUserStats_SetAchievement");
    if (self != &s().stats) return false;
    s().achievements[name] = true;
    return true;
}
STUB_EXPORT bool SteamAPI_ISteamUserStats_StoreStats(void* self) {
    count("SteamAPI_ISteamUserStats_StoreStats");
    return self == &s().stats;
}
STUB_EXPORT bool SteamAPI_ISteamUserStats_IndicateAchievementProgress(void* self, const char* name, uint32_t cur, uint32_t max) {
    count("SteamAPI_ISteamUserStats_IndicateAchievementProgress");
    return self == &s().stats && !s().achievements.count(name) && cur < max;
}
STUB_EXPORT uint64_t SteamAPI_ISteamUserStats_FindLeaderboard(void* self, const char* name) {
    count("SteamAPI_ISteamUserStats_FindLeaderboard");
    if (self != &s().stats) return 0;
    // LeaderboardFindResult_t (1104): the handle (the board's index + 1), then whether it was found.
    uint8_t result[12] = {};
    const auto it = s().boards.find(name);
    if (it != s().boards.end()) {
        const uint64_t handle = static_cast<uint64_t>(std::distance(s().boards.begin(), it)) + 1;
        std::memcpy(result, &handle, 8);
        result[8] = 1;
    }
    return answer_later(1104, result, sizeof(result));
}
STUB_EXPORT uint64_t SteamAPI_ISteamUserStats_UploadLeaderboardScore(void* self, uint64_t board, int32_t method, int32_t score,
                                                                     const int32_t*, int32_t) {
    count("SteamAPI_ISteamUserStats_UploadLeaderboardScore");
    if (self != &s().stats || board == 0 || board > s().boards.size() || method != 1) return 0;
    auto it = s().boards.begin();
    std::advance(it, static_cast<long>(board - 1));
    auto best = s().best.find(it->first);
    if (best == s().best.end() || score > best->second) s().best[it->first] = score;
    // LeaderboardScoreUploaded_t (1106): m_bSuccess first.
    uint8_t result[28] = {1};
    return answer_later(1106, result, sizeof(result));
}
STUB_EXPORT bool SteamAPI_ManualDispatch_GetAPICallResult(int32_t pipe, uint64_t call, void* out, int size, int expected,
                                                          bool* failed) {
    count("SteamAPI_ManualDispatch_GetAPICallResult");
    const auto it = s().results.find(call);
    if (pipe != 7 || it == s().results.end() || it->second.id != expected ||
        size < static_cast<int>(it->second.bytes.size()))
        return false;
    std::memcpy(out, it->second.bytes.data(), it->second.bytes.size());
    *failed = false;
    s().results.erase(it);
    return true;
}
STUB_EXPORT bool SteamAPI_ISteamRemoteStorage_FileExists(void* self, const char* name) {
    return self == &s().storage && s().files.count(name) != 0;
}
STUB_EXPORT int32_t SteamAPI_ISteamRemoteStorage_GetFileSize(void* self, const char* name) {
    const auto it = s().files.find(name);
    return self == &s().storage && it != s().files.end() ? static_cast<int32_t>(it->second.size()) : 0;
}
STUB_EXPORT int32_t SteamAPI_ISteamRemoteStorage_FileRead(void* self, const char* name, void* data, int32_t size) {
    const auto it = s().files.find(name);
    if (self != &s().storage || it == s().files.end()) return 0;
    const int32_t n = std::min(size, static_cast<int32_t>(it->second.size()));
    std::memcpy(data, it->second.data(), static_cast<size_t>(n));
    return n;
}
STUB_EXPORT bool SteamAPI_ISteamRemoteStorage_FileWrite(void* self, const char* name, const void* data, int32_t size) {
    count("SteamAPI_ISteamRemoteStorage_FileWrite");
    if (self != &s().storage || size < 0) return false;
    s().files[name].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    return true;
}
STUB_EXPORT void SteamAPI_ISteamFriends_ActivateGameOverlay(void* self, const char* dialog) {
    if (self != &s().friends) return;
    s().overlay = dialog;
    std::printf("steam stub: overlay %s\n", dialog);
    std::fflush(stdout);
}

// ---- overlay pages and rich presence (step 3); printed so a player run's test can see them
STUB_EXPORT const char* MceSteamStub_Presence(const char* key) {
    const auto it = s().presence.find(key);
    return it == s().presence.end() ? "" : it->second.c_str();
}
STUB_EXPORT int MceSteamStub_PresenceCount() { return static_cast<int>(s().presence.size()); }
STUB_EXPORT bool SteamAPI_ISteamFriends_SetRichPresence(void* self, const char* key, const char* value) {
    if (self != &s().friends || std::strlen(key) > 64 || std::strlen(value) > 256) return false;
    if (*value) s().presence[key] = value;
    else s().presence.erase(key);
    std::printf("steam stub: rich presence %s=%s\n", key, value);
    std::fflush(stdout);
    return true;
}
STUB_EXPORT void SteamAPI_ISteamFriends_ClearRichPresence(void* self) {
    if (self == &s().friends) s().presence.clear();
}
STUB_EXPORT void SteamAPI_ISteamFriends_ActivateGameOverlayToStore(void* self, uint32_t app, int32_t) {
    if (self == &s().friends) s().overlay = "store:" + std::to_string(app);
}
STUB_EXPORT void SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage(void* self, const char* url, int32_t) {
    if (self == &s().friends) s().overlay = std::string("web:") + url;
}
