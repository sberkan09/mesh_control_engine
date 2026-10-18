// MP3 (dr_mp3) and FLAC (dr_flac) decoding into PcmClip (M-IMPORTMEDIA step 2). The "dr_libs wall" keeps the dr_*
// symbols in this file and drlibs_impl.c. Input is untrusted, so both decoders get an allocator that refuses past
// kAudioDecodeMemory (a FLAC header can size its block buffers; an MP3 stream can be arbitrarily long) and the read
// loop stops at kAudioMaxFrames / kAudioMaxSamples instead of trusting any frame count in the file.

#include "audio_decode.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "dr_flac.h"
#include "dr_mp3.h"

namespace mce::cook {

namespace {

// malloc with a size header so free and realloc know what they release; refuses past the cap.
struct Budget {
    u64 used = 0;
    u64 cap = kAudioDecodeMemory;
};
constexpr usize kHeader = 16;  // keeps the payload 16-byte aligned

void* budget_malloc(size_t n, void* user) {
    Budget* b = static_cast<Budget*>(user);
    if (n > b->cap || b->used > b->cap - n) return nullptr;
    unsigned char* p = static_cast<unsigned char*>(std::malloc(n + kHeader));
    if (!p) return nullptr;
    std::memcpy(p, &n, sizeof n);
    b->used += n;
    return p + kHeader;
}
void budget_free(void* p, void* user) {
    if (!p) return;
    Budget* b = static_cast<Budget*>(user);
    unsigned char* base = static_cast<unsigned char*>(p) - kHeader;
    size_t n = 0;
    std::memcpy(&n, base, sizeof n);
    b->used -= n;
    std::free(base);
}
void* budget_realloc(void* p, size_t n, void* user) {
    if (!p) return budget_malloc(n, user);
    Budget* b = static_cast<Budget*>(user);
    unsigned char* base = static_cast<unsigned char*>(p) - kHeader;
    size_t old = 0;
    std::memcpy(&old, base, sizeof old);
    if (n > b->cap || b->used - old > b->cap - n) return nullptr;  // used - old cannot underflow: old is counted in used
    unsigned char* q = static_cast<unsigned char*>(std::realloc(base, n + kHeader));
    if (!q) return nullptr;
    std::memcpy(q, &n, sizeof n);
    b->used = b->used - old + n;
    return q + kHeader;
}

bool fail(FixedString<128>& why, const char* msg) {
    why = msg;
    return false;
}

// Reads frames through `read(buf, n)` until it returns 0, into out.samples. Both libraries return the frames read.
template <class Read>
bool drain(Read&& read, u32 channels, PcmClip& out, FixedString<128>& why) {
    constexpr u32 kChunk = 4096;
    f32 buf[kChunk * 2];
    const u64 max_frames = std::min<u64>(kAudioMaxFrames, kAudioMaxSamples / channels);
    u64 frames = 0;
    while (frames < max_frames) {
        const u64 want = std::min<u64>(kChunk, max_frames - frames);
        const u64 got = read(buf, want);
        if (got == 0) break;
        const usize at = out.samples.size();
        out.samples.resize(at + usize(got) * channels);
        std::memcpy(out.samples.data() + at, buf, usize(got) * channels * sizeof(f32));
        frames += got;
    }
    if (frames == 0) return fail(why, "no audio frames could be decoded");
    if (frames >= max_frames) return fail(why, "the clip is longer than the cook accepts (one hour, or 128M samples)");
    return true;
}

}  // namespace

bool decode_mp3(Span<const byte> bytes, PcmClip& out, FixedString<128>& why) {
    out = PcmClip{};
    if (bytes.size() < 4) return fail(why, "not an MP3 file");
    Budget budget;
    drmp3_allocation_callbacks cb{&budget, budget_malloc, budget_realloc, budget_free};
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, bytes.data(), bytes.size(), &cb)) return fail(why, "not a readable MP3 file");
    bool ok = false;
    if (mp3.channels < 1 || mp3.channels > 2) fail(why, "MP3: need 1 or 2 channels");
    else if (mp3.sampleRate < 1000 || mp3.sampleRate > 384000) fail(why, "MP3: sample rate out of range");
    else {
        out.channels = mp3.channels;
        out.sample_rate = mp3.sampleRate;
        ok = drain([&](f32* buf, u64 n) { return u64(drmp3_read_pcm_frames_f32(&mp3, n, buf)); }, out.channels, out, why);
    }
    drmp3_uninit(&mp3);
    return ok;
}

bool decode_flac(Span<const byte> bytes, PcmClip& out, FixedString<128>& why) {
    out = PcmClip{};
    if (bytes.size() < 4) return fail(why, "not a FLAC file");
    Budget budget;
    drflac_allocation_callbacks cb{&budget, budget_malloc, budget_realloc, budget_free};
    drflac* flac = drflac_open_memory(bytes.data(), bytes.size(), &cb);
    if (!flac) return fail(why, "not a readable FLAC file");
    bool ok = false;
    if (flac->channels < 1 || flac->channels > 2) fail(why, "FLAC: need 1 or 2 channels");
    else if (flac->sampleRate < 1000 || flac->sampleRate > 384000) fail(why, "FLAC: sample rate out of range");
    else {
        out.channels = flac->channels;
        out.sample_rate = flac->sampleRate;
        ok = drain([&](f32* buf, u64 n) { return u64(drflac_read_pcm_frames_f32(flac, n, buf)); }, out.channels, out, why);
    }
    drflac_close(flac);
    return ok;
}

}  // namespace mce::cook
