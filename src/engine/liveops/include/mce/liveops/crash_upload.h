#pragma once
// Crash reports sent on the next launch (M-LIVEOPS step 1). mce/crash.h writes
// `<dump_dir>/<app>-<stamp>-<pid>.crash` when the game dies; on the next launch CrashReports lists
// the ones not sent yet and, once consent is Granted, hands them one at a time to an uploader (a
// vendor, or OutboxUploader's local folder). A sent report is renamed `.crash.sent`; the newest
// kKeepSent of those are kept for the player's own support requests, older ones deleted. A report
// that fails to upload stays and is tried again on the next launch.
//
// Reports name every module by build id, never carry symbols, and are symbolicated off-device
// against the symbol archive ship.py keeps (scripts/symbolicate.py, or the vendor with the same
// symbols uploaded).

#include <atomic>

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/types.h"

namespace mce::liveops {

// The uploader's answer, from any thread.
class CrashUploadSink {
public:
    virtual void upload_done(bool ok) = 0;

protected:
    ~CrashUploadSink() = default;
};

class ICrashUploader {
public:
    virtual ~ICrashUploader() = default;
    virtual const char* name() const = 0;
    // Sends one report (the .crash file's text, `file_name` its base name); answers through the
    // sink exactly once, now or later. Main thread.
    virtual void upload(StringView file_name, Span<const byte> report, CrashUploadSink& sink) = 0;
};

class CrashReports final : public CrashUploadSink {
public:
    static constexpr u32 kKeepSent = 10;
    static constexpr usize kMaxReportBytes = 1u << 20;  // a larger file is not a report crash.cpp wrote

    // Lists `dir`'s unsent *.crash files, oldest name first (names carry a UTC stamp), and prunes
    // old .sent ones. Returns how many wait.
    u32 scan(StringView dir);
    void set_uploader(ICrashUploader* u) { uploader_ = u; }
    // Main thread: with consent Granted and an uploader, starts the next upload when none is in flight.
    void update();

    u32 pending() const { return static_cast<u32>(files_.size()) - next_; }
    u32 sent() const { return sent_; }
    u32 failed() const { return failed_; }
    bool busy() const { return in_flight_; }
    StringView dir() const { return dir_.view(); }

    void upload_done(bool ok) override;

private:
    void finish_current(bool ok);

    ICrashUploader* uploader_ = nullptr;
    FixedString<512> dir_;
    Array<FixedString<160>> files_;  // base names, sorted
    u32 next_ = 0;                   // files_[next_] is the next to send
    u32 sent_ = 0;
    u32 failed_ = 0;
    bool in_flight_ = false;
    bool stopped_ = false;       // an upload failed: the rest wait for the next launch
    std::atomic<u8> answer_{0};  // 0 none, 1 ok, 2 failed (upload_done from any thread)
};

// Copies each report into a folder: the local uploader (desktop, CI, a studio's own collection).
class OutboxUploader final : public ICrashUploader {
public:
    explicit OutboxUploader(StringView dir) : dir_(dir) {}
    const char* name() const override { return "outbox"; }
    void upload(StringView file_name, Span<const byte> report, CrashUploadSink& sink) override;
    StringView dir() const { return dir_.view(); }

private:
    FixedString<512> dir_;
};

}  // namespace mce::liveops
