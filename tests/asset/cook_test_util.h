#pragma once
// Fixture helpers for cook tests: a throwaway project tree under the build directory, plus tiny
// PNG (stored deflate) and WAV writers so tests author their own sources.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "mce/types.h"

namespace cook_test {

namespace fs = std::filesystem;
using mce::u16;
using mce::u32;
using mce::u8;

inline std::string fresh_dir(const std::string& name) {
    const fs::path p = fs::path(MCE_TEST_TMP_DIR) / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p.generic_string();
}

inline void write_file(const std::string& path, const std::string& data) {
    fs::create_directories(fs::path(path).parent_path());
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

inline std::string read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

inline void put32be(std::string& s, u32 v) {
    for (int i = 3; i >= 0; --i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
inline void put32le(std::string& s, u32 v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
inline void put16le(std::string& s, u16 v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8));
}

inline u32 crc32_of(const std::string& s) {
    u32 c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return ~c;
}

// RGBA8 PNG with stored (uncompressed) deflate blocks.
inline std::string make_png(u32 w, u32 h, const std::vector<u8>& rgba) {
    std::string raw;
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.append(reinterpret_cast<const char*>(rgba.data() + y * w * 4), w * 4);
    }
    std::string z = "\x78\x01";
    for (size_t off = 0; off < raw.size() || off == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n == raw.size();
        z.push_back(last ? 1 : 0);
        put16le(z, static_cast<u16>(n));
        put16le(z, static_cast<u16>(~n));
        z.append(raw, off, n);
        off += n;
        if (last) break;
    }
    u32 a = 1, b = 0;
    for (unsigned char ch : raw) {
        a = (a + ch) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);
    auto chunk = [](std::string& out, const char* type, const std::string& data) {
        put32be(out, static_cast<u32>(data.size()));
        std::string td = std::string(type, 4) + data;
        out += td;
        put32be(out, crc32_of(td));
    };
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put32be(ihdr, w);
    put32be(ihdr, h);
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}

inline std::string make_gradient_png(u32 w, u32 h, u8 seed = 0) {
    std::vector<u8> px(w * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* p = &px[(y * w + x) * 4];
            p[0] = static_cast<u8>(x * 50 + seed);
            p[1] = static_cast<u8>(y * 70);
            p[2] = static_cast<u8>(200 - seed);
            p[3] = 255;
        }
    return make_png(w, h, px);
}

// 16-bit PCM sine, optional 'smpl' loop chunk [loop_start, loop_end).
inline std::string make_wav(u32 rate, u16 channels, u32 frames, double freq, double amp = 0.5, int loop_start = -1, int loop_end = -1) {
    std::string data;
    for (u32 i = 0; i < frames; ++i)
        for (u16 c = 0; c < channels; ++c)
            put16le(data, static_cast<u16>(static_cast<short>(std::lround(amp * 32767.0 * std::sin(2.0 * 3.14159265358979 * freq * i / rate)))));
    std::string body = "WAVE";
    body += "fmt ";
    put32le(body, 16);
    put16le(body, 1);
    put16le(body, channels);
    put32le(body, rate);
    put32le(body, rate * channels * 2);
    put16le(body, static_cast<u16>(channels * 2));
    put16le(body, 16);
    if (loop_start >= 0) {
        body += "smpl";
        put32le(body, 36 + 24);
        for (int i = 0; i < 7; ++i) put32le(body, 0);
        put32le(body, 1);  // loop count
        put32le(body, 0);  // sampler data
        put32le(body, 0);  // cue id
        put32le(body, 0);  // type
        put32le(body, static_cast<u32>(loop_start));
        put32le(body, static_cast<u32>(loop_end - 1));  // inclusive
        put32le(body, 0);
        put32le(body, 0);
    }
    body += "data";
    put32le(body, static_cast<u32>(data.size()));
    body += data;
    std::string riff = "RIFF";
    put32le(riff, static_cast<u32>(body.size()));
    return riff + body;
}

inline std::string sidecar(const char* guid, const char* importer, const char* settings = "{}") {
    return std::string("{ \"guid\": \"") + guid + "\", \"importer\": \"" + importer + "\", \"importer_version\": 1, \"settings\": " + settings +
           " }\n";
}

}  // namespace cook_test
