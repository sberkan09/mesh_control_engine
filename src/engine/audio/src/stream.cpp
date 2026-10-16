#include "stream.h"

#include <cstring>

#include "mce/log.h"

namespace mce::audio::detail {

bool StreamRing::map_frame(const ClipPayload& src, bool looping, u64 t, u64& out_frame) {
    if (t < src.frame_count) {
        out_frame = t;
        return true;
    }
    if (!looping || src.loop_start >= src.frame_count) return false;
    const u64 body = src.frame_count - src.loop_start;
    out_frame = src.loop_start + (t - src.frame_count) % body;
    return true;
}

void StreamRing::reset(const ClipPayload& src, bool looping) {
    source = src;
    loop = looping;
    fill_page = 0;
    decoder.close();
    decode_at = 0;
    if (!src.frames && src.encoded) {
        const auto opened = decoder.open(Span<const byte>(src.encoded, src.encoded_bytes));
        if (!opened || decoder.channels() != src.channels) {
            MCE_LOG_ERROR("audio: streamed Vorbis clip does not open ({}); it plays as silence",
                          opened ? "channel count differs" : errc_name(opened.error().code));
            decoder.close();
        }
    }
    consumed.store(0, std::memory_order_relaxed);
    filled.store(0, std::memory_order_release);
    pump(kStreamPages);
}

u32 StreamRing::pump(u32 max_pages) {
    u32 written = 0;
    const u64 done = consumed.load(std::memory_order_acquire);
    // The mixer skipped pages we were late with (underrun): never fill what it has passed.
    if (fill_page < done) fill_page = done;
    while (written < max_pages && fill_page < done + kStreamPages) {
        f32* dst = pages[fill_page % kStreamPages];
        const u64 first = fill_page * kStreamPageFrames;
        const u32 ch = source.channels;
        u32 i = 0;
        while (i < kStreamPageFrames) {
            u64 f;
            if (!map_frame(source, loop, first + i, f)) break;
            // A run of consecutive source frames: up to the page's end or the clip's (loop) end.
            const u64 left = source.frame_count - f;
            const u32 run = static_cast<u32>(left < kStreamPageFrames - i ? left : kStreamPageFrames - i);
            const f32* s = nullptr;
            u32 got = run;
            if (source.frames) {
                s = source.frames + f * ch;
            } else {
                if (!decoder.is_open()) break;
                if (f != decode_at && !decoder.seek(f)) break;
                got = decoder.read(decoded, run);
                decode_at = f + got;
                if (got == 0) break;
                s = decoded;
            }
            for (u32 k = 0; k < got; ++k) {
                dst[(i + k) * 2 + 0] = s[k * ch];
                dst[(i + k) * 2 + 1] = s[k * ch + (ch == 2 ? 1 : 0)];
            }
            i += got;
        }
        if (i < kStreamPageFrames) std::memset(dst + i * 2, 0, sizeof(f32) * 2 * (kStreamPageFrames - i));
        ++fill_page;
        filled.store(fill_page, std::memory_order_release);
        ++written;
    }
    return written;
}

}  // namespace mce::audio::detail
