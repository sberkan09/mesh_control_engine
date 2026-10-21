#include <doctest/doctest.h>

#include "mce/crash.h"
#include "mce/log.h"

#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#endif

using namespace mce;

TEST_CASE("crash: default dump dir is per user") {
    const FixedString<512> d = crash::default_dump_dir("Game");
    CHECK(d.view().ends_with("/MCE/Game/crashes"));
}

#if !defined(_WIN32) && !defined(__SANITIZE_ADDRESS__)
TEST_CASE("crash: a fatal signal writes a report and still dies of it") {
    namespace fs = std::filesystem;
    char tmpl[] = "/tmp/mce_crash_XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    const fs::path dir = fs::path(tmpl) / "nested" / "crashes";

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        log::LogDesc ld;
        ld.console = false;
        log::init(ld);
        MCE_LOG_WARN("crumb before the crash");
        crash::CrashDesc cd;
        cd.app_name = "crashy";
        cd.version = "1.2.3 test";
        cd.dump_dir = dir.c_str();
        if (!crash::install(cd) || !crash::installed()) _exit(3);
        crash::trigger_test_crash();
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);

    std::string report;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("crashy-", 0) != 0 || e.path().extension() != ".crash") continue;
        std::ifstream f(e.path());
        std::stringstream ss;
        ss << f.rdbuf();
        report = ss.str();
    }
    REQUIRE_FALSE(report.empty());
    CHECK(report.rfind("MCE crash report v1\napp crashy\nversion 1.2.3 test\n", 0) == 0);
    CHECK(report.find("signal 11 (SIGSEGV)") != std::string::npos);
    CHECK(report.find("addr 0x0") != std::string::npos);
    CHECK(report.find("\n#0 0x") != std::string::npos);
    CHECK(report.find("build_id ") != std::string::npos);
    CHECK(report.find("crumb before the crash") != std::string::npos);
    CHECK(report.find("-crash_test requested") != std::string::npos);
    fs::remove_all(tmpl);
}
namespace {
void vendor_handler(int) { _exit(42); }  // a crash vendor's handler, installed before the engine's
}  // namespace

TEST_CASE("crash: a handler installed before the engine's still gets the crash") {
    namespace fs = std::filesystem;
    char tmpl[] = "/tmp/mce_crash_XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    const fs::path dir = fs::path(tmpl) / "crashes";

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        struct sigaction sa {};
        sa.sa_handler = &vendor_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
        crash::CrashDesc cd;
        cd.app_name = "chained";
        cd.version = "1";
        cd.dump_dir = dir.c_str();
        if (!crash::install(cd)) _exit(3);
        crash::trigger_test_crash();
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 42);  // the earlier handler ran ...
    bool report = false;               // ... after the engine wrote its report
    for (const auto& e : fs::directory_iterator(dir))
        report = report || (e.path().filename().string().rfind("chained-", 0) == 0 && e.path().extension() == ".crash");
    CHECK(report);
    fs::remove_all(tmpl);
}
#endif
