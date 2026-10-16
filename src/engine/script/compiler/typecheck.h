#pragma once
// mce_script_compiler (M-SCRIPT 4): Luau's type checker over one script, against mce.d.luau
// (write_definitions) and Luau's built-in libraries. Nonstrict by default, as Luau is; a script
// that starts with --!strict gets strict checking. Built with exceptions (the one such file in the
// engine): Luau's analysis throws on internal limits, which become an issue here.

#include "names_check.h"

namespace mce::script::detail {

// Appends the type errors, in source order.
void check_types(StringView name, StringView source, Array<NameIssue>& out);

}  // namespace mce::script::detail
