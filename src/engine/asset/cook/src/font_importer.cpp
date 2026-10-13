// TTF -> CHNK_FONT: glyph atlas baked at cook with stb_truetype + stb_rect_pack (Skyline), plus
// metrics and the kern-table pairs (assets.md §5.2; "atlas built at cook, not runtime" is the
// FR-R2D-060 acceptance test). MSDF via msdf-atlas-gen is the v0.5 upgrade, same chunk type.
//
// sidecar settings: { "size": 32, "ranges": [[32, 126]], "padding": 1, "strings": true }
//   size     pixel height (ascent - descent)
//   ranges   inclusive codepoint ranges; codepoints the font lacks are skipped
//   strings  also every codepoint the project's string tables use (assets/loc/*.csv, M-MOBILE 11a),
//            with their upper- and lower-case forms in each table's locale (Turkish İ and ı), so a
//            translation never shows a missing glyph; the tables become dependencies of the font
// Kerning comes from the legacy 'kern' table only (GPOS kerning is not read).
// stb_truetype trusts its input: fonts are cooked from the project's own tree, never at runtime.

#include <algorithm>

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

class FontImporter final : public IImporter {
public:
    StringView id() const override { return "font"; }
    u32 version() const override { return 1; }
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
        if (!detail::check_keys(s, {"size", "ranges", "padding", "strings"}, file, kStage, diags)) return false;
        if (const JsonValue v = s.valid() ? s.get("strings") : JsonValue(); v.valid() && !v.is_bool()) {
            detail::error_at(diags, Errc::ImportFailed, file, v, kStage, "'strings' must be true or false");
            return false;
        }
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
        const int offset = in.source.size() < 12 ? -1 : stbtt_GetFontOffsetForIndex(data, 0);
        if (offset < 0 || !stbtt_InitFont(&info, data, offset)) {
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
                if (stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0 || cp == 32) {
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
                if (stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0 || cp == 32) cps.push_back(cp);
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

        // Pack, growing the atlas until everything fits.
        Array<int> cp_ints;
        for (u32 cp : cps) cp_ints.push_back(static_cast<int>(cp));
        Array<stbtt_packedchar> packed;
        packed.resize(cps.size());
        Array<u8> atlas;
        u32 aw = 128, ah = 128;
        bool packed_ok = false;
        while (aw <= 4096 && ah <= 4096) {
            atlas.clear();
            atlas.resize(static_cast<usize>(aw) * ah, 0);
            stbtt_pack_context pc;
            if (stbtt_PackBegin(&pc, atlas.data(), static_cast<int>(aw), static_cast<int>(ah), 0, static_cast<int>(padding), nullptr)) {
                stbtt_PackSetOversampling(&pc, 1, 1);
                stbtt_pack_range range{};
                range.font_size = static_cast<float>(size);
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
        if (!packed_ok) {
            diags.error(Errc::ImportFailed, file, 0, 0, kStage, "glyphs do not fit a 4096x4096 atlas (reduce size or ranges)");
            return false;
        }

        const f32 scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(size));
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);

        Array<asset::FontGlyph> glyphs;
        for (usize i = 0; i < cps.size(); ++i) {
            const stbtt_packedchar& p = packed[i];
            glyphs.push_back(asset::FontGlyph{cps[i], p.x0, p.y0, p.x1, p.y1, p.xoff, p.yoff, p.xoff2, p.yoff2, p.xadvance});
        }

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
                    cps_for(k.glyph2, [&](u32 right) { kerns.push_back(asset::FontKern{left, right, static_cast<f32>(k.advance) * scale}); });
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
