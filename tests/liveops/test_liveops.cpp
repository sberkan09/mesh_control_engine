// M-LIVEOPS step 1: the consent gate, analytics events (validation, queueing, the file backend) and
// crash reports sent on the next launch (scan, consent, rename to .sent, retry after a failure).

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "mce/liveops/liveops.h"

using namespace mce;
using namespace mce::liveops;
namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const char* name) {
    const fs::path p = fs::path(MCE_TEST_TMP_DIR) / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

void write_text(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Recorder final : analytics::IBackend {
    std::string log;
    Consent last = Consent::Unknown;
    const char* name() const override { return "recorder"; }
    void send(const analytics::Event& e) override {
        log += std::string(e.name.view());
        for (const analytics::Param& p : e.params.params()) log += " " + std::string(p.key.view());
        log += ";";
    }
    void set_consent(Consent c) override { last = c; }
};

struct ScriptedUploader final : ICrashUploader {
    bool answer_now = true;
    bool ok = true;
    CrashUploadSink* pending = nullptr;
    std::string got;
    const char* name() const override { return "scripted"; }
    void upload(StringView file_name, Span<const byte> report, CrashUploadSink& sink) override {
        got += std::string(file_name) + "=" + std::string(reinterpret_cast<const char*>(report.data()), report.size()) +
               ";";
        if (answer_now)
            sink.upload_done(ok);
        else
            pending = &sink;
    }
};

}  // namespace

TEST_CASE("analytics: names and params follow the vendors' rules") {
    CHECK(analytics::valid_name("level_end"));
    CHECK(analytics::valid_name("a"));
    CHECK(analytics::valid_name(std::string(40, 'a').c_str()));
    CHECK_FALSE(analytics::valid_name(std::string(41, 'a').c_str()));
    CHECK_FALSE(analytics::valid_name(""));
    CHECK_FALSE(analytics::valid_name("1st"));
    CHECK_FALSE(analytics::valid_name("_x"));
    CHECK_FALSE(analytics::valid_name("level end"));
    CHECK_FALSE(analytics::valid_name("firebase_x"));
    CHECK_FALSE(analytics::valid_name("ga_x"));

    analytics::reset();
    set_consent(Consent::Unknown);
    CHECK(analytics::event("ok", analytics::Params().add("level", 3).add("t", 1.5).add("who", "mira")));
    CHECK_FALSE(analytics::event("bad name"));
    CHECK_FALSE(analytics::event("dup", analytics::Params().add("a", 1).add("a", 2)));
    CHECK_FALSE(analytics::event("long", analytics::Params().add("s", std::string(101, 'x').c_str())));
    CHECK(analytics::event("edge", analytics::Params().add("s", std::string(100, 'x').c_str())));
    analytics::Params many;
    for (int i = 0; i < 26; ++i) many.add((std::string("p") + std::to_string(i)).c_str(), i);
    CHECK(many.bad());
    CHECK_FALSE(analytics::event("many", many));
    const analytics::Stats st = analytics::stats();
    CHECK(st.queued == 2);
    CHECK(st.rejected == 4);
    analytics::reset();
}

TEST_CASE("analytics: consent decides delivery") {
    analytics::reset();
    Recorder rec;
    set_consent(Consent::Unknown);
    analytics::set_backend(&rec);
    analytics::event("a");
    analytics::event("b", analytics::Params().add("x", 1));
    analytics::update();
    CHECK(rec.log.empty());  // unknown: held
    CHECK(analytics::stats().queued == 2);

    set_consent(Consent::Granted);
    analytics::update();
    CHECK(rec.log == "a;b x;");
    CHECK(rec.last == Consent::Granted);
    CHECK(analytics::stats().sent == 2);

    set_consent(Consent::Unknown);
    analytics::event("c");
    set_consent(Consent::Denied);  // a held event is dropped, a new one never queued
    analytics::update();
    analytics::event("d");
    analytics::update();
    CHECK(rec.log == "a;b x;");
    CHECK(rec.last == Consent::Denied);
    CHECK(analytics::stats().dropped == 2);
    CHECK(analytics::stats().queued == 0);

    // The queue is bounded: the oldest go first.
    set_consent(Consent::Unknown);
    analytics::reset();
    for (u32 i = 0; i < analytics::kMaxQueued + 5; ++i) analytics::event("e");
    CHECK(analytics::stats().queued == analytics::kMaxQueued);
    CHECK(analytics::stats().dropped == 5);

    analytics::set_backend(nullptr);
    set_consent(Consent::Granted);
    analytics::update();  // no backend: dropped
    CHECK(analytics::stats().queued == 0);
    set_consent(Consent::Unknown);
    analytics::reset();
}

TEST_CASE("analytics: the file backend writes one JSON object per line") {
    const fs::path dir = fresh_dir("analytics_file");
    const std::string path = (dir / "events.jsonl").string();
    analytics::reset();
    {
        analytics::FileBackend file(StringView(path.c_str()));
        analytics::set_backend(&file);
        set_consent(Consent::Granted);
        analytics::event("level_end", analytics::Params().add("level", 3).add("time_s", 41.5).add("note", "a \"q\"\n"));
        analytics::event("quit");
        analytics::update();
        analytics::set_backend(nullptr);
    }
    const std::string text = read_text(path);
    CHECK(text.find("\"name\":\"level_end\",\"params\":{\"level\":3,\"time_s\":41.5,\"note\":\"a \\\"q\\\"\\n\"}}\n") !=
          std::string::npos);
    CHECK(text.find("\"name\":\"quit\",\"params\":{}}\n") != std::string::npos);
    CHECK(text.rfind("{\"t\":", 0) == 0);
    set_consent(Consent::Unknown);
    analytics::reset();
}

TEST_CASE("crash reports: sent on the next launch once consent allows, then kept as .sent") {
    const fs::path dir = fresh_dir("crash_upload");
    write_text(dir / "game-20261014T050000Z-11.crash", "first");
    write_text(dir / "game-20261013T050000Z-10.crash", "older");
    write_text(dir / "notes.txt", "not a report");
    for (int i = 0; i < 12; ++i)
        write_text(dir / ("game-2026100" + std::to_string(i % 10) + "T0" + std::to_string(i) + ".crash.sent"), "x");

    set_consent(Consent::Unknown);
    ScriptedUploader up;
    CrashReports reports;
    CHECK(reports.scan(StringView(dir.string().c_str())) == 2);
    u32 sent_left = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(dir))
        if (e.path().string().ends_with(".sent")) ++sent_left;
    CHECK(sent_left == CrashReports::kKeepSent);  // old .sent pruned

    reports.set_uploader(&up);
    reports.update();
    CHECK(up.got.empty());  // no consent yet
    set_consent(Consent::Denied);
    reports.update();
    CHECK(up.got.empty());

    set_consent(Consent::Granted);
    reports.update();
    reports.update();
    CHECK(up.got == "game-20261013T050000Z-10.crash=older;game-20261014T050000Z-11.crash=first;");
    CHECK(reports.sent() == 2);
    CHECK(reports.pending() == 0);
    CHECK(fs::exists(dir / "game-20261013T050000Z-10.crash.sent"));
    CHECK_FALSE(fs::exists(dir / "game-20261014T050000Z-11.crash"));

    CrashReports relaunch;
    CHECK(relaunch.scan(StringView(dir.string().c_str())) == 0);  // nothing is sent twice
    set_consent(Consent::Unknown);
}

TEST_CASE("crash reports: a failed upload waits for the next launch; answers may come later") {
    const fs::path dir = fresh_dir("crash_retry");
    write_text(dir / "a.crash", "A");
    write_text(dir / "b.crash", "B");
    set_consent(Consent::Granted);

    ScriptedUploader up;
    up.ok = false;
    {
        CrashReports reports;
        reports.scan(StringView(dir.string().c_str()));
        reports.set_uploader(&up);
        reports.update();
        reports.update();
        CHECK(up.got == "a.crash=A;");  // stopped after the failure
        CHECK(reports.failed() == 1);
        CHECK(fs::exists(dir / "a.crash"));
    }

    up = ScriptedUploader{};
    up.answer_now = false;
    CrashReports reports;
    CHECK(reports.scan(StringView(dir.string().c_str())) == 2);
    reports.set_uploader(&up);
    reports.update();
    CHECK(reports.busy());
    reports.update();  // still in flight: nothing new starts
    CHECK(up.got == "a.crash=A;");
    REQUIRE(up.pending != nullptr);
    up.pending->upload_done(true);
    reports.update();  // collects the answer
    CHECK(reports.sent() == 1);
    reports.update();
    CHECK(up.got == "a.crash=A;b.crash=B;");
    up.pending->upload_done(true);
    reports.update();
    CHECK(reports.sent() == 2);
    set_consent(Consent::Unknown);
}

TEST_CASE("service: the outbox uploader and the file backend end to end") {
    const fs::path dir = fresh_dir("service");
    fs::create_directories(dir / "crashes");
    write_text(dir / "crashes" / "game-1.crash", "report text");
    const std::string events = (dir / "events.jsonl").string();
    analytics::reset();
    set_consent(Consent::Unknown);

    analytics::FileBackend file(StringView(events.c_str()));
    OutboxUploader outbox(StringView((dir / "outbox").string().c_str()));
    Service svc;
    ServiceDesc d;
    d.analytics = &file;
    d.crash_uploader = &outbox;
    d.crash_dir.assign(StringView((dir / "crashes").string().c_str()));
    svc.init(d);
    CHECK(svc.crashes().pending() == 1);
    analytics::event("session_start");
    svc.update();
    CHECK_FALSE(fs::exists(dir / "outbox" / "game-1.crash"));
    CHECK_FALSE(fs::exists(events));

    set_consent(Consent::Granted);
    svc.update();
    CHECK(read_text(dir / "outbox" / "game-1.crash") == "report text");
    CHECK(fs::exists(dir / "crashes" / "game-1.crash.sent"));
    svc.shutdown();
    CHECK(analytics::backend() == nullptr);
    CHECK(read_text(events).find("session_start") != std::string::npos);
    set_consent(Consent::Unknown);
    analytics::reset();
}
