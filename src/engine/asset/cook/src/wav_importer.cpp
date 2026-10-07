// WAV -> CHNK_SFX: 48 kHz float32 interleaved, peak-normalized to -1 dBFS (assets.md §5.2).
//
// sidecar settings: { "normalize": true, "loop": false, "loop_start": 0, "loop_end": 0 }
//   loop_start / loop_end  source-rate frames, [start, end); default to the file's 'smpl' loop.
//   Loop points are validated at cook (end > start, inside the clip) so gapless music is a data
//   guarantee rather than a mixer hope (FR-AST-100).
// Long-form streaming pages (CHNK_AUDP) and the IMA-ADPCM variant come with the mixer (M3/v0.5).

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/cook/audio.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.audio";

class WavImporter final : public IImporter {
public:
    StringView id() const override { return "wav"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Sfx; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".wav"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue s = in.settings;
        const StringView file = in.display_path;
        if (!detail::check_keys(s, {"normalize", "loop", "loop_start", "loop_end"}, file, kStage, diags)) return false;

        PcmClip clip;
        FixedString<128> why;
        if (!decode_wav(in.source, clip, why)) {
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
        asset::write_sfx(h, resampled.span(), payload);
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Sfx;
        d.version = asset::chunk_version(asset::SFX_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_wav_importer() { return mem::make_unique<WavImporter>(mem::heap()); }

}  // namespace mce::cook
