#include "mce/liveops/crash_upload.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "mce/liveops/consent.h"
#include "mce/log.h"

namespace mce::liveops {
namespace {

namespace stdfs = std::filesystem;
stdfs::path to_path(StringView s) { return stdfs::path(s.begin(), s.end()); }

bool read_report(const stdfs::path& p, Array<byte>& out) {
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return false;
    byte buf[4096];
    usize n = 0;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        if (out.size() + n > CrashReports::kMaxReportBytes) {
            ok = false;
            break;
        }
        for (usize i = 0; i < n; ++i) out.push_back(buf[i]);
    }
    std::fclose(f);
    return ok;
}

}  // namespace

u32 CrashReports::scan(StringView dir) {
    dir_.assign(dir);
    files_.clear();
    next_ = 0;
    stopped_ = false;
    std::error_code ec;
    Array<FixedString<160>> sent;
    for (const stdfs::directory_entry& e : stdfs::directory_iterator(to_path(dir), ec)) {
        if (!e.is_regular_file(ec)) continue;
        const auto name = e.path().filename().string();  // mce-allow: std::filesystem's name type
        const StringView n(name.data(), name.size());
        if (n.size() >= 160) continue;
        if (n.ends_with(".crash"))
            files_.push_back(FixedString<160>(n));
        else if (n.ends_with(".crash.sent"))
            sent.push_back(FixedString<160>(n));
    }
    auto by_name = [](const FixedString<160>& a, const FixedString<160>& b) { return a.view() < b.view(); };
    std::sort(files_.begin(), files_.end(), by_name);
    std::sort(sent.begin(), sent.end(), by_name);
    for (usize i = 0; i + kKeepSent < sent.size(); ++i) stdfs::remove(to_path(dir) / to_path(sent[i].view()), ec);
    return pending();
}

void CrashReports::update() {
    if (in_flight_) {
        const u8 a = answer_.exchange(0, std::memory_order_acq_rel);
        if (a != 0) finish_current(a == 1);
        return;
    }
    if (stopped_ || !uploader_ || next_ >= files_.size() || consent() != Consent::Granted) return;
    const StringView name = files_[next_].view();
    Array<byte> report;
    const stdfs::path path = to_path(dir_.view()) / to_path(name);
    if (!read_report(path, report)) {
        MCE_LOG_WARN("liveops: crash report {} cannot be read or is too large; skipped", name);
        ++next_;
        ++failed_;
        return;
    }
    in_flight_ = true;
    answer_.store(0, std::memory_order_release);
    uploader_->upload(name, report.span(), *this);
    const u8 a = answer_.exchange(0, std::memory_order_acq_rel);  // an uploader may answer at once
    if (a != 0) finish_current(a == 1);
}

void CrashReports::upload_done(bool ok) { answer_.store(ok ? 1 : 2, std::memory_order_release); }

void CrashReports::finish_current(bool ok) {
    in_flight_ = false;
    const StringView name = files_[next_].view();
    if (!ok) {
        MCE_LOG_WARN("liveops: crash report {} was not sent; trying again on the next launch", name);
        ++failed_;
        stopped_ = true;  // offline or refused: the rest wait too
        return;
    }
    std::error_code ec;
    const stdfs::path from = to_path(dir_.view()) / to_path(name);
    FixedString<192> sent_name(name);
    sent_name.append(".sent");
    stdfs::rename(from, to_path(dir_.view()) / to_path(sent_name.view()), ec);
    if (ec) stdfs::remove(from, ec);  // never send it twice
    MCE_LOG_INFO("liveops: crash report {} sent", name);
    ++sent_;
    ++next_;
}

void OutboxUploader::upload(StringView file_name, Span<const byte> report, CrashUploadSink& sink) {
    std::error_code ec;
    const stdfs::path dir = to_path(dir_.view());
    stdfs::create_directories(dir, ec);
    const stdfs::path out = dir / to_path(file_name);
    std::FILE* f = std::fopen(out.string().c_str(), "wb");
    bool ok = f != nullptr;
    if (f) {
        ok = std::fwrite(report.data(), 1, report.size(), f) == report.size();
        ok = std::fclose(f) == 0 && ok;
    }
    sink.upload_done(ok);
}

}  // namespace mce::liveops
