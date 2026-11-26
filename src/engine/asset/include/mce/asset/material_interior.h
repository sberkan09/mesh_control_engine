#pragma once
// M-VISUAL4 5: window interior mapping. A material keeps the number of window cells across one texture repeat in
// MaterialChunk::flags bits 6..7 (0 off, 1, 2, 3 for 1, 2, 4 cells in both directions), the two bits formats.h left free.
// The forward shader then shows a room behind every glossy (glass) texel of that grid (docs/milestones/M-VISUAL4.md,
// step 5). A narrow header on purpose, like material_retro.h: formats.h (included by about half the tree) is not touched,
// so its MaterialFlags enum names no constant for these bits; they live here. A material cooked before reads 0 (off).

#include "mce/asset/formats.h"
#include "mce/types.h"

namespace mce::asset {

namespace MaterialInterior {
enum : u32 {
    Shift = 6,
    Mask = 0x3u << Shift,
};
}

// Window cells across one texture repeat: 0 (off), 1, 2 or 4.
inline u32 material_interior_cells(const MaterialChunk& m) {
    const u32 code = (m.flags & MaterialInterior::Mask) >> MaterialInterior::Shift;
    return code == 0 ? 0u : 1u << (code - 1);
}

// Whether `cells` is a value the bits can hold (0, 1, 2 or 4).
inline bool valid_interior_cells(u32 cells) { return cells == 0 || cells == 1 || cells == 2 || cells == 4; }

// Writes the cell count (an invalid one writes 0, off) and leaves every other flags bit.
inline void set_material_interior_cells(MaterialChunk& m, u32 cells) {
    const u32 code = cells == 1 ? 1u : cells == 2 ? 2u : cells == 4 ? 3u : 0u;
    m.flags = (m.flags & ~MaterialInterior::Mask) | (code << MaterialInterior::Shift);
}

}  // namespace mce::asset
