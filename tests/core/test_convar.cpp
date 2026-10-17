#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
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
MCE_CVAR(cv_test_saved_db, "test.saved_db", 0.0f, "saved float", CVarFlags::Saved);
MCE_CVAR(cv_test_saved_tier, "test.saved_tier", "auto", "saved string", CVarFlags::Saved | CVarFlags::RequiresRestart);
MCE_CVAR(cv_test_saved_flag, "test.saved_flag", true, "saved bool", CVarFlags::Saved);

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

TEST_CASE("command line: a negative number is a value, not the next flag") {
    const char* argv[] = {"exe", "-test.int", "-12", "-test.str", "-3.5,4,-.5", "-test.bool"};
    CHECK(console::parse_command_line(6, argv) == 0);
    CHECK(cv_test_int.get_int() == -12);
    CHECK(cv_test_str.get_string() == "-3.5,4,-.5");
    CHECK(cv_test_bool.get_bool());
    cv_test_int.reset();
    cv_test_str.reset();
    cv_test_bool.reset();
}

TEST_CASE("command line: unknown convars can wait for a late registration (#213)") {
    const char* argv[] = {"exe", "-test.int", "3", "-test.late", "9", "-test.late_flag", "-test.never", "1"};
    CHECK(console::parse_command_line(8, argv, /*defer_unknown=*/true) == 0);  // nothing reported yet
    CHECK(cv_test_int.get_int() == 3);
    {
        ConVar late("test.late", 0, "registered after the parse, like a game module's");
        ConVar late_flag("test.late_flag", false, "a bare flag");
        CHECK(console::apply_deferred_command_line() == 1);  // test.never is still unknown
        CHECK(late.get_int() == 9);
        CHECK(late_flag.get_bool());
    }
    CHECK(console::apply_deferred_command_line() == 0);  // applied once, then forgotten
    cv_test_int.reset();
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

namespace {
MCE_CVAR(cv_test_saved_f, "test.saved.f", 1.0f, "saved float", CVarFlags::Saved);
}  // namespace

TEST_CASE("M-ACCESS: a Saved convar change moves the settings generation") {
    const u64 gen = console::saved_generation();
    cv_test_saved_f.set_float(0.5f);
    CHECK(console::saved_generation() > gen);
    const u64 gen2 = console::saved_generation();
    cv_test_int.set_int(7);  // not Saved: never moves it
    CHECK(console::saved_generation() == gen2);
    cv_test_int.reset();
    cv_test_saved_f.reset();
    CHECK(cv_test_saved_f.is_default());
}

TEST_CASE("saved convars round-trip through the settings file") {
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/mce_test_settings.cfg";
    std::remove(path.c_str());
    {  // a file from an earlier run: a module's convar not registered here, and the three saved ones
        std::FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f);
        std::fputs("game.difficulty hard\ntest.saved_db -12\ntest.saved_tier low\ntest.saved_flag false\n", f);
        std::fclose(f);
    }
    REQUIRE(cv_test_saved_tier.set_string("high"));  // the command line said -test.saved_tier high: it wins
    REQUIRE(console::load_settings(StringView(path.c_str())));
    CHECK(cv_test_saved_db.get_float() == -12.0f);
    CHECK(cv_test_saved_tier.get_string() == "high");
    CHECK_FALSE(cv_test_saved_flag.get_bool());

    // The options screen turns the music down; the command-line tier stays out of the file.
    REQUIRE(cv_test_saved_db.set_float(-6.0f));
    REQUIRE(console::save_settings(StringView(path.c_str())));
    std::string text;
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        REQUIRE(f);
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f)) text += buf;
        std::fclose(f);
    }
    CHECK(text.find("game.difficulty hard\n") != std::string::npos);  // kept for the module that owns it
    CHECK(text.find("test.saved_db -6\n") != std::string::npos);
    CHECK(text.find("test.saved_tier low\n") != std::string::npos);  // not "high"
    CHECK(text.find("test.saved_flag false\n") != std::string::npos);
    std::remove(path.c_str());
    cv_test_saved_db.reset();
    cv_test_saved_tier.reset();
    cv_test_saved_flag.reset();
}

TEST_CASE("a missing settings file is no error") {
    CHECK(console::load_settings("/nonexistent-dir/settings.cfg"));
}

TEST_CASE("project defaults: a defaults file moves the default, a settings value and the command line still win (M-WORKFLOW 7)") {
    const std::string dir = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp";
    const std::string defaults = dir + "/mce_test_defaults.cfg";
    const std::string settings = dir + "/mce_test_defaults_settings.cfg";
    cv_test_saved_db.reset();  // earlier cases may leave a value
    cv_test_saved_tier.reset();
    cv_test_saved_flag.reset();
    std::remove(settings.c_str());
    {
        std::FILE* f = std::fopen(defaults.c_str(), "wb");
        REQUIRE(f);
        std::fputs("# a project's own defaults\ntest.saved_db -9\ntest.saved_tier medium\ntest.saved_flag false\ntest.nope 1\ntest.int oops\n", f);
        std::fclose(f);
    }
    {  // a player's saved choice, from an earlier run
        std::FILE* f = std::fopen(settings.c_str(), "wb");
        REQUIRE(f);
        std::fputs("test.saved_db -3\n", f);
        std::fclose(f);
    }
    REQUIRE(cv_test_saved_tier.set_string("high"));  // the command line said -test.saved_tier high
    REQUIRE(console::set_defaults(StringView(defaults.c_str())));  // unknown names and a bad value only warn
    CHECK(cv_test_int.get_int() == 3);
    CHECK(cv_test_saved_flag.get_bool() == false);  // untouched: takes the project's default
    CHECK(cv_test_saved_flag.is_default());
    CHECK(cv_test_saved_tier.get_string() == "high");  // the command line wins
    CHECK_FALSE(cv_test_saved_tier.is_default());

    REQUIRE(console::load_settings(StringView(settings.c_str())));
    CHECK(cv_test_saved_db.get_float() == -3.0f);  // the saved choice beats the project's -9
    cv_test_saved_db.reset();
    CHECK(cv_test_saved_db.get_float() == -9.0f);  // reset goes back to the project's default

    // Nothing the file set is written back as a player's choice.
    REQUIRE(console::save_settings(StringView(settings.c_str())));
    std::string text;
    {
        std::FILE* f = std::fopen(settings.c_str(), "rb");
        REQUIRE(f);
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f)) text += buf;
        std::fclose(f);
    }
    CHECK(text.find("test.saved_flag") == std::string::npos);
    CHECK(text.find("test.saved_tier") == std::string::npos);

    std::remove(defaults.c_str());
    std::remove(settings.c_str());
    cv_test_saved_tier.set_default("auto");
    cv_test_saved_flag.set_default("true");
    cv_test_saved_db.set_default("0");
    cv_test_saved_tier.reset();
    cv_test_saved_flag.reset();
    cv_test_saved_db.reset();
    CHECK(console::set_defaults("/nonexistent-dir/defaults.cfg"));  // a missing file is no error
}
