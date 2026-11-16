// BC1 / BC4 / BC5 / BC7 encoders for the texture importer (M-CONTENT 1b-ii). bc7enc_rdo, pinned by commit in
// cmake/Dependencies.cmake: BC7 through bc7enc.cpp, BC1/BC4/BC5 through rgbcx. Single-threaded scalar code; the
// settings below are part of the cooked bytes (a change bumps TextureImporter::version()).

#include "texture_bc.h"

#include <cstring>
#include <mutex>

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

namespace mce::cook {

namespace {

constexpr u32 kBc1Level = 10;  // rgbcx::encode_bc1 quality level (0..18), 4-color blocks only

std::once_flag g_init;

void init_tables() {
    std::call_once(g_init, [] {
        bc7enc_compress_block_init();
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
    });
}

// Fills a 4x4 RGBA block at (bx, by) from the level, repeating the edge texels past the border (clamp).
void gather_block(const u8* rgba, u32 w, u32 h, u32 bx, u32 by, u8 block[64]) {
    for (u32 y = 0; y < 4; ++y) {
        const u32 sy = by * 4 + y < h ? by * 4 + y : h - 1;
        for (u32 x = 0; x < 4; ++x) {
            const u32 sx = bx * 4 + x < w ? bx * 4 + x : w - 1;
            std::memcpy(block + (y * 4 + x) * 4, rgba + (usize(sy) * w + sx) * 4, 4);
        }
    }
}

u32 block_bytes(BcFormat f) { return f == BcFormat::BC1 || f == BcFormat::BC4 ? 8u : 16u; }

}  // namespace

asset::TextureFormat bc_texture_format(BcFormat f) {
    switch (f) {
        case BcFormat::BC1: return asset::TextureFormat::BC1;
        case BcFormat::BC4: return asset::TextureFormat::BC4;
        case BcFormat::BC5: return asset::TextureFormat::BC5;
        case BcFormat::BC7: return asset::TextureFormat::BC7;
    }
    return asset::TextureFormat::BC7;
}

u64 bc_level_bytes(BcFormat f, u32 width, u32 height) { return u64((width + 3) / 4) * ((height + 3) / 4) * block_bytes(f); }

void encode_bc_level(BcFormat f, bool srgb, const u8* rgba, u32 width, u32 height, u8* out, bool* alpha_dropped) {
    init_tables();
    bc7enc_compress_block_params p;
    if (f == BcFormat::BC7) {
        bc7enc_compress_block_params_init(&p);
        p.m_uber_level = 0;
        p.m_max_partitions = 16;
        if (srgb) bc7enc_compress_block_params_init_perceptual_weights(&p);
        else bc7enc_compress_block_params_init_linear_weights(&p);
    }
    const u32 bw = (width + 3) / 4, bh = (height + 3) / 4, bytes = block_bytes(f);
    u8 block[64];
    for (u32 by = 0; by < bh; ++by)
        for (u32 bx = 0; bx < bw; ++bx) {
            gather_block(rgba, width, height, bx, by, block);
            u8* dst = out + (usize(by) * bw + bx) * bytes;
            switch (f) {
                case BcFormat::BC7: bc7enc_compress_block(dst, block, &p); break;
                case BcFormat::BC5: rgbcx::encode_bc5(dst, block, 0, 1, 4); break;
                case BcFormat::BC4: rgbcx::encode_bc4(dst, block, 4); break;
                case BcFormat::BC1:
                    if (alpha_dropped)
                        for (u32 i = 0; i < 16; ++i)
                            if (block[i * 4 + 3] < 255) *alpha_dropped = true;
                    rgbcx::encode_bc1(kBc1Level, dst, block, false, false);
                    break;
            }
        }
}

void decode_bc_level(BcFormat f, const u8* blocks, u32 width, u32 height, u8* rgba) {
    init_tables();
    const u32 bw = (width + 3) / 4, bh = (height + 3) / 4, bytes = block_bytes(f);
    u8 px[64];
    for (u32 by = 0; by < bh; ++by)
        for (u32 bx = 0; bx < bw; ++bx) {
            const u8* src = blocks + (usize(by) * bw + bx) * bytes;
            switch (f) {
                case BcFormat::BC7: {
                    bc7decomp::color_rgba c[16];
                    bc7decomp::unpack_bc7(src, c);
                    for (u32 i = 0; i < 16; ++i) {
                        px[i * 4 + 0] = c[i].r;
                        px[i * 4 + 1] = c[i].g;
                        px[i * 4 + 2] = c[i].b;
                        px[i * 4 + 3] = c[i].a;
                    }
                    break;
                }
                case BcFormat::BC5:
                    std::memset(px, 0, sizeof px);
                    rgbcx::unpack_bc5(src, px, 0, 1, 4);
                    for (u32 i = 0; i < 16; ++i) px[i * 4 + 3] = 255;
                    break;
                case BcFormat::BC4:
                    std::memset(px, 0, sizeof px);
                    rgbcx::unpack_bc4(src, px, 4);
                    for (u32 i = 0; i < 16; ++i) px[i * 4 + 3] = 255;
                    break;
                case BcFormat::BC1: rgbcx::unpack_bc1(src, px, true); break;
            }
            for (u32 y = 0; y < 4 && by * 4 + y < height; ++y)
                for (u32 x = 0; x < 4 && bx * 4 + x < width; ++x)
                    std::memcpy(rgba + (usize(by * 4 + y) * width + bx * 4 + x) * 4, px + (y * 4 + x) * 4, 4);
        }
}

}  // namespace mce::cook
