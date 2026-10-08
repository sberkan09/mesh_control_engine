#include "mce/audio/clip_bank.h"

#include "mce/log.h"

namespace mce::audio {

Result<ClipPayload> clip_payload_from_sfx(Span<const byte> payload) {
    constexpr const char* kStage = "audio.clip.sfx";
    MCE_TRY_ASSIGN(asset::SfxView v, asset::SfxView::read(payload));
    const asset::SfxHeader& h = *v.header;
    if (h.sample_rate != kSampleRate) return Error(Errc::Unsupported, kStage, "sample rate is not 48 kHz");
    if (h.channels != 1 && h.channels != 2) return Error(Errc::Unsupported, kStage, "channels must be 1 or 2");
    u64 frames = h.frame_count;
    u64 loop_start = 0;
    if ((h.flags & asset::SfxFlags::Looping) != 0) {
        if (h.loop_end <= h.loop_start || h.loop_end > frames) return Error(Errc::OutOfBounds, kStage, "bad loop points");
        frames = h.loop_end;
        loop_start = h.loop_start;
    }
    if (frames == 0) return Error(Errc::InvalidArgument, kStage, "empty clip");
    if (frames > 0xFFFFFFFFull) return Error(Errc::Unsupported, kStage, "clip longer than 2^32 frames");
    if (v.samples.size() < frames * h.channels) return Error(Errc::Truncated, kStage);

    ClipPayload p;
    p.frames = v.samples.data();
    p.frame_count = static_cast<u32>(frames);
    p.channels = h.channels;
    p.sample_rate = h.sample_rate;
    p.loop_start = static_cast<u32>(loop_start);
    return p;
}

ClipBank::ClipBank(asset::AssetSystem& assets, AudioDevice& device, mem::IAllocator& a)
    : assets_(&assets), device_(&device), entries_(a) {}

ClipBank::~ClipBank() { release_all(); }

ClipHandle ClipBank::register_entry(Entry& e) {
    const auto payload = clip_payload_from_sfx(assets_->resolve(e.ref));
    if (!payload) {
        MCE_LOG_ERROR("audio: bad Sfx asset {} ({})", asset::to_string(e.ref.guid).c_str(), errc_name(payload.error().code));
        return {};
    }
    auto clip = device_->register_clip(payload.value());
    if (!clip) {
        MCE_LOG_ERROR("audio: cannot register clip {} ({})", asset::to_string(e.ref.guid).c_str(), errc_name(clip.error().code));
        return {};
    }
    return clip.value();
}

ClipHandle ClipBank::acquire(asset::Guid guid) {
    if (guid.is_null()) return {};
    if (const Entry* e = entries_.find(guid)) return e->clip;

    Entry& e = entries_.insert_or_assign(guid, Entry{});
    e.ref.guid = guid;
    auto h = assets_->load_sync<asset::Sfx>(guid);
    if (!h) {
        MCE_LOG_ERROR("audio: cannot load Sfx asset {} ({})", asset::to_string(guid).c_str(), errc_name(h.error().code));
        return {};
    }
    e.ref.handle = h.value();
    e.generation = h.value().generation;
    e.clip = register_entry(e);
    return e.clip;
}

ClipHandle ClipBank::find(asset::Guid guid) const {
    const Entry* e = entries_.find(guid);
    return e ? e->clip : ClipHandle{};
}

void ClipBank::drop(Entry& e) {
    if (!e.clip.is_null()) device_->release_clip(e.clip);
    if (e.ref.handle.valid()) assets_->release(e.ref.handle);
    e = Entry{};
}

void ClipBank::release(asset::Guid guid) {
    if (Entry* e = entries_.find(guid)) {
        drop(*e);
        entries_.erase(guid);
    }
}

void ClipBank::release_all() {
    for (auto& kv : entries_) drop(kv.value);
    entries_.clear();
}

void ClipBank::sync() {
    for (auto& kv : entries_) {
        Entry& e = kv.value;
        if (!e.ref.handle.valid()) continue;
        const u32 live = assets_->generation(e.ref.handle.index);
        if (live == e.generation) continue;
        // Re-stamps the handle to the new generation; the asset retain carries over (it is
        // counted per row, not per generation).
        if (!e.clip.is_null()) device_->release_clip(e.clip);
        e.clip = register_entry(e);
        e.generation = e.ref.handle.generation;
    }
}

}  // namespace mce::audio
