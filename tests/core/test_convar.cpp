#include <doctest/doctest.h>

#include <cstdio>
#include <string>

#include "mce/convar.h"

using namespace mce;

namespace {
MCE_CVAR(cv_test_int, "test.int", 3, "test int", CVarFlags::None);
MCE_CVAR(cv_test_float, "test.float", 0.5f, "test float", CVarFlags::None);
MCE_CVAR(cv_test_bool, "test.bool", false, "test bool", CVarFlags::None);
MCE_CVAR(cv_test_str, "test.str", "hello", "test string", CVarFlags::None);
MCE_CVAR(cv_test_ro, "test.readonly", 1, "read-only", CVarFlags::ReadOnly);
MCE_CVAR(cv_test_cheat, "test.cheat", 1.0f, "cheat", CVarFlags::Cheat);

int g_calls = 0;
StringView g_last_arg;
void test_fn(Span<const StringView> args) {
    ++g_calls;
    g_last_arg = args.empty() ? StringView() : args[0];
}
MCE_CONFUNC(cf_test, "test.fn", &test_fn, "test confunc");

int g_changes = 0;
}  // namespace

TEST_CASE("convars register and are found by name") {
    CHECK(ConVar::find("test.int") == &cv_test_int);
    CHECK(ConVar::find("time_scale") != nullptr);  // core convar from app.cpp
    CHECK(ConVar::find("nope") == nullptr);
    Array<ConVar*> all;
    ConVar::collect(all);
    for (usize i = 1; i < all.size(); ++i) CHECK(StringView(all[i - 1]->name()) < StringView(all[i]->name()));
}

TEST_CASE("typed get/set and parsing") {
    cv_test_int.reset();
    CHECK(cv_test_int.get_int() == 3);
    CHECK(cv_test_int.set_from_string("42"));
    CHECK(cv_test_int.get_int() == 42);
    CHECK_FALSE(cv_test_int.set_from_string("4x"));
    CHECK(cv_test_float.set_from_string("2.25"));
    CHECK(cv_test_float.get_float() == 2.25f);
    CHECK(cv_test_bool.set_from_string("on"));
    CHECK(cv_test_bool.get_bool());
    CHECK(cv_test_str.set_from_string("world"));
    CHECK(cv_test_str.get_string() == "world");
    FixedString<128> s;
    cv_test_int.to_string(s);
    CHECK(s == "42");
    cv_test_int.reset();
    cv_test_str.reset();
    CHECK(cv_test_str.get_string() == "hello");
}

TEST_CASE("string convars hold long paths and refuse what does not fit") {
    const std::string path = "/Users/someone/Projects/a/deeply/nested/folder/" + std::string(200, 'x') + "/libgame.so";
    CHECK(cv_test_str.set_string(StringView(path.c_str()), false));
    CHECK(cv_test_str.get_string() == StringView(path.c_str()));
    const std::string too_long(2000, 'y');
    CHECK_FALSE(cv_test_str.set_string(StringView(too_long.c_str()), false));
    CHECK(cv_test_str.get_string() == StringView(path.c_str()));  // unchanged, never a cut-off value
    cv_test_str.reset();
    CHECK(cv_test_str.get_string() == "hello");
}

TEST_CASE("read-only convars reject sets unless forced") {
    CHECK_FALSE(cv_test_ro.set_int(5));
    CHECK(cv_test_ro.get_int() == 1);
    CHECK(cv_test_ro.set_int(5, true));
    CHECK(cv_test_ro.get_int() == 5);
    cv_test_ro.reset();
}

TEST_CASE("change callbacks fire") {
    g_changes = 0;
    cv_test_int.set_on_change([](ConVar&) { ++g_changes; });
    cv_test_int.set_int(9);
    CHECK(g_changes == 1);
    cv_test_int.set_on_change(nullptr);
    cv_test_int.reset();
}

TEST_CASE("console execute") {
    CHECK(console::execute("test.int 17"));
    CHECK(cv_test_int.get_int() == 17);
    CHECK(console::execute("test.str \"two words\""));
    CHECK(cv_test_str.get_string() == "two words");
    g_calls = 0;
    CHECK(console::execute("test.fn abc"));
    CHECK(g_calls == 1);
    CHECK(g_last_arg == "abc");
    CHECK_FALSE(console::execute("does.not.exist 1"));
    cv_test_int.reset();
    cv_test_str.reset();
}

TEST_CASE("command line parsing") {
    g_calls = 0;
    const char* argv[] = {"exe", "-test.int", "8", "-test.bool", "+test.fn", "x", "-test.readonly", "7", "-unknown"};
    const u32 unknown = console::parse_command_line(9, argv);
    CHECK(unknown == 1);
    CHECK(cv_test_int.get_int() == 8);
    CHECK(cv_test_bool.get_bool());
    CHECK(g_calls == 1);
    CHECK(cv_test_ro.get_int() == 7);  // command line may set read-only (boot-time config)
    cv_test_int.reset();
    cv_test_bool.reset();
    cv_test_ro.reset();
}

TEST_CASE("command line -help") {
    CHECK_FALSE(console::help_requested());
    const char* argv[] = {"exe", "--help"};
    CHECK(console::parse_command_line(2, argv) == 0);
    CHECK(console::help_requested());
    ConVar::find(StringId("help"))->reset();
    const char* argv2[] = {"exe", "-h"};
    CHECK(console::parse_command_line(2, argv2) == 0);
    CHECK(console::help_requested());
    ConVar::find(StringId("help"))->reset();
    CHECK_FALSE(console::help_requested());
    // The reference names every convar (the test's own among them) with its help text.
    std::FILE* f = std::tmpfile();
    REQUIRE(f != nullptr);
    console::print_help("exe", "usage: exe [options]", f);
    REQUIRE(std::fseek(f, 0, SEEK_SET) == 0);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f)) text += buf;
    std::fclose(f);
    CHECK(text.find("usage: exe [options]") != std::string::npos);
    CHECK(text.find("-test.int <int>") != std::string::npos);
    CHECK(text.find("-help <bool>") != std::string::npos);
    CHECK(text.find("+test.fn") != std::string::npos);
}

TEST_CASE("cheat lock (shipped builds)") {
    REQUIRE(console::cheats_allowed());
    CHECK(console::execute("test.cheat 4"));
    CHECK(cv_test_cheat.get_float() == 4.0f);

    console::set_cheats_allowed(false);  // locking resets every cheat convar
    CHECK(cv_test_cheat.get_float() == 1.0f);
    CHECK_FALSE(console::execute("test.cheat 9"));
    CHECK(cv_test_cheat.get_float() == 1.0f);
    CHECK(console::execute("test.cheat"));  // reading is still fine
    CHECK(console::execute("test.int 5"));  // and so are ordinary convars
    const char* argv[] = {"exe", "-test.cheat", "7", "-test.bool"};
    CHECK(console::parse_command_line(4, argv) == 1);
    CHECK(cv_test_cheat.get_float() == 1.0f);
    CHECK(cv_test_bool.get_bool());       // the token after the refused value still parses
    CHECK(cv_test_cheat.set_float(2.0f));  // code may still set it
    cv_test_cheat.reset();

    console::set_cheats_allowed(true);
    CHECK(console::execute("test.cheat 3"));
    CHECK(cv_test_cheat.get_float() == 3.0f);
    cv_test_cheat.reset();
    cv_test_int.reset();
    cv_test_bool.reset();
}
