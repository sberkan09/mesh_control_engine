// Cook-side JSON (positions, errors, canonical form) and audio processing (WAV decode,
// resampler accuracy, normalization).

#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/audio.h"
#include "mce/cook/json.h"

using namespace mce;
using namespace mce::cook;

TEST_CASE("json: values carry line and column") {
    JsonDoc doc;
    JsonError err;
    const char* text =
        "// sidecar\n"
        "{\n"
        "  \"guid\": \"abc\",\n"
        "  \"n\": [1, -2.5e2, true, null],\n"
        "  /* block */ \"o\": {\"k\": \"\\u00e9\\ud83d\\ude00\"}\n"
        "}\n";
    REQUIRE(doc.parse(text, err));
    const JsonValue root = doc.root();
    REQUIRE(root.is_object());
    CHECK(root.size() == 3);
    CHECK(root.get("guid").as_string() == "abc");
    CHECK(root.get("guid").line() == 3);
    CHECK(root.get("guid").col() == 11);
    const JsonValue n = root.get("n");
    REQUIRE(n.is_array());
    CHECK(n.at(1).as_number() == -250.0);
    CHECK(n.at(0).is_integer());
    CHECK(n.at(1).is_integer());  // -250 is integral
    CHECK(n.at(2).as_bool());
    CHECK(n.at(3).is_null());
    CHECK(n.at(3).line() == 4);
    CHECK(root.get("o").get("k").as_string() == "\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(!root.get("missing").valid());
}

TEST_CASE("json: syntax errors report position") {
    struct Case {
        const char* text;
        u32 line;
    };
    for (const Case& c : {Case{"{\"a\": 1,\n \"a\": 2}", 2}, Case{"{\"a\" 1}", 1}, Case{"[1, 2,\n\n]", 3}, Case{"{\"a\": tru}", 1},
                          Case{"\"unterminated", 1}, Case{"[01]", 1}, Case{"{} x", 1}, Case{"[\"\\ud800\"]", 1}}) {
        JsonDoc doc;
        JsonError err;
        CHECK_MESSAGE(!doc.parse(c.text, err), c.text);
        CHECK_MESSAGE(err.line == c.line, c.text);
        CHECK(!err.message.empty());
    }
}

TEST_CASE("json: deep nesting is rejected, not a stack overflow") {
    std::string s(1000, '[');
    s += std::string(1000, ']');
    JsonDoc doc;
    JsonError err;
    CHECK(!doc.parse(s, err));
}

TEST_CASE("json: canonical writer sorts keys and uses shortest numbers") {
    JsonDoc a, b;
    JsonError err;
    REQUIRE(a.parse("{ \"b\": [1.5, 0.1, -0], \"a\": {\"y\": \"q\\\"\", \"x\": false} }", err));
    REQUIRE(b.parse("{\"a\":{\"x\":false,\"y\":\"q\\\"\"},\n\n \"b\":[1.50, 1e-1, 0]}", err));
    String ca, cb;
    write_json(a.root(), ca, true);
    write_json(b.root(), cb, true);
    CHECK(ca.view() == "{\"a\":{\"x\":false,\"y\":\"q\\\"\"},\"b\":[1.5,0.1,0]}");
    CHECK(ca.view() == cb.view());
    String pretty;
    write_json(a.root(), pretty, false);
    JsonDoc re;
    CHECK(re.parse(pretty, err));  // the pretty form parses back
}

TEST_CASE("audio: decode 16-bit PCM with a smpl loop") {
    const std::string wav = cook_test::make_wav(44100, 2, 1000, 440.0, 0.5, 100, 900);
    PcmClip clip;
    FixedString<128> why;
    REQUIRE(decode_wav(Span<const byte>(reinterpret_cast<const byte*>(wav.data()), wav.size()), clip, why));
    CHECK(clip.sample_rate == 44100);
    CHECK(clip.channels == 2);
    CHECK(clip.frames() == 1000);
    CHECK(clip.has_loop);
    CHECK(clip.loop_start == 100);
    CHECK(clip.loop_end == 900);
    CHECK(clip.samples[2] == doctest::Approx(0.5 * std::sin(2 * 3.14159265358979 * 440.0 / 44100)).epsilon(0.001));
}

TEST_CASE("audio: malformed WAVs are rejected with a reason") {
    PcmClip clip;
    FixedString<128> why;
    const std::string junk("RIFF\x04\x00\x00\x00WAVX", 12);  // explicit length: the literal holds NULs
    CHECK(!decode_wav(Span<const byte>(reinterpret_cast<const byte*>(junk.data()), junk.size()), clip, why));
    CHECK(!why.empty());
    std::string wav = cook_test::make_wav(44100, 1, 100, 440.0);
    wav.resize(wav.size() - 150);  // data chunk now claims more than the file holds
    CHECK(!decode_wav(Span<const byte>(reinterpret_cast<const byte*>(wav.data()), wav.size()), clip, why));
}

TEST_CASE("audio: 44.1 kHz -> 48 kHz resample keeps a 1 kHz sine clean") {
    const u32 src = 44100, dst = 48000, frames = 4410;
    Array<f32> in;
    for (u32 i = 0; i < frames; ++i) in.push_back(static_cast<f32>(0.5 * std::sin(2 * 3.14159265358979 * 1000.0 * i / src)));
    Array<f32> out;
    resample(in.span(), 1, src, dst, out);
    CHECK(out.size() == (frames * u64(dst) + src - 1) / src);
    double err2 = 0, sig2 = 0;
    for (usize n = 64; n + 64 < out.size(); ++n) {  // skip the kernel's edge ramps
        const double ideal = 0.5 * std::sin(2 * 3.14159265358979 * 1000.0 * static_cast<double>(n) / dst);
        err2 += (out[n] - ideal) * (out[n] - ideal);
        sig2 += ideal * ideal;
    }
    CHECK(std::sqrt(err2 / sig2) < 0.005);  // < 0.5% distortion (assets.md §5.2)
    CHECK(rescale_frames(44100, 44100, 48000) == 48000);
}

TEST_CASE("audio: resample is a pass-through at the target rate and deterministic") {
    Array<f32> in;
    for (int i = 0; i < 100; ++i) in.push_back(static_cast<f32>(i) / 100.0f);
    Array<f32> out;
    resample(in.span(), 2, 48000, 48000, out);
    CHECK(out.size() == 100);
    CHECK(out[37] == in[37]);
    Array<f32> a, b;
    resample(in.span(), 2, 22050, 48000, a);
    resample(in.span(), 2, 22050, 48000, b);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0);
}

TEST_CASE("audio: peak normalization to -1 dBFS") {
    f32 s[4] = {0.1f, -0.25f, 0.2f, 0.0f};
    const f32 g = normalize_peak(Span<f32>(s, 4), kMinus1dBFS);
    CHECK(g == doctest::Approx(kMinus1dBFS / 0.25f));
    CHECK(std::fabs(s[1]) == doctest::Approx(kMinus1dBFS));
    f32 z[2] = {0, 0};
    CHECK(normalize_peak(Span<f32>(z, 2), kMinus1dBFS) == 1.0f);
}
