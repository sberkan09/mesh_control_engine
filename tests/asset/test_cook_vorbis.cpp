// M-AUDIO2 1 (ADR-0044): compressed clips. The cook encodes Ogg Vorbis (libvorbis) for "compress":
// "vorbis" and for .ogg sources; asset::VorbisReader (stb_vorbis) decodes it within a set error of the
// source; the ClipBank decodes an in-memory clip at load, and a streamed one decodes page by page,
// sample for sample the same as the whole-clip decode, across the loop seam.

#include <doctest/doctest.h>

#include <cmath>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/asset/vorbis.h"
#include "mce/audio/clip_bank.h"
#include "mce/cook/audio.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr const char* kPcm = "a0d10000-0000-4000-8000-0000000000c1";
constexpr const char* kSqueezed = "a0d10000-0000-4000-8000-0000000000c2";
constexpr const char* kOgg = "a0d10000-0000-4000-8000-0000000000c3";

std::string report(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

// Two tones per channel at 48 kHz: something a codec has to work for.
Array<f32> tones(u32 frames) {
    Array<f32> a;
    a.resize(static_cast<usize>(frames) * 2);
    for (u32 i = 0; i < frames; ++i) {
        const f64 t = static_cast<f64>(i) / 48000.0;
        a[i * 2] = static_cast<f32>(0.4 * std::sin(2 * 3.14159265358979 * 440 * t) + 0.2 * std::sin(2 * 3.14159265358979 * 1250 * t));
        a[i * 2 + 1] = static_cast<f32>(0.3 * std::sin(2 * 3.14159265358979 * 660 * t) + 0.1 * std::sin(2 * 3.14159265358979 * 3100 * t));
    }
    return a;
}

f64 snr_db(Span<const f32> ref, Span<const f32> got) {
    f64 sig = 0, err = 0;
    for (usize i = 0; i < ref.size(); ++i) {
        sig += static_cast<f64>(ref[i]) * ref[i];
        const f64 d = static_cast<f64>(ref[i]) - got[i];
        err += d * d;
    }
    return 10.0 * std::log10(sig / (err > 1e-30 ? err : 1e-30));
}

std::string as_string(const Array<byte>& b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

mem::UniquePtr<audio::AudioDevice> offline_device() {
    audio::AudioDesc d;
    d.backend = audio::Backend::Offline;
    auto r = audio::AudioDevice::create(d);
    REQUIRE(r.has_value());
    return std::move(r).value();
}

}  // namespace

TEST_CASE("vorbis: encode then decode stays within the error bound, a tenth of the size, same bytes twice") {
    const Array<f32> src = tones(48000);
    Array<byte> a, b;
    FixedString<128> why;
    REQUIRE_MESSAGE(cook::encode_vorbis(src.span(), 2, 48000, 0.4f, a, why), why.c_str());
    REQUIRE(cook::encode_vorbis(src.span(), 2, 48000, 0.4f, b, why));
    CHECK(as_string(a) == as_string(b));
    MESSAGE("1 s stereo: " << a.size() << " bytes of Vorbis for " << src.size() * sizeof(f32) << " of float PCM");
    CHECK(a.size() * 10 < src.size() * sizeof(f32));

    Array<f32> out;
    u32 channels = 0, rate = 0;
    REQUIRE(asset::decode_vorbis(a.span(), out, channels, rate));
    CHECK(channels == 2);
    CHECK(rate == 48000);
    REQUIRE(out.size() == src.size());  // sample-exact length: the granule position trims the tail
    const f64 snr = snr_db(src.span(), out.span());
    MESSAGE("SNR " << snr << " dB");
    CHECK(snr > 20.0);

    // Seek lands on the exact sample the whole decode has there.
    asset::VorbisReader r;
    REQUIRE(r.open(a.span()));
    CHECK(r.frames() == 48000);
    REQUIRE(r.seek(31'337));
    f32 frame[8 * 2];
    REQUIRE(r.read(frame, 8) == 8);
    for (u32 i = 0; i < 16; ++i) CHECK(frame[i] == doctest::Approx(out[31'337 * 2 + i]).epsilon(1e-5));

    const byte junk[64] = {};
    CHECK_FALSE(r.open(Span<const byte>(junk, sizeof(junk))));
    CHECK_FALSE(r.is_open());
}

TEST_CASE("vorbis: WAV with compress vorbis, and .ogg sources, cook to Vorbis Sfx chunks") {
    const std::string root = fresh_dir("vorbis_cook");
    const std::string wav = make_wav(44100, 2, 44100, 440.0, 0.5, 0, 44100);  // 1 s, looping over all of it
    write_file(root + "/assets/sfx/pcm.wav", wav);
    write_file(root + "/assets/sfx/pcm.wav.mceasset", sidecar(kPcm, "wav"));
    write_file(root + "/assets/sfx/small.wav", wav);
    write_file(root + "/assets/sfx/small.wav.mceasset", sidecar(kSqueezed, "wav", R"({ "compress": "vorbis", "quality": 0.2 })"));
    Array<byte> ogg;
    FixedString<128> why;
    const Array<f32> src = tones(24000);
    REQUIRE(cook::encode_vorbis(src.span(), 2, 48000, 0.6f, ogg, why));
    write_file(root + "/assets/music/theme.ogg", as_string(ogg));
    write_file(root + "/assets/music/theme.ogg.mceasset", sidecar(kOgg, "ogg"));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(report(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    auto load = [&](const char* guid) {
        auto h = assets.load_sync<asset::Sfx>(asset::parse_guid(guid).value());
        REQUIRE(h);
        auto v = asset::SfxView::read(assets.resolve(h.value()));
        REQUIRE(v);
        return v.value();
    };
    const asset::SfxView pcm = load(kPcm);
    const asset::SfxView small = load(kSqueezed);
    const asset::SfxView theme = load(kOgg);
    CHECK((pcm.header->flags & asset::SfxFlags::Vorbis) == 0);
    CHECK((small.header->flags & asset::SfxFlags::Vorbis) != 0);
    CHECK((small.header->flags & asset::SfxFlags::Looping) != 0);
    CHECK(small.header->frame_count == pcm.header->frame_count);
    CHECK(small.header->loop_end == pcm.header->loop_end);
    CHECK(small.samples.empty());
    CHECK(small.vorbis.size() * 10 < pcm.samples.size() * sizeof(f32));

    // The compressed clip decodes to the PCM one within the bound.
    Array<f32> decoded;
    u32 channels = 0, rate = 0;
    REQUIRE(asset::decode_vorbis(small.vorbis, decoded, channels, rate));
    REQUIRE(decoded.size() == pcm.samples.size());
    CHECK(snr_db(pcm.samples, decoded.span()) > 20.0);

    // .ogg defaults to Vorbis and is re-encoded at 48 kHz, normalized to -1 dBFS.
    CHECK((theme.header->flags & asset::SfxFlags::Vorbis) != 0);
    CHECK(theme.header->channels == 2);
    CHECK(theme.header->frame_count == 24000);
    REQUIRE(asset::decode_vorbis(theme.vorbis, decoded, channels, rate));
    f32 peak = 0;
    for (f32 s : decoded) peak = std::fabs(s) > peak ? std::fabs(s) : peak;
    CHECK(peak == doctest::Approx(0.891f).epsilon(0.05));

    // Bad settings are errors naming the key.
    const std::string bad = fresh_dir("vorbis_cook_bad");
    write_file(bad + "/assets/sfx/a.wav", wav);
    write_file(bad + "/assets/sfx/a.wav.mceasset", sidecar(kPcm, "wav", R"({ "compress": "mp3" })"));
    const cook::CookReport rb = svc.cook(StringView(bad.c_str()), cook::CookOptions{});
    CHECK_FALSE(rb.ok());
    CHECK(report(rb).find("compress") != std::string::npos);
}

TEST_CASE("vorbis: the ClipBank decodes at load; a stream decodes page by page, the same samples across the loop") {
    const std::string root = fresh_dir("vorbis_play");
    // 70 001 frames, not a page multiple, looping from 12 345: the seam lands mid-page and forces a seek.
    const std::string wav = make_wav(48000, 2, 70'001, 330.0, 0.5, 12'345, 70'001);
    write_file(root + "/assets/music/loop.wav", wav);
    write_file(root + "/assets/music/loop.wav.mceasset", sidecar(kSqueezed, "wav", R"({ "compress": "vorbis" })"));
    write_file(root + "/assets/music/loop2.wav", wav);
    write_file(root + "/assets/music/loop2.wav.mceasset", sidecar(kOgg, "wav", R"({ "compress": "vorbis" })"));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(report(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::Guid guid = asset::parse_guid(kSqueezed).value();

    // The reference: the whole clip decoded once.
    auto h = assets.load_sync<asset::Sfx>(guid);
    REQUIRE(h);
    const auto payload = audio::clip_payload_from_sfx(assets.resolve(h.value()));
    REQUIRE(payload);
    CHECK(payload.value().frames == nullptr);
    CHECK(payload.value().encoded_bytes > 0);
    CHECK(payload.value().frame_count == 70'001);
    CHECK(payload.value().loop_start == 12'345);
    Array<f32> ref;
    u32 channels = 0, rate = 0;
    REQUIRE(asset::decode_vorbis(Span<const byte>(payload.value().encoded, payload.value().encoded_bytes), ref, channels, rate));

    auto dev = offline_device();
    CHECK_FALSE(dev->register_clip(payload.value()));  // an encoded clip streams, or the bank decodes it

    audio::ClipBank bank(assets, *dev);
    const audio::ClipHandle mem_clip = bank.acquire(guid);
    REQUIRE_FALSE(mem_clip.is_null());
    CHECK_FALSE(dev->is_stream(mem_clip));
    const audio::ClipHandle streamed = bank.acquire(asset::parse_guid(kOgg).value(), true);
    REQUIRE_FALSE(streamed.is_null());
    CHECK(dev->is_stream(streamed));

    auto render_and_compare = [&](audio::ClipHandle clip, u32 seconds) {
        audio::PlayParams p;
        p.loop = true;
        p.priority = 0;
        p.bus = audio::Bus_Music;
        const audio::AudioHandle v = dev->play(clip, p);
        REQUIRE_FALSE(v.is_null());
        Array<f32> block;
        block.resize(480 * 2);
        f64 worst = 0;
        for (u32 done = 0; done < 48000 * seconds; done += 480) {
            dev->render_offline(block.data(), 480);
            dev->update();
            for (u32 i = 0; i < 480; ++i) {
                const u64 t = done + i;
                const u64 src = t < 70'001 ? t : 12'345 + (t - 70'001) % (70'001 - 12'345);
                for (u32 c = 0; c < 2; ++c) {
                    const f64 d = std::fabs(static_cast<f64>(block[i * 2 + c]) - ref[src * 2 + c]);
                    worst = d > worst ? d : worst;
                }
            }
        }
        dev->stop(v);
        dev->render_offline(block.data(), 480);
        dev->update();
        return worst;
    };
    const f64 mem_err = render_and_compare(mem_clip, 4);
    const f64 stream_err = render_and_compare(streamed, 4);
    MESSAGE("worst sample error: in memory " << mem_err << ", streamed " << stream_err);
    CHECK(mem_err == 0.0);
    CHECK(stream_err < 1e-4);
    CHECK(dev->stats().underruns == 0);

    bank.release_all();
    CHECK_FALSE(dev->is_clip_registered(mem_clip));
    assets.release(asset::AnyHandle(h.value()));
}
