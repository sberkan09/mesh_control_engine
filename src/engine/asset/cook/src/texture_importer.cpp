// PNG/JPG -> CHNK_TEX, RGBA8 with a box-filtered mip chain (assets.md §5.2; BCn is v0.5). Phone
// cooks (ImportInput::texture_compression, mce_cook --textures astc) store every level as ASTC 4x4
// blocks instead, encoded from the same RGBA8 chain (astc-encoder, medium quality, one thread).
//
// sidecar settings: { "srgb": true, "mips": true }
//   srgb  color data is sRGB-encoded: mips are averaged in linear space and the chunk carries the
//         Srgb flag so the renderer samples through an sRGB view (never double-corrects)
//   mips  generate the full chain down to 1x1

#include <cmath>
#include <cstring>

#include <astcenc.h>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "stb_image.h"

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

class TextureImporter final : public IImporter {
public:
    StringView id() const override { return "texture"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Texture; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".png" || ext == ".jpg" || ext == ".jpeg"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue s = in.settings;
        if (!detail::check_keys(s, {"srgb", "mips"}, in.display_path, kStage, diags)) return false;
        bool is_srgb = true, mips = true;
        if (!detail::read_bool(s, "srgb", true, is_srgb, in.display_path, kStage, diags)) return false;
        if (!detail::read_bool(s, "mips", true, mips, in.display_path, kStage, diags)) return false;

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
        asset::write_texture(format, 1, is_srgb ? asset::TextureFlags::Srgb : 0u, mip_data.span(), payload);
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
