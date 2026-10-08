#pragma once
// The 03_platformer level, shared by the game (spawning) and the host (the tilemap it draws).

#include "mce/containers/array.h"
#include "mce/math/math.h"

namespace platformer {

using mce::i32;
using mce::u32;
using mce::usize;

// ---- the built-in level ---------------------------------------------------------------------------
// Legend: # ground   = one-way plank   P player start   E enemy   o coin   ^ spikes
//         M platform moving sideways   V platform moving up/down   F goal flag
//         C crate (dynamic)   O platform orbiting a 1.5 m circle with a crate riding on it
// One character = one 1 m tile. Rows run top to bottom; short rows are padded with air.
inline constexpr const char* kLevel[] = {
    "#                                                                                                  #",
    "#                                                                                                  #",
    "#                                                                                                  #",
    "#                                                                                                  #",
    "#                                                                                                  #",
    "#                                                                                                  #",
    "#                                                                 o                                #",
    "#                                                               =====                              #",
    "#                                                    o  oE o                                       #",
    "#         O                                       #############                                    #",
    "#                                                                                       o          #",
    "#             o o                   o                                                 =====        #",
    "#            =====                                                                                 #",
    "#                       o         ##   ##     V                                   ##           F   #",
    "#C     o o o                    ####   ##                                       ####       #########",
    "#C P              E           ###### ^^## E                 E     ^^  o o o   ######       #########",
    "#####################   M   #################    ###################################################",
    "#####################       #################    ###################################################",
};

inline constexpr u32 kLevelRows = u32(sizeof(kLevel) / sizeof(kLevel[0]));

struct Level {
    const char* const* rows = kLevel;
    u32 height = kLevelRows;
    u32 width = 0;

    Level() {
        for (u32 r = 0; r < height; ++r) {
            u32 n = 0;
            while (rows[r][n] != '\0') ++n;
            if (n > width) width = n;
        }
    }
    // (x, y) in tiles, y = 0 is the bottom row.
    char at(i32 x, i32 y) const {
        if (x < 0 || y < 0 || x >= static_cast<i32>(width) || y >= static_cast<i32>(height)) return ' ';
        const char* row = rows[height - 1 - static_cast<u32>(y)];
        for (i32 i = 0; i < x; ++i)
            if (row[i] == '\0') return ' ';
        return row[x] == '\0' ? ' ' : row[x];
    }
    static mce::math::vec2 center(i32 x, i32 y) { return {static_cast<mce::f32>(x) + 0.5f, static_cast<mce::f32>(y) + 0.5f}; }
};

// The level as tiles for the host's tilemap: width * height, row 0 = bottom.
// 0 empty, 1 ground top, 2 ground fill, 3 one-way plank.
struct TileGrid {
    u32 width = 0, height = 0;
    mce::Array<u32> tiles;
};

inline void level_tiles(TileGrid& out) {
    const Level lv;
    out.width = lv.width;
    out.height = lv.height;
    out.tiles.clear();
    out.tiles.resize(usize(lv.width) * lv.height);
    for (i32 y = 0; y < static_cast<i32>(lv.height); ++y)
        for (i32 x = 0; x < static_cast<i32>(lv.width); ++x) {
            const char c = lv.at(x, y);
            u32 t = 0;
            if (c == '#') t = lv.at(x, y + 1) == '#' ? 2u : 1u;
            else if (c == '=') t = 3;
            out.tiles[usize(y) * lv.width + usize(x)] = t;
        }
}

}  // namespace platformer
