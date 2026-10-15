// M-STEAM step 1: the Steamworks loader against a stub steam_api (steam_stub.cpp), so the open, the
// init answers, the callback pump and the shutdown run in CI without Steam.

#include <doctest/doctest.h>

#include <cstdlib>

#include "mce/platform/library.h"
#include "mce/platform/steam.h"

using namespace mce;
namespace steam = mce::platform::steam;

namespace {

// The stub's own controls, looked up in the same library the loader opened.
struct StubControls {
    void* lib = nullptr;
    void (*reset)() = nullptr;
    void (*set_init)(int) = nullptr;
    void (*set_restart)(bool) = nullptr;
    void (*set_deck)(bool) = nullptr;
    int (*calls)(const char*) = nullptr;
    void (*push)(int, const void*, int) = nullptr;
    StubControls() {
        auto l = platform::load_library(MCE_STEAM_STUB);
        REQUIRE(l.has_value());
        lib = l.value();
        reset = reinterpret_cast<void (*)()>(platform::library_symbol(lib, "MceSteamStub_Reset"));
        set_init = reinterpret_cast<void (*)(int)>(platform::library_symbol(lib, "MceSteamStub_SetInitResult"));
        set_restart = reinterpret_cast<void (*)(bool)>(platform::library_symbol(lib, "MceSteamStub_SetRestart"));
        set_deck = reinterpret_cast<void (*)(bool)>(platform::library_symbol(lib, "MceSteamStub_SetDeck"));
        calls = reinterpret_cast<int (*)(const char*)>(platform::library_symbol(lib, "MceSteamStub_Calls"));
        push = reinterpret_cast<void (*)(int, const void*, int)>(platform::library_symbol(lib, "MceSteamStub_PushCallback"));
        REQUIRE(reset);
        REQUIRE(push);
        reset();
    }
    ~StubControls() { platform::unload_library(lib); }
};

steam::Config stub_config(u32 app_id = 0) {
    steam::Config c;
    c.library_path = MCE_STEAM_STUB;
    c.app_id = app_id;
    return c;
}

struct Seen {
    i32 ids[8] = {};
    u32 sizes[8] = {};
    u32 n = 0;
};
void record(void* user, const steam::Callback& cb) {
    Seen& s = *static_cast<Seen*>(user);
    s.ids[s.n] = cb.id;
    s.sizes[s.n] = static_cast<u32>(cb.data.size());
    ++s.n;
}

}  // namespace

TEST_CASE("steam: no library is an ordinary 'no Steam', and nothing answers while off") {
    steam::Config c;
    c.library_path = "/nonexistent/libsteam_api.so";
    FixedString<256> why;
    CHECK(steam::init(c, &why) == steam::InitResult::NoLibrary);
    CHECK(why.view().find("nonexistent") != StringView::npos);
    CHECK_FALSE(steam::running());
    CHECK(steam::run_callbacks(nullptr, nullptr) == 0);
    CHECK(steam::app_id() == 0);
    CHECK(StringView(steam::persona_name()).empty());
    CHECK(steam::entry("SteamAPI_Shutdown") == nullptr);
    steam::shutdown();  // a no-op
    CHECK(StringView(steam::init_result_name(steam::InitResult::NoSteamClient)) == "no_steam_client");
    CHECK(StringView(steam::library_file_name()).find("steam_api") != StringView::npos);
}

TEST_CASE("steam: init through the stub, the player and device, callbacks pumped once a frame, shutdown") {
    StubControls stub;
    stub.set_deck(true);
    REQUIRE(steam::init(stub_config(480)) == steam::InitResult::Ok);
    CHECK(steam::running());
    CHECK(stub.calls("SteamAPI_ManualDispatch_Init") == 1);  // manual dispatch is chosen before init
    CHECK(stub.calls("SteamAPI_InitFlat") == 1);
    CHECK(StringView(std::getenv("SteamAppId")) == "480");  // what steam_appid.txt would say
    CHECK(steam::app_id() == 480);
    CHECK(steam::steam_id() == 76561197960287930ull);
    CHECK(StringView(steam::persona_name()) == "Stub Player");
    CHECK(steam::on_steam_deck());
    CHECK_FALSE(steam::big_picture());
    CHECK(steam::entry("SteamAPI_ISteamUtils_GetAppID") != nullptr);
    const char* names[] = {"SteamAPI_SteamUtils_v099", "SteamAPI_SteamUtils_v010"};
    CHECK(steam::interface(Span<const char* const>(names, 2)) != nullptr);  // the first that resolves
    const char* none[] = {"SteamAPI_SteamNothing_v001"};
    CHECK(steam::interface(Span<const char* const>(none, 1)) == nullptr);
    CHECK(steam::init(stub_config(480)) == steam::InitResult::Ok);  // again: already running
    CHECK(stub.calls("SteamAPI_InitFlat") == 1);

    // Two callbacks pushed before a frame arrive in order on that frame's pump, each freed.
    const u8 a[12] = {1};
    const u8 b[3] = {2};
    stub.push(331, a, sizeof(a));
    stub.push(703, b, sizeof(b));
    Seen seen;
    CHECK(steam::run_callbacks(&record, &seen) == 2);
    CHECK(seen.ids[0] == 331);
    CHECK(seen.sizes[0] == 12);
    CHECK(seen.ids[1] == 703);
    CHECK(stub.calls("SteamAPI_ManualDispatch_RunFrame") == 1);
    CHECK(stub.calls("SteamAPI_ManualDispatch_FreeLastCallback") == 2);
    CHECK(steam::run_callbacks(nullptr, nullptr) == 0);

    steam::shutdown();
    CHECK(stub.calls("SteamAPI_Shutdown") == 1);
    CHECK_FALSE(steam::running());
    CHECK(steam::app_id() == 0);
}

TEST_CASE("steam: the client's refusals and a relaunch through Steam leave Steam off") {
    StubControls stub;
    FixedString<256> why;
    stub.set_init(2);
    CHECK(steam::init(stub_config(), &why) == steam::InitResult::NoSteamClient);
    CHECK(why.view() == "stub: refused");
    CHECK_FALSE(steam::running());
    stub.set_init(3);
    CHECK(steam::init(stub_config()) == steam::InitResult::VersionMismatch);
    stub.set_init(1);
    CHECK(steam::init(stub_config()) == steam::InitResult::Failed);
    stub.set_init(0);
    stub.set_restart(true);
    steam::Config c = stub_config(480);
    c.restart_if_necessary = true;
    CHECK(steam::init(c) == steam::InitResult::Restarting);
    CHECK_FALSE(steam::running());
    c.restart_if_necessary = false;  // a dev run does not ask
    CHECK(steam::init(c) == steam::InitResult::Ok);
    steam::shutdown();
}

TEST_CASE("steam: a library without the flat API's manual dispatch is refused") {
    steam::Config c;
    c.library_path = MCE_STEAM_STUB_OLD;
    CHECK(steam::init(c) == steam::InitResult::MissingEntry);
    CHECK_FALSE(steam::running());
}
