#pragma once
// M-TRANSLUCENT 41a: the translucency and wrap factors a material keeps quantized in MaterialChunk::flags
// (bits 8..15 and 16..23, 0..255 for 0..1; MaterialFlags::TranslucencyMask / WrapMask). A narrow header on
// purpose: only the importers, the renderer's .cpp files, the editor form and the tests include it, so the
// wide headers stay as they are. The inline bodies read MaterialChunk::flags, so it needs formats.h (a
// forward declaration cannot serve them); every includer already includes formats.h. Inline bodies, no state.
//
// A material cooked before this reads 0, 0 (off): its bits 8..31 are zero.

#include <cmath>

#include "mce/asset/formats.h"
#include "mce/types.h"

namespace mce::asset {

// v clamped to [0, 1] (NaN to 0), then 0..255.
inline u32 quantize_unit(f32 v) {
    if (!(v > 0.0f)) return 0u;  // also NaN
    if (v > 1.0f) v = 1.0f;
    return static_cast<u32>(std::lround(v * 255.0f));
}

inline f32 dequantize_unit(u32 q) { return static_cast<f32>(q) / 255.0f; }

// 0 = off: the translucency and wrap bits of a flags word.
inline u32 translucency_bits(u32 flags) { return flags & (MaterialFlags::TranslucencyMask | MaterialFlags::WrapMask); }

inline f32 material_translucency(const MaterialChunk& m) {
    return dequantize_unit((m.flags & MaterialFlags::TranslucencyMask) >> MaterialFlags::TranslucencyShift);
}

inline f32 material_wrap(const MaterialChunk& m) {
    return dequantize_unit((m.flags & MaterialFlags::WrapMask) >> MaterialFlags::WrapShift);
}

// Writes both fields and leaves every other flags bit.
inline void set_material_translucency(MaterialChunk& m, f32 translucency, f32 wrap) {
    m.flags = (m.flags & ~(MaterialFlags::TranslucencyMask | MaterialFlags::WrapMask)) |
              (quantize_unit(translucency) << MaterialFlags::TranslucencyShift) | (quantize_unit(wrap) << MaterialFlags::WrapShift);
}

}  // namespace mce::asset
