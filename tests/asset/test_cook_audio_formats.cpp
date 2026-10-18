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

namespace {

u8 crc8(const std::string& s) {
    u8 c = 0;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; ++k) c = u8((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1));
    }
    return c;
}
u16 crc16(const std::string& s) {
    u16 c = 0;
    for (unsigned char ch : s) {
        c = u16(c ^ (u16(ch) << 8));
        for (int k = 0; k < 8; ++k) c = u16((c & 0x8000) ? ((c << 1) ^ 0x8005) : (c << 1));
    }
    return c;
}

// A FLAC whose frames are all CONSTANT subframes (a few bytes of file per 65535 frames): `blocks` blocks of 65535 frames,
// mono 8-bit at `rate`, or with `channels` declared in STREAMINFO. Only the first frame header carries channels = 1.
std::string tiny_flac(u32 rate, u32 channels, u32 blocks) {
    constexpr u32 kBlock = 65535;
    std::string s = "fLaC";
    s.push_back(char(0x80));  // last metadata block, STREAMINFO
    s.push_back(0), s.push_back(0), s.push_back(34);
    s.push_back(char(kBlock >> 8)), s.push_back(char(kBlock & 255));  // min block
    s.push_back(char(kBlock >> 8)), s.push_back(char(kBlock & 255));  // max block
    for (int i = 0; i < 6; ++i) s.push_back(0);                       // frame sizes unknown
    const u64 total = u64(blocks) * kBlock;
    const u64 packed = (u64(rate) << 44) | (u64(channels - 1) << 41) | (u64(7) << 36) | (total & 0xFFFFFFFFFull);
    for (int i = 7; i >= 0; --i) s.push_back(char((packed >> (i * 8)) & 255));
    for (int i = 0; i < 16; ++i) s.push_back(0);  // MD5 unknown
    for (u32 n = 0; n < blocks; ++n) {
        std::string f;
        f.push_back(char(0xFF)), f.push_back(char(0xF8));  // sync, fixed block size
        f.push_back(char(0x70));                           // block size: 16-bit value follows; rate from STREAMINFO
        f.push_back(0);                                    // mono, size from STREAMINFO
        if (n < 128) f.push_back(char(n));
        else if (n < 2048) f.push_back(char(0xC0 | (n >> 6))), f.push_back(char(0x80 | (n & 63)));
        else f.push_back(char(0xE0 | (n >> 12))), f.push_back(char(0x80 | ((n >> 6) & 63))), f.push_back(char(0x80 | (n & 63)));
        f.push_back(char((kBlock - 1) >> 8)), f.push_back(char((kBlock - 1) & 255));
        f.push_back(char(crc8(f)));
        f.push_back(0);  // subframe: CONSTANT, no wasted bits
        f.push_back(char(0x11));
        const u16 c = crc16(f);
        f.push_back(char(c >> 8)), f.push_back(char(c & 255));
        s += f;
    }
    return s;
}

}  // namespace

TEST_CASE("the tiny FLAC writer makes a stream the decoder reads") {
    PcmClip c;
    FixedString<128> why;
    const std::string f = tiny_flac(8000, 1, 3);
    INFO(why.c_str());
    REQUIRE(cook::decode_flac(span_of(f), c, why));
    CHECK(c.frames() == 3 * 65535);
    CHECK(c.channels == 1);
}

TEST_CASE("a 1 kHz FLAC of a few KB is refused: the cap counts the clip after the resample to 48 kHz") {
    // 2,048 blocks x 65,535 frames = 134M source frames at 1 kHz would be 6.4G samples at 48 kHz (and 37 hours).
    const std::string f = tiny_flac(1000, 1, 2048);
    CHECK(f.size() < 64 * 1024);
    PcmClip c;
    FixedString<128> why;
    CHECK_FALSE(cook::decode_flac(span_of(f), c, why));
    CHECK(c.samples.size() <= cook::kAudioMaxSamples);
    const Cooked cooked = cook_clip("aud_flac_slow", "t.flac", f);
    CHECK_FALSE(cooked.ok);
}

TEST_CASE("a long clip at 48 kHz is refused at the sample cap, a clip just under it is read") {
    PcmClip c;
    FixedString<128> why;
    const std::string under = tiny_flac(48000, 1, 4);
    CHECK(cook::decode_flac(span_of(under), c, why));
    // 2,049 blocks of 65,535 frames = 134.3M frames at 48 kHz: just over 128 Mi samples (2,048 blocks is 2,048 frames under)
    const std::string over = tiny_flac(48000, 1, 2049);
    CHECK_FALSE(cook::decode_flac(span_of(over), c, why));
}

TEST_CASE("channel counts and sample rates outside what the cook takes are refused") {
    PcmClip c;
    FixedString<128> why;
    CHECK_FALSE(cook::decode_flac(span_of(tiny_flac(44100, 3, 1)), c, why));
    CHECK_FALSE(cook::decode_flac(span_of(tiny_flac(500, 1, 1)), c, why));
}

TEST_CASE("an unreadable clip is a cook error naming the file") {
    const Cooked c = cook_clip("aud_bad_mp3", "t.mp3", std::string(2000, 'x'));
    CHECK_FALSE(c.ok);
    CHECK(c.diags.find("t.mp3") != std::string::npos);
    const Cooked f = cook_clip("aud_bad_flac", "t.flac", std::string(2000, 'x'));
    CHECK_FALSE(f.ok);
}
