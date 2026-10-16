#pragma once
// mce_script_compiler (M-SCRIPT 3): the "no hidden state" check over a parsed script.
//
// A script's memory is its components (ADR-0041, open questions 2 and 3). Rejected at compile time:
//   - inside a function, assigning a top-level local, writing into a table a top-level local holds
//     (`t.x = ...`, `t[k] = ...`, `function t.f()`), or changing one through table.insert / remove /
//     clear / sort / move, rawset, setmetatable or buffer.write* / fill / copy: such a value would
//     survive between ticks where state_hash, saves and replays cannot see it;
//   - a table or a function used as a table key where the source shows it (`{[{}] = 1}`,
//     `seen[f] = true` with `local f = function ...`): iteration order over such keys follows
//     addresses, which differ between runs.
// Top-level code itself may build constant tables and handles: it runs once, when the script loads.

#include "mce/fixed_string.h"
#include "mce/types.h"

namespace Luau {
class AstStatBlock;
}

namespace mce::script::detail {

struct StateIssue {
    u32 line = 0;  // 1-based
    u32 col = 0;   // 1-based
    FixedString<256> message;
};

// False and the first issue in source order when the script keeps hidden state.
bool check_state(Luau::AstStatBlock* root, StateIssue& issue);

}  // namespace mce::script::detail
