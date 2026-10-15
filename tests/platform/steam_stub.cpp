// A stand-in for Valve's steam_api library (M-STEAM step 1): the flat entry points mce_platform opens,
// with answers the test sets through MceSteamStub_* and a count of every call. Built as a shared
// library so the loader's real path (open, look up by name, close) runs in CI without Steam.

#include <cstdint>
#include <cstring>
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
    int utils = 0, user = 0, friends = 0;  // the interface objects' addresses are what matters
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
