// The font importer (M-FUZZ 3b): a damaged .ttf stops at the cook with "not a TrueType/OpenType font" instead of
// reaching stb_truetype, which trusts its input (a damaged glyph box once crashed the atlas bake).

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace cook_test;

namespace {

std::string diags_of(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

cook::CookReport cook_font(const std::string& name, const std::string& bytes) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/fonts/f.ttf", bytes);
    write_file(root + "/assets/fonts/f.ttf.mceasset",
               R"({ "guid": "10c00000-0000-4000-8000-0000000000f7", "importer": "font", "importer_version": 2, "settings": { "size": 13, "ranges": [[32, 126]] } })");
    cook::CookService svc;
    return svc.cook(StringView(root.c_str()), cook::CookOptions{});
}

u32 be32(const std::string& s, size_t o) {
    return (u32(static_cast<unsigned char>(s[o])) << 24) | (u32(static_cast<unsigned char>(s[o + 1])) << 16) |
           (u32(static_cast<unsigned char>(s[o + 2])) << 8) | u32(static_cast<unsigned char>(s[o + 3]));
}
void put16(std::string& s, size_t o, u32 v) {
    s[o] = static_cast<char>(v >> 8);
    s[o + 1] = static_cast<char>(v);
}
void put32(std::string& s, size_t o, u32 v) {
    put16(s, o, v >> 16);
    put16(s, o + 2, v & 0xFFFF);
}

// Offset and length of a table, found in the directory.
bool table(const std::string& s, const char* tag, size_t& off, size_t& len) {
    const size_t n = (static_cast<unsigned char>(s[4]) << 8) | static_cast<unsigned char>(s[5]);
    for (size_t i = 0; i < n; ++i) {
        const size_t r = 12 + 16 * i;
        if (std::memcmp(s.data() + r, tag, 4) == 0) {
            off = be32(s, r + 8);
            len = be32(s, r + 12);
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("font importer: a sound font cooks; damaged tables, boxes and offsets are refused before stb_truetype") {
    const std::string good = read_file(std::string(MCE_TEST_DATA_DIR) + "/fonts/ProggyClean.ttf");
    REQUIRE(!good.empty());
    {
        const cook::CookReport r = cook_font("font_good", good);
        INFO(diags_of(r));
        CHECK(r.ok());
    }
    struct Case {
        const char* name;
        std::string bytes;
    };
    std::vector<Case> cases;
    size_t off = 0, len = 0;
    {  // a table that runs past the end of the file
        std::string b = good;
        REQUIRE(table(b, "glyf", off, len));
        const size_t n = (static_cast<unsigned char>(b[4]) << 8) | static_cast<unsigned char>(b[5]);
        for (size_t i = 0; i < n; ++i)
            if (std::memcmp(b.data() + 12 + 16 * i, "glyf", 4) == 0) put32(b, 12 + 16 * i + 12, 0xFFFFFFF0u);
        cases.push_back({"glyf past the end", b});
    }
    {  // unitsPerEm 0 would make the scale infinite
        std::string b = good;
        REQUIRE(table(b, "head", off, len));
        put16(b, off + 18, 0);
        cases.push_back({"unitsPerEm 0", b});
    }
    {  // no horizontal metrics
        std::string b = good;
        REQUIRE(table(b, "hhea", off, len));
        put16(b, off + 34, 0);
        cases.push_back({"numberOfHMetrics 0", b});
    }
    {  // more glyphs than loca holds
        std::string b = good;
        REQUIRE(table(b, "maxp", off, len));
        put16(b, off + 4, 0xFFFF);
        cases.push_back({"numGlyphs too large", b});
    }
    {  // a glyph box turned inside out: this is what gave the packer a negative width
        std::string b = good;
        size_t loca_off = 0, loca_len = 0, head_off = 0, head_len = 0, glyf_off = 0, glyf_len = 0;
        REQUIRE(table(b, "loca", loca_off, loca_len));
        REQUIRE(table(b, "head", head_off, head_len));
        REQUIRE(table(b, "glyf", glyf_off, glyf_len));
        const bool long_loca = ((static_cast<unsigned char>(b[head_off + 50]) << 8) | static_cast<unsigned char>(b[head_off + 51])) != 0;
        for (size_t g = 0; g < 200; ++g) {
            const size_t a = long_loca ? be32(b, loca_off + 4 * g) : 2u * ((static_cast<unsigned char>(b[loca_off + 2 * g]) << 8) | static_cast<unsigned char>(b[loca_off + 2 * g + 1]));
            const size_t e = long_loca ? be32(b, loca_off + 4 * (g + 1)) : 2u * ((static_cast<unsigned char>(b[loca_off + 2 * g + 2]) << 8) | static_cast<unsigned char>(b[loca_off + 2 * g + 3]));
            if (e - a < 10) continue;
            put16(b, glyf_off + a + 2, 0x7000);  // xMin far above xMax
            break;
        }
        cases.push_back({"inverted glyph box", b});
    }
    cases.push_back({"half the file", good.substr(0, good.size() / 2)});
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const cook::CookReport r = cook_font(std::string("font_bad"), c.bytes);
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("not a TrueType/OpenType font") != std::string::npos);
    }
}

namespace {

// A minimal TrueType font built here: glyphs are raw glyf entries, the cmap is format 12 with the given groups.
struct Group {
    u32 first, last, glyph;
};
std::string b16(u32 v) { return std::string{static_cast<char>(v >> 8), static_cast<char>(v)}; }
std::string b32(u32 v) { return b16(v >> 16) + b16(v & 0xFFFF); }

// One contour of `n` on-curve points at x = 0, y = 0, 1, 2, ...; the box is (0, 0, 0, n - 1).
std::string simple_glyph(u32 n) {
    std::string g = b16(1) + b16(0) + b16(0) + b16(0) + b16(n - 1) + b16(n - 1) + b16(0);
    g += std::string(n, static_cast<char>(0x35));  // on curve, x unchanged, y one positive byte
    for (u32 i = 0; i < n; ++i) g += static_cast<char>(i == 0 ? 0 : 1);
    return g;
}
// `times` copies of glyph `of`, each at offset (0, 0); `n` is the points of `of`, which gives the box.
std::string composite(u32 of, u32 times, u32 n) {
    std::string g = b16(0xFFFF) + b16(0) + b16(0) + b16(0) + b16(n - 1);
    for (u32 i = 0; i < times; ++i) g += b16(i + 1 == times ? 0x0003 : 0x0023) + b16(of) + b16(0) + b16(0);
    return g;
}

std::string make_font(const std::vector<std::string>& glyphs, const std::vector<Group>& groups) {
    const u32 n = static_cast<u32>(glyphs.size());
    std::string head(54, '\0');
    head.replace(0, 4, b32(0x00010000));
    head.replace(18, 2, b16(1000));
    head.replace(50, 2, b16(1));  // long loca
    std::string maxp = b32(0x00010000) + b16(n);
    std::string hhea(36, '\0');
    hhea.replace(4, 2, b16(800));
    hhea.replace(6, 2, b16(0xFF38));  // descent -200
    hhea.replace(34, 2, b16(1));
    std::string hmtx = b16(500) + b16(0) + std::string(2u * (n - 1), '\0');
    std::string cmap = b16(0) + b16(1) + b16(3) + b16(10) + b32(12);
    cmap += b16(12) + b16(0) + b32(16 + 12 * static_cast<u32>(groups.size())) + b32(0) + b32(static_cast<u32>(groups.size()));
    for (const Group& g : groups) cmap += b32(g.first) + b32(g.last) + b32(g.glyph);
    std::string glyf, loca;
    for (const std::string& g : glyphs) {
        loca += b32(static_cast<u32>(glyf.size()));
        glyf += g;
        while (glyf.size() % 4) glyf += '\0';
    }
    loca += b32(static_cast<u32>(glyf.size()));
    const std::pair<const char*, const std::string*> tables[] = {{"cmap", &cmap}, {"glyf", &glyf}, {"head", &head}, {"hhea", &hhea},
                                                                 {"hmtx", &hmtx}, {"loca", &loca}, {"maxp", &maxp}};
    std::string out = b32(0x00010000) + b16(7) + b16(0) + b16(0) + b16(0);
    u32 at = 12 + 16 * 7;
    std::string body;
    for (const auto& t : tables) {
        out += t.first + b32(0) + b32(at + static_cast<u32>(body.size())) + b32(static_cast<u32>(t.second->size()));
        body += *t.second;
        while (body.size() % 4) body += '\0';
    }
    return out + body;
}

}  // namespace

TEST_CASE("font importer: cmap glyph ids, deep or fanned-out composites and CFF outlines are refused; a small sound font cooks") {
    const std::vector<Group> a = {{0x41, 0x41, 1}};
    const std::string simple = simple_glyph(4);
    {
        const cook::CookReport r = cook_font("font_built_good", make_font({"", simple}, a));
        INFO(diags_of(r));
        CHECK(r.ok());
    }
    {  // a composite of a simple glyph, one level
        const cook::CookReport r = cook_font("font_built_comp", make_font({"", simple, composite(1, 3, 4)}, {{0x41, 0x41, 2}}));
        INFO(diags_of(r));
        CHECK(r.ok());
    }
    struct Case {
        const char* name;
        std::string bytes;
    };
    std::vector<Case> bad;
    // A cmap that names a glyph the font lacks: the codepoint is skipped (the font still bakes its space), and
    // stb_truetype is never asked about the glyph, whose metrics it would read past hmtx.
    for (const Group& g : {Group{0x41, 0x41, 0x80000000u}, Group{0x41, 0x41, 5}, Group{0x41, 0x50, 1}}) {
        const cook::CookReport r = cook_font("font_built_gid", make_font({"", simple}, {g}));
        INFO(diags_of(r));
        CHECK(r.ok());
    }
    {  // glyph k is a composite of glyph k - 1: each is fine on its own, the chain is what blows the stack
        std::vector<std::string> g = {"", simple};
        for (u32 k = 2; k < 60; ++k) g.push_back(composite(k - 1, 1, 4));
        bad.push_back({"composite chain", make_font(g, {{0x41, 0x41, 59}})});
    }
    {  // 9,000 copies of a 2-point glyph: 18,000 points once expanded
        std::vector<std::string> g = {"", simple_glyph(2), composite(1, 9000, 2)};
        bad.push_back({"composite fan-out", make_font(g, {{0x41, 0x41, 2}})});
    }
    {  // components of empty glyphs cost no points, but stb_truetype walks every one: 5,000 x 5,000 of them
        std::vector<std::string> g = {"", simple, composite(0, 5000, 2), composite(2, 5000, 2)};
        bad.push_back({"composite fan-out over empty glyphs", make_font(g, {{0x41, 0x41, 3}})});
    }
    {  // CFF outlines: the 'glyf' entry renamed
        std::string f = make_font({"", simple}, a);
        const size_t at = f.find("glyf");
        REQUIRE(at != std::string::npos);
        f.replace(at, 4, "CFF ");
        bad.push_back({"CFF outlines", f});
    }
    for (const Case& c : bad) {
        CAPTURE(std::string(c.name));
        const cook::CookReport r = cook_font("font_built_bad", c.bytes);
        CHECK_FALSE(r.ok());
    }
}
