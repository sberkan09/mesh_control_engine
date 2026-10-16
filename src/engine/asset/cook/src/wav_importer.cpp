// WAV / Ogg Vorbis -> CHNK_SFX: 48 kHz, peak-normalized to -1 dBFS (assets.md §5.2), stored as float32
// interleaved ("compress": "pcm") or re-encoded as Ogg Vorbis ("compress": "vorbis", M-AUDIO2 1).
//
// sidecar settings: { "normalize": true, "loop": false, "loop_start": 0, "loop_end": 0,
//                     "compress": "pcm" (.wav) | "vorbis" (.ogg), "quality": 0.4 }
//   loop_start / loop_end  source-rate frames, [start, end); default to the file's 'smpl' loop (WAV).
//   Loop points are validated at cook (end > start, inside the clip) so gapless music is a data
//   guarantee rather than a mixer hope (FR-AST-100).
//   compress  "vorbis" stores the resampled, normalized clip as Ogg Vorbis (about a tenth of the size);
//             the runtime decodes it at load, or page by page when the clip streams.
//   quality   Vorbis VBR quality, -0.1 .. 1 (0.4 is about 128 kbit/s stereo).

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/asset/vorbis.h"
#include "mce/cook/audio.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.audio";

bool decode_ogg(Span<const byte> bytes, PcmClip& out, FixedString<128>& why) {
    const Result<void> r = asset::decode_vorbis(bytes, out.samples, out.channels, out.sample_rate);
    if (!r) {
        why = "not a readable Ogg Vorbis file";
        return false;
    }
    if (out.channels < 1 || out.channels > 2 || out.sample_rate == 0 || out.samples.empty()) {
        why = "Ogg Vorbis: need 1 or 2 channels and at least one frame";
        return false;
    }
    return true;
}

// One class for both source formats: `ogg` picks the decoder and the default compression.
class ClipImporter final : public IImporter {
public:
    explicit ClipImporter(bool ogg) : ogg_(ogg) {}
    StringView id() const override { return ogg_ ? "ogg" : "wav"; }
    u32 version() const override { return 2; }  // 2: "compress" / "quality" (M-AUDIO2 1)
    ChunkType type() const override { return ChunkType::Sfx; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == (ogg_ ? ".ogg" : ".wav"); }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue s = in.settings;
        const StringView file = in.display_path;
        if (!detail::check_keys(s, {"normalize", "loop", "loop_start", "loop_end", "compress", "quality"}, file, kStage, diags))
            return false;

        bool vorbis = ogg_;
        if (const JsonValue c = s.get("compress"); c.valid()) {
            const StringView v = c.is_string() ? c.as_string() : StringView();
            if (v != "pcm" && v != "vorbis") {
                detail::error_at(diags, Errc::ImportFailed, file, c, kStage, "\"compress\" must be \"pcm\" or \"vorbis\"");
                return false;
            }
            vorbis = v == "vorbis";
        }
        f64 quality = 0.4;
        if (!detail::read_number(s, "quality", 0.4, quality, file, kStage, diags)) return false;
        if (quality < -0.1 || quality > 1.0) {
            detail::error_at(diags, Errc::ImportFailed, file, s.get("quality"), kStage, "\"quality\" must be in [-0.1, 1]");
            return false;
        }

        PcmClip clip;
        FixedString<128> why;
        if (!(ogg_ ? decode_ogg(in.source, clip, why) : decode_wav(in.source, clip, why))) {
            diags.error(Errc::ImportFailed, file, 0, 0, kStage, why.view());
            return false;
        }

        bool normalize = true, loop = clip.has_loop;
        f64 loop_start = static_cast<f64>(clip.loop_start), loop_end = static_cast<f64>(clip.loop_end);
        if (!detail::read_bool(s, "normalize", true, normalize, file, kStage, diags)) return false;
        if (!detail::read_bool(s, "loop", clip.has_loop, loop, file, kStage, diags)) return false;
        if (!detail::read_number(s, "loop_start", loop_start, loop_start, file, kStage, diags)) return false;
        if (!detail::read_number(s, "loop_end", loop_end, loop_end, file, kStage, diags)) return false;
        const u64 frames = clip.frames();
        if (loop && !s.get("loop_end").valid() && !clip.has_loop) loop_end = static_cast<f64>(frames);  // whole clip
        if (loop) {
            const bool integral = loop_start == static_cast<f64>(static_cast<u64>(loop_start)) &&
                                  loop_end == static_cast<f64>(static_cast<u64>(loop_end));
            if (!integral || loop_start < 0 || !(loop_start < loop_end) || loop_end > static_cast<f64>(frames)) {
                String msg;
                appendf(msg, "invalid loop points [%.0f, %.0f) for a clip of %llu frames (need 0 <= start < end <= frames)", loop_start,
                        loop_end, static_cast<unsigned long long>(frames));
                detail::error_at(diags, Errc::ImportFailed, file, s.get("loop_end").valid() ? s.get("loop_end") : s.get("loop_start"),
                                 kStage, msg);
                return false;
            }
        }

        Array<f32> resampled;
        resample(clip.samples.span(), clip.channels, clip.sample_rate, kCookSampleRate, resampled);
        const f32 gain = normalize ? normalize_peak(resampled.span(), kMinus1dBFS) : 1.0f;

        asset::SfxHeader h{};
        h.sample_rate = kCookSampleRate;
        h.channels = clip.channels;
        h.frame_count = resampled.size() / clip.channels;
        h.gain = gain;
        if (loop) {
            h.flags = asset::SfxFlags::Looping;
            h.loop_start = rescale_frames(static_cast<u64>(loop_start), clip.sample_rate, kCookSampleRate);
            h.loop_end = rescale_frames(static_cast<u64>(loop_end), clip.sample_rate, kCookSampleRate);
            if (h.loop_end > h.frame_count) h.loop_end = h.frame_count;
            if (h.loop_start >= h.loop_end) h.loop_start = h.loop_end - 1;
        }
        Array<byte> payload;
        if (vorbis) {
            Array<byte> stream;
            if (!encode_vorbis(resampled.span(), clip.channels, kCookSampleRate, static_cast<f32>(quality), stream, why)) {
                diags.error(Errc::ImportFailed, file, 0, 0, kStage, why.view());
                return false;
            }
            asset::write_sfx_vorbis(h, stream.span(), payload);
        } else {
            asset::write_sfx(h, resampled.span(), payload);
        }
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Sfx;
        d.version = asset::chunk_version(asset::SFX_VERSION, vorbis ? 1 : 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }

private:
    bool ogg_;
};

}  // namespace

mem::UniquePtr<IImporter> make_wav_importer() { return mem::make_unique<ClipImporter>(mem::heap(), false); }
mem::UniquePtr<IImporter> make_ogg_importer() { return mem::make_unique<ClipImporter>(mem::heap(), true); }

}  // namespace mce::cook
