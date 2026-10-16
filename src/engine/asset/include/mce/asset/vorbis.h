#pragma once
// Ogg Vorbis read from memory (M-AUDIO2 step 1, ADR-0044): the decoder for clips the cook compressed
// ("compress": "vorbis") and for .ogg sources it imports. Wraps stb_vorbis, which only this module's
// vorbis.cpp sees (the stb_vorbis wall in scripts/ci/check_walls.py).
//
// The decoder works in an arena the reader owns (engine heap, grown at open until the stream's setup
// fits): stb_vorbis never calls malloc, and nothing allocates after open(). Decoding is plain integer
// and float code with no threads, so one stream decodes to the same samples every time on a host.

#include "mce/containers/array.h"
#include "mce/memory/allocator.h"
#include "mce/result.h"

namespace mce::asset {

class VorbisReader {
public:
    explicit VorbisReader(mem::IAllocator& a = mem::heap());
    ~VorbisReader();
    VorbisReader(const VorbisReader&) = delete;
    VorbisReader& operator=(const VorbisReader&) = delete;

    // Opens an Ogg Vorbis stream held in memory; `data` must outlive the reader (or the next open).
    Result<void> open(Span<const byte> data);
    void close();
    bool is_open() const { return v_ != nullptr; }

    u32 channels() const { return channels_; }
    u32 sample_rate() const { return sample_rate_; }
    u64 frames() const { return frames_; }  // the stream's length in sample frames

    // Positions the next read at `frame` (sample-exact). False past the end or on a damaged stream.
    bool seek(u64 frame);
    // Decodes up to `max_frames` frames into `out` (channels() floats per frame, interleaved).
    // Returns the frames written; 0 at the end of the stream.
    u32 read(f32* out, u32 max_frames);

private:
    void* v_ = nullptr;  // stb_vorbis*
    Array<byte> arena_;
    u32 channels_ = 0, sample_rate_ = 0;
    u64 frames_ = 0;
};

// The whole stream at once, interleaved f32.
Result<void> decode_vorbis(Span<const byte> data, Array<f32>& out, u32& channels, u32& sample_rate);

}  // namespace mce::asset
