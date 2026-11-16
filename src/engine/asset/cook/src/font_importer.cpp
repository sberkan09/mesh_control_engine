// TTF -> CHNK_FONT: glyph atlas baked at cook with stb_truetype + stb_rect_pack (Skyline), plus
// metrics and the kern-table pairs (assets.md §5.2; "atlas built at cook, not runtime" is the
// FR-R2D-060 acceptance test). The atlas is coverage, or with "sdf" a single-channel signed
// distance field (stb_truetype's), which the sprite shader thresholds so text stays sharp far
// above the baked size (M-UI-DATA 4). MSDF via msdf-atlas-gen remains the corner-exact upgrade.
//
// sidecar settings: { "size": 32, "ranges": [[32, 126]], "padding": 1, "strings": true, "sdf": true, "spread": 6 }
//   size     pixel height (ascent - descent)
//   sdf      bake a distance field instead of coverage (default false)
//   spread   sdf only: pixels at `size` the field reaches past the edge, an integer in [1, 32]
//            (default size / 8, at least 2); outlines thicker than this fall off the field
//   ranges   inclusive codepoint ranges; codepoints the font lacks are skipped
//   strings  also every codepoint the project's string tables use (assets/loc/*.csv, M-MOBILE 11a),
//            with their upper- and lower-case forms in each table's locale (Turkish İ and ı), so a
//            translation never shows a missing glyph; the tables become dependencies of the font
// Kerning comes from the legacy 'kern' table only (GPOS kerning is not read).
// stb_truetype trusts its input, so font_structure_ok walks the tables it reads (directory, head/maxp/hhea/hmtx, cmap,
// loca/glyf with composite depth, kern) before it is called: a font is a file a modder or an agent drops into the tree.

#include <algorithm>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/loc/loc.h"
#include "strings_csv.h"
#include "stb_rect_pack.h"
#include "stb_truetype.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.font";
constexpr u32 kMaxCodepoints = 65536;
constexpr usize kMaxKerns = 1u << 20;  // pairs x codepoints per glyph can multiply out; a font has far fewer

struct Range {
    u32 first;
    u32 last;
};

bool wants_strings(JsonValue s) {
    const JsonValue v = s.valid() ? s.get("strings") : JsonValue();
    return v.valid() && v.is_bool() && v.as_bool();
}

// Appends every codepoint of UTF-8 `text` (already validated by the string importer).
void add_codepoints(StringView text, Array<u32>& out) {
    const auto* p = reinterpret_cast<const u8*>(text.data());
    for (usize i = 0; i < text.size();) {
        const u8 c = p[i];
        u32 len = 1, cp = c;
        if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F;
        else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F;
        else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07;
        if (i + len > text.size()) break;
        for (u32 k = 1; k < len; ++k) cp = (cp << 6) | (p[i + k] & 0x3F);
        i += len;
        if (cp >= 32) out.push_back(cp);
    }
}

// The codepoints of every string table the font depends on, in its own and both case forms.
// Tables that do not parse are skipped: the string importer reports them.
void string_table_codepoints(const ImportInput& in, Array<u32>& cps) {
    if (!in.lookup) return;
    Array<Guid> tables;
    in.lookup->assets_of_type(ChunkType::StringTable, tables);
    for (const Guid& g : tables) {
        Span<const byte> bytes;
        StringView path;
        if (!in.lookup->source_bytes(g, bytes, path)) continue;
        loc::LocaleTag locale;
        detail::locale_from_path(path, locale);
        loc::StringTable table;
        DiagList ignored;
        detail::parse_string_csv(StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path, table, ignored);
        for (usize i = 0; i < table.size(); ++i) {
            const StringView text = table.entry(i).text;
            add_codepoints(text, cps);
            Array<char> cased;
            loc::to_upper(locale.view(), text, cased);
            loc::to_lower(locale.view(), text, cased);
            add_codepoints(StringView(cased.data(), cased.size()), cps);
        }
    }
    std::sort(cps.begin(), cps.end());
    usize uniq = 0;
    for (usize i = 0; i < cps.size(); ++i)
        if (i == 0 || cps[i] != cps[i - 1]) cps[uniq++] = cps[i];
    cps.resize(uniq);
}


// ---- font_structure_ok: what stb_truetype reads must lie inside the file -------------------------------------------
// (M-FUZZ 3b: a damaged glyph box gave stbrp_pack_rects a negative width; the glyf, loca and cmap parsers read with
// unchecked offsets.) It accepts every well-formed font the importer baked before; anything it refuses is damaged.

struct FontBytes {
    const u8* d;
    usize n;
    bool ok = true;
    u32 u8_at(usize o) { if (o + 1 > n) { ok = false; return 0; } return d[o]; }
    u32 u16_at(usize o) { if (o + 2 > n) { ok = false; return 0; } return (u32(d[o]) << 8) | d[o + 1]; }
    i32 i16_at(usize o) { const u32 v = u16_at(o); return static_cast<i32>(v >= 0x8000 ? v - 0x10000 : v); }
    u32 u32_at(usize o) { if (o + 4 > n) { ok = false; return 0; } return (u32(d[o]) << 24) | (u32(d[o + 1]) << 16) | (u32(d[o + 2]) << 8) | d[o + 3]; }
};

struct FontTable {
    usize off = 0, len = 0;
    bool present = false;
};

FontTable find_table(FontBytes& f, usize dir, u32 num, const char* tag) {
    for (u32 i = 0; i < num; ++i) {
        const usize r = dir + 12 + 16u * i;
        if (std::memcmp(f.d + r, tag, 4) == 0) return FontTable{f.u32_at(r + 8), f.u32_at(r + 12), true};
    }
    return {};
}

// The extent of a glyph's points, in font units.
struct Extent {
    f64 x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    bool any = false;
    u64 points = 0;  // outline points once components are expanded (stb_truetype copies them per use)
    u64 cost = 0;    // outline points plus component records, expanded: the work stb_truetype does for the glyph
    u32 height = 0;  // component nesting below this glyph
    void add(f64 x, f64 y) {
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        any = true;
    }
};

// A glyph's bytes: contour counts, flags and coordinates all fit, its points lie inside the box its header names (the
// rasterizer sizes the bitmap from that box), and a composite only names glyphs that exist, placed by offsets, to a
// depth stb_truetype's recursion can take. `ext` receives the points' extent.
constexpr u64 kMaxGlyphPoints = 16384;  // outline points of one glyph, components expanded
constexpr u64 kMaxGlyphCost = 65536;    // points plus components stb_truetype walks (it keeps no memo)

struct GlyphMemo;
bool glyph_check(FontBytes& f, const FontTable& glyf, const Array<u32>& loca, u32 g, u32 num_glyphs, u32 depth, Extent& ext, GlyphMemo& memo);

struct GlyphMemo {
    Array<u8> state;  // 0 not seen, 1 ok, 2 refused
    Array<Extent> extent;
};

bool glyph_ok(FontBytes& f, const FontTable& glyf, const Array<u32>& loca, u32 g, u32 num_glyphs, u32 depth, Extent& ext,
              GlyphMemo& memo) {
    if (depth > 6) return false;
    if (memo.state[g] != 0) {
        ext = memo.extent[g];
        return memo.state[g] == 1 && depth + ext.height <= 6;  // the nesting stb_truetype recurses through
    }
    memo.state[g] = 2;  // a cycle through this glyph is refused
    if (!glyph_check(f, glyf, loca, g, num_glyphs, depth, ext, memo)) return false;
    memo.state[g] = 1;
    memo.extent[g] = ext;
    return true;
}

bool glyph_check(FontBytes& f, const FontTable& glyf, const Array<u32>& loca, u32 g, u32 num_glyphs, u32 depth, Extent& ext, GlyphMemo& memo) {
    const usize a = loca[g], b = loca[g + 1];
    if (b == a) return true;
    if (b - a < 10) return false;
    const usize base = glyf.off + a, len = b - a;
    const i32 nc = f.i16_at(base);
    const i32 bx0 = f.i16_at(base + 2), by0 = f.i16_at(base + 4), bx1 = f.i16_at(base + 6), by1 = f.i16_at(base + 8);
    if (bx0 > bx1 || by0 > by1) return false;  // inverted box
    Extent mine;
    if (nc >= 0) {
        usize p = 10 + 2u * static_cast<u32>(nc);
        if (p + 2 > len) return false;
        u32 prev = 0, npts = 0;
        for (i32 c = 0; c < nc; ++c) {
            const u32 e = f.u16_at(base + 10 + 2u * static_cast<u32>(c));
            if (c > 0 && e < prev) return false;
            prev = e;
        }
        if (nc > 0) npts = prev + 1;
        if (npts > kMaxGlyphPoints) return false;
        mine.points = npts;
        mine.cost = npts + 1;
        p += 2 + f.u16_at(base + p);  // instructions
        if (p > len) return false;
        Array<u8> flags;
        flags.resize(npts);
        usize xbytes = 0, ybytes = 0;
        for (u32 i = 0; i < npts;) {
            if (p + 1 > len) return false;
            const u32 fl = f.u8_at(base + p++);
            u32 rep = 1;
            if (fl & 8) {
                if (p + 1 > len) return false;
                rep += f.u8_at(base + p++);
            }
            if (i + rep > npts) return false;
            for (u32 k = 0; k < rep; ++k) flags[i + k] = static_cast<u8>(fl);
            xbytes += rep * ((fl & 2) ? 1u : ((fl & 16) ? 0u : 2u));
            ybytes += rep * ((fl & 4) ? 1u : ((fl & 32) ? 0u : 2u));
            i += rep;
        }
        if (p + xbytes + ybytes > len || !f.ok) return false;
        i32 x = 0, y = 0;
        usize xp = base + p, yp = base + p + xbytes;
        Array<i32> xs, ys;
        xs.resize(npts);
        ys.resize(npts);
        for (u32 i = 0; i < npts; ++i) {
            const u32 fl = flags[i];
            if (fl & 2) { const i32 d = static_cast<i32>(f.u8_at(xp++)); x += (fl & 16) ? d : -d; }
            else if (!(fl & 16)) { x += f.i16_at(xp); xp += 2; }
            xs[i] = x;
        }
        for (u32 i = 0; i < npts; ++i) {
            const u32 fl = flags[i];
            if (fl & 4) { const i32 d = static_cast<i32>(f.u8_at(yp++)); y += (fl & 32) ? d : -d; }
            else if (!(fl & 32)) { y += f.i16_at(yp); yp += 2; }
            ys[i] = y;
        }
        for (u32 i = 0; i < npts; ++i) mine.add(xs[i], ys[i]);
    } else {
        usize p = 10;
        for (;;) {
            if (p + 4 > len) return false;
            const u32 fl = f.u16_at(base + p), gi = f.u16_at(base + p + 2);
            p += 4;
            if (gi >= num_glyphs || gi == g || !(fl & 2)) return false;  // stb_truetype cannot place a component by matching points
            f64 dx, dy;
            if (fl & 1) { dx = f.i16_at(base + p); dy = f.i16_at(base + p + 2); p += 4; }
            else { dx = static_cast<i8>(f.u8_at(base + p)); dy = static_cast<i8>(f.u8_at(base + p + 1)); p += 2; }
            p += (fl & 8) ? 2u : (fl & 0x40) ? 4u : (fl & 0x80) ? 8u : 0u;  // scale, x/y scale or 2x2 matrix
            if (p > len) return false;
            Extent sub;
            if (!glyph_ok(f, glyf, loca, gi, num_glyphs, depth + 1, sub, memo)) return false;
            mine.points += sub.points;
            mine.cost += sub.cost + 1;
            mine.height = std::max(mine.height, sub.height + 1);
            if (mine.points > kMaxGlyphPoints || mine.cost > kMaxGlyphCost) return false;
            if (sub.any) {
                mine.add(sub.x0 + dx, sub.y0 + dy);
                mine.add(sub.x1 + dx, sub.y1 + dy);
            }
            if (!(fl & 0x20)) break;
        }
    }
    if (!f.ok) return false;
    ext = mine;
    return true;
}

// stb_truetype reads one cmap subtable: the last record of the Unicode platform, or Microsoft's BMP / full Unicode
// encoding. That one is checked against the table; the others are never read.
bool cmap_ok(FontBytes& f, const FontTable& cmap) {
    if (cmap.len < 4) return false;
    const u32 n = f.u16_at(cmap.off + 2);
    if (cmap.len < 4 + 8u * n) return false;
    u64 sub = 0;
    bool chosen = false;
    for (u32 i = 0; i < n; ++i) {
        const usize rec = cmap.off + 4 + 8u * i;
        const u32 platform = f.u16_at(rec), encoding = f.u16_at(rec + 2);
        if (platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10))) {
            sub = f.u32_at(rec + 4);
            chosen = true;
        }
    }
    if (!chosen) return f.ok;  // stb refuses the font itself
    if (sub == 0 || sub + 6 > cmap.len) return false;
    const usize at = cmap.off + static_cast<usize>(sub), room = cmap.len - static_cast<usize>(sub);
    const u32 format = f.u16_at(at);
    if (format == 0) {
        const u32 length = f.u16_at(at + 2);
        if (length < 262 || length > room) return false;
    } else if (format == 4) {
        const u32 segx2 = f.u16_at(at + 6);
        if (segx2 == 0 || (segx2 & 1) || room < 16 + 4u * segx2) return false;  // end, start, delta, range offset arrays
        // The search fields are what stb_truetype's binary search runs on; every font writes the values the spec derives.
        u32 pow2 = 1, log2 = 0;
        while (pow2 * 2 <= segx2 / 2) pow2 *= 2, ++log2;
        if (f.u16_at(at + 8) != 2 * pow2 || f.u16_at(at + 10) != log2 || f.u16_at(at + 12) != segx2 - 2 * pow2) return false;
        // idRangeOffset pointers read glyph ids from past the arrays: bound each one.
        const usize ro = at + 16 + 3u * segx2;
        const usize starts = at + 16 + segx2;
        for (u32 sg = 0; sg < segx2 / 2; ++sg) {
            const u32 off = f.u16_at(ro + 2u * sg);
            if (off == 0) continue;
            const u64 span = static_cast<u64>(f.u16_at(at + 14 + 2u * sg)) + 1 - f.u16_at(starts + 2u * sg);
            if (span > 65536 || ro + 2u * sg + off + 2 * span > cmap.off + cmap.len) return false;
        }
    } else if (format == 6) {
        if (room < 10 + 2u * static_cast<usize>(f.u16_at(at + 8))) return false;
    } else if (format == 12 || format == 13) {
        if (room < 16 || f.u32_at(at + 12) > (room - 16) / 12) return false;
    } else {
        return false;  // 2, 8, 10, 14: stb_truetype asserts on them
    }
    return f.ok;
}

bool font_structure_ok(const u8* data, usize size, int offset) {
    if (size < 16) return false;
    FontBytes f{data, size};
    const usize o = static_cast<usize>(offset);
    const u32 num = f.u16_at(o + 4);
    if (!f.ok || num == 0 || num > 256 || o + 12 + 16u * num > size) return false;
    for (u32 i = 0; i < num; ++i) {
        const usize r = o + 12 + 16u * i;
        if (static_cast<u64>(f.u32_at(r + 8)) + f.u32_at(r + 12) > size) return false;
    }
    const FontTable head = find_table(f, o, num, "head"), maxp = find_table(f, o, num, "maxp"), hhea = find_table(f, o, num, "hhea"),
                    hmtx = find_table(f, o, num, "hmtx"), cmap = find_table(f, o, num, "cmap"), glyf = find_table(f, o, num, "glyf"),
                    loca = find_table(f, o, num, "loca"), kern = find_table(f, o, num, "kern");
    if (!head.present || head.len < 54 || !maxp.present || maxp.len < 6 || !hhea.present || hhea.len < 36 || !hmtx.present ||
        !cmap.present)
        return false;
    const u32 units = f.u16_at(head.off + 18), loca_fmt = f.u16_at(head.off + 50);
    const u32 num_glyphs = f.u16_at(maxp.off + 4), num_hm = f.u16_at(hhea.off + 34);
    if (units < 16 || units > 16384 || loca_fmt > 1 || num_glyphs == 0 || num_hm == 0 || num_hm > num_glyphs) return false;
    if (f.i16_at(hhea.off + 4) - f.i16_at(hhea.off + 6) <= 0) return false;  // ascent - descent scales the font
    if (hmtx.len < 4u * num_hm + 2u * (num_glyphs - num_hm)) return false;
    if (!cmap_ok(f, cmap)) return false;
    if (kern.present && f.u16_at(kern.off + 2) >= 1 && f.u16_at(kern.off + 8) == 1) {  // the table stb reads, whatever its length says
        if (kern.len < 18 || f.u16_at(kern.off + 10) > (kern.len - 18) / 6) return false;
    }
    if (glyf.present) {
        if (!loca.present || loca.len < (loca_fmt ? 4u : 2u) * (num_glyphs + 1)) return false;
        Array<u32> starts;
        starts.resize(num_glyphs + 1);
        u32 prev = 0;
        for (u32 i = 0; i <= num_glyphs; ++i) {
            const u32 v = loca_fmt ? f.u32_at(loca.off + 4u * i) : 2u * f.u16_at(loca.off + 2u * i);
            if (v < prev || v > glyf.len) return false;
            starts[i] = prev = v;
        }
        GlyphMemo memo;
        memo.state.resize(num_glyphs);
        memo.extent.resize(num_glyphs);
        for (u32 g = 0; g < num_glyphs; ++g) {
            Extent unused;
            if (!glyph_ok(f, glyf, starts, g, num_glyphs, 0, unused, memo)) return false;
        }
    } else {
        return false;  // CFF outlines (OpenType 'OTTO'): stb_truetype sizes its buffer at 512 MB, so offsets are not bounded
    }
    return f.ok;
}

// The glyph the cmap names for `cp` when stb_truetype can be asked about it (hmtx and loca are indexed by it unchecked);
// -1 for a codepoint the font lacks (space is baked blank even then).
int usable_glyph(const stbtt_fontinfo& info, u32 cp) {
    const int g = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
    if (g < 0 || g >= info.numGlyphs) return -1;
    return (g != 0 || cp == 32) ? g : -1;
}

// Coverage: stb_truetype's packer bakes antialiased glyphs, growing the atlas until all fit.
bool bake_coverage(const unsigned char* data, f32 size, const Array<u32>& cps, int padding, Array<asset::FontGlyph>& glyphs,
                   Array<u8>& atlas, u32& aw, u32& ah) {
    Array<int> cp_ints;
    for (u32 cp : cps) cp_ints.push_back(static_cast<int>(cp));
    Array<stbtt_packedchar> packed;
    packed.resize(cps.size());
    bool packed_ok = false;
    while (aw <= 4096 && ah <= 4096) {
        atlas.clear();
        atlas.resize(static_cast<usize>(aw) * ah, 0);
        stbtt_pack_context pc;
        if (stbtt_PackBegin(&pc, atlas.data(), static_cast<int>(aw), static_cast<int>(ah), 0, padding, nullptr)) {
            stbtt_PackSetOversampling(&pc, 1, 1);
            stbtt_pack_range range{};
            range.font_size = size;
            range.array_of_unicode_codepoints = cp_ints.data();
            range.num_chars = static_cast<int>(cp_ints.size());
            range.chardata_for_range = packed.data();
            packed_ok = stbtt_PackFontRanges(&pc, data, 0, &range, 1) != 0;
            stbtt_PackEnd(&pc);
        }
        if (packed_ok) break;
        if (aw <= ah) aw *= 2;
        else ah *= 2;
    }
    if (!packed_ok) return false;
    for (usize i = 0; i < cps.size(); ++i) {
        const stbtt_packedchar& p = packed[i];
        glyphs.push_back(asset::FontGlyph{cps[i], p.x0, p.y0, p.x1, p.y1, p.xoff, p.yoff, p.xoff2, p.yoff2, p.xadvance});
    }
    return true;
}

// Signed distance field: 128 on the outline, +128/spread per pixel inside, clamped to [0, 255].
// Each glyph's bitmap carries `spread` pixels of field around the outline; the quads include it.
bool bake_sdf(const stbtt_fontinfo& info, f32 scale, const Array<u32>& cps, int spread, int padding, Array<asset::FontGlyph>& glyphs,
              Array<u8>& atlas, u32& aw, u32& ah) {
    struct Bitmap {
        unsigned char* pixels = nullptr;
        int w = 0, h = 0, xoff = 0, yoff = 0;
    };
    Array<Bitmap> bitmaps;
    bitmaps.resize(cps.size());
    for (usize i = 0; i < cps.size(); ++i) {
        Bitmap& b = bitmaps[i];
        b.pixels = stbtt_GetCodepointSDF(&info, scale, static_cast<int>(cps[i]), spread, 128, 128.0f / static_cast<f32>(spread), &b.w, &b.h,
                                         &b.xoff, &b.yoff);
        if (!b.pixels) b.w = b.h = 0;  // blank glyphs (space) have no field
    }
    Array<stbrp_rect> rects;
    rects.resize(cps.size());
    Array<stbrp_node> nodes;
    bool packed_ok = false;
    while (aw <= 4096 && ah <= 4096) {
        for (usize i = 0; i < cps.size(); ++i) {
            rects[i] = stbrp_rect{};
            rects[i].id = static_cast<int>(i);
            rects[i].w = bitmaps[i].w ? bitmaps[i].w + padding : 0;
            rects[i].h = bitmaps[i].h ? bitmaps[i].h + padding : 0;
        }
        nodes.resize(aw);
        stbrp_context rc;
        stbrp_init_target(&rc, static_cast<int>(aw - padding), static_cast<int>(ah - padding), nodes.data(), static_cast<int>(aw));
        packed_ok = stbrp_pack_rects(&rc, rects.data(), static_cast<int>(rects.size())) != 0;
        if (packed_ok) break;
        if (aw <= ah) aw *= 2;
        else ah *= 2;
    }
    if (packed_ok) {
        atlas.clear();
        atlas.resize(static_cast<usize>(aw) * ah, 0);
        for (usize i = 0; i < cps.size(); ++i) {
            const Bitmap& b = bitmaps[i];
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&info, static_cast<int>(cps[i]), &adv, &lsb);
            const u16 x0 = static_cast<u16>(rects[i].x + padding), y0 = static_cast<u16>(rects[i].y + padding);
            for (int y = 0; y < b.h; ++y)
                for (int x = 0; x < b.w; ++x) atlas[usize(y0 + y) * aw + x0 + x] = b.pixels[y * b.w + x];
            glyphs.push_back(asset::FontGlyph{cps[i], x0, y0, static_cast<u16>(x0 + b.w), static_cast<u16>(y0 + b.h), static_cast<f32>(b.xoff),
                                              static_cast<f32>(b.yoff), static_cast<f32>(b.xoff + b.w), static_cast<f32>(b.yoff + b.h),
                                              static_cast<f32>(adv) * scale});
        }
    }
    for (Bitmap& b : bitmaps)
        if (b.pixels) stbtt_FreeSDF(b.pixels, nullptr);
    return packed_ok;
}

class FontImporter final : public IImporter {
public:
    StringView id() const override { return "font"; }
    u32 version() const override { return 2; }
    ChunkType type() const override { return ChunkType::Font; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".ttf" || ext == ".otf"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        (void)diags;
        if (!wants_strings(in.settings) || !in.lookup) return;
        Array<Guid> tables;
        in.lookup->assets_of_type(ChunkType::StringTable, tables);
        for (const Guid& g : tables) {
            DepRef d;
            d.guid = g;
            d.expected = ChunkType::StringTable;
            d.where = "strings";
            out.push_back(d);
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue s = in.settings;
        const StringView file = in.display_path;
        if (!detail::check_keys(s, {"size", "ranges", "padding", "strings", "sdf", "spread"}, file, kStage, diags)) return false;
        for (const char* key : {"strings", "sdf"}) {
            if (const JsonValue v = s.valid() ? s.get(key) : JsonValue(); v.valid() && !v.is_bool()) {
                String msg;
                appendf(msg, "'%s' must be true or false", key);
                detail::error_at(diags, Errc::ImportFailed, file, v, kStage, msg);
                return false;
            }
        }
        const bool sdf = [&] {
            const JsonValue v = s.valid() ? s.get("sdf") : JsonValue();
            return v.valid() && v.as_bool();
        }();
        f64 size = 32, padding = 1;
        if (!detail::read_number(s, "size", 32, size, file, kStage, diags)) return false;
        if (!detail::read_number(s, "padding", 1, padding, file, kStage, diags)) return false;
        if (!(size >= 4 && size <= 512)) {
            detail::error_at(diags, Errc::ImportFailed, file, s.get("size"), kStage, "'size' must be in [4, 512] pixels");
            return false;
        }
        if (!(padding >= 0 && padding <= 16) || padding != static_cast<f64>(static_cast<int>(padding))) {
            detail::error_at(diags, Errc::ImportFailed, file, s.get("padding"), kStage, "'padding' must be an integer in [0, 16]");
            return false;
        }
        f64 spread = std::max(2.0, static_cast<f64>(static_cast<int>(size / 8)));
        if (s.valid() && s.get("spread").valid()) {
            if (!sdf) {
                detail::error_at(diags, Errc::ImportFailed, file, s.get("spread"), kStage, "'spread' needs \"sdf\": true");
                return false;
            }
            if (!detail::read_number(s, "spread", spread, spread, file, kStage, diags)) return false;
            if (!(spread >= 1 && spread <= 32) || spread != static_cast<f64>(static_cast<int>(spread))) {
                detail::error_at(diags, Errc::ImportFailed, file, s.get("spread"), kStage, "'spread' must be an integer in [1, 32]");
                return false;
            }
        }

        Array<Range> ranges;
        if (const JsonValue r = s.get("ranges"); r.valid()) {
            if (!r.is_array() || r.size() == 0) {
                detail::error_at(diags, Errc::ImportFailed, file, r, kStage, "'ranges' must be a non-empty array of [first, last] pairs");
                return false;
            }
            for (u32 i = 0; i < r.size(); ++i) {
                const JsonValue p = r.at(i);
                if (!p.is_array() || p.size() != 2 || !p.at(0).is_integer() || !p.at(1).is_integer() || p.at(0).as_number() < 0 ||
                    p.at(1).as_number() < p.at(0).as_number() || p.at(1).as_number() > 0x10FFFF) {
                    detail::error_at(diags, Errc::ImportFailed, file, p, kStage, "range must be [first, last] codepoints, first <= last <= 0x10FFFF");
                    return false;
                }
                ranges.push_back(Range{static_cast<u32>(p.at(0).as_number()), static_cast<u32>(p.at(1).as_number())});
            }
        } else {
            ranges.push_back(Range{32, 126});
        }

        const auto* data = reinterpret_cast<const unsigned char*>(in.source.data());
        stbtt_fontinfo info;
        const int offset = in.source.size() < 16 ? -1 : stbtt_GetFontOffsetForIndex(data, 0);
        if (offset < 0 || !font_structure_ok(data, in.source.size(), offset) || !stbtt_InitFont(&info, data, offset)) {
            diags.error(Errc::ImportFailed, file, 0, 0, kStage, "not a TrueType/OpenType font");
            return false;
        }

        // The string tables' codepoints; ones the font lacks would show as missing glyphs in play.
        if (wants_strings(s)) {
            Array<u32> used;
            string_table_codepoints(in, used);
            String missing;
            u32 missing_count = 0;
            for (const u32 cp : used) {
                if (usable_glyph(info, cp) >= 0) {
                    ranges.push_back(Range{cp, cp});
                } else if (cp != 0x307) {  // the combining dot of lower("İ") outside Turkish draws on the i anyway
                    if (missing_count < 8) appendf(missing, "%sU+%04X", missing_count ? ", " : "", cp);
                    ++missing_count;
                }
            }
            if (missing_count) {
                String msg;
                appendf(msg, "the font has no glyph for %u character%s the string tables use: %s%s", missing_count,
                        missing_count == 1 ? "" : "s", missing.c_str(), missing_count > 8 ? ", ..." : "");
                diags.warning(file, 0, 0, kStage, msg);
            }
        }

        // Codepoints present in the font, sorted and unique.
        Array<u32> cps;
        for (const Range& r : ranges) {
            for (u32 cp = r.first; cp <= r.last; ++cp) {
                if (usable_glyph(info, cp) >= 0) cps.push_back(cp);
                if (cps.size() > kMaxCodepoints) {
                    diags.error(Errc::ImportFailed, file, 0, 0, kStage, "too many codepoints (max 65536)");
                    return false;
                }
            }
        }
        std::sort(cps.begin(), cps.end());
        usize uniq = 0;
        for (usize i = 0; i < cps.size(); ++i)
            if (i == 0 || cps[i] != cps[i - 1]) cps[uniq++] = cps[i];
        cps.resize(uniq);
        if (cps.empty()) {
            diags.error(Errc::ImportFailed, file, 0, 0, kStage, "the font has none of the requested codepoints");
            return false;
        }

        const f32 scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(size));
        const int max_side = sdf ? 256 : 1024;  // the distance field costs pixels x outline points per glyph
        // The bake sizes each glyph's bitmap from its header box, and its rasterizer assumes the outline lies inside that
        // bitmap. A glyph whose box is inverted or huge is a damaged font (an error); one whose outline leaves the box
        // is skipped with a warning, as if the font lacked it.
        {
            Array<u32> kept;
            u32 skipped = 0, first_skipped = 0;
            for (const u32 cp : cps) {
                const int gid = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
                int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
                stbtt_GetGlyphBitmapBox(&info, gid, scale, scale, &ix0, &iy0, &ix1, &iy1);
                if (ix1 < ix0 || iy1 < iy0 || ix1 - ix0 > max_side || iy1 - iy0 > max_side) {
                    String msg;
                    appendf(msg, "the glyph for U+%04X has a box of %d x %d pixels at this size; the font is damaged", cp, ix1 - ix0, iy1 - iy0);
                    diags.error(Errc::ImportFailed, file, 0, 0, kStage, msg);
                    return false;
                }
                stbtt_vertex* verts = nullptr;
                const int nv = stbtt_GetGlyphShape(&info, gid, &verts);
                // The rasterizer clips x and the bottom edge itself; its one precondition is that, when the bitmap's top row is
                // the glyph's first (iy0 == 0), no edge lies entirely above it. The distance field writes only inside its bitmap.
                bool inside = true;
                for (int i = 0; i < nv && inside && !sdf && iy0 == 0; ++i) {
                    const stbtt_vertex& v = verts[i];
                    const f32 ys[2] = {-static_cast<f32>(v.y) * scale, -static_cast<f32>(v.cy) * scale};
                    const int n = v.type == STBTT_vcurve ? 2 : 1;
                    for (int k = 0; k < n; ++k)
                        if (ys[k] < 0.0f) inside = false;
                }
                if (verts) stbtt_FreeShape(&info, verts);
                if (inside) {
                    kept.push_back(cp);
                } else {
                    if (skipped++ == 0) first_skipped = cp;
                }
            }
            if (skipped) {
                String msg;
                appendf(msg, "%u glyph%s skipped: the outline rises above the box the font gives it (first U+%04X)", skipped, skipped == 1 ? "" : "s", first_skipped);
                diags.warning(file, 0, 0, kStage, msg);
                cps = kept;
                if (cps.empty()) {
                    diags.error(Errc::ImportFailed, file, 0, 0, kStage, "the font has none of the requested codepoints");
                    return false;
                }
            }
        }
        Array<asset::FontGlyph> glyphs;
        Array<u8> atlas;
        u32 aw = 128, ah = 128;
        const bool baked = sdf ? bake_sdf(info, scale, cps, static_cast<int>(spread), static_cast<int>(padding), glyphs, atlas, aw, ah)
                               : bake_coverage(data, static_cast<f32>(size), cps, static_cast<int>(padding), glyphs, atlas, aw, ah);
        if (!baked) {
            diags.error(Errc::ImportFailed, file, 0, 0, kStage, "glyphs do not fit a 4096x4096 atlas (reduce size or ranges)");
            return false;
        }

        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);

        // Kern pairs: glyph-index pairs from the kern table, mapped back to our codepoints.
        Array<asset::FontKern> kerns;
        const int kern_len = stbtt_GetKerningTableLength(&info);
        if (kern_len > 0) {
            Array<stbtt_kerningentry> table;
            table.resize(static_cast<usize>(kern_len));
            stbtt_GetKerningTable(&info, table.data(), kern_len);
            struct GlyphCp {
                int glyph;
                u32 cp;
            };
            Array<GlyphCp> map;
            for (u32 cp : cps) map.push_back(GlyphCp{stbtt_FindGlyphIndex(&info, static_cast<int>(cp)), cp});
            std::sort(map.begin(), map.end(), [](const GlyphCp& a, const GlyphCp& b) { return a.glyph != b.glyph ? a.glyph < b.glyph : a.cp < b.cp; });
            auto cps_for = [&](int glyph, auto&& fn) {
                const GlyphCp* it = std::lower_bound(map.begin(), map.end(), glyph, [](const GlyphCp& g, int v) { return g.glyph < v; });
                for (; it != map.end() && it->glyph == glyph; ++it) fn(it->cp);
            };
            for (const stbtt_kerningentry& k : table) {
                if (k.advance == 0) continue;
                cps_for(k.glyph1, [&](u32 left) {
                    cps_for(k.glyph2, [&](u32 right) { if (kerns.size() < kMaxKerns) kerns.push_back(asset::FontKern{left, right, static_cast<f32>(k.advance) * scale}); });
                });
            }
            std::sort(kerns.begin(), kerns.end(), [](const asset::FontKern& a, const asset::FontKern& b) {
                return a.left != b.left ? a.left < b.left : a.right < b.right;
            });
        }

        asset::FontHeader h{};
        h.pixel_height = static_cast<f32>(size);
        h.ascent = static_cast<f32>(ascent) * scale;
        h.descent = static_cast<f32>(descent) * scale;
        h.line_gap = static_cast<f32>(gap) * scale;
        h.atlas_width = aw;
        h.atlas_height = ah;
        h.flags = sdf ? asset::FontFlags::Sdf : 0u;
        h.sdf_spread = sdf ? static_cast<f32>(spread) : 0.0f;
        Array<byte> payload;
        asset::write_font(h, glyphs.span(), kerns.span(), Span<const byte>(reinterpret_cast<const byte*>(atlas.data()), atlas.size()), payload);
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Font;
        d.version = asset::chunk_version(asset::FONT_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_font_importer() { return mem::make_unique<FontImporter>(mem::heap()); }

}  // namespace mce::cook
