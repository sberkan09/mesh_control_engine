// M-CINEMA 5 (ADR-0042): render::VideoDecoder on a committed 2 s MPEG-1 clip (64x48, 25 fps, red for
// the first second and blue for the second, a 440 Hz MP2 sine): its header, the picture at a time
// forward and back, the frames a forward step decodes, and the audio decode the cook uses.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>

#include "mce/render/video.h"

using namespace mce;

namespace {

std::string clip_bytes() {
    std::string s;
    std::FILE* f = std::fopen(MCE_TEST_VIDEO, "rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

Span<const byte> as_bytes(const std::string& s) { return {reinterpret_cast<const byte*>(s.data()), s.size()}; }

// The picture's centre pixel.
void centre(const render::VideoDecoder& d, u8& r, u8& g, u8& b) {
    const render::VideoInfo& i = d.info();
    const usize at = (static_cast<usize>(i.height / 2) * i.width + i.width / 2) * 4;
    r = d.rgba()[at];
    g = d.rgba()[at + 1];
    b = d.rgba()[at + 2];
}

}  // namespace

TEST_CASE("video: the decoder reads the header and shows the frame at a time") {
    const std::string bytes = clip_bytes();
    REQUIRE(!bytes.empty());
    render::VideoDecoder d;
    REQUIRE(d.open(as_bytes(bytes)));
    CHECK(d.info().width == 64);
    CHECK(d.info().height == 48);
    CHECK(d.info().frame_rate == doctest::Approx(25.0f));
    CHECK(d.info().duration > 1.5f);  // the last packet's time stamp: short of the end
    CHECK(d.count_frames() == 50);
    d.set_duration(2.0f);
    CHECK(d.info().audio_streams == 1);
    CHECK(d.picture_time() < 0.0);
    REQUIRE(d.rgba().size() == 64u * 48u * 4u);

    u8 r = 0, g = 0, b = 0;
    REQUIRE(d.show(0.5));
    CHECK(d.rgba()[3] == 255);  // opaque
    centre(d, r, g, b);
    CHECK(r > 200);
    CHECK(b < 60);
    CHECK(d.picture_time() == doctest::Approx(0.48).epsilon(0.05));

    // Forward a few frames: only those frames are decoded.
    const u32 before = d.frames_decoded();
    CHECK(d.show(0.7));
    CHECK(d.frames_decoded() - before == 5);
    CHECK_FALSE(d.show(0.71));  // the same frame: nothing to upload

    // Across the cut, and back again (a seek).
    REQUIRE(d.show(1.5));
    centre(d, r, g, b);
    CHECK(b > 200);
    CHECK(r < 60);
    REQUIRE(d.show(0.2));
    centre(d, r, g, b);
    CHECK(r > 200);
    CHECK(d.picture_time() == doctest::Approx(0.2).epsilon(0.05));

    // Past the end the last picture stays.
    d.show(5.0);
    centre(d, r, g, b);
    CHECK(b > 200);
}

TEST_CASE("video: the decoder decodes the sound and refuses what is not MPEG-1") {
    const std::string bytes = clip_bytes();
    render::VideoDecoder d;
    REQUIRE(d.open(as_bytes(bytes), true));
    CHECK(d.info().sample_rate == 44100);
    Array<f32> pcm;
    REQUIRE(d.decode_audio(pcm));
    CHECK(pcm.size() % 2 == 0);
    CHECK(pcm.size() / 2 == doctest::Approx(88200.0).epsilon(0.03));
    f32 peak = 0.0f;
    for (f32 s : pcm) peak = s > peak ? s : peak;
    CHECK(peak > 0.05f);  // ffmpeg's sine source peaks at 1/8

    const std::string junk(4096, 'x');
    CHECK_FALSE(d.open(as_bytes(junk)));
    CHECK_FALSE(d.is_open());
    CHECK_FALSE(d.open({}));
}
