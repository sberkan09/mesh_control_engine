#pragma once
// PNG in and out for the tools around the cook (golden-image tests, FR-DBG-020; editor thumbnails):
// decodes any PNG to RGBA8 and encodes RGBA8 as PNG. Rows top first, as read_pixels returns them.

#include "mce/core.h"

namespace mce::cook {

struct Image {
    u32 width = 0;
    u32 height = 0;
    Array<u8> rgba;  // width * height * 4
};

Result<Image> decode_png(Span<const byte> bytes);
// Appends the PNG of `rgba` (width * height * 4 bytes) to `out`.
Result<void> encode_png(u32 width, u32 height, Span<const u8> rgba, Array<byte>& out);

}  // namespace mce::cook
