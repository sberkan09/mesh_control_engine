#include <doctest/doctest.h>

#include "mce/convar.h"
#include "mce/editor/console_model.h"

using namespace mce;
using namespace mce::editor;

namespace {

MCE_CVAR(cv_test_editor_value, "test_editor.value", 7, "Console model test convar", CVarFlags::None);
MCE_CVAR(cv_test_editor_valve, "test_editor.valve", false, "Console model test convar", CVarFlags::None);

i32 g_calls = 0;
void test_func(Span<const StringView>) { ++g_calls; }
MCE_CONFUNC(cf_test_editor_ping, "test_editor.ping", test_func, "Console model test confunc");

// The test main logs at Warn; the console model is about Info lines.
struct InfoLevel {
    InfoLevel() { log::set_level(log::Level::Info); }
    ~InfoLevel() { log::set_level(log::Level::Warn); }
};

bool contains_line(const ConsoleModel& m, StringView needle) {
    for (usize i = 0; i < m.size(); ++i)
        if (m.line(i).text.view().find(needle) != StringView::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("ConsoleModel drains new log lines once") {
    InfoLevel info;
    ConsoleModel m;
    m.poll();
    const usize before = m.size();
    MCE_LOG_INFO("console-model line A");
    MCE_LOG_WARN("console-model line B");
    CHECK(m.poll() == 2);
    CHECK(m.size() == before + 2);
    CHECK(m.line(m.size() - 1).level == log::Level::Warn);
    CHECK(m.line(m.size() - 2).text.view().find("line A") != StringView::npos);
    CHECK(m.poll() == 0);
}

TEST_CASE("ConsoleModel scrollback keeps the newest lines") {
    InfoLevel info;
    ConsoleModel m(4);
    m.poll();
    for (int i = 0; i < 10; ++i) MCE_LOG_INFO("scroll {}", i);
    m.poll();
    REQUIRE(m.size() == 4);
    CHECK(m.line(0).text.view().find("scroll 6") != StringView::npos);
    CHECK(m.line(3).text.view().find("scroll 9") != StringView::npos);
    const u64 rev = m.revision();
    m.clear();
    CHECK(m.size() == 0);
    CHECK(m.revision() != rev);
}

TEST_CASE("ConsoleModel exec sets convars, calls confuncs, rejects unknown names") {
    InfoLevel info;
    ConsoleModel m;
    CHECK(m.exec("test_editor.value 42"));
    CHECK(cv_test_editor_value.get_int() == 42);
    const i32 calls = g_calls;
    CHECK(m.exec("  test_editor.ping  "));
    CHECK(g_calls == calls + 1);
    CHECK_FALSE(m.exec("test_editor.no_such_thing 1"));
    CHECK_FALSE(m.exec("   "));
    m.poll();
    CHECK(contains_line(m, "> test_editor.value 42"));
    cv_test_editor_value.reset();
}

TEST_CASE("ConsoleModel history browses back and forward") {
    ConsoleModel m;
    m.exec("test_editor.value 1");
    m.exec("test_editor.value 2");
    m.exec("test_editor.value 2");  // repeated entries collapse
    CHECK(m.history_size() == 2);
    CHECK(m.browse_history(-1) == "test_editor.value 2");
    CHECK(m.browse_history(-1) == "test_editor.value 1");
    CHECK(m.browse_history(-1) == "test_editor.value 1");  // clamps at the oldest
    CHECK(m.browse_history(1) == "test_editor.value 2");
    CHECK(m.browse_history(1).empty());  // past the newest: back to an empty line
    cv_test_editor_value.reset();
}

TEST_CASE("ConsoleModel completes convar and confunc names") {
    ConsoleModel m;
    Array<const char*> names;
    CHECK(m.complete("test_editor.", names) == 3);
    CHECK(StringView(names[0]) == "test_editor.ping");
    CHECK(StringView(names[1]) == "test_editor.value");
    CHECK(StringView(names[2]) == "test_editor.valve");
    CHECK(m.complete_common("test_editor.va").view() == "test_editor.val");
    CHECK(m.complete_common("test_editor.p").view() == "test_editor.ping");
    CHECK(m.complete_common("zzz_nothing").view() == "zzz_nothing");
}

TEST_CASE("find_script_location reads the .luau path and line out of an error line") {
    ScriptLocation loc;
    REQUIRE(find_script_location("script failed: scripts/push.luau:3: boom", loc));
    CHECK(loc.path == "scripts/push.luau");
    CHECK(loc.line == 3);
    REQUIRE(find_script_location("error: assets/scripts/a.luau:2:13: unknown component 'Movr'", loc));
    CHECK(loc.path == "assets/scripts/a.luau");
    CHECK(loc.line == 2);
    REQUIRE(find_script_location("script failed: \"ui/hud.luau:12: x\"", loc));
    CHECK(loc.path == "ui/hud.luau");
    CHECK_FALSE(find_script_location("script failed: not enough memory", loc));
    CHECK_FALSE(find_script_location("see .luau:5 here", loc));
    CHECK_FALSE(find_script_location("a.luau: no line", loc));
}
