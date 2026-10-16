#pragma once
// Procedural level generators (M-PROCGEN 1): each is a pure function of (kind, parameters, seed),
// so the seed is the level. No clock, no global random, no float and no unordered container, so
// the same inputs give the same grid on every machine and in Debug and Release (FR-RUN-040).
//
//   auto g = kit::procgen::generate_rooms(64, 48, /*seed=*/7, {});   // Result<ProcGrid>
//   g.value().at(x, y)        // kFloor or kWall; (0, 0) is the top-left cell, rows grow downwards
//   g.value().start, .exit    // two floor cells, `exit` the farthest from `start` by steps
//
// Random numbers come from named child streams of the seed (Rng::child), so adding a step to one
// generator never shifts the numbers another one draws.
//
// Every generator promises: a one-cell wall border, exactly one 4-connected floor region (smaller
// pockets are filled with wall), at least `min_floor_percent` of the cells open, and a start and
// an exit in that region. A draw that misses the floor target is retried with the next attempt's
// stream, at most kMaxAttempts times; then the call fails with Errc::InvalidArgument and a stage
// that says which generator gave up, never a hang.

#include "mce/containers/array.h"
#include "mce/hash.h"
#include "mce/result.h"
#include "mce/types.h"

namespace mce::kit::procgen {

inline constexpr u8 kWall = 0;
inline constexpr u8 kFloor = 1;
inline constexpr u32 kMinSize = 8;
inline constexpr u32 kMaxSize = 512;
inline constexpr u32 kMaxAttempts = 16;

// A splitmix64 stream: tiny state, passes the usual tests, identical everywhere.
class Rng {
public:
    explicit constexpr Rng(u64 seed = 0) : state_(seed) {}
    // The stream a named step of a generator draws from: same (seed, name) = same numbers.
    static constexpr Rng child(u64 seed, StringView name, u64 index = 0) {
        return Rng(hash_combine(hash_combine(seed, fnv1a64(name)), index));
    }
    constexpr u64 next_u64() {
        state_ += 0x9e3779b97f4a7c15ull;
        return mix64(state_);
    }
    constexpr u32 next_u32() { return static_cast<u32>(next_u64() >> 32); }
    // 0 .. n-1 (n == 0 gives 0). The modulo bias is below 2^-32 * n: irrelevant for level sizes.
    constexpr u32 below(u32 n) { return n == 0 ? 0 : static_cast<u32>((static_cast<u64>(next_u32()) * n) >> 32); }
    // lo .. hi inclusive (hi < lo gives lo).
    constexpr i32 range(i32 lo, i32 hi) { return hi < lo ? lo : lo + static_cast<i32>(below(static_cast<u32>(hi - lo) + 1)); }
    // True with probability percent / 100.
    constexpr bool chance(u32 percent) { return below(100) < percent; }

private:
    u64 state_;
};

struct Cell {
    i32 x = 0;
    i32 y = 0;
    friend bool operator==(Cell, Cell) = default;
};

struct ProcGrid {
    u32 width = 0;
    u32 height = 0;
    Array<u8> cells;  // row-major, row 0 on top; kWall or kFloor
    Cell start;
    Cell exit;

    u8 at(i32 x, i32 y) const {
        return x < 0 || y < 0 || x >= static_cast<i32>(width) || y >= static_cast<i32>(height) ? kWall : cells[static_cast<usize>(y) * width + x];
    }
    u32 floor_count() const;
    // Order-sensitive hash of the size, the cells, the start and the exit.
    u64 hash() const;
    // One char per cell, '#' wall, '.' floor, 'S' start, 'E' exit, rows separated by '\n'.
    void to_ascii(Array<char>& out) const;
};

// Rooms joined by corridors, laid out by binary space partition. Leaves of at least `leaf_min`
// cells hold one room that fills 50-90 % of its width and height (at least `room_min`); sibling
// subtrees are joined by an L-shaped corridor `corridor` cells wide (1 or 2).
struct RoomsParams {
    u32 leaf_min = 8;
    u32 room_min = 3;
    u32 corridor = 1;
    u32 min_floor_percent = 15;
};
Result<ProcGrid> generate_rooms(u32 width, u32 height, u64 seed, const RoomsParams& p = {});

// Cellular-automata caves: `fill_percent` of the interior starts as wall, then `iterations` passes
// of "wall with more than four wall neighbours of eight, floor with fewer than four, else unchanged"
// (the border counts as wall). Below about 16 x 16 the smoothing often leaves too little floor and the
// call fails with the named error; use rooms or a walk for tiny grids.
struct CavesParams {
    u32 fill_percent = 50;
    u32 iterations = 5;
    u32 min_floor_percent = 25;
};
Result<ProcGrid> generate_caves(u32 width, u32 height, u64 seed, const CavesParams& p = {});

// A drunkard's walk from the middle: random orthogonal steps (staying off the border) carve floor
// until `coverage_percent` of the cells are open or `max_steps` steps are used; `turn_percent` is
// the chance of picking a new direction on a step (otherwise it keeps going).
struct WalkParams {
    u32 coverage_percent = 35;
    u32 turn_percent = 40;
    u32 max_steps = 0;  // 0: ten times the cell count
};
Result<ProcGrid> generate_walk(u32 width, u32 height, u64 seed, const WalkParams& p = {});

// Value noise thresholded: `octaves` layers of integer value noise on a lattice of `scale` cells
// (halving per octave, half the weight each); cells below `floor_percent` of the noise range are
// floor. Smooth, blobby caverns; fixed-point only.
struct NoiseParams {
    u32 scale = 8;
    u32 octaves = 2;
    u32 floor_percent = 55;
    u32 min_floor_percent = 20;
};
Result<ProcGrid> generate_noise(u32 width, u32 height, u64 seed, const NoiseParams& p = {});

}  // namespace mce::kit::procgen
