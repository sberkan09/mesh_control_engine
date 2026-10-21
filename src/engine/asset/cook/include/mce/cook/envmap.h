#pragma once
// .mcenv — a baked environment (M-VISUAL 2: a reflection probe the editor captured), cooked by the
// "envmap" importer into a CHNK_TEX RGBA8 2D array, one layer per pre-filter level, linear (no
// sRGB flag, no mips). The renderer decodes the layers back into its IBL array.
//
//   bytes 0..7   "MCENV001"
//   u32 width, height, layers, encoding   (little endian; encoding 1 = RGBM, rgb * a * kEnvRgbmRange)
//   width * height * 4 bytes per layer, layers contiguous, rows in texture order (row 0 = v 0)

#include "mce/core.h"

namespace mce::cook {

inline constexpr char kEnvMagic[8] = {'M', 'C', 'E', 'N', 'V', '0', '0', '1'};
inline constexpr u32 kEnvEncodingRgbm = 1;
inline constexpr f32 kEnvRgbmRange = 16.0f;  // the brightest radiance an RGBM texel holds
inline constexpr usize kEnvHeaderBytes = 24;

// Appends a .mcenv file's bytes to `out`. `rgba` is width * height * 4 * layers bytes.
void write_envmap(u32 width, u32 height, u32 layers, Span<const u8> rgba, Array<byte>& out);

}  // namespace mce::cook
