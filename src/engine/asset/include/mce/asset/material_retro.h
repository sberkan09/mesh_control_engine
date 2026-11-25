#pragma once
// M-HEADLIGHT 1: the retro-reflection factor a material keeps quantized in MaterialChunk::flags (bits 24..31, 0..255 for
// 0..1; MaterialFlags::RetroMask). Reflective road paint throws light back toward its source, so under a headlight it is
// brighter than its diffuse colour. This step only stores the factor; the shader term is M-HEADLIGHT 2. A narrow header on
// purpose, like material_translucency.h: formats.h stays unchanged apart from the two enum entries. A material cooked before
// reads 0 (off): its bits 24..31 are zero.

#include "mce/asset/formats.h"
#include "mce/asset/material_translucency.h"
#include "mce/types.h"

namespace mce::asset {

inline f32 material_retro(const MaterialChunk& m) {
    return dequantize_unit((m.flags & MaterialFlags::RetroMask) >> MaterialFlags::RetroShift);
}

// Writes the factor and leaves every other flags bit.
inline void set_material_retro(MaterialChunk& m, f32 retro) {
    m.flags = (m.flags & ~MaterialFlags::RetroMask) | (quantize_unit(retro) << MaterialFlags::RetroShift);
}

}  // namespace mce::asset
