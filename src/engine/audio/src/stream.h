#pragma once
// Streamed-clip page ring (audio-input-ui §3.6). One writer (the filler: update(), later the IO
// worker) and one reader (the mixer). The filler publishes whole pages through `filled`
// (release); the mixer reads only pages below it and reports its progress through `consumed`.
// Neither side ever blocks; a page the filler is late with is mixed as silence (underrun) and
// the voice keeps its timeline.
//
// Page p always holds stream-timeline frames [p·F, (p+1)·F), F = kStreamPageFrames, so the
// timeline → source mapping is a pure function — loop wrap-around is bit-exact by construction.
//
// An encoded source (ClipPayload::encoded, Ogg Vorbis, M-AUDIO2 1) decodes on the filler as the
// pages fill: runs of consecutive source frames are read in one go, and the reader seeks only when
// the mapped frame jumps (the loop wrap, or pages the mixer skipped). The audio thread never decodes.

#include <atomic>

#include "mce/asset/vorbis.h"
#include "mce/audio/audio.h"

namespace mce::audio::detail {

struct StreamRing {
    // Mixer-visible (RT): published page count and consumed page count, both monotonic.
    alignas(64) std::atomic<u64> filled{0};
    alignas(64) std::atomic<u64> consumed{0};
    f32 pages[kStreamPages][kStreamPageFrames * kChannels];  // always stereo (mono upmixed at fill)

    // Filler-only state.
    ClipPayload source;
    bool loop = false;
    u64 fill_page = 0;
    asset::VorbisReader decoder;  // open while `source` is encoded
    u64 decode_at = 0;            // source frame the decoder reads next
    f32 decoded[kStreamPageFrames * 2];

    // Main thread, while no voice reads this ring: rewind to the start of `src` and prime it. An
    // encoded `src` that does not open plays as silence (logged).
    void reset(const ClipPayload& src, bool looping);
    // Fills pages ahead of the mixer, at most `max_pages`. Returns pages written.
    u32 pump(u32 max_pages = kStreamPages);
    // Source frame for a stream-timeline frame; false past the end of a non-looping clip.
    static bool map_frame(const ClipPayload& src, bool looping, u64 t, u64& out_frame);
};

}  // namespace mce::audio::detail
