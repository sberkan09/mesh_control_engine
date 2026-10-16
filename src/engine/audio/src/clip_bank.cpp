#include "mce/audio/clip_bank.h"

#include "mce/asset/vorbis.h"
#include "mce/log.h"

namespace mce::audio {

Result<ClipPayload> clip_payload_from_sfx(Span<const byte> payload) {
    constexpr const char* kStage = "audio.clip.sfx";
    MCE_TRY_ASSIGN(asset::SfxView v, asset::SfxView::read(payload));
    if (v.set) return Error(Errc::Unsupported, kStage, "a sound set: ClipBank::pick plays it");
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
    ClipPayload p;
    if ((h.flags & asset::SfxFlags::Vorbis) != 0) {
        if (v.vorbis.size() > 0xFFFFFFFFull) return Error(Errc::Unsupported, kStage, "Vorbis stream over 4 GB");
        p.encoded = v.vorbis.data();
        p.encoded_bytes = static_cast<u32>(v.vorbis.size());
    } else {
        if (v.samples.size() < frames * h.channels) return Error(Errc::Truncated, kStage);
        p.frames = v.samples.data();
    }
    p.frame_count = static_cast<u32>(frames);
    p.channels = h.channels;
    p.sample_rate = h.sample_rate;
    p.loop_start = static_cast<u32>(loop_start);
    return p;
}

Result<void> decode_clip(ClipPayload& p, Array<f32>& pcm) {
    constexpr const char* kStage = "audio.clip.vorbis";
    u32 channels = 0, rate = 0;
    MCE_TRY(asset::decode_vorbis(Span<const byte>(p.encoded, p.encoded_bytes), pcm, channels, rate));
    if (channels != p.channels || rate != p.sample_rate) return Error(Errc::InvalidArgument, kStage, "stream differs from its header");
    if (pcm.size() < static_cast<usize>(p.frame_count) * channels) return Error(Errc::Truncated, kStage, "stream shorter than its header");
    p.frames = pcm.data();
    p.encoded = nullptr;
    p.encoded_bytes = 0;
    return ok();
}

ClipBank::ClipBank(asset::AssetSystem& assets, AudioDevice& device, mem::IAllocator& a)
    : assets_(&assets), device_(&device), entries_(a), retired_(a) {}

ClipBank::~ClipBank() { release_all(); }

ClipHandle ClipBank::register_entry(Entry& e) {
    // A sound set registers nothing: it keeps its members' GUIDs and picks per play.
    if (const auto view = asset::SfxView::read(assets_->resolve(e.ref)); view && view.value().set) {
        e.set = true;
        e.params = *view.value().set;
        e.members.clear();
        for (const asset::Guid g : view.value().set_clips) e.members.push_back(g);
        e.cursor = 0;
        e.last = 0xFFFFFFFFu;
        return {};
    }
    e.set = false;
    const auto payload = clip_payload_from_sfx(assets_->resolve(e.ref));
    if (!payload) {
        MCE_LOG_ERROR("audio: bad Sfx asset {} ({})", asset::to_string(e.ref.guid).c_str(), errc_name(payload.error().code));
        return {};
    }
    ClipPayload p = payload.value();
    auto in_memory = [&]() -> Result<ClipHandle> {
        if (p.encoded) {
            const Result<void> decoded = decode_clip(p, e.pcm);
            if (!decoded) return decoded.error();
        }
        return device_->register_clip(p);
    };
    Result<ClipHandle> clip = e.stream ? device_->register_stream(p) : in_memory();
    if (!clip && e.stream && clip.error().code == Errc::OutOfMemory) {
        MCE_LOG_WARN("audio: no free stream for {}; it plays from memory", asset::to_string(e.ref.guid).c_str());
        clip = in_memory();
    }
    if (!clip) {
        MCE_LOG_ERROR("audio: cannot register clip {} ({})", asset::to_string(e.ref.guid).c_str(), errc_name(clip.error().code));
        return {};
    }
    return clip.value();
}

ClipHandle ClipBank::acquire(asset::Guid guid, bool stream) {
    if (guid.is_null()) return {};
    if (const Entry* e = entries_.find(guid)) return e->clip;

    Entry& e = entries_.insert_or_assign(guid, Entry{});
    e.ref.guid = guid;
    e.stream = stream;
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

namespace {
// splitmix64: decorrelated values from one seed.
u64 mix(u64& s) {
    u64 z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
f32 unit(u64 v) { return static_cast<f32>(v >> 40) * (1.f / 16777216.f); }  // [0, 1)
}  // namespace

ClipPick ClipBank::pick(asset::Guid guid, u64 seed, bool stream) {
    ClipPick out;
    out.clip = acquire(guid, stream);
    Entry* e = entries_.find(guid);
    if (!e || !e->set || e->members.empty()) return out;
    const u32 n = static_cast<u32>(e->members.size());
    u64 s = seed;
    const u64 r = mix(s);
    u32 index = 0;
    if (e->params.pick == asset::SoundSetPick::Sequence) {
        index = e->cursor++ % n;
    } else {
        index = static_cast<u32>(r % n);
        if ((e->params.flags & asset::SoundSetFlags::NoRepeat) && n > 1 && index == e->last)
            index = (index + 1 + static_cast<u32>((r >> 32) % (n - 1))) % n;
        e->last = index;
    }
    out.pitch = e->params.pitch_min + (e->params.pitch_max - e->params.pitch_min) * unit(mix(s));
    out.gain_db = e->params.gain_min_db + (e->params.gain_max_db - e->params.gain_min_db) * unit(mix(s));
    const asset::Guid member = e->members[index];  // `e` may move: acquire() can grow the table
    if (member == guid) return out;
    out.clip = acquire(member, stream);
    if (const Entry* m = entries_.find(member); m && m->set) {
        MCE_LOG_ERROR("audio: sound set {} contains another set; it plays nothing", asset::to_string(guid).c_str());
        out.clip = {};
    }
    return out;
}

bool ClipBank::is_set(asset::Guid guid) const {
    const Entry* e = entries_.find(guid);
    return e && e->set;
}

ClipHandle ClipBank::find(asset::Guid guid) const {
    const Entry* e = entries_.find(guid);
    return e ? e->clip : ClipHandle{};
}

void ClipBank::retire(Entry& e) {
    if (!e.clip.is_null()) device_->release_clip(e.clip);
    if (!e.pcm.empty()) retired_.push_back(Retired{e.clip, std::move(e.pcm)});
    e.pcm = Array<f32>();
    e.clip = {};
}

void ClipBank::sweep() {
    for (usize i = retired_.size(); i-- > 0;)
        if (!device_->is_clip_registered(retired_[i].clip)) {
            retired_[i] = std::move(retired_.back());
            retired_.pop_back();
        }
}

void ClipBank::drop(Entry& e) {
    retire(e);
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
    // The mixer may be inside a block reading these clips, and it takes a bounded number of
    // commands per callback: wait for every release before dropping the retains, or a pack
    // unmounted right after (a quitting player) is still read by the audio thread (#145).
    for (auto& kv : entries_)
        if (!kv.value.clip.is_null()) device_->release_clip(kv.value.clip);
    device_->finish_releases();
    for (auto& kv : entries_) {
        kv.value.clip = {};
        drop(kv.value);
    }
    entries_.clear();
    retired_.clear();
}

void ClipBank::sync() {
    sweep();
    for (auto& kv : entries_) {
        Entry& e = kv.value;
        if (!e.ref.handle.valid()) continue;
        const u32 live = assets_->generation(e.ref.handle.index);
        if (live == e.generation) continue;
        // Re-stamps the handle to the new generation; the asset retain carries over (it is
        // counted per row, not per generation).
        retire(e);
        e.clip = register_entry(e);
        e.generation = e.ref.handle.generation;
    }
}

}  // namespace mce::audio
