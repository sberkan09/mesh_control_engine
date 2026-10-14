// The GameServicesCatalog chunk (M-LIVEOPS step 2; mce/liveops/game_services.h).
//
// Layout v1 (little-endian):
//   ServicesHeader | ServicesEntry[achievements + leaderboards + cloud saves] | string blob

#include <cstring>

#include "mce/liveops/game_services.h"

namespace mce::liveops {

namespace {

constexpr const char* kStage = "liveops.services_catalog";

struct ServicesHeader {
    u32 achievement_count;
    u32 leaderboard_count;
    u32 cloud_count;
    u32 strings_size;
    u64 reserved;
};
static_assert(sizeof(ServicesHeader) == 24);

struct StrRef {
    u32 offset;
    u32 size;
};

struct ServicesEntry {
    StrRef id;
    StrRef ios;
    StrRef android;
    u32 value;  // achievements: steps; leaderboards: 1 = lower is better; cloud saves: 0
    u32 reserved;
};
static_assert(sizeof(ServicesEntry) == 32);

StrRef add_string(Array<byte>& blob, StringView s) {
    const StrRef r{static_cast<u32>(blob.size()), static_cast<u32>(s.size())};
    for (char c : s) blob.push_back(static_cast<byte>(c));
    return r;
}

void append(Array<byte>& out, const void* p, usize n) {
    const usize at = out.size();
    out.resize_uninitialized(at + n);
    std::memcpy(out.data() + at, p, n);
}

bool read_string(const char* blob, u32 blob_size, StrRef r, StringView& out) {
    if (r.offset > blob_size || r.size > blob_size - r.offset) return false;
    out = StringView(blob + r.offset, r.size);
    return true;
}

template <class T>
u16 find_id(Span<const T> items, StringView id) {
    for (usize i = 0; i < items.size(); ++i)
        if (items[i].id.view() == id) return static_cast<u16>(i);
    return kNoService;
}

}  // namespace

u16 ServicesCatalog::find_achievement(StringView id) const { return find_id(achievements_.span(), id); }
u16 ServicesCatalog::find_leaderboard(StringView id) const { return find_id(leaderboards_.span(), id); }

u16 ServicesCatalog::find_cloud_save(StringView name) const {
    for (usize i = 0; i < cloud_.size(); ++i)
        if (cloud_[i].view() == name) return static_cast<u16>(i);
    return kNoService;
}

bool ServicesCatalog::add_achievement(const AchievementInfo& a) {
    if (a.id.empty() || a.steps == 0 || find_achievement(a.id.view()) != kNoService ||
        achievements_.size() >= kNoService)
        return false;
    achievements_.push_back(a);
    return true;
}

bool ServicesCatalog::add_leaderboard(const LeaderboardInfo& l) {
    if (l.id.empty() || find_leaderboard(l.id.view()) != kNoService || leaderboards_.size() >= kNoService) return false;
    leaderboards_.push_back(l);
    return true;
}

bool ServicesCatalog::add_cloud_save(StringView name) {
    const FixedString<64> n(name);
    if (n.empty() || n.truncated() || find_cloud_save(name) != kNoService || cloud_.size() >= kNoService) return false;
    cloud_.push_back(n);
    return true;
}

void ServicesCatalog::clear() {
    achievements_.clear();
    leaderboards_.clear();
    cloud_.clear();
}

void encode_services_catalog(const ServicesCatalog& c, Array<byte>& out) {
    Array<byte> blob;
    Array<ServicesEntry> entries;
    for (const AchievementInfo& a : c.achievements()) {
        ServicesEntry e{};
        e.id = add_string(blob, a.id.view());
        e.ios = add_string(blob, a.ios_id.view());
        e.android = add_string(blob, a.android_id.view());
        e.value = a.steps;
        entries.push_back(e);
    }
    for (const LeaderboardInfo& l : c.leaderboards()) {
        ServicesEntry e{};
        e.id = add_string(blob, l.id.view());
        e.ios = add_string(blob, l.ios_id.view());
        e.android = add_string(blob, l.android_id.view());
        e.value = l.lower_is_better ? 1 : 0;
        entries.push_back(e);
    }
    for (const FixedString<64>& s : c.cloud_saves()) {
        ServicesEntry e{};
        e.id = add_string(blob, s.view());
        e.ios = add_string(blob, {});
        e.android = add_string(blob, {});
        entries.push_back(e);
    }
    ServicesHeader h{};
    h.achievement_count = static_cast<u32>(c.achievements().size());
    h.leaderboard_count = static_cast<u32>(c.leaderboards().size());
    h.cloud_count = static_cast<u32>(c.cloud_saves().size());
    h.strings_size = static_cast<u32>(blob.size());
    out.clear();
    append(out, &h, sizeof(h));
    if (!entries.empty()) append(out, entries.data(), entries.size() * sizeof(ServicesEntry));
    if (!blob.empty()) append(out, blob.data(), blob.size());
}

Result<void> decode_services_catalog(Span<const byte> payload, ServicesCatalog& out) {
    out.clear();
    if (payload.size() < sizeof(ServicesHeader)) return Error(Errc::Truncated, kStage);
    ServicesHeader h;
    std::memcpy(&h, payload.data(), sizeof(h));
    if (h.reserved != 0) return Error(Errc::ReservedNonZero, kStage);
    if (h.achievement_count >= kNoService || h.leaderboard_count >= kNoService || h.cloud_count >= kNoService)
        return Error(Errc::OutOfBounds, kStage);
    const u64 count = u64(h.achievement_count) + h.leaderboard_count + h.cloud_count;
    const u64 need = sizeof(h) + count * sizeof(ServicesEntry) + h.strings_size;
    if (payload.size() < need) return Error(Errc::Truncated, kStage, {}, static_cast<u32>(payload.size()));
    const byte* entries = payload.data() + sizeof(h);
    const char* blob = reinterpret_cast<const char*>(entries + count * sizeof(ServicesEntry));
    for (u64 i = 0; i < count; ++i) {
        ServicesEntry e;
        std::memcpy(&e, entries + i * sizeof(ServicesEntry), sizeof(e));
        if (e.reserved != 0) return Error(Errc::ReservedNonZero, kStage, {}, static_cast<u32>(i));
        StringView id, ios, android;
        if (!read_string(blob, h.strings_size, e.id, id) || !read_string(blob, h.strings_size, e.ios, ios) ||
            !read_string(blob, h.strings_size, e.android, android))
            return Error(Errc::OutOfBounds, kStage, {}, static_cast<u32>(i));
        bool added = false;
        if (i < h.achievement_count) {
            AchievementInfo a;
            a.id = id;
            a.ios_id = ios;
            a.android_id = android;
            a.steps = e.value;
            added = !a.id.truncated() && !a.ios_id.truncated() && !a.android_id.truncated() && out.add_achievement(a);
        } else if (i < u64(h.achievement_count) + h.leaderboard_count) {
            if (e.value > 1) return Error(Errc::InvalidArgument, kStage, id, static_cast<u32>(i));
            LeaderboardInfo l;
            l.id = id;
            l.ios_id = ios;
            l.android_id = android;
            l.lower_is_better = e.value == 1;
            added = !l.id.truncated() && !l.ios_id.truncated() && !l.android_id.truncated() && out.add_leaderboard(l);
        } else {
            if (e.value != 0 || !ios.empty() || !android.empty())
                return Error(Errc::InvalidArgument, kStage, id, static_cast<u32>(i));
            added = out.add_cloud_save(id);
        }
        if (!added) {
            out.clear();
            return Error(Errc::InvalidArgument, kStage, id,
                         static_cast<u32>(i));  // empty, duplicate, too long or 0 steps
        }
    }
    return ok();
}

}  // namespace mce::liveops
