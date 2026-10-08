#include "stream.h"

#include <cstring>

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
        for (u32 i = 0; i < kStreamPageFrames; ++i) {
            u64 f;
            if (!map_frame(source, loop, first + i, f)) {
                std::memset(dst + i * 2, 0, sizeof(f32) * 2 * (kStreamPageFrames - i));
                break;
            }
            const f32* s = source.frames + f * ch;
            dst[i * 2 + 0] = s[0];
            dst[i * 2 + 1] = ch == 2 ? s[1] : s[0];
        }
        ++fill_page;
        filled.store(fill_page, std::memory_order_release);
        ++written;
    }
    return written;
}

}  // namespace mce::audio::detail
