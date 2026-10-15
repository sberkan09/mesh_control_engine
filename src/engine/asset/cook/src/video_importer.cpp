// MPEG-1 (.mpg) -> CHNK_VID (M-CINEMA 5, ADR-0043): the program stream's bytes as they are, checked
// by opening them with render::VideoDecoder, and its MP2 audio decoded once to 48 kHz f32 so the
// mixer plays it like any clip. Encode a source with:
//   ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -f mpeg out.mpg
//
// sidecar settings: { "audio": true, "normalize": false }
//   audio      keep the sound track (false drops it: a silent video costs no PCM)
//   normalize  peak-normalize the sound to -1 dBFS

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/cook/audio.h"
#include "mce/render/video.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.video";
constexpr u32 kMaxSide = 1920;           // the decoder's frame stays a texture every backend takes
// Above this a Low-tier phone may miss its frame budget: decode + RGBA convert measured 1.1 ms a frame
// at 640x360 and 4.0 ms at 1280x720 on one desktop core (ADR-0043); a phone core is 3-5x slower.
constexpr u32 kLowTierPixels = 854 * 480;

class VideoImporter final : public IImporter {
public:
    StringView id() const override { return "video"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Video; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".mpg" || ext == ".mpeg"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue s = in.settings;
        const StringView file = in.display_path;
        if (!detail::check_keys(s, {"audio", "normalize"}, file, kStage, diags)) return false;
        bool audio = true, normalize = false;
        if (!detail::read_bool(s, "audio", true, audio, file, kStage, diags)) return false;
        if (!detail::read_bool(s, "normalize", false, normalize, file, kStage, diags)) return false;

        render::VideoDecoder dec;
        if (!dec.open(in.source)) {
            diags.error(Errc::ImportFailed, file, 0, 0, kStage,
                        "not an MPEG-1 program stream with a video stream (encode with: ffmpeg -i in.mp4 -c:v mpeg1video "
                        "-q:v 4 -c:a mp2 -f mpeg out.mpg)");
            return false;
        }
        const u32 frames = dec.count_frames();
        const render::VideoInfo info = dec.info();
        if (info.width > kMaxSide || info.height > kMaxSide || !(info.frame_rate > 0.0f)) {
            String msg;
            appendf(msg, "video is %ux%u at %.2f fps: the engine plays up to %ux%u", info.width, info.height,
                    static_cast<f64>(info.frame_rate), kMaxSide, kMaxSide);
            diags.error(Errc::ImportFailed, file, 0, 0, kStage, msg);
            return false;
        }
        if (frames == 0) {
            diags.error(Errc::ImportFailed, file, 0, 0, kStage, "the video stream holds no picture");
            return false;
        }
        if (info.width * info.height > kLowTierPixels) {
            String msg;
            appendf(msg, "video is %ux%u: above 854x480 its decode may not fit a Low-tier phone's frame budget", info.width,
                    info.height);
            diags.warning(file, 0, 0, kStage, msg);
        }

        Array<f32> pcm;
        f32 gain = 1.0f;
        if (audio && info.audio_streams > 0) {
            render::VideoDecoder sound;
            Array<f32> source;
            if (sound.open(in.source, true) && sound.decode_audio(source) && sound.info().sample_rate > 0) {
                resample(source.span(), 2, sound.info().sample_rate, kCookSampleRate, pcm);
                if (normalize) gain = normalize_peak(pcm.span(), kMinus1dBFS);
            } else {
                diags.warning(file, 0, 0, kStage, "the sound track could not be decoded; the video cooks silent");
            }
        }
        if (pcm.size() / 2 > 0xFFFFFFFFull) pcm.clear();

        asset::VideoHeader h{};
        h.width = info.width;
        h.height = info.height;
        h.frame_rate = info.frame_rate;
        h.duration = static_cast<f32>(frames) / info.frame_rate;
        h.audio_channels = pcm.empty() ? 0u : 2u;
        h.audio_frames = static_cast<u32>(pcm.size() / 2);
        h.audio_gain = gain;
        Array<byte> payload;
        asset::write_video(h, in.source, pcm.span(), payload);
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Video;
        d.version = asset::chunk_version(asset::VIDEO_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_video_importer() { return mem::make_unique<VideoImporter>(mem::heap()); }

}  // namespace mce::cook
