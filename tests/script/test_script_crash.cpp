// M-SCRIPT 4b: a crash while a script runs says which script and where in it. A script spins; the
// child process aborts from a timer, and the crash report names the script and its Luau stack.

#include <doctest/doctest.h>

#if !defined(_WIN32) && !defined(__SANITIZE_ADDRESS__)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "mce/crash.h"
#include "mce/log.h"
#include "mce/script/compiler.h"

using namespace mce;

TEST_CASE("script crash: the report names the running script and its Luau stack") {
    namespace fs = std::filesystem;
    char tmpl[] = "/tmp/mce_script_crash_XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    const fs::path dir = fs::path(tmpl) / "crashes";

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        log::LogDesc ld;
        ld.console = false;
        log::init(ld);
        signal(SIGABRT, SIG_DFL);  // not doctest's handler: the report is the engine's
        crash::CrashDesc cd;
        cd.app_name = "scriptcrash";
        cd.dump_dir = dir.c_str();
        if (!crash::install(cd)) _exit(3);
        script::ScriptBudget budget;
        budget.steps_per_tick = 0xFFFFFFFFu;  // the timer ends the run, not the budget
        script::ScriptHost host(budget);
        if (!script::add_source(host, "scripts/spin.luau", "local function spin()\n    while true do end\nend\nfunction update()\n    spin()\nend\n"))
            _exit(4);
        World w;
        host.install(w);
        signal(SIGALRM, [](int) { abort(); });
        alarm(1);
        w.tick(1.0 / 60, 1);
        _exit(5);
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);

    std::string report;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".crash") continue;
        std::ifstream f(e.path());
        std::stringstream ss;
        ss << f.rdbuf();
        report = ss.str();
    }
    INFO(report);
    REQUIRE_FALSE(report.empty());
    CHECK(report.find("context:\n  luau: update() of script scripts/spin.luau\n") != std::string::npos);
    CHECK(report.find("    at scripts/spin.luau:2 in spin\n") != std::string::npos);
    CHECK(report.find("    at scripts/spin.luau:5 in update\n") != std::string::npos);
    fs::remove_all(tmpl);
}
#endif
