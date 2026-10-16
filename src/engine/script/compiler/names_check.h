#pragma once
// mce_script_compiler (M-SCRIPT 4): names a script spells out, checked against the engine before
// Luau's type checker sees them, so a typo reads "unknown component 'Movr' (did you mean
// 'Mover'?)" at its own line and column:
//   - the components, events and fields in string literals passed to mce.field, get, set, each,
//     has, add, remove, spawn, events and emit (and the keys of their table literals);
//   - the base-library names the sandbox removes (print, os, io, debug, coroutine, require,
//     getfenv/setfenv, collectgarbage, math.random), with what to use instead.
// Names built at run time are left to the run-time errors.

#include "mce/containers/array.h"
#include "state_check.h"

namespace mce::script::detail {

using NameIssue = StateIssue;

// Appends every issue, in source order.
void check_names(Luau::AstStatBlock* root, Array<NameIssue>& out);

}  // namespace mce::script::detail
