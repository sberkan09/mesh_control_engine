#pragma once
// VideoDecoder (M-CINEMA 5, ADR-0043): MPEG-1 video (and its MP2 audio) decoded from memory with
// pl_mpeg. The player's video textures step one decoder per playing VideoPlayer to the sim's
// playhead; the cook's .mpg importer reads the header and decodes the audio once (the runtime plays
// that PCM through the mixer, so a decoder here only ever decodes pictures at run time).
//
// Pictures come out as RGBA8 (width × height × 4, rows top to bottom). Stepping forward decodes the
// frames in between; a step back or a jump of more than a second seeks (to the intra frame before
// the target, then decodes forward to it), so scrubbing a timeline costs one seek, not a replay.

#include "mce/containers/array.h"
#include "mce/types.h"

namespace mce::render {

struct VideoInfo {
    u32 width = 0;
    u32 height = 0;
    f32 frame_rate = 0.0f;  // frames per second
    f32 duration = 0.0f;    // seconds of video (from the stream's last time stamp: a little short; see count_frames)
    u32 audio_streams = 0;  // 0 = silent
    u32 sample_rate = 0;    // the audio stream's, when there is one
};

class VideoDecoder {
public:
    VideoDecoder() = default;
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    ~VideoDecoder();

    // Opens an MPEG-1 program stream held in memory. The bytes are not copied: they must outlive
    // the decoder (or the next open/close). With `audio` false only the video stream is decoded.
    // False when the bytes are not an MPEG-1 program stream with a video stream.
    bool open(Span<const byte> data, bool audio = false);
    void close();
    bool is_open() const { return plm_ != nullptr; }
    const VideoInfo& info() const { return info_; }

    // Moves the picture to the frame shown at `seconds` (clamped to the video). True when the
    // picture changed (rgba() holds the new one).
    bool show(f64 seconds);
    // The shown picture's time in seconds; < 0 before the first show().
    f64 picture_time() const { return picture_time_; }
    Span<const u8> rgba() const { return {rgba_.data(), rgba_.size()}; }
    // Frames decoded since open (seeks included): the cost a test or a budget check reads.
    u32 frames_decoded() const { return frames_decoded_; }

    // Decodes every picture to count them (the cook's exact length: frames / frame_rate), then
    // rewinds. Costs one full decode.
    u32 count_frames();
    // The length show() clamps to (the cooked header's, which count_frames measured).
    void set_duration(f32 seconds) { info_.duration = seconds; }

    // Decodes the whole audio stream: interleaved stereo f32 at info().sample_rate. Opened with
    // audio = true; it moves the stream to its end (open again to decode pictures).
    bool decode_audio(Array<f32>& out);

private:
    void* plm_ = nullptr;  // plm_t
    VideoInfo info_;
    Array<u8> rgba_;
    f64 picture_time_ = -1.0;
    u32 frames_decoded_ = 0;
};

}  // namespace mce::render
