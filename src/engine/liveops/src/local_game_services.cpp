// LocalGameServices: the folder that stands in for Game Center / Play Games on desktop and in CI.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "mce/liveops/game_services.h"
#include "mce/log.h"

namespace mce::liveops {

namespace {

namespace stdfs = std::filesystem;
stdfs::path to_path(StringView s) { return stdfs::path(s.begin(), s.end()); }

}  // namespace

void LocalGameServices::start(const ServicesCatalog& catalog, GameServicesSink& sink) {
    catalog_ = &catalog;
    sink_ = &sink;
    std::error_code ec;
    stdfs::create_directories(to_path(dir_.view()) / "cloud", ec);
}

void LocalGameServices::sign_in(bool /*interactive*/) { sink_->signed_in(!offline_); }

void LocalGameServices::log_line(StringView json) {
    FixedString<512> path(dir_.view());
    path.append("/game_services.jsonl");
    std::FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) {
        MCE_LOG_WARN("game_services: cannot write {}", path.view());
        return;
    }
    std::fwrite(json.data(), 1, json.size(), f);
    std::fputc('\n', f);
    std::fclose(f);
}

void LocalGameServices::report_achievement(u16 achievement, u32 steps) {
    if (offline_) {
        sink_->achievement_sent(achievement, false);
        return;
    }
    char line[160];
    std::snprintf(line, sizeof(line), "{\"achievement\":\"%s\",\"steps\":%u}",
                  catalog_->achievements()[achievement].id.c_str(), steps);
    log_line(line);
    sink_->achievement_sent(achievement, true);
}

void LocalGameServices::submit_score(u16 leaderboard, i64 score) {
    if (offline_) {
        sink_->score_sent(leaderboard, false);
        return;
    }
    char line[160];
    std::snprintf(line, sizeof(line), "{\"leaderboard\":\"%s\",\"score\":%" PRId64 "}",
                  catalog_->leaderboards()[leaderboard].id.c_str(), score);
    log_line(line);
    sink_->score_sent(leaderboard, true);
}

FixedString<512> LocalGameServices::blob_path(u16 slot) const {
    FixedString<512> p(dir_.view());
    p.append("/cloud/");
    p.append(catalog_->cloud_saves()[slot].view());
    p.append(".blob");
    return p;
}

namespace {

// <version u64><bytes>; false when absent or unreadable (version 0, no bytes).
bool read_blob(const char* path, u64& version, Array<byte>* bytes) {
    version = 0;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    bool ok = std::fread(&version, sizeof(version), 1, f) == 1;
    if (ok && bytes) {
        byte buf[4096];
        usize n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            for (usize i = 0; i < n; ++i) bytes->push_back(buf[i]);
    }
    std::fclose(f);
    if (!ok) version = 0;
    return ok;
}

}  // namespace

void LocalGameServices::cloud_save(u16 slot, u64 base_version, Span<const byte> bytes) {
    if (offline_) {
        sink_->cloud_saved(slot, CloudResult::Failed, 0);
        return;
    }
    const FixedString<512> path = blob_path(slot);
    u64 current = 0;
    read_blob(path.c_str(), current, nullptr);
    if (current != base_version) {
        sink_->cloud_saved(slot, CloudResult::Conflict, current);
        return;
    }
    FixedString<512> tmp(path.view());
    tmp.append(".tmp");
    const u64 next = current + 1;
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    bool ok = f != nullptr;
    if (f) {
        ok = std::fwrite(&next, sizeof(next), 1, f) == 1;
        ok = (bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size()) && ok;
        ok = std::fclose(f) == 0 && ok;
    }
    std::error_code ec;
    if (ok) stdfs::rename(to_path(tmp.view()), to_path(path.view()), ec);
    sink_->cloud_saved(slot, ok && !ec ? CloudResult::Ok : CloudResult::Failed, ok && !ec ? next : 0);
}

void LocalGameServices::cloud_load(u16 slot) {
    if (offline_) {
        sink_->cloud_loaded(slot, CloudResult::Failed, 0, {});
        return;
    }
    Array<byte> bytes;
    u64 version = 0;
    read_blob(blob_path(slot).c_str(), version, &bytes);
    sink_->cloud_loaded(slot, CloudResult::Ok, version, bytes.span());
}

}  // namespace mce::liveops
