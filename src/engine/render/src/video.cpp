#include "mce/render/video.h"

#include <cstddef>  // size_t, before pl_mpeg.h (PLM_NO_STDIO)

#include "pl_mpeg.h"

namespace mce::render {

namespace {

plm_t* as_plm(void* p) { return static_cast<plm_t*>(p); }

}  // namespace

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    if (plm_) plm_destroy(as_plm(plm_));
    plm_ = nullptr;
    info_ = VideoInfo{};
    rgba_.clear();
    picture_time_ = -1.0;
    frames_decoded_ = 0;
}

bool VideoDecoder::open(Span<const byte> data, bool audio) {
    close();
    if (data.empty()) return false;
    // pl_mpeg reads a memory buffer and never writes it (free_when_done = 0: the bytes stay ours).
    plm_t* p = plm_create_with_memory(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(data.data())), data.size(), 0);
    if (!p) return false;
    plm_ = p;
    if (!plm_has_headers(p) || plm_get_num_video_streams(p) < 1 || plm_get_width(p) <= 0 || plm_get_height(p) <= 0) {
        close();
        return false;
    }
    plm_set_audio_enabled(p, audio ? 1 : 0);
    plm_set_video_enabled(p, audio ? 0 : 1);
    plm_set_loop(p, 0);
    info_.width = static_cast<u32>(plm_get_width(p));
    info_.height = static_cast<u32>(plm_get_height(p));
    info_.frame_rate = static_cast<f32>(plm_get_framerate(p));
    info_.duration = static_cast<f32>(plm_get_duration(p));
    info_.audio_streams = static_cast<u32>(plm_get_num_audio_streams(p));
    info_.sample_rate = info_.audio_streams ? static_cast<u32>(plm_get_samplerate(p)) : 0u;
    if (!audio) {
        // plm_frame_to_rgba writes r, g, b and skips the fourth byte: opaque from the start.
        rgba_.resize(static_cast<usize>(info_.width) * info_.height * 4u, u8{0});
        for (usize i = 3; i < rgba_.size(); i += 4) rgba_[i] = 255;
    }
    return true;
}

bool VideoDecoder::show(f64 seconds) {
    if (!plm_ || rgba_.empty()) return false;
    plm_t* p = as_plm(plm_);
    const f64 end = info_.duration > 0.0f ? static_cast<f64>(info_.duration) : 0.0;
    const f64 t = seconds < 0.0 ? 0.0 : (end > 0.0 && seconds > end ? end : seconds);
    const f64 frame = info_.frame_rate > 0.0f ? 1.0 / static_cast<f64>(info_.frame_rate) : 1.0 / 30.0;
    const f64 eps = frame * 1e-3;
    plm_frame_t* shown = nullptr;
    if (picture_time_ < 0.0 || t < picture_time_ - eps || t > picture_time_ + 1.0) {
        // pl_mpeg seeks to its own length at most (the last packet's time stamp): the forward
        // steps below cover the rest.
        shown = plm_seek_frame(p, t - frame + eps, 1);  // the first frame at or after that: the one showing at t
        if (!shown) return false;
        ++frames_decoded_;
    }
    // Forward: decode until the next frame would start after t.
    while (t + eps >= (shown ? shown->time : picture_time_) + frame) {
        plm_frame_t* f = plm_decode_video(p);
        if (!f) break;  // the stream ended: the last picture stays
        ++frames_decoded_;
        shown = f;
    }
    if (!shown) return false;
    picture_time_ = shown->time;
    plm_frame_to_rgba(shown, rgba_.data(), static_cast<int>(info_.width * 4u));
    return true;
}

u32 VideoDecoder::count_frames() {
    if (!plm_ || rgba_.empty()) return 0;
    plm_t* p = as_plm(plm_);
    plm_rewind(p);
    u32 n = 0;
    while (plm_decode_video(p)) ++n;
    plm_rewind(p);
    picture_time_ = -1.0;
    return n;
}

bool VideoDecoder::decode_audio(Array<f32>& out) {
    out.clear();
    if (!plm_ || info_.audio_streams == 0) return false;
    plm_t* p = as_plm(plm_);
    while (plm_samples_t* s = plm_decode_audio(p)) {
        const usize at = out.size();
        out.resize(at + static_cast<usize>(s->count) * 2u);
        for (usize i = 0; i < static_cast<usize>(s->count) * 2u; ++i) out[at + i] = s->interleaved[i];
    }
    return !out.empty();
}

}  // namespace mce::render
