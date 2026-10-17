// PNG/JPG -> CHNK_TEX, RGBA8 with a box-filtered mip chain (assets.md §5.2). Phone
// cooks (ImportInput::texture_compression, mce_cook --textures astc) store every level as ASTC 4x4
// blocks instead, encoded from the same RGBA8 chain (astc-encoder, medium quality, one thread). Desktop
// cooks with --textures bc store BC7 / BC5 / BC4 / BC1 (texture_bc.cpp, bc7enc_rdo), see "compress".
//
// sidecar settings: { "srgb": true, "mips": true, "filter": "nearest", "max_size": 1024, "compress": "auto" }
//   srgb  color data is sRGB-encoded: mips are averaged in linear space and the chunk carries the
//         Srgb flag so the renderer samples through an sRGB view (never double-corrects)
//   mips  generate the full chain down to 1x1
//   filter  "nearest" (pixel art, the default) or "linear": how kit sprites sample it (LinearFilter flag)
//   max_size  (M-CONTENT 1a) largest side, a power of two 1..16384: a bigger image is box-halved (the same filter as the
//         mips, sRGB-aware, no renormalizing of normal maps) until it fits, BEFORE the mips. Default: no limit. The
//         project-wide cap (ImportInput::max_texture_size, mce_cook --max-texture-size) applies as min(sidecar, cap).
//   compress  (M-CONTENT 1b) "none" | "auto" | "bc7" | "bc5" | "bc4" | "bc1" | "astc". The cook mode decides what a value
//         produces; a missing key means the mode's default (rgba8: RGBA8, bc: as "auto", astc: ASTC 4x4):
//           value               --textures rgba8   --textures bc          --textures astc
//           missing             RGBA8              as auto                ASTC 4x4
//           none                RGBA8              RGBA8                  RGBA8
//           auto                RGBA8              the rule (choose_auto) ASTC 4x4
//           bc7|bc5|bc4|bc1     RGBA8              that format            ASTC 4x4
//           astc                RGBA8              RGBA8                  ASTC 4x4
//         BC5 and BC4 are linear data: "srgb": true with them is an error. BC needs both base sides to be multiples of 4;
//         otherwise the texture cooks to RGBA8 with a warning. Not accepted on .hdr/.exr (BC6H is out of scope).
//
// HDR sources (M-VISUAL3 4): Radiance .hdr (stb) and OpenEXR .exr (tinyexr: uncompressed, ZIP, PIZ, the
// first RGB(A) layer) cook to TextureFormat::RGBA16F, linear, never Srgb. Settings: { "mips": true,
// "max_size": 2048, "exposure": 0, "filter": ... }
//   max_size  largest side, a power of two: the image is box-halved until it fits (a float sky is 8 bytes a texel)
//   exposure  EV applied at cook (x 2^EV)
// Values are clamped to [0, 65504] (the half range), NaN and Inf become 0, mips are box-filtered in float.
// "srgb": true is an error. Phone cooks (--textures astc) keep RGBA16F for these.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <astcenc.h>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "stb_image.h"
#include "texture_bc.h"
#include "tinyexr.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.texture";

struct SrgbTables {
    f32 to_linear[256];
    SrgbTables() {
        for (int i = 0; i < 256; ++i) {
            const f64 c = i / 255.0;
            to_linear[i] = static_cast<f32>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
    }
    // Nearest 8-bit code for a linear value: binary search over the (monotonic) decode table, so
    // encode is the exact inverse of decode and needs no pow() of its own.
    u8 to_srgb(f32 lin) const {
        int lo = 0, hi = 255;
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (to_linear[mid] < lin) lo = mid + 1;
            else hi = mid;
        }
        if (lo > 0 && (lin - to_linear[lo - 1]) <= (to_linear[lo] - lin)) --lo;
        return static_cast<u8>(lo);
    }
};

const SrgbTables& srgb() {
    static const SrgbTables t;
    return t;
}

void downsample(const u8* src, u32 sw, u32 sh, u8* dst, u32 dw, u32 dh, bool is_srgb) {
    const SrgbTables& t = srgb();
    for (u32 y = 0; y < dh; ++y) {
        const u32 y0 = y * 2 < sh ? y * 2 : sh - 1;
        const u32 y1 = y * 2 + 1 < sh ? y * 2 + 1 : sh - 1;
        for (u32 x = 0; x < dw; ++x) {
            const u32 x0 = x * 2 < sw ? x * 2 : sw - 1;
            const u32 x1 = x * 2 + 1 < sw ? x * 2 + 1 : sw - 1;
            const u8* p[4] = {src + (y0 * sw + x0) * 4, src + (y0 * sw + x1) * 4, src + (y1 * sw + x0) * 4, src + (y1 * sw + x1) * 4};
            u8* out = dst + (y * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                if (is_srgb && c < 3) {
                    const f32 sum = t.to_linear[p[0][c]] + t.to_linear[p[1][c]] + t.to_linear[p[2][c]] + t.to_linear[p[3][c]];
                    out[c] = t.to_srgb(sum * 0.25f);
                } else {
                    out[c] = static_cast<u8>((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
                }
            }
        }
    }
}

// One RGBA8 level -> ASTC 4x4 blocks with an already configured context. False on an encoder error.
bool encode_astc(astcenc_context* ctx, const u8* rgba, u32 w, u32 h, Array<u8>& out) {
    out.resize_uninitialized(static_cast<usize>(asset::texture_level_bytes(asset::TextureFormat::ASTC_4x4, w, h)));
    void* slices[1] = {const_cast<u8*>(rgba)};  // astcenc reads only
    astcenc_image img{w, h, 1, ASTCENC_TYPE_U8, slices};
    const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const astcenc_error e = astcenc_compress_image(ctx, &img, &swz, out.data(), out.size(), 0);
    astcenc_compress_reset(ctx);
    return e == ASTCENC_SUCCESS;
}

// ------------------------------------------------------------------ HDR (RGBA16F)

constexpr int kMaxHdrSide = 16384;
constexpr i64 kMaxHdrPixels = i64(8192) * 8192;  // 64 MP: 1 GiB of float RGBA at most

// IEEE half from a finite float already inside [0, 65504], round to nearest even.
u16 to_half(f32 f) {
    u32 x;
    std::memcpy(&x, &f, 4);
    const i32 e = i32((x >> 23) & 0xFFu) - 127 + 15;
    u32 m = x & 0x7FFFFFu;
    if (e <= 0) {
        if (e < -10) return 0;
        m |= 0x800000u;
        const u32 shift = u32(14 - e);
        return u16((m + (1u << (shift - 1)) - 1 + ((m >> shift) & 1u)) >> shift);
    }
    const u32 h = (u32(e) << 10) | (m >> 13);
    const u32 round = (m & 0x1FFFu) > 0x1000u || ((m & 0x1FFFu) == 0x1000u && (h & 1u)) ? 1u : 0u;
    return u16(h + round);  // a carry into the exponent is still correct
}

// One level of 2x2 box filtering in float (odd sizes drop the last row / column, as the 8-bit chain does).
void downsample_f(const f32* src, u32 sw, u32 sh, f32* dst, u32 dw, u32 dh) {
    for (u32 y = 0; y < dh; ++y) {
        const u32 y0 = y * 2 < sh ? y * 2 : sh - 1;
        const u32 y1 = y * 2 + 1 < sh ? y * 2 + 1 : sh - 1;
        for (u32 x = 0; x < dw; ++x) {
            const u32 x0 = x * 2 < sw ? x * 2 : sw - 1;
            const u32 x1 = x * 2 + 1 < sw ? x * 2 + 1 : sw - 1;
            const f32* p[4] = {src + (usize(y0) * sw + x0) * 4, src + (usize(y0) * sw + x1) * 4, src + (usize(y1) * sw + x0) * 4,
                               src + (usize(y1) * sw + x1) * 4};
            f32* out = dst + (usize(y) * dw + x) * 4;
            for (int c = 0; c < 4; ++c) out[c] = (p[0][c] + p[1][c] + p[2][c] + p[3][c]) * 0.25f;
        }
    }
}

// stb reads missing bytes of a truncated Radiance file as zeros (or, in the flat form, as uninitialized stack), and
// reports success. Reading through callbacks lets the cook see the first read past the end and refuse the file.
struct StbMemory {
    const unsigned char* data;
    usize size;
    usize pos;
    bool past_end;
};
int stb_mem_read(void* user, char* dst, int n) {
    auto* m = static_cast<StbMemory*>(user);
    const usize left = m->size - m->pos;
    const usize got = usize(n) < left ? usize(n) : left;
    if (got == 0 && n > 0) m->past_end = true;
    std::memcpy(dst, m->data + m->pos, got);
    m->pos += got;
    return int(got);
}
void stb_mem_skip(void* user, int n) {
    auto* m = static_cast<StbMemory*>(user);
    if (n < 0 || usize(n) > m->size - m->pos) {
        m->pos = m->size;
        m->past_end = true;
    } else {
        m->pos += usize(n);
    }
}
int stb_mem_eof(void* user) {
    const auto* m = static_cast<const StbMemory*>(user);
    return m->pos >= m->size;
}

// Owns a decoder's float buffer (stb and tinyexr both allocate with malloc); freed on every path.
struct DecodedPixels {
    f32* p = nullptr;
    DecodedPixels() = default;
    DecodedPixels(const DecodedPixels&) = delete;
    DecodedPixels& operator=(const DecodedPixels&) = delete;
    ~DecodedPixels() { reset(); }
    void reset() {
        std::free(p);
        p = nullptr;
    }
};

// Decodes a Radiance .hdr or an OpenEXR file to linear float RGBA (alpha 1 when the file has none); the decoder's
// own buffer is kept in `px` (w * h * 4 floats), not copied. The header is read and its size checked first, so a
// tiny hostile file cannot force a huge allocation. False with a diagnostic on a decode error or a refused size.
bool decode_hdr(const ImportInput& in, bool exr, DecodedPixels& px, u32& w, u32& h, DiagList& diags) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(in.source.data());
    const usize size = in.source.size();
    auto fail = [&](const char* what, const char* detail) {
        String msg(what);
        if (detail && *detail) {
            msg += ": ";
            msg += detail;
        }
        diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, msg);
        return false;
    };
    // Sizes are checked in 64-bit math: each side 1..16384 and the pixel count under kMaxHdrPixels.
    auto check_size = [&](i64 sw, i64 sh) {
        if (sw <= 0 || sh <= 0) return fail("image dimensions out of range (an empty image; each side must be 1..16384)", nullptr);
        if (sw > kMaxHdrSide || sh > kMaxHdrSide) return fail("image dimensions out of range (each side must be 1..16384): downscale the source", nullptr);
        if (sw * sh > kMaxHdrPixels) {
            char msg[160];
            std::snprintf(msg, sizeof msg, "image is %lld x %lld, over the limit of 64 megapixels (8192 x 8192): downscale the source",
                          static_cast<long long>(sw), static_cast<long long>(sh));
            return fail(msg, nullptr);
        }
        return true;
    };
    int iw = 0, ih = 0;
    if (exr) {
        // tinyexr's one-call LoadEXRFromMemory leaks the parsed header when the pixel load fails or a channel is
        // missing, so the header-based path is used and both the header and the image are freed here, on every path.
        struct ExrState {
            EXRHeader hdr;
            EXRImage img;
            bool image_loaded = false;  // FreeEXRImage after a failed load frees pointers tinyexr never set: only free a good one
            ExrState() {
                InitEXRHeader(&hdr);
                InitEXRImage(&img);
            }
            ~ExrState() {
                if (image_loaded) FreeEXRImage(&img);
                FreeEXRHeader(&hdr);
            }
        } ex;
        const char* err = nullptr;
        EXRVersion ver;
        if (ParseEXRVersionFromMemory(&ver, bytes, size) != TINYEXR_SUCCESS) return fail("EXR decode failed", "not an OpenEXR file");
        if (ParseEXRHeaderFromMemory(&ex.hdr, &ver, bytes, size, &err) != TINYEXR_SUCCESS) {
            const bool ok = fail("EXR decode failed", err);
            if (err) FreeEXRErrorMessage(err);
            return ok;
        }
        // the size first: a hostile header must not allocate gigabytes of floats
        const i64 dw = i64(ex.hdr.data_window.max_x) - ex.hdr.data_window.min_x + 1, dh = i64(ex.hdr.data_window.max_y) - ex.hdr.data_window.min_y + 1;
        if (!check_size(dw, dh)) return false;
        for (int c = 0; c < ex.hdr.num_channels; ++c)  // half channels are read as float
            if (ex.hdr.pixel_types[c] == TINYEXR_PIXELTYPE_HALF) ex.hdr.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
        if (LoadEXRImageFromMemory(&ex.img, &ex.hdr, bytes, size, &err) != TINYEXR_SUCCESS) {
            const bool ok = fail("EXR decode failed", err);
            if (err) FreeEXRErrorMessage(err);
            return ok;
        }
        ex.image_loaded = true;
        iw = ex.img.width;
        ih = ex.img.height;
        if (!check_size(iw, ih)) return false;
        // To RGBA as LoadEXRFromMemory does: a single channel is grey (and alpha), otherwise R, G, B and an optional A.
        int idx[4] = {-1, -1, -1, -1};
        for (int c = 0; c < ex.hdr.num_channels; ++c) {
            const char* n = ex.hdr.channels[c].name;
            if (n[0] && !n[1]) {
                if (n[0] == 'R') idx[0] = c;
                else if (n[0] == 'G') idx[1] = c;
                else if (n[0] == 'B') idx[2] = c;
                else if (n[0] == 'A') idx[3] = c;
            }
        }
        const bool grey = ex.hdr.num_channels == 1;
        if (!grey) {
            if (idx[0] < 0) return fail("EXR decode failed", "R channel not found");
            if (idx[1] < 0) return fail("EXR decode failed", "G channel not found");
            if (idx[2] < 0) return fail("EXR decode failed", "B channel not found");
        }
        const usize npx = usize(iw) * usize(ih);
        px.p = static_cast<f32*>(std::calloc(npx * 4, sizeof(f32)));  // zeroed: a tiled file may leave texels unwritten
        if (!px.p) return fail("EXR decode failed", "out of memory");
        auto put = [&](usize dst, unsigned char* const* chans, usize src) {
            const f32* const* ch = reinterpret_cast<const f32* const*>(chans);
            f32* o = px.p + dst * 4;
            if (grey) {
                o[0] = o[1] = o[2] = o[3] = ch[0][src];
            } else {
                o[0] = ch[idx[0]][src];
                o[1] = ch[idx[1]][src];
                o[2] = ch[idx[2]][src];
                o[3] = idx[3] >= 0 ? ch[idx[3]][src] : 1.0f;
            }
        };
        if (ex.hdr.tiled) {
            const usize tx = usize(ex.hdr.tile_size_x), ty = usize(ex.hdr.tile_size_y);
            for (int it = 0; it < ex.img.num_tiles; ++it) {
                for (usize j = 0; j < ty; ++j) {
                    for (usize i = 0; i < tx; ++i) {
                        const usize ii = usize(ex.img.tiles[it].offset_x) * tx + i, jj = usize(ex.img.tiles[it].offset_y) * ty + j;
                        if (ii >= usize(iw) || jj >= usize(ih)) continue;  // outside the image: edge tile padding
                        put(ii + jj * usize(iw), ex.img.tiles[it].images, i + j * tx);
                    }
                }
            }
        } else {
            for (usize i = 0; i < npx; ++i) put(i, ex.img.images, i);
        }
    } else {
        int comp = 0;
        if (!stbi_info_from_memory(bytes, int(size), &iw, &ih, &comp)) return fail("image decode failed", stbi_failure_reason());
        if (!check_size(iw, ih)) return false;
        StbMemory mem{bytes, size, 0, false};
        const stbi_io_callbacks cb{stb_mem_read, stb_mem_skip, stb_mem_eof};
        px.p = stbi_loadf_from_callbacks(&cb, &mem, &iw, &ih, &comp, 4);
        if (!px.p) return fail("image decode failed", stbi_failure_reason());
        if (mem.past_end) return fail("image decode failed", "the file is truncated (its pixel data ends early)");
    }
    if (!check_size(iw, ih)) return false;  // the decoder disagreeing with its own header; px frees itself
    w = u32(iw);
    h = u32(ih);
    return true;
}

bool cook_hdr(const ImportInput& in, bool exr, asset::ContainerWriter& out, DiagList& diags) {
    const JsonValue s = in.settings;
    if (!detail::check_keys(s, {"srgb", "mips", "filter", "max_size", "exposure"}, in.display_path, kStage, diags)) return false;
    bool mips = true;
    if (const JsonValue sv = s.get("srgb"); sv.valid()) {
        bool is_srgb = false;
        if (!detail::read_bool(s, "srgb", false, is_srgb, in.display_path, kStage, diags)) return false;
        if (is_srgb) {
            detail::error_at(diags, Errc::ParseError, in.display_path, sv, kStage,
                             "'srgb': true is not valid for an HDR image (.hdr / .exr hold linear float values, "
                             "never sRGB-encoded colors): remove \"srgb\" from the sidecar or set it to false");
            return false;
        }
    }
    if (!detail::read_bool(s, "mips", true, mips, in.display_path, kStage, diags)) return false;
    bool linear = true;
    if (const JsonValue f = s.get("filter"); f.valid()) {
        const StringView v = f.is_string() ? f.as_string() : StringView();
        if (v != "nearest" && v != "linear") {
            detail::error_at(diags, Errc::ParseError, in.display_path, f, kStage, "'filter' must be \"nearest\" or \"linear\"");
            return false;
        }
        linear = v == "linear";
    }
    f64 max_size_v = 2048.0, exposure_ev = 0.0;
    if (!detail::read_number(s, "max_size", 2048.0, max_size_v, in.display_path, kStage, diags)) return false;
    if (!detail::read_number(s, "exposure", 0.0, exposure_ev, in.display_path, kStage, diags)) return false;
    const u32 max_size = max_size_v >= 1.0 && max_size_v <= kMaxHdrSide ? u32(max_size_v) : 0u;
    if (max_size == 0 || f64(max_size) != max_size_v || (max_size & (max_size - 1)) != 0) {
        detail::error_at(diags, Errc::ParseError, in.display_path, s.get("max_size"), kStage,
                         "'max_size' must be a power of two from 1 to 16384 (2048 by default)");
        return false;
    }
    if (!(exposure_ev >= -32.0 && exposure_ev <= 32.0)) {
        detail::error_at(diags, Errc::ParseError, in.display_path, s.get("exposure"), kStage, "'exposure' is in EV and must be within -32..32");
        return false;
    }
    if (in.source.size() > 0x7FFFFFFF) {
        diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "image file too large");
        return false;
    }

    DecodedPixels px;
    u32 w = 0, h = 0;
    if (!decode_hdr(in, exr, px, w, h, diags)) return false;

    // Exposure, then the half range: negatives and NaN/Inf become 0, the rest clamps to 65504. In place, in the
    // decoder's buffer: the full-size image is never copied.
    const f32 gain = static_cast<f32>(std::exp2(exposure_ev));
    const usize count = usize(w) * h * 4;
    for (usize i = 0; i < count; ++i) {
        f32 v = px.p[i];
        if (i % 4 != 3) v *= gain;
        px.p[i] = std::isfinite(v) ? (v < 0.0f ? 0.0f : (v > 65504.0f ? 65504.0f : v)) : 0.0f;
    }

    // Power-of-two downscale until the largest side fits, then the mip chain, both box-filtered in float.
    // `cur` reads the decoder's buffer first and the owned array after the first reduction (then px is freed).
    const f32* cur = px.p;
    Array<f32> owned, next;
    auto step = [&](u32 sw, u32 sh, u32 nw, u32 nh) {
        next.resize_uninitialized(usize(nw) * nh * 4);
        downsample_f(cur, sw, sh, next.data(), nw, nh);
        owned = static_cast<Array<f32>&&>(next);
        cur = owned.data();
        px.reset();
    };
    while (w > max_size || h > max_size) {
        const u32 nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        step(w, h, nw, nh);
        w = nw;
        h = nh;
    }

    Array<Array<u16>> levels;
    Array<asset::TextureMipData> mip_data;
    for (u32 lw = w, lh = h;;) {
        Array<u16>& half = levels.emplace_back();
        half.resize_uninitialized(usize(lw) * lh * 4);
        for (usize i = 0; i < half.size(); ++i) half[i] = to_half(cur[i]);
        if (!mips || (lw == 1 && lh == 1)) break;
        const u32 nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
        step(lw, lh, nw, nh);
        lw = nw;
        lh = nh;
    }
    u32 lw = w, lh = h;
    for (const Array<u16>& half : levels) {
        mip_data.push_back(asset::TextureMipData{lw, lh, Span<const byte>(reinterpret_cast<const byte*>(half.data()), half.size() * sizeof(u16))});
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }

    Array<byte> payload;
    const asset::TextureFormat format = asset::TextureFormat::RGBA16F;
    asset::write_texture(format, 1, linear ? asset::TextureFlags::LinearFilter : 0u, mip_data.span(), payload);
    asset::ContainerWriter::ChunkDesc d;
    d.type = ChunkType::Texture;
    d.version = asset::chunk_version(asset::TEXTURE_VERSION, 0);
    d.user_flags = static_cast<u32>(format);
    d.guid = in.guid;
    out.add(d, payload.span());
    return true;
}

// What one texture cooks to under the current mode (see the table in the file comment).
enum class Target : u8 { Rgba8, Astc, Bc };
enum class CompressKey : u8 { Missing, None, Auto, Bc7, Bc5, Bc4, Bc1, Astc };

// `auto` under --textures bc, on the base level after max_size. Integer-only, so the choice is deterministic.
// PROVISIONAL: the 99% and the length bounds are guesses; they will be calibrated with RunPod measurements
// (M-CONTENT 1b-iv tunes them on the Lantern Keep and Poly Haven normal maps).
// Order: (0) LUT strips stay RGBA8 (colour grading cannot take block error), (1) srgb -> BC7, (2) at least 99% of
// texels decode to a unit normal -> BC5, (3) grey opaque data -> BC4, (4) BC7. Never picks BC1.
// Returns false for "keep RGBA8" (a LUT strip), else sets `fmt`; `normal_map` says rule 2 fired.
bool choose_auto(const u8* rgba, u32 w, u32 h, bool srgb, BcFormat& fmt, bool& normal_map) {
    normal_map = false;
    if ((h >= 16 && w == h * h) || (w >= 16 && h == w * w)) return false;
    if (srgb) {
        fmt = BcFormat::BC7;
        return true;
    }
    const usize n = usize(w) * h;
    usize normals = 0;
    bool grey = true;
    for (usize i = 0; i < n; ++i) {
        const u8* p = rgba + i * 4;
        if (p[3] != 255) grey = false;
        if (p[0] != p[1] || p[1] != p[2]) grey = false;
        const i64 x = 2 * i64(p[0]) - 255, y = 2 * i64(p[1]) - 255, z = 2 * i64(p[2]) - 255;
        const i64 len2 = (x * x + y * y + z * z) * 100;  // within [0.81, 1.21] * 255^2, scaled by 100
        if (p[3] == 255 && z > 0 && len2 >= 81 * 255 * 255 && len2 <= 121 * 255 * 255) ++normals;
    }
    if (normals * 100 >= n * 99) {
        normal_map = true;
        fmt = BcFormat::BC5;
        return true;
    }
    fmt = grey ? BcFormat::BC4 : BcFormat::BC7;
    return true;
}

class TextureImporter final : public IImporter {
public:
    StringView id() const override { return "texture"; }
    u32 version() const override { return 1; }  // bump with any encoder setting in texture_bc.cpp
    ChunkType type() const override { return ChunkType::Texture; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override {
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr" || ext == ".exr";
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const String ext = lower_extension(in.display_path);
        if (ext.view() == ".hdr" || ext.view() == ".exr") return cook_hdr(in, ext.view() == ".exr", out, diags);
        const JsonValue s = in.settings;
        if (!detail::check_keys(s, {"srgb", "mips", "filter", "max_size", "compress"}, in.display_path, kStage, diags)) return false;
        bool is_srgb = true, mips = true, linear = false;
        if (!detail::read_bool(s, "srgb", true, is_srgb, in.display_path, kStage, diags)) return false;
        if (!detail::read_bool(s, "mips", true, mips, in.display_path, kStage, diags)) return false;
        if (const JsonValue f = s.get("filter"); f.valid()) {
            const StringView v = f.is_string() ? f.as_string() : StringView();
            if (v != "nearest" && v != "linear") {
                detail::error_at(diags, Errc::ParseError, in.display_path, f, kStage, "'filter' must be \"nearest\" or \"linear\"");
                return false;
            }
            linear = v == "linear";
        }

        CompressKey key = CompressKey::Missing;
        if (const JsonValue cv = s.get("compress"); cv.valid()) {
            const StringView v = cv.is_string() ? cv.as_string() : StringView();
            if (v == "none") key = CompressKey::None;
            else if (v == "auto") key = CompressKey::Auto;
            else if (v == "bc7") key = CompressKey::Bc7;
            else if (v == "bc5") key = CompressKey::Bc5;
            else if (v == "bc4") key = CompressKey::Bc4;
            else if (v == "bc1") key = CompressKey::Bc1;
            else if (v == "astc") key = CompressKey::Astc;
            else {
                detail::error_at(diags, Errc::ParseError, in.display_path, cv, kStage,
                                 "'compress' must be one of \"none\", \"auto\", \"bc7\", \"bc5\", \"bc4\", \"bc1\" or \"astc\" "
                                 "(or leave the key out for the cook mode's default)");
                return false;
            }
            if ((key == CompressKey::Bc5 || key == CompressKey::Bc4) && is_srgb) {
                detail::error_at(diags, Errc::ParseError, in.display_path, cv, kStage,
                                 "'compress' bc5 and bc4 store linear data: set \"srgb\": false (normal, height and mask "
                                 "maps), or use \"bc7\" or \"bc1\" for colour");
                return false;
            }
        }

        // max_size: no limit unless the sidecar or the project cap says so; the smaller of the two wins.
        u32 cap = in.max_texture_size;
        if (const JsonValue mv = s.get("max_size"); mv.valid()) {
            f64 mv_num = 0.0;
            if (!detail::read_number(s, "max_size", 0.0, mv_num, in.display_path, kStage, diags)) return false;
            const u32 side = mv_num >= 1.0 && mv_num <= kMaxHdrSide ? u32(mv_num) : 0u;
            if (side == 0 || f64(side) != mv_num || (side & (side - 1)) != 0) {
                detail::error_at(diags, Errc::ParseError, in.display_path, mv, kStage,
                                 "'max_size' must be a power of two from 1 to 16384 (no limit by default), for example "
                                 "\"max_size\": 1024: change it in the .mceasset sidecar, or remove the key to use the project-wide cap");
                return false;
            }
            if (cap == 0 || side < cap) cap = side;
        }

        if (in.source.size() > 0x7FFFFFFF) {
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "image file too large");
            return false;
        }
        int w = 0, h = 0, comp = 0;
        stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(in.source.data()), static_cast<int>(in.source.size()), &w, &h,
                                            &comp, 4);
        if (!px) {
            String msg("image decode failed: ");
            msg += stbi_failure_reason();
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, msg);
            return false;
        }
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
            stbi_image_free(px);
            diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "image dimensions out of range (1..16384)");
            return false;
        }

        // Power-of-two downscale until the largest side fits `cap`, box-filtered like the mips, before the chain.
        u32 cw = static_cast<u32>(w), ch = static_cast<u32>(h);
        const u8* base = px;
        Array<u8> reduced, next;
        if (cap != 0) {
            while (cw > cap || ch > cap) {
                const u32 nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
                next.resize_uninitialized(usize(nw) * nh * 4);
                downsample(base, cw, ch, next.data(), nw, nh, is_srgb);
                reduced = static_cast<Array<u8>&&>(next);
                base = reduced.data();
                cw = nw;
                ch = nh;
            }
        }
        const u32 bw = cw, bh = ch;  // the base level

        // Mip chain storage: level i lives at offsets[i] in `levels`.
        Array<u8> levels;
        Array<asset::TextureMipData> mip_data;
        Array<usize> offsets;
        usize total = 0;
        for (u32 lw = cw, lh = ch;;) {
            offsets.push_back(total);
            total += static_cast<usize>(lw) * lh * 4;
            if (!mips || (lw == 1 && lh == 1)) break;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        levels.resize_uninitialized(total);
        std::memcpy(levels.data(), base, static_cast<usize>(bw) * bh * 4);
        stbi_image_free(px);
        reduced.clear();
        for (usize i = 1; i < offsets.size(); ++i) {
            const u32 nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
            downsample(levels.data() + offsets[i - 1], cw, ch, levels.data() + offsets[i], nw, nh, is_srgb);
            cw = nw;
            ch = nh;
        }
        cw = bw;
        ch = bh;
        // What this texture cooks to: the cook mode and the sidecar's "compress" (the table in the file comment).
        Target target = Target::Rgba8;
        BcFormat bcf = BcFormat::BC7;
        if (in.texture_compression == TextureCompression::Astc4x4) {
            target = key == CompressKey::None ? Target::Rgba8 : Target::Astc;
        } else if (in.texture_compression == TextureCompression::Bc) {
            switch (key) {
                case CompressKey::None:
                case CompressKey::Astc: break;
                case CompressKey::Bc7: target = Target::Bc, bcf = BcFormat::BC7; break;
                case CompressKey::Bc5: target = Target::Bc, bcf = BcFormat::BC5; break;
                case CompressKey::Bc4: target = Target::Bc, bcf = BcFormat::BC4; break;
                case CompressKey::Bc1: target = Target::Bc, bcf = BcFormat::BC1; break;
                case CompressKey::Missing:
                case CompressKey::Auto: {
                    bool normal_map = false;
                    if (choose_auto(levels.data(), cw, ch, is_srgb, bcf, normal_map)) {
                        target = Target::Bc;
                        if (normal_map) {
                            String msg("'compress' auto chose bc5 (the texture looks like a normal map); set \"compress\" to override");
                            diags.warning(in.display_path, 0, 0, kStage, msg);
                        }
                    }
                    break;
                }
            }
            if (target == Target::Bc && ((cw & 3) != 0 || (ch & 3) != 0)) {
                char buf[256];
                std::snprintf(buf, sizeof buf,
                              "texture is %ux%u: block compression needs both sides to be multiples of 4, so it is cooked as "
                              "RGBA8. Resize the image to a multiple of 4 or set a \"max_size\" that gives one",
                              cw, ch);
                const String msg(buf);
                diags.warning(in.display_path, 0, 0, kStage, msg);
                target = Target::Rgba8;
            }
        }
        const bool astc = target == Target::Astc;
        const bool bc = target == Target::Bc;
        Array<Array<u8>> blocks;  // per level, when encoding ASTC or BC
        if (bc) {
            blocks.resize(offsets.size());
            u32 lw = cw, lh = ch;
            bool alpha_dropped = false;
            for (usize i = 0; i < offsets.size(); ++i) {
                blocks[i].resize_uninitialized(usize(bc_level_bytes(bcf, lw, lh)));
                encode_bc_level(bcf, is_srgb, levels.data() + offsets[i], lw, lh, blocks[i].data(), &alpha_dropped);
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
            }
            if (alpha_dropped)
                diags.warning(in.display_path, 0, 0, kStage,
                              "'compress' bc1 is opaque: the texture has alpha below 255, which is dropped. Use \"bc7\" to keep it");
        }
        if (astc) {
            astcenc_config cfg;
            astcenc_context* ctx = nullptr;
            if (astcenc_config_init(is_srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_MEDIUM, 0, &cfg) !=
                    ASTCENC_SUCCESS ||
                astcenc_context_alloc(&cfg, 1, &ctx, nullptr) != ASTCENC_SUCCESS) {
                diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "ASTC encoder setup failed");
                return false;
            }
            blocks.resize(offsets.size());
            u32 lw = cw, lh = ch;
            bool ok = true;
            for (usize i = 0; i < offsets.size() && ok; ++i) {
                ok = encode_astc(ctx, levels.data() + offsets[i], lw, lh, blocks[i]);
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
            }
            astcenc_context_free(ctx);
            if (!ok) {
                diags.error(Errc::ImportFailed, in.display_path, 0, 0, kStage, "ASTC encoding failed");
                return false;
            }
        }
        for (usize i = 0; i < offsets.size(); ++i) {
            if (astc || bc) {
                const Span<const byte> bytes(reinterpret_cast<const byte*>(blocks[i].data()), blocks[i].size());
                mip_data.push_back(asset::TextureMipData{cw, ch, bytes});
                cw = cw > 1 ? cw / 2 : 1;
                ch = ch > 1 ? ch / 2 : 1;
                continue;
            }
            const usize bytes = static_cast<usize>(cw) * ch * 4;
            mip_data.push_back(asset::TextureMipData{cw, ch, Span<const byte>(reinterpret_cast<const byte*>(levels.data() + offsets[i]), bytes)});
            cw = cw > 1 ? cw / 2 : 1;
            ch = ch > 1 ? ch / 2 : 1;
        }

        Array<byte> payload;
        const asset::TextureFormat format = astc ? asset::TextureFormat::ASTC_4x4
                                            : bc ? bc_texture_format(bcf) : asset::TextureFormat::RGBA8;
        const u32 flags = (is_srgb ? asset::TextureFlags::Srgb : 0u) | (linear ? asset::TextureFlags::LinearFilter : 0u);
        asset::write_texture(format, 1, flags, mip_data.span(), payload);
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Texture;
        d.version = asset::chunk_version(asset::TEXTURE_VERSION, 0);
        d.user_flags = static_cast<u32>(format);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_texture_importer() { return mem::make_unique<TextureImporter>(mem::heap()); }

}  // namespace mce::cook
