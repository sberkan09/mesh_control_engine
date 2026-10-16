// PlatformGameServices: Game Center / Play Games behind IGameServicesProvider, with versions kept in
// the cloud blob.

#include "mce/liveops/platform_games.h"

#include <cstring>

#include "mce/log.h"

namespace mce::liveops {

namespace {

constexpr u32 kBlobMagic = 0x5347434D;  // "MCGS"
constexpr u32 kBlobFormat = 1;
constexpr usize kBlobHeader = 16;  // magic, format, version

template <class T>
void put(Array<byte>& out, T v) {
    const usize at = out.size();
    out.resize_uninitialized(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
}

}  // namespace

void encode_cloud_blob(u64 version, Span<const byte> bytes, Array<byte>& out) {
    out.clear();
    out.reserve(kBlobHeader + bytes.size());
    put(out, kBlobMagic);
    put(out, kBlobFormat);
    put(out, version);
    for (byte b : bytes) out.push_back(b);
}

bool decode_cloud_blob(Span<const byte> blob, u64& version, Span<const byte>& bytes) {
    if (blob.size() < kBlobHeader) return false;
    u32 magic = 0;
    u32 format = 0;
    u64 v = 0;
    std::memcpy(&magic, blob.data(), 4);
    std::memcpy(&format, blob.data() + 4, 4);
    std::memcpy(&v, blob.data() + 8, 8);
    if (magic != kBlobMagic || format != kBlobFormat || v == 0) return false;
    version = v;
    bytes = Span<const byte>(blob.data() + kBlobHeader, blob.size() - kBlobHeader);
    return true;
}

PlatformGameServices::PlatformGameServices(IPlatformGamesBackend& backend, mem::IAllocator& a)
    : backend_(backend), alloc_(a), slots_(a) {}

template <class Info>
StringView PlatformGameServices::store_id(const Info& e) const {
    switch (backend_.store_ids()) {
        case StoreIds::Ios: return e.ios_id.view();
        case StoreIds::Android: return e.android_id.view();
        case StoreIds::Steam: return e.steam_id.empty() ? e.id.view() : e.steam_id.view();
    }
    return {};
}

void PlatformGameServices::start(const ServicesCatalog& catalog, GameServicesSink& sink) {
    catalog_ = &catalog;
    sink_ = &sink;
    slots_.clear();
    for (usize i = 0; i < catalog.cloud_saves().size(); ++i) slots_.emplace_back(Slot{Slot::Op::None, 0, Array<byte>(alloc_)});
    started_ = backend_.start(*this);
    if (!started_) MCE_LOG_WARN("game_services: {} did not start; everything stays on this device", backend_.name());
}

void PlatformGameServices::stop() {
    if (started_) backend_.stop();
    started_ = false;
}

void PlatformGameServices::sign_in(bool interactive) {
    if (!started_) return sink_->signed_in(false);
    backend_.sign_in(interactive);
}

void PlatformGameServices::report_achievement(u16 achievement, u32 steps) {
    const AchievementInfo& a = catalog_->achievements()[achievement];
    const StringView id = store_id(a);
    if (!started_ || id.empty()) {
        if (started_) MCE_LOG_WARN("game_services: achievement '{}' has no {} id", a.id.view(), backend_.name());
        return sink_->achievement_sent(achievement, false);
    }
    backend_.report_achievement(id, steps < a.steps ? steps : a.steps, a.steps, achievement);
}

void PlatformGameServices::submit_score(u16 leaderboard, i64 score) {
    const LeaderboardInfo& l = catalog_->leaderboards()[leaderboard];
    const StringView id = store_id(l);
    if (!started_ || id.empty()) {
        if (started_) MCE_LOG_WARN("game_services: leaderboard '{}' has no {} id", l.id.view(), backend_.name());
        return sink_->score_sent(leaderboard, false);
    }
    backend_.submit_score(id, score, leaderboard);
}

void PlatformGameServices::cloud_save(u16 slot, u64 base_version, Span<const byte> bytes) {
    if (!started_ || slot >= slots_.size()) return sink_->cloud_saved(slot, CloudResult::Failed, 0);
    Slot& s = slots_[slot];
    s.op = Slot::Op::Save;
    s.base = base_version;
    encode_cloud_blob(base_version + 1, bytes, s.blob);
    backend_.read_blob(catalog_->cloud_saves()[slot].view(), slot);
}

void PlatformGameServices::cloud_load(u16 slot) {
    if (!started_ || slot >= slots_.size()) return sink_->cloud_loaded(slot, CloudResult::Failed, 0, {});
    slots_[slot].op = Slot::Op::Load;
    backend_.read_blob(catalog_->cloud_saves()[slot].view(), slot);
}

void PlatformGameServices::show_achievements() {
    if (started_) backend_.show_achievements();
}

void PlatformGameServices::show_leaderboard(u16 leaderboard) {
    if (!started_ || leaderboard >= catalog_->leaderboards().size()) return;
    const LeaderboardInfo& l = catalog_->leaderboards()[leaderboard];
    backend_.show_leaderboard(store_id(l));
}

void PlatformGameServices::platform_signed_in(bool ok) { sink_->signed_in(ok); }
void PlatformGameServices::platform_achievement(u16 tag, bool ok) { sink_->achievement_sent(tag, ok); }
void PlatformGameServices::platform_score(u16 tag, bool ok) { sink_->score_sent(tag, ok); }

void PlatformGameServices::platform_blob_read(u16 tag, BlobRead r, Span<const byte> blob) {
    if (tag >= slots_.size()) return;
    Slot& s = slots_[tag];
    const Slot::Op op = s.op;
    u64 version = 0;
    Span<const byte> bytes;
    bool ours = r == BlobRead::Missing;  // nothing in the cloud yet: version 0
    if (r == BlobRead::Ok) {
        ours = decode_cloud_blob(blob, version, bytes);
        if (!ours)
            MCE_LOG_WARN("game_services: the cloud save '{}' is not this engine's; left untouched",
                         catalog_->cloud_saves()[tag].view());
    }
    if (op == Slot::Op::Load) {
        s.op = Slot::Op::None;
        if (!ours) return sink_->cloud_loaded(tag, CloudResult::Failed, 0, {});
        return sink_->cloud_loaded(tag, CloudResult::Ok, version, bytes);
    }
    if (op != Slot::Op::Save) return;
    if (!ours) {
        s.op = Slot::Op::None;
        return sink_->cloud_saved(tag, CloudResult::Failed, 0);
    }
    if (version != s.base) {
        s.op = Slot::Op::None;
        return sink_->cloud_saved(tag, CloudResult::Conflict, version);
    }
    backend_.write_blob(catalog_->cloud_saves()[tag].view(), s.blob.span(), tag);
}

void PlatformGameServices::platform_blob_written(u16 tag, bool ok) {
    if (tag >= slots_.size()) return;
    Slot& s = slots_[tag];
    s.op = Slot::Op::None;
    sink_->cloud_saved(tag, ok ? CloudResult::Ok : CloudResult::Failed, ok ? s.base + 1 : 0);
}

}  // namespace mce::liveops
