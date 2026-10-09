#pragma once
// Ember Step's levels, shared by the game (spawning) and the host (the tilemap it draws).

#include "mce/containers/array.h"
#include "mce/math/math.h"

namespace platformer {

using mce::i32;
using mce::u32;
using mce::usize;

// ---- the levels ----------------------------------------------------------------------------------
// Legend: # ground   = one-way plank   P player start   E enemy   o coin   ^ spikes
//         M platform moving sideways   V platform moving up/down   F goal flag
//         C crate (dynamic)   O platform orbiting a 1.5 m circle with a crate riding on it
//         X crumbling block (gives way shortly after the player lands on it, then grows back)
// One character = one 1 m tile. Rows run top to bottom; short rows are padded with air.
//
// Each level after the first leans on one mechanic: 2 crumbling blocks, 3 wall jumps, 4 dashes
// over spike pits, 5 all of them. The layouts were checked for reachability (start -> every coin
// -> flag) against the movement limits in platformer.cpp: a jump clears 4 tiles across or 2 up,
// a dash 7 across, a wall jump climbs a shaft at most 3 tiles wide.
inline constexpr const char* kLevel1[] = {  // First Steps (the attract-mode bot's level)
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

inline constexpr const char* kLevel2[] = {  // Crumble Bridge
    "#                                                                                          #",
    "#                                                                                          #",
    "#                                                                                          #",
    "#                                                                                          #",
    "#                                                                                          #",
    "#                                        o   o                                             #",
    "#                                      XXX   XXX     XXX      o o o              F         #",
    "#                 o o o o            XXX                     XXXXXXX        ##########     #",
    "#                                  XXX                                ===   ##########     #",
    "#    P       XXXXXXXXXXXXXX      XXX            E            o  E    ###    ##########     #",
    "##########                  ####   #########################^^^#########^^^#################",
    "##########^^^^^^^^^^^^^^^^^^####^^^#########################################################",
};

inline constexpr const char* kLevel3[] = {  // The Chimney
    "#############################################",
    "#                                    ##     #",
    "#                                    ##  F  #",
    "#                                    ## ### #",
    "#                     o   o          ##     #",
    "#      #   #        =========        ==     #",
    "#      #   #   o                   ####     #",
    "#      # o #  ===          #   #            #",
    "#      #   #               # o #            #",
    "#      #   #               #   #   =====    #",
    "#      # o #               # o #            #",
    "#      #   #       E       #   #       ^^^^^#",
    "#      #   #    ######     #   #     ###### #",
    "#      # o #               # o #            #",
    "#      #   #               #   #            #",
    "#      #   #       o       #   #   o o      #",
    "#      #   #     =====     # o #  =====     #",
    "#  P   #   #               #   #            #",
    "#          #       ^^      #   #            #",
    "######## ######################  ############",
    "#############################################",
};

inline constexpr const char* kLevel4[] = {  // Ember Dash
    "#                                                                                                  #",
    "#                                                                                                  #",
    "#                                                                                                  #",
    "#                                              o                              o                    #",
    "#                                   o         o                     o                              #",
    "#  P            o        o                  #####                  o        ####          o  F     #",
    "#######        o        ####      ####                   E        ####      ####      ##############",
    "#######      #####      ####      ####                ######      ####      ####      ##############",
    "#######^^^^^^#####^^^^^^####^^^^^^####^^^^^^^^^^^^^^^^######^^^^^^####^^^^^^####^^^^^^##############",
    "####################################################################################################",
};

inline constexpr const char* kLevel5[] = {  // The Summit
    "#                                                                                                   #",
    "#                                                                                                   #",
    "#                                             o                                                     #",
    "#                                        #########       o                                          #",
    "#                                        ##             XXX    o                                    #",
    "#                                      o ##                   XXX     E         o                   #",
    "#                                   ##   ##                         #####      ####            F    #",
    "#                                   ##   ##                                              ############",
    "#                                   ##   ##                                              ############",
    "#                                   ## o ##                                              ############",
    "#                                   ##   ##                                              ############",
    "#                                   ##   ##                                              ############",
    "#                               M   ##   ##                                              ############",
    "#                 o                 ## o ##                                              ############",
    "#           o    XXX       E             ##                                              ############",
    "#  P       XXX         ########          ##                                              ############",
    "#######                ########     #####                                                ############",
    "#######                ########     #####                                                ############",
    "#######^^^^^^^^^^^^^^^^########^^^^^#####^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^############",
    "#####################################################################################################",
};

struct LevelDef {
    const char* name;
    const char* const* rows;
    u32 height;
};
template <usize N>
constexpr LevelDef level_def(const char* name, const char* const (&rows)[N]) {
    return {name, rows, static_cast<u32>(N)};
}
inline constexpr LevelDef kLevels[] = {
    level_def("FIRST STEPS", kLevel1),   level_def("CRUMBLE BRIDGE", kLevel2), level_def("THE CHIMNEY", kLevel3),
    level_def("EMBER DASH", kLevel4),    level_def("THE SUMMIT", kLevel5),
};
inline constexpr u32 kLevelCount = u32(sizeof(kLevels) / sizeof(kLevels[0]));

struct Level {
    const char* const* rows = kLevel1;
    u32 height = kLevels[0].height;
    u32 width = 0;

    explicit Level(u32 index = 0) {
        const LevelDef& d = kLevels[index < kLevelCount ? index : 0];
        rows = d.rows;
        height = d.height;
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

inline void level_tiles(u32 level, TileGrid& out) {
    const Level lv(level);
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
