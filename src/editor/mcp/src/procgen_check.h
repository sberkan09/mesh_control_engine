#pragma once
// The seeded-level arguments shared by the procgen_run tool and the .mctest "reachable" check
// (M-PROCGEN 4): one generator kind, a grid size, a first seed, how many seeds, and the generators'
// parameters. Private to mce_mcp_core.

#include "mce/cook/json.h"
#include "mce/cook/string.h"
#include "mce/kit/procgen.h"

namespace mce::mcp {

struct ProcgenRun {
    cook::String kind;  // rooms, caves, walk or noise
    u32 width = 0;
    u32 height = 0;
    u64 seed = 1;   // the first seed
    u64 count = 1;  // seeds seed .. seed + count - 1
    kit::procgen::RoomsParams rooms;
    kit::procgen::CavesParams caves;
    kit::procgen::WalkParams walk;
    kit::procgen::NoiseParams noise;
};

// Reads kind, width, height, seed, count and the generator parameters from `obj` (an object). Appends
// the problem to `err` and returns false when an argument is missing or out of range.
bool parse_procgen_run(const cook::JsonValue& obj, ProcgenRun& out, cook::String& err);

Result<kit::procgen::ProcGrid> procgen_generate(const ProcgenRun& run, u64 seed);

// Is the exit reached from the start over floor cells (4-neighbours)? A check independent of the
// generators' own connectivity guarantee.
bool procgen_exit_reachable(const kit::procgen::ProcGrid& g);

}  // namespace mce::mcp
