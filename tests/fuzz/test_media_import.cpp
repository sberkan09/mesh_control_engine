// M-FUZZ 5 (M-IMPORTMEDIA 3): the texture and audio formats the committed samples do not carry. TGA and BMP are built
// here (raw, RLE and 32-bit TGA, a 24-bit BMP), MP3 and FLAC come from tests/asset/data/audio. Each seed is mutated
// byte-wise, and its first bytes (the header: dimensions, bit depth, sample rate, channel count, frame sizes) are also
// overwritten one at a time with extreme values, which is what reaches the dimension and frame caps.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "mce/asset/file_io.h"
#include "mce/cook/importer.h"
#include "import_support.h"
#include "mce/log.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;

namespace {

u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 300u;
}

void put16(std::string& s, u32 v) {
    s.push_back(char(v & 255));
    s.push_back(char((v >> 8) & 255));
}
void put32(std::string& s, u32 v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

constexpr u32 kW = 9, kH = 5;

void pixel(std::string& s, u32 x, u32 y, bool alpha) {
    s.push_back(char(x * 27));
    s.push_back(char(y * 50));
    s.push_back(char((x + y) * 13));
    if (alpha) s.push_back(char(255 - x * 20));
}

// image type 2 = uncompressed true colour, 10 = RLE (every run a raw packet of one pixel).
std::string make_tga(u32 bpp, bool rle) {
    std::string s;
    s.push_back(0);
    s.push_back(0);
    s.push_back(rle ? 10 : 2);
    put16(s, 0), put16(s, 0), s.push_back(0);
    put16(s, 0), put16(s, 0);
    put16(s, kW), put16(s, kH);
    s.push_back(char(bpp));
    s.push_back(char(bpp == 32 ? 8 : 0));
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            if (rle) s.push_back(0);
            pixel(s, x, y, bpp == 32);
        }
    return s;
}

std::string make_bmp() {
    const u32 stride = (kW * 3 + 3) & ~3u;
    std::string s = "BM";
    put32(s, 54 + stride * kH);
    put32(s, 0);
    put32(s, 54);
    put32(s, 40);
    put32(s, kW);
    put32(s, kH);
    put16(s, 1);
    put16(s, 24);
    put32(s, 0), put32(s, stride * kH), put32(s, 2835), put32(s, 2835), put32(s, 0), put32(s, 0);
    for (u32 y = 0; y < kH; ++y) {
        std::string row;
        for (u32 x = 0; x < kW; ++x) pixel(row, x, y, false);
        while (row.size() < stride) row.push_back(0);
        s += row;
    }
    return s;
}

void fuzz_seed(const char* label, const std::string& ext, const std::string& good) {
    cook::ImporterRegistry reg;
    cook::ImporterRegistry::add_builtins(reg);
    log::set_level(log::Level::Off);
    const cook::IImporter* imp = reg.for_extension(StringView(ext.c_str(), ext.size()));
    REQUIRE_MESSAGE(imp != nullptr, "no importer for " << ext);
    const std::string rel = std::string("media/seed") + ext;
    const Span<const byte> src(reinterpret_cast<const byte*>(good.data()), good.size());
    REQUIRE_MESSAGE(fuzz::import_bytes(*imp, rel, src), label << " must cook untouched");

    u64 mutants = 0, accepted = 0;
    // Header sweep: each of the first bytes set to a few extreme values.
    const usize head = std::min<usize>(good.size(), 48);
    for (usize i = 0; i < head; ++i)
        for (const int v : {0x00, 0x01, 0x7f, 0x80, 0xff}) {
            std::string t = good;
            t[i] = static_cast<char>(v);
            INFO("replay: " << label << " header byte " << i << " = " << v);
            accepted += fuzz::import_bytes(*imp, rel, Span<const byte>(reinterpret_cast<const byte*>(t.data()), t.size()));
            ++mutants;
        }
    // Truncation at every header-sized prefix and a few body cuts.
    for (usize len = 0; len < head + 8 && len < good.size(); ++len) {
        INFO("replay: " << label << " truncated to " << len);
        accepted += fuzz::import_bytes(*imp, rel, Span<const byte>(reinterpret_cast<const byte*>(good.data()), len));
        ++mutants;
    }
    fuzz::Mutator m(0x6d656469u ^ static_cast<std::uint32_t>(good.size()));
    std::vector<std::uint64_t> storage;
    const u32 n = iterations();
    for (u32 it = 0; it < n; ++it) {
        const usize len = m.mutate(src, storage);
        INFO("replay: " << label << " iteration " << it);
        accepted += fuzz::import_bytes(*imp, rel, Span<const byte>(reinterpret_cast<const byte*>(storage.data()), len));
        ++mutants;
    }
    std::printf("fuzz: %-22s %6llu mutants, %6llu accepted\n", label, static_cast<unsigned long long>(mutants),
                static_cast<unsigned long long>(accepted));
}

std::string audio_seed(const char* name) {
    Array<byte> bytes;
    const std::string path = std::string(MCE_SOURCE_DIR) + "/tests/asset/data/audio/" + name;
    REQUIRE_MESSAGE(asset::read_file(path, bytes), "missing " << path);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

TEST_CASE("fuzz: TGA and BMP textures survive mutated source") {
    fuzz_seed("tga 24 raw", ".tga", make_tga(24, false));
    fuzz_seed("tga 24 rle", ".tga", make_tga(24, true));
    fuzz_seed("tga 32 raw", ".tga", make_tga(32, false));
    fuzz_seed("bmp 24", ".bmp", make_bmp());
}

TEST_CASE("fuzz: MP3 and FLAC audio survive mutated source") {
    fuzz_seed("mp3 tone", ".mp3", audio_seed("tone.mp3"));
    fuzz_seed("flac tone", ".flac", audio_seed("tone.flac"));
}
