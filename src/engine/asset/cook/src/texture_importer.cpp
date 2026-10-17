// PNG/JPG -> CHNK_TEX, RGBA8 with a box-filtered mip chain (assets.md §5.2; BCn is v0.5). Phone
// cooks (ImportInput::texture_compression, mce_cook --textures astc) store every level as ASTC 4x4
// blocks instead, encoded from the same RGBA8 chain (astc-encoder, medium quality, one thread).
//
// sidecar settings: { "srgb": true, "mips": true, "filter": "nearest" }
//   srgb  color data is sRGB-encoded: mips are averaged in linear space and the chunk carries the
//         Srgb flag so the renderer samples through an sRGB view (never double-corrects)
//   mips  generate the full chain down to 1x1
//   filter  "nearest" (pixel art, the default) or "linear": how kit sprites sample it (LinearFilter flag)
//
// HDR sources (M-VISUAL3 4): Radiance .hdr (stb) and OpenEXR .exr (tinyexr: uncompressed, ZIP, PIZ, the
// first RGB(A) layer) cook to TextureFormat::RGBA16F, linear, never Srgb. Settings: { "mips": true,
// "max_size": 2048, "exposure": 0, "filter": ... }
//   max_size  largest side, a power of two: the image is box-halved until it fits (a float sky is 8 bytes a texel)
//   exposure  EV applied at cook (x 2^EV)
// Values are clamped to [0, 65504] (the half range), NaN and Inf become 0, mips are box-filtered in float.
// "srgb": true is an error. Phone cooks (--textures astc) keep RGBA16F for these.

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <astcenc.h>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "stb_image.h"
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

// Decodes a Radiance .hdr or an OpenEXR file to linear float RGBA (alpha 1 when the file has none).
// False with a diagnostic on a decode error or an out-of-range size.
bool decode_hdr(const ImportInput& in, bool exr, Array<f32>& rgba, u32& w, u32& h, DiagList& diags) {
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
    int iw = 0, ih = 0;
    f32* px = nullptr;
    if (exr) {
        const char* err = nullptr;
        {  // the size first: a hostile header must not allocate gigabytes of floats
            EXRVersion ver;
            EXRHeader hdr;
            InitEXRHeader(&hdr);
            if (ParseEXRVersionFromMemory(&ver, bytes, size) != TINYEXR_SUCCESS) return fail("EXR decode failed", "not an OpenEXR file");
            if (ParseEXRHeaderFromMemory(&hdr, &ver, bytes, size, &err) != TINYEXR_SUCCESS) {
                const bool ok = fail("EXR decode failed", err);
                if (err) FreeEXRErrorMessage(err);
                return ok;
            }
            const i64 dw = i64(hdr.data_window.max_x) - hdr.data_window.min_x + 1, dh = i64(hdr.data_window.max_y) - hdr.data_window.min_y + 1;
            FreeEXRHeader(&hdr);
            if (dw <= 0 || dh <= 0 || dw > kMaxHdrSide || dh > kMaxHdrSide)
                return fail("image dimensions out of range (1..16384)", nullptr);
        }
        if (LoadEXRFromMemory(&px, &iw, &ih, bytes, size, &err) != TINYEXR_SUCCESS) {
            const bool ok = fail("EXR decode failed", err);
            if (err) FreeEXRErrorMessage(err);
            return ok;
        }
    } else {
        int comp = 0;
        if (!stbi_info_from_memory(bytes, int(size), &iw, &ih, &comp)) return fail("image decode failed", stbi_failure_reason());
        if (iw <= 0 || ih <= 0 || iw > kMaxHdrSide || ih > kMaxHdrSide) return fail("image dimensions out of range (1..16384)", nullptr);
        px = stbi_loadf_from_memory(bytes, int(size), &iw, &ih, &comp, 4);
        if (!px) return fail("image decode failed", stbi_failure_reason());
    }
    if (iw <= 0 || ih <= 0 || iw > kMaxHdrSide || ih > kMaxHdrSide) {
        std::free(px);  // stb and tinyexr both allocate with malloc
        return fail("image dimensions out of range (1..16384)", nullptr);
    }
    w = u32(iw);
    h = u32(ih);
    rgba.resize_uninitialized(usize(w) * h * 4);
    std::memcpy(rgba.data(), px, rgba.size() * sizeof(f32));
    std::free(px);
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

    Array<f32> cur;
    u32 w = 0, h = 0;
    if (!decode_hdr(in, exr, cur, w, h, diags)) return false;

    // Exposure, then the half range: negatives and NaN/Inf become 0, the rest clamps to 65504.
    const f32 gain = static_cast<f32>(std::exp2(exposure_ev));
    for (usize i = 0; i < cur.size(); ++i) {
        f32 v = cur[i];
        if (i % 4 != 3) v *= gain;
        cur[i] = std::isfinite(v) ? (v < 0.0f ? 0.0f : (v > 65504.0f ? 65504.0f : v)) : 0.0f;
    }

    // Power-of-two downscale until the largest side fits, then the mip chain, both box-filtered in float.
    Array<f32> next;
    while (w > max_size || h > max_size) {
        const u32 nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        next.resize_uninitialized(usize(nw) * nh * 4);
        downsample_f(cur.data(), w, h, next.data(), nw, nh);
        cur = static_cast<Array<f32>&&>(next);
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
        next.resize_uninitialized(usize(nw) * nh * 4);
        downsample_f(cur.data(), lw, lh, next.data(), nw, nh);
        cur = static_cast<Array<f32>&&>(next);
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

class TextureImporter final : public IImporter {
public:
    StringView id() const override { return "texture"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Texture; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override {
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr" || ext == ".exr";
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const String ext = lower_extension(in.display_path);
        if (ext.view() == ".hdr" || ext.view() == ".exr") return cook_hdr(in, ext.view() == ".exr", out, diags);
        const JsonValue s = in.settings;
        if (!detail::check_keys(s, {"srgb", "mips", "filter"}, in.display_path, kStage, diags)) return false;
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

        // Mip chain storage: level i lives at offsets[i] in `levels`.
        Array<u8> levels;
        Array<asset::TextureMipData> mip_data;
        Array<usize> offsets;
        u32 cw = static_cast<u32>(w), ch = static_cast<u32>(h);
        usize total = 0;
        for (u32 lw = cw, lh = ch;;) {
            offsets.push_back(total);
            total += static_cast<usize>(lw) * lh * 4;
            if (!mips || (lw == 1 && lh == 1)) break;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        levels.resize_uninitialized(total);
        std::memcpy(levels.data(), px, static_cast<usize>(cw) * ch * 4);
        stbi_image_free(px);
        for (usize i = 1; i < offsets.size(); ++i) {
            const u32 nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
            downsample(levels.data() + offsets[i - 1], cw, ch, levels.data() + offsets[i], nw, nh, is_srgb);
            cw = nw;
            ch = nh;
        }
        cw = static_cast<u32>(w);
        ch = static_cast<u32>(h);
        const bool astc = in.texture_compression == TextureCompression::Astc4x4;
        Array<Array<u8>> blocks;  // per level, when encoding ASTC
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
            if (astc) {
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
        const asset::TextureFormat format = astc ? asset::TextureFormat::ASTC_4x4 : asset::TextureFormat::RGBA8;
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
