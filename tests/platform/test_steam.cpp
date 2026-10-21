// M-STEAM steps 1-4: the Steamworks loader and Steam as the game services backend against a stub
// steam_api (steam_stub.cpp), so the open, the init answers, the callback pump, asynchronous call
// results, achievements, leaderboards, Steam Cloud and the shutdown run in CI without Steam.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mce/platform/game_services.h"
#include "mce/platform/library.h"
#include "mce/platform/steam.h"
#include "mce/platform/steam_input.h"

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
    template <typename F>
    F get(const char* name) {
        F f = reinterpret_cast<F>(platform::library_symbol(lib, name));
        REQUIRE(f);
        return f;
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

namespace {

namespace gs = mce::platform::game_services;

struct Answers {
    gs::AnswerKind kind[16] = {};
    bool ok[16] = {};
    bool missing[16] = {};
    u16 tag[16] = {};
    std::string bytes[16];
    u32 n = 0;
};
void collect(void* user, const gs::Answer& a) {
    Answers& s = *static_cast<Answers*>(user);
    REQUIRE(s.n < 16);
    s.kind[s.n] = a.kind;
    s.ok[s.n] = a.ok;
    s.missing[s.n] = a.missing;
    s.tag[s.n] = a.tag;
    s.bytes[s.n].assign(reinterpret_cast<const char*>(a.bytes.data()), a.bytes.size());
    ++s.n;
}

}  // namespace

TEST_CASE("steam: an asynchronous call's result reaches the one waiting for it") {
    StubControls stub;
    REQUIRE(steam::init(stub_config()) == steam::InitResult::Ok);
    CHECK_FALSE(steam::await_call(0, 1104, [](void*, Span<const byte>, bool) {}, nullptr));  // the invalid handle
    stub.get<void (*)(const char*)>("MceSteamStub_AddBoard")("best_time");
    const auto find = reinterpret_cast<u64 (*)(void*, const char*)>(steam::entry("SteamAPI_ISteamUserStats_FindLeaderboard"));
    const char* stats_names[] = {"SteamAPI_SteamUserStats_v013"};
    void* stats = steam::interface(Span<const char* const>(stats_names, 1));
    REQUIRE(find);
    REQUIRE(stats);
    struct Got {
        int calls = 0;
        bool failed = true;
        u64 board = 0;
    } got;
    REQUIRE(steam::await_call(find(stats, "best_time"), 1104,
                              [](void* u, Span<const byte> r, bool failed) {
                                  Got& g = *static_cast<Got*>(u);
                                  ++g.calls;
                                  g.failed = failed;
                                  if (r.size() >= 8) std::memcpy(&g.board, r.data(), 8);
                              },
                              &got));
    CHECK(got.calls == 0);  // answered on a later frame
    CHECK(steam::run_callbacks(nullptr, nullptr) == 1);
    CHECK(got.calls == 1);
    CHECK_FALSE(got.failed);
    CHECK(got.board == 1);
    CHECK(steam::run_callbacks(nullptr, nullptr) == 0);
    CHECK(got.calls == 1);  // once
    steam::shutdown();
    CHECK_FALSE(steam::await_call(5, 1104, [](void*, Span<const byte>, bool) {}, nullptr));  // Steam is off
}

TEST_CASE("steam: game services on Steam: sign-in, achievements, leaderboards, Steam Cloud, the overlay") {
    StubControls stub;
    CHECK(gs::backend() == gs::Backend::None);  // no Steam yet
    REQUIRE(steam::init(stub_config(480)) == steam::InitResult::Ok);
    CHECK(gs::backend() == gs::Backend::Steam);
    CHECK(StringView(gs::backend_name(gs::Backend::Steam)) == "steam");
    stub.get<void (*)(const char*)>("MceSteamStub_AddBoard")("BEST_TIME");
    const auto achieved = stub.get<bool (*)(const char*)>("MceSteamStub_Achieved");
    const auto best = stub.get<int32_t (*)(const char*)>("MceSteamStub_Best");

    Answers seen;
    REQUIRE(gs::start(&collect, &seen));
    gs::sign_in(false);
    REQUIRE(seen.n == 1);
    CHECK(seen.kind[0] == gs::AnswerKind::SignedIn);
    CHECK(seen.ok[0]);

    // A one-shot achievement unlocks and is stored; progress below the total only shows.
    gs::report_achievement("ACH_FIRST_WIN", 1, 1, 3);
    gs::report_achievement("win_10", 4, 10, 4);
    REQUIRE(seen.n == 3);
    CHECK((seen.kind[1] == gs::AnswerKind::Achievement && seen.ok[1] && seen.tag[1] == 3));
    CHECK((seen.ok[2] && seen.tag[2] == 4));
    CHECK(achieved("ACH_FIRST_WIN"));
    CHECK_FALSE(achieved("win_10"));
    CHECK(stub.calls("SteamAPI_ISteamUserStats_StoreStats") == 1);

    // A score: the board is found on one frame, the upload answers on the next; the handle is kept.
    gs::submit_score("BEST_TIME", 4200, 7);
    gs::submit_score("NO_SUCH_BOARD", 1, 8);
    CHECK(seen.n == 3);
    steam::run_callbacks(nullptr, nullptr);
    REQUIRE(seen.n == 4);
    CHECK((seen.kind[3] == gs::AnswerKind::Score && !seen.ok[3] && seen.tag[3] == 8));  // not on the Steamworks page
    steam::run_callbacks(nullptr, nullptr);
    REQUIRE(seen.n == 5);
    CHECK((seen.kind[4] == gs::AnswerKind::Score && seen.ok[4] && seen.tag[4] == 7));
    CHECK(best("BEST_TIME") == 4200);
    gs::submit_score("BEST_TIME", i64(1) << 40, 9);  // clamped to Steam's 32 bits
    steam::run_callbacks(nullptr, nullptr);
    REQUIRE(seen.n == 6);
    CHECK(seen.ok[5]);
    CHECK(best("BEST_TIME") == INT32_MAX);
    CHECK(stub.calls("SteamAPI_ISteamUserStats_FindLeaderboard") == 2);  // BEST_TIME once, the missing one once

    // Steam Cloud: nothing saved, then a write read back.
    gs::read_blob("progress", 0);
    REQUIRE(seen.n == 7);
    CHECK((seen.kind[6] == gs::AnswerKind::BlobRead && seen.ok[6] && seen.missing[6]));
    const char blob[] = "MCGS level=3";
    gs::write_blob("progress", Span<const byte>(reinterpret_cast<const byte*>(blob), sizeof(blob) - 1), 0);
    gs::read_blob("progress", 0);
    REQUIRE(seen.n == 9);
    CHECK((seen.kind[7] == gs::AnswerKind::BlobWritten && seen.ok[7]));
    CHECK((seen.kind[8] == gs::AnswerKind::BlobRead && seen.ok[8] && !seen.missing[8]));
    CHECK(seen.bytes[8] == "MCGS level=3");

    gs::show_achievements();
    CHECK(StringView(stub.get<const char* (*)()>("MceSteamStub_Overlay")()) == "achievements");
    gs::show_leaderboard("BEST_TIME");
    CHECK(StringView(stub.get<const char* (*)()>("MceSteamStub_Overlay")()) == "stats");

    // Signed out of the client: sign-in says so.
    stub.get<void (*)(bool)>("MceSteamStub_SetLoggedOn")(false);
    gs::sign_in(true);
    REQUIRE(seen.n == 10);
    CHECK_FALSE(seen.ok[9]);

    gs::stop();
    steam::shutdown();
    CHECK(gs::backend() == gs::Backend::None);
    Answers after;
    CHECK_FALSE(gs::start(&collect, &after));
    gs::report_achievement("ACH_FIRST_WIN", 1, 1, 1);
    REQUIRE(after.n == 1);
    CHECK_FALSE(after.ok[0]);
    gs::stop();
}

TEST_CASE("steam: the overlay's state from its callback, its pages, and rich presence") {
    StubControls stub;
    CHECK_FALSE(steam::overlay_active());
    CHECK_FALSE(steam::set_rich_presence("status", "x"));  // Steam is off
    REQUIRE(steam::init(stub_config(480)) == steam::InitResult::Ok);
    const auto overlay = stub.get<const char* (*)()>("MceSteamStub_Overlay");
    const auto presence = stub.get<const char* (*)(const char*)>("MceSteamStub_Presence");
    const auto count = stub.get<int (*)()>("MceSteamStub_PresenceCount");

    const u8 open[12] = {1, 1};
    const u8 closed[12] = {0, 1};
    stub.push(331, open, sizeof(open));
    steam::run_callbacks(nullptr, nullptr);
    CHECK(steam::overlay_active());
    stub.push(331, closed, sizeof(closed));
    steam::run_callbacks(nullptr, nullptr);
    CHECK_FALSE(steam::overlay_active());

    steam::show_overlay("friends");
    CHECK(StringView(overlay()) == "friends");
    steam::show_store_page();
    CHECK(StringView(overlay()) == "store:480");
    steam::show_web_page("https://example.org/news");
    CHECK(StringView(overlay()) == "web:https://example.org/news");

    CHECK(steam::set_rich_presence("steam_display", "#StatusInLevel"));
    CHECK(steam::set_rich_presence("level", "Ashen Court"));
    CHECK(StringView(presence("level")) == "Ashen Court");
    CHECK(count() == 2);
    CHECK(steam::set_rich_presence("level", ""));  // removes the key
    CHECK(count() == 1);
    steam::clear_rich_presence();
    CHECK(count() == 0);

    stub.push(331, open, sizeof(open));
    steam::run_callbacks(nullptr, nullptr);
    steam::shutdown();
    CHECK_FALSE(steam::overlay_active());  // a new run starts with it closed
    REQUIRE(steam::init(stub_config(480)) == steam::InitResult::Ok);
    CHECK_FALSE(steam::overlay_active());
    steam::shutdown();
}

TEST_CASE("steam: Steam Input reads the actions of the set by name, and names their inputs") {
    StubControls stub;
    stub.get<void (*)()>("MceSteamStub_ResetInput")();
    const auto pads = stub.get<void (*)(int)>("MceSteamStub_SetPads");
    const auto hold = stub.get<void (*)(const char*, float, float)>("MceSteamStub_Hold");
    const auto release = stub.get<void (*)(const char*)>("MceSteamStub_Release");
    const auto active_set = stub.get<const char* (*)()>("MceSteamStub_ActiveSet");
    namespace sin = steam::input;
    CHECK_FALSE(sin::start());  // Steam is off
    REQUIRE(steam::init(stub_config(480)) == steam::InitResult::Ok);
    REQUIRE(sin::start());
    CHECK(sin::running());
    CHECK(stub.calls("SteamAPI_ISteamInput_Init") == 1);

    sin::Action actions[2];
    actions[0].name.assign("Jump");
    actions[1].name.assign("Move");
    actions[1].kind = sin::Kind::Analog;
    sin::Value values[2];
    // No controller: nothing read, no set activated.
    CHECK(sin::frame("Game", Span<const sin::Action>(actions, 2), Span<sin::Value>(values, 2)) == 0);
    CHECK_FALSE(values[0].active);
    CHECK(stub.calls("SteamAPI_ISteamInput_RunFrame") == 1);
    CHECK(stub.calls("SteamAPI_ISteamInput_ActivateActionSet") == 0);
    FixedString<64> name;
    FixedString<256> glyph;
    CHECK_FALSE(sin::origin("Game", actions[0], name, glyph));

    pads(2);
    hold("Jump", 1.f, 0.f);
    hold("Move", 0.5f, -1.f);
    CHECK(sin::frame("Game", Span<const sin::Action>(actions, 2), Span<sin::Value>(values, 2)) == 2);
    CHECK(sin::controllers() == 2);
    CHECK(StringView(active_set()) == "Game");
    CHECK(values[0].active);
    CHECK(values[0].x == 1.f);
    CHECK(values[1].active);
    CHECK(values[1].x == 0.5f);
    CHECK(values[1].y == -1.f);

    release("Jump");
    release("Move");
    sin::frame("Game", Span<const sin::Action>(actions, 2), Span<sin::Value>(values, 2));
    CHECK(values[0].active);  // bound, just not held
    CHECK(values[0].x == 0.f);
    CHECK(values[1].x == 0.f);

    REQUIRE(sin::origin("Game", actions[0], name, glyph));
    CHECK(name.view() == "Stub Jump");
    CHECK(glyph.view().size() > 0);

    sin::stop();
    CHECK_FALSE(sin::running());
    CHECK(stub.calls("SteamAPI_ISteamInput_Shutdown") == 1);
    CHECK(sin::frame("Game", Span<const sin::Action>(actions, 2), Span<sin::Value>(values, 2)) == 0);
    steam::shutdown();
}
