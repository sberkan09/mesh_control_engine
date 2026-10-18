// MP3 and FLAC clips (M-IMPORTMEDIA 2): FLAC decodes to the samples of the WAV it was made from, MP3 to the same tone
// within the codec's tolerance; both cook to a 48 kHz CHNK_SFX (FLAC as PCM, MP3 as Vorbis by default, "compress"
// overrides); damaged and hostile files are cook errors or clips, never crashes or runaway allocations.
//
// Fixtures in tests/asset/data/audio, made with ffmpeg from one 0.25 s, 44.1 kHz stereo 440 Hz tone:
//   ffmpeg -f lavfi -i "sine=frequency=440:duration=0.25:sample_rate=44100" -af "pan=stereo|c0=c0|c1=0.5*c0" -c:a pcm_s16le tone.wav
//   ffmpeg -i tone.wav -c:a flac -compression_level 8 tone.flac
//   ffmpeg -i tone.wav -c:a libmp3lame -b:a 64k -ar 44100 tone.mp3

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "../../src/engine/asset/cook/src/audio_decode.h"
#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::CookOptions;
using cook::CookService;
using cook::PcmClip;

namespace {

constexpr const char* kClip = "e3e3e3e3-0000-4000-8000-000000000001";

std::string fixture(const char* name) { return read_file(std::string(MCE_TEST_DATA_DIR) + "/audio/" + name); }

Span<const byte> span_of(const std::string& s) { return Span<const byte>(reinterpret_cast<const byte*>(s.data()), s.size()); }

double rms(const PcmClip& c) {
    double acc = 0;
    for (usize i = 0; i < c.samples.size(); ++i) acc += double(c.samples[i]) * double(c.samples[i]);
    return std::sqrt(acc / double(c.samples.size()));
}

struct Cooked {
    bool ok = false;
    std::string diags;
    u32 rate = 0, channels = 0;
    u64 frames = 0;
    bool vorbis = false;
};

Cooked cook_clip(const char* name, const char* file, const std::string& bytes, const char* settings = "{}") {
    Cooked c;
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/" + file, bytes);
    write_file(root + "/assets/" + file + ".mceasset", sidecar(kClip, std::string(file).substr(std::string(file).rfind('.') + 1).c_str(), settings));
    CookService svc;
    const cook::CookReport r = svc.cook(root.c_str(), CookOptions{});
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        c.diags += line.c_str();
        c.diags += "\n";
    }
    c.ok = r.ok();
    if (!c.ok) return c;
    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {cooked};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto h = as.load_sync<Sfx>(parse_guid(kClip).value());
    REQUIRE(h);
    auto v = SfxView::read(as.resolve(h.value()));
    REQUIRE(v);
    c.rate = v.value().header->sample_rate;
    c.channels = v.value().header->channels;
    c.frames = v.value().header->frame_count;
    c.vorbis = !v.value().vorbis.empty();
    as.release(AnyHandle(h.value()));
    return c;
}

}  // namespace

TEST_CASE("FLAC decodes to the samples of the WAV it came from") {
    const std::string wav = fixture("tone.wav"), flac = fixture("tone.flac");
    PcmClip a, b;
    FixedString<128> why;
    REQUIRE(cook::decode_wav(span_of(wav), a, why));
    REQUIRE(cook::decode_flac(span_of(flac), b, why));
    CHECK(b.channels == 2);
    CHECK(b.sample_rate == 44100);
    REQUIRE(b.samples.size() == a.samples.size());
    double worst = 0;
    for (usize i = 0; i < a.samples.size(); ++i) worst = std::max(worst, std::fabs(double(a.samples[i]) - double(b.samples[i])));
    CHECK(worst <= 1.0 / 32768.0);  // lossless 16-bit
}

TEST_CASE("MP3 decodes to the same tone within the codec's tolerance") {
    const std::string wav = fixture("tone.wav"), mp3 = fixture("tone.mp3");
    PcmClip a, b;
    FixedString<128> why;
    REQUIRE(cook::decode_wav(span_of(wav), a, why));
    REQUIRE(cook::decode_mp3(span_of(mp3), b, why));
    CHECK(b.channels == 2);
    CHECK(b.sample_rate == 44100);
    CHECK(b.frames() > a.frames() - 100);   // an encoder may add (but here trims) priming frames
    CHECK(b.frames() < a.frames() + 2500);
    CHECK(rms(b) == doctest::Approx(rms(a)).epsilon(0.15));
}

TEST_CASE("MP3 and FLAC cook to a 48 kHz clip; MP3 is stored as Vorbis, FLAC as PCM, \"compress\" overrides") {
    const Cooked flac = cook_clip("aud_flac", "t.flac", fixture("tone.flac"));
    INFO(flac.diags);
    REQUIRE(flac.ok);
    CHECK(flac.rate == 48000);
    CHECK(flac.channels == 2);
    CHECK(flac.frames == doctest::Approx(12000.0).epsilon(0.01));
    CHECK_FALSE(flac.vorbis);

    const Cooked mp3 = cook_clip("aud_mp3", "t.mp3", fixture("tone.mp3"));
    INFO(mp3.diags);
    REQUIRE(mp3.ok);
    CHECK(mp3.rate == 48000);
    CHECK(mp3.vorbis);

    const Cooked mp3_pcm = cook_clip("aud_mp3_pcm", "t.mp3", fixture("tone.mp3"), "{ \"compress\": \"pcm\" }");
    REQUIRE(mp3_pcm.ok);
    CHECK_FALSE(mp3_pcm.vorbis);

    const Cooked flac_vorbis = cook_clip("aud_flac_vorbis", "t.flac", fixture("tone.flac"), "{ \"compress\": \"vorbis\" }");
    REQUIRE(flac_vorbis.ok);
    CHECK(flac_vorbis.vorbis);
}

TEST_CASE("damaged MP3 and FLAC files never crash: an error or a clip") {
    for (const char* name : {"tone.flac", "tone.mp3"}) {
        const std::string good = fixture(name);
        const bool flac = std::string(name) == "tone.flac";
        // every truncation length on a stride, and a byte flip on a stride
        for (size_t cut = 0; cut < good.size(); cut += good.size() / 97 + 1) {
            PcmClip c;
            FixedString<128> why;
            const std::string part = good.substr(0, cut);
            (void)(flac ? cook::decode_flac(span_of(part), c, why) : cook::decode_mp3(span_of(part), c, why));
        }
        for (size_t at = 0; at < good.size(); at += good.size() / 211 + 1) {
            std::string bad = good;
            bad[at] = char(bad[at] ^ 0x5A);
            PcmClip c;
            FixedString<128> why;
            (void)(flac ? cook::decode_flac(span_of(bad), c, why) : cook::decode_mp3(span_of(bad), c, why));
        }
    }
    PcmClip c;
    FixedString<128> why;
    CHECK_FALSE(cook::decode_flac(span_of(std::string()), c, why));
    CHECK_FALSE(cook::decode_mp3(span_of(std::string()), c, why));
    const std::string junk(4096, '\x7f');
    CHECK_FALSE(cook::decode_flac(span_of(junk), c, why));
    CHECK_FALSE(cook::decode_mp3(span_of(junk), c, why));
}

TEST_CASE("a FLAC that claims billions of frames is read for the frames it has") {
    std::string flac = fixture("tone.flac");
    REQUIRE(flac.size() > 42);
    // STREAMINFO (after "fLaC" and the 4-byte block header): the 36-bit total sample count sits in bytes 13..17 of the
    // block; set it to its maximum.
    const size_t info = 8;
    flac[info + 13] = char(flac[info + 13] | 0x0F);
    for (size_t i = 14; i <= 17; ++i) flac[info + i] = char(0xFF);
    PcmClip c;
    FixedString<128> why;
    if (cook::decode_flac(span_of(flac), c, why)) CHECK(c.frames() <= cook::kAudioMaxFrames);
}

TEST_CASE("an unreadable clip is a cook error naming the file") {
    const Cooked c = cook_clip("aud_bad_mp3", "t.mp3", std::string(2000, 'x'));
    CHECK_FALSE(c.ok);
    CHECK(c.diags.find("t.mp3") != std::string::npos);
    const Cooked f = cook_clip("aud_bad_flac", "t.flac", std::string(2000, 'x'));
    CHECK_FALSE(f.ok);
}
