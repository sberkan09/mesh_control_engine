#include <doctest/doctest.h>

#include "mce/containers/array.h"
#include "mce/state_stack.h"

using namespace mce;

namespace {
Array<const char*>* g_log = nullptr;

struct State {
    const char* name;
    explicit State(const char* n) : name(n) {}
    virtual ~State() = default;
    void on_enter() { g_log->push_back(name); }
    void on_exit() { g_log->push_back("exit"); }
    void on_pause() { g_log->push_back("pause"); }
    void on_resume() { g_log->push_back("resume"); }
};
}  // namespace

TEST_CASE("StateStack defers transitions to apply()") {
    Array<const char*> log;
    g_log = &log;
    {
        StateStack<State> stack;
        stack.push(mem::make_unique<State>(mem::heap(), "boot"));
        CHECK(stack.top() == nullptr);  // deferred
        CHECK(stack.apply() == 1);
        CHECK(StringView(stack.top()->name) == "boot");

        stack.replace(mem::make_unique<State>(mem::heap(), "game"));
        stack.push(mem::make_unique<State>(mem::heap(), "pause"));
        stack.apply();
        CHECK(stack.depth() == 2);
        CHECK(StringView(stack.top()->name) == "pause");

        stack.pop();
        stack.apply();
        CHECK(StringView(stack.top()->name) == "game");
    }
    const char* expected[] = {"boot", "exit", "game", "pause", "pause", "exit", "resume", "exit"};
    REQUIRE(log.size() == 8);
    for (usize i = 0; i < 8; ++i) CHECK(StringView(log[i]) == StringView(expected[i]));
    g_log = nullptr;
}

TEST_CASE("StateStack has_pending is true from a queued op until apply() runs it") {
    Array<const char*> log;
    g_log = &log;
    StateStack<State> stack;
    CHECK_FALSE(stack.has_pending());
    stack.push(mem::make_unique<State>(mem::heap(), "boot"));
    CHECK(stack.has_pending());
    stack.apply();
    CHECK_FALSE(stack.has_pending());
    stack.pop();
    CHECK(stack.has_pending());
    stack.clear();  // drops the queued op and exits what is left
    CHECK_FALSE(stack.has_pending());
    CHECK(stack.depth() == 0);
}
