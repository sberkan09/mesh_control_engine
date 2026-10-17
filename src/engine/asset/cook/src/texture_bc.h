#pragma once
// Block-compressed texture encoding for the cook (M-CONTENT 1b-ii): BC7, BC5, BC4 and BC1 from RGBA8 levels, through
// bc7enc_rdo (the only translation unit that includes it is texture_bc.cpp). Cook-internal.
//
// The encoder settings in texture_bc.cpp are part of the output: changing one bumps the texture importer's version().
// The encoders are scalar float code built with -ffp-contract=off, so the bytes are the same on every x86_64 compiler we
// run; the arm64 equality is checked by tests/asset/test_cook_bc_arch.cpp (cook goldens are guaranteed on x86_64 only).

#include "mce/asset/formats.h"

namespace mce::cook {

enum class BcFormat : u8 { BC1, BC4, BC5, BC7 };

// The asset-side format of a BcFormat.
asset::TextureFormat bc_texture_format(BcFormat f);

// Bytes of one level: ceil(w/4) * ceil(h/4) blocks, 8 bytes for BC1/BC4 and 16 for BC5/BC7 (texture_level_bytes).
u64 bc_level_bytes(BcFormat f, u32 width, u32 height);

// Encodes one level of `rgba` (width * height * 4 bytes, tightly packed) into bc_level_bytes(f, width, height) bytes at
// `out`. A level whose sides are not multiples of 4 (the small mips) is padded to whole blocks by repeating the edge
// texels. `srgb` selects perceptual weights for BC7. BC1 ignores alpha; `*alpha_dropped` is set when it was below 255.
// Thread-safe: the one-time encoder table setup runs under std::call_once.
void encode_bc_level(BcFormat f, bool srgb, const u8* rgba, u32 width, u32 height, u8* out, bool* alpha_dropped = nullptr);

// Decodes one level back to RGBA8 (width * height * 4 bytes at `rgba`), for tests and image-quality gates. BC4 gives
// (r, r, r, 255)... as the GPU samples it: (r, 0, 0, 255); BC5 gives (r, g, 0, 255); BC1 and BC7 give all four.
void decode_bc_level(BcFormat f, const u8* blocks, u32 width, u32 height, u8* rgba);

}  // namespace mce::cook
