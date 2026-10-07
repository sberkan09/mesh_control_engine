#include "mce/cook/audio.h"

#include <cmath>
#include <cstring>

namespace mce::cook {

namespace {

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }

bool fail(FixedString<128>& why, const char* msg) {
    why.assign(msg);
    return false;
}

constexpr u16 kFormatPcm = 1;
constexpr u16 kFormatFloat = 3;
constexpr u16 kFormatExtensible = 0xFFFE;

}  // namespace

bool decode_wav(Span<const byte> bytes, PcmClip& out, FixedString<128>& why) {
    const u8* d = reinterpret_cast<const u8*>(bytes.data());
    const usize n = bytes.size();
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return fail(why, "not a RIFF/WAVE file");

    u16 format = 0, channels = 0, bits = 0, block_align = 0;
    u32 rate = 0;
    const u8* data = nullptr;
    usize data_size = 0;
    bool have_fmt = false;
    out = PcmClip{};

    usize pos = 12;
    while (pos + 8 <= n) {
        const u8* ck = d + pos;
        const u32 size = rd32(ck + 4);
        const usize body = pos + 8;
        if (size > n - body) return fail(why, "chunk extends past end of file");
        if (std::memcmp(ck, "fmt ", 4) == 0) {
            if (size < 16) return fail(why, "fmt chunk too small");
            format = rd16(ck + 8);
            channels = rd16(ck + 10);
            rate = rd32(ck + 12);
            block_align = rd16(ck + 20);
            bits = rd16(ck + 22);
            if (format == kFormatExtensible) {
                if (size < 40) return fail(why, "extensible fmt chunk too small");
                format = rd16(ck + 8 + 24);  // first two bytes of the SubFormat GUID
            }
            have_fmt = true;
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data = ck + 8;
            data_size = size;
        } else if (std::memcmp(ck, "smpl", 4) == 0 && size >= 36) {
            const u32 loops = rd32(ck + 8 + 28);
            if (loops > 0 && size >= 36 + 24) {
                const u8* loop = ck + 8 + 36;
                out.loop_start = rd32(loop + 8);
                out.loop_end = static_cast<u64>(rd32(loop + 12)) + 1;  // smpl end is inclusive
                out.has_loop = true;
            }
        }
        pos = body + size + (size & 1u);  // chunks are word-aligned
    }
    if (!have_fmt) return fail(why, "missing fmt chunk");
    if (!data) return fail(why, "missing data chunk");
    if (channels == 0 || channels > 2) return fail(why, "only mono and stereo are supported");
    if (rate < 1000 || rate > 384000) return fail(why, "sample rate out of range");
    const bool is_float = format == kFormatFloat;
    if (format != kFormatPcm && !is_float) return fail(why, "unsupported WAV encoding (PCM or IEEE float only)");
    if (is_float ? (bits != 32 && bits != 64) : (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return fail(why, "unsupported bit depth");
    const u32 bytes_per_sample = bits / 8u;
    if (block_align != bytes_per_sample * channels) return fail(why, "inconsistent block alignment");

    const usize frames = data_size / block_align;
    out.sample_rate = rate;
    out.channels = channels;
    out.samples.resize_uninitialized(frames * channels);
    for (usize i = 0; i < frames * channels; ++i) {
        const u8* p = data + i * bytes_per_sample;
        f32 v;
        if (is_float) {
            if (bits == 32) {
                f32 f;
                std::memcpy(&f, p, 4);
                v = f;
            } else {
                f64 f;
                std::memcpy(&f, p, 8);
                v = static_cast<f32>(f);
            }
            if (!std::isfinite(v)) return fail(why, "non-finite float sample");
        } else if (bits == 8) {
            v = static_cast<f32>(static_cast<int>(p[0]) - 128) / 128.0f;
        } else if (bits == 16) {
            v = static_cast<f32>(static_cast<i16>(rd16(p))) / 32768.0f;
        } else if (bits == 24) {
            const i32 s = static_cast<i32>((static_cast<u32>(p[0]) << 8) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 24)) >> 8;
            v = static_cast<f32>(s) / 8388608.0f;
        } else {
            v = static_cast<f32>(static_cast<f64>(static_cast<i32>(rd32(p))) / 2147483648.0);
        }
        out.samples[i] = v;
    }
    if (out.has_loop && !(out.loop_start < out.loop_end && out.loop_end <= frames)) {
        out.has_loop = false;  // a malformed smpl loop is ignored; explicit sidecar loops are validated strictly
    }
    return true;
}

u64 rescale_frames(u64 frames, u32 src_rate, u32 dst_rate) {
    return (frames * dst_rate + src_rate / 2) / src_rate;
}

void resample(Span<const f32> in, u32 channels, u32 src_rate, u32 dst_rate, Array<f32>& out) {
    out.clear();
    if (channels == 0) return;
    const u64 in_frames = in.size() / channels;
    if (src_rate == dst_rate) {
        out.resize_uninitialized(in.size());
        if (!in.empty()) std::memcpy(out.data(), in.data(), in.size_bytes());
        return;
    }
    constexpr int kHalf = 16;  // 32 taps: k in [-15, 16]
    constexpr f64 kPi = 3.14159265358979323846;
    const f64 fc = src_rate > dst_rate ? static_cast<f64>(dst_rate) / src_rate : 1.0;  // cutoff, in input Nyquist units
    const u64 out_frames = (in_frames * dst_rate + src_rate - 1) / src_rate;
    out.resize_uninitialized(out_frames * channels);

    f64 w[2 * kHalf];
    for (u64 n = 0; n < out_frames; ++n) {
        const u64 num = n * src_rate;
        const i64 base = static_cast<i64>(num / dst_rate);
        const f64 frac = static_cast<f64>(num % dst_rate) / dst_rate;
        f64 wsum = 0.0;
        for (int k = -kHalf + 1; k <= kHalf; ++k) {
            const f64 x = k - frac;  // distance from the output instant, input samples
            f64 s = 1.0;
            if (x != 0.0) s = std::sin(kPi * fc * x) / (kPi * fc * x);
            const f64 t = x / kHalf;
            const f64 win = std::fabs(t) < 1.0 ? 0.42 + 0.5 * std::cos(kPi * t) + 0.08 * std::cos(2.0 * kPi * t) : 0.0;
            w[k + kHalf - 1] = s * win;
            wsum += w[k + kHalf - 1];
        }
        for (u32 c = 0; c < channels; ++c) {
            f64 acc = 0.0;
            for (int k = -kHalf + 1; k <= kHalf; ++k) {
                const i64 j = base + k;
                if (j < 0 || j >= static_cast<i64>(in_frames)) continue;
                acc += w[k + kHalf - 1] * in[static_cast<usize>(j) * channels + c];
            }
            out[n * channels + c] = static_cast<f32>(wsum != 0.0 ? acc / wsum : 0.0);  // unity DC gain
        }
    }
}

f32 normalize_peak(Span<f32> samples, f32 target) {
    f32 peak = 0.0f;
    for (f32 s : samples) peak = std::fabs(s) > peak ? std::fabs(s) : peak;
    if (peak == 0.0f) return 1.0f;
    const f32 gain = target / peak;
    for (f32& s : samples) s *= gain;
    return gain;
}

}  // namespace mce::cook
